#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""U1 TP2多形状单会话：HCCL Stock / MemFabric T1 / MemFabric D4严格进程隔离。
注意：Event测设备流完成窗口（可能包含Host下发空档），非纯核执行周期。
"""
from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import math
import os
from pathlib import Path
import time

import torch
import torch.distributed as dist
import torch.nn.functional as F
import torch_npu  # noqa: F401
import vllm_ascend.vllm_ascend_C as extension

from u1_spec import (
    ADD_CORES, CHUNK, D4_DEVICE_SHA, FUSED_ARM, ITERS, K, KVER_SHA,
    MEMFABRIC_HOST_SHA, MM_CORES, N, PLUGIN_SHA, SHAPES, WARMUP,
    arm_for, eligible, order_for, plan, session_id,
)


def _sha(path: str) -> str:
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def parse():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--scope", choices=SHAPES, required=True)
    p.add_argument("--phase", choices=ITERS, required=True)
    p.add_argument("--stage", type=int, choices=range(1, 9), required=True)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--store-url", default=os.environ.get(
        "VLLM_ASCEND_310P_MEMFABRIC_STORE_URL", "tcp://127.0.0.1:8581"))
    a = p.parse_args()
    if a.phase in ("smoke", "screen") and a.stage not in (1, 2):
        p.error("screen stage must be 1/2")
    if int(os.environ.get("WORLD_SIZE", "0")) != 2:
        p.error("U1 requires two TP workers")
    if not a.out.is_dir():
        p.error("precreated output directory required")
    if any((a.out / f"{session_id(a.scope, a.stage)}_rank{r}.json").exists()
           for r in (0, 1)):
        p.error("refuse to overwrite session JSON")
    if ("MF_KFC_BYPASS" in os.environ or "MF310P_V2_NOWARMUP" in os.environ or
            "MF310P_V2_AR_ONLY_NOWARMUP" in os.environ):
        p.error("MemFabric first-write/KFC bypass forbidden")
    if not os.environ.get("MF_SDMA_ORCH_JSON", "").endswith("/libmf_sdma_orch_v11.json"):
        p.error("KVER11 must be explicit")
    identity = {
        "U1_DEVICE_SHA256": D4_DEVICE_SHA, "U1_PLUGIN_SHA256": PLUGIN_SHA,
        "U1_HOST_SHA256": MEMFABRIC_HOST_SHA, "U1_KVER_SHA256": KVER_SHA,
    }
    for key, expected in identity.items():
        if os.environ.get(key) != expected:
            p.error(f"{key} missing or wrong. Stop instead of running other binaries")
    if _sha(extension.__file__) != PLUGIN_SHA:
        p.error("actually imported Python Host extension is not the pinned diagnostic ON build")
    return a


def bind_trial():
    lib = ctypes.CDLL(os.environ["V2_HOST_LIB"])
    P, U32, U64 = ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint64
    api = {
        "mf310p_v2_trial_create": ([ctypes.c_int, ctypes.c_char_p, U32, ctypes.POINTER(P)], ctypes.c_int),
        "mf310p_v2_trial_ib_probe": ([P, U32, P], ctypes.c_int),
        "mf310p_v2_trial_run_nz_direct_small": ([P, U64, U64, U64, U32, U32, U32, P], ctypes.c_int),
        "mf310p_v2_trial_run": ([P, U64, U64, U64, U32, U32, P], ctypes.c_int),
        "mf310p_v2_trial_check_status": ([P, ctypes.POINTER(U64)], ctypes.c_int),
        "mf310p_v2_trial_destroy": ([P], ctypes.c_int),
    }
    for name, (types, restype) in api.items():
        fn = getattr(lib, name)
        fn.argtypes, fn.restype = types, restype
    return lib


def require(rc, context):
    if rc != 0:
        raise RuntimeError(f"{context} failed with rc={rc}")


def inputs_for(rank, m, device, group, weight):
    xs, refs = [], []
    for variant in range(2):
        torch.manual_seed(1200 + rank * 17 + m + variant * 1009)
        x = torch.randn(m, K, dtype=torch.float16, device=device)
        partial = F.linear(x, weight).float().cpu()
        dist.all_reduce(partial, group=group)
        xs.append(x)
        refs.append(partial.to(device=device, dtype=torch.float16))
    if torch.equal(refs[0], refs[1]):
        raise ValueError(f"M{m}: two test input variants produced same reference")
    return xs, refs


def measure_one(op, stream, ev0, ev1):
    t0 = time.perf_counter_ns()
    ev0.record(stream)
    y = op()
    ev1.record(stream)
    stream.synchronize()
    t1 = time.perf_counter_ns()
    wall = (t1 - t0) / 1000.0
    event = ev0.elapsed_time(ev1) * 1000.0
    if not all(math.isfinite(v) and v > 0 for v in (wall, event)):
        raise RuntimeError("nonfinite or zero NPU/host latency")
    return y, round(wall, 3), round(event, 3)


def check_protocol(lib, handle):
    status = ctypes.c_uint64()
    require(lib.mf310p_v2_trial_check_status(handle, ctypes.byref(status)), "status read")
    if status.value != 0:
        raise RuntimeError(f"MemFabric protocol status {status.value}")


def main():
    a = parse()
    rank = int(os.environ["RANK"])
    local_rank = int(os.environ["LOCAL_RANK"])
    torch.npu.set_device(local_rank)
    device = torch.device(f"npu:{local_rank}")
    arm = arm_for(a.scope, a.stage)
    # Stock与MemFabric禁止同进程共存，沿用F1-C已验证的group顺序。
    if arm == "stock_fp16":
        dist.init_process_group(backend="hccl")
        cpu_group = dist.new_group(backend="gloo")
        hccl = dist.group.WORLD
    else:
        dist.init_process_group(backend="gloo")
        cpu_group = dist.group.WORLD
        hccl = None
    stream = torch.npu.current_stream()
    raw_stream = ctypes.c_void_p(stream.npu_stream)
    handle = ctypes.c_void_p()
    lib = None
    error = None
    try:
        order = tuple(plan(a.scope, a.phase)[a.stage-1]["M_order"])
        torch.manual_seed(4242)
        w = torch.randn(N, K, device=device, dtype=torch.float16)
        w_nz = torch_npu.npu_format_cast(w, 29)
        # 所有CPU-gloo参考在MemFabric建池之前完成。
        all_inputs = {m: inputs_for(rank, m, device, cpu_group, w) for m in order}
        outputs = {m: torch.zeros((m, N), device=device, dtype=torch.float16) for m in order}
        stream.synchronize()
        if arm != "stock_fp16":
            lib = bind_trial()
            dist.barrier(group=cpu_group)
            require(lib.mf310p_v2_trial_create(
                rank, a.store_url.encode(), 256 if a.scope == "small" else CHUNK,
                ctypes.byref(handle)), "create")
            if not handle.value:
                raise RuntimeError("trial_create returned NULL")
            require(lib.mf310p_v2_trial_ib_probe(handle, 1000, raw_stream), "ib_probe")
            stream.synchronize()
            check_protocol(lib, handle)
        results = {}
        for m in order:
            if not eligible(a.scope, m):
                raise ValueError(f"unsupported combination {a.scope}/M{m}")
            xs, refs = all_inputs[m]
            output = outputs[m]
            e0 = torch.npu.Event(enable_timing=True)
            e1 = torch.npu.Event(enable_timing=True)

            def once(i):
                variant = i % 2
                x, ref = xs[variant], refs[variant]
                if arm == "stock_fp16":
                    def op():
                        y = F.linear(x, w)
                        if y.dtype != torch.float16:
                            raise RuntimeError("Stock F.linear not FP16")
                        dist.all_reduce(y, op=dist.ReduceOp.SUM, group=hccl)
                        return y
                elif arm == "memfabric_t1":
                    def op():
                        require(lib.mf310p_v2_trial_run_nz_direct_small(
                            handle, x.data_ptr(), w_nz.data_ptr(), output.data_ptr(),
                            m, MM_CORES, ADD_CORES, raw_stream), "T1")
                        return output
                else:
                    def op():
                        require(lib.mf310p_v2_trial_run(
                            handle, x.data_ptr(), w_nz.data_ptr(), output.data_ptr(),
                            m, 1, raw_stream), "D4")
                        return output

                y, wall, ev = measure_one(op, stream, e0, e1)
                if lib is not None:
                    check_protocol(lib, handle)
                if not torch.equal(y, ref):
                    raise AssertionError(f"rank{rank} {arm} M{m} variant{variant}: "
                                         f"{int((y != ref).sum().item())} mismatched FP16 elements")
                return {"i": i, "variant": variant, "event_us": ev, "wall_us": wall}

            for i in range(WARMUP[a.phase]):
                dist.barrier(group=cpu_group)
                once(i)
            dist.barrier(group=cpu_group)
            samples = []
            for i in range(ITERS[a.phase]):
                dist.barrier(group=cpu_group)
                z = once(i + WARMUP[a.phase])
                z["i"] = i
                samples.append(z)
            results[str(m)] = {
                "M": m, "arm": arm, "warmup": WARMUP[a.phase],
                "iters": ITERS[a.phase], "samples": samples,
                "status": 0 if lib is not None else None,
                "bit_exact": True,
            }
            if arm == "stock_fp16" and a.phase == "screen":
                # 顺带测原生MM-only：不创建额外进程，不把两项的中位数差
                # 解释为纯HCCL时延（包含发射/输出分配、缓存与调度差异）。
                local_refs = [F.linear(x, w) for x in xs]
                stream.synchronize()
                local_samples = []
                for i in range(ITERS[a.phase]):
                    variant = (i + WARMUP[a.phase]) % 2
                    dist.barrier(group=cpu_group)
                    y, wall, event = measure_one(
                        lambda x=xs[variant]: F.linear(x, w), stream, e0, e1)
                    if not torch.equal(y, local_refs[variant]):
                        raise AssertionError(f"U1 local MM-only wrong rank{rank} M{m}")
                    local_samples.append({"i": i, "variant": variant,
                                          "event_us": event, "wall_us": wall})
                results[str(m)]["native_mm_only"] = {
                    "samples": local_samples, "iters": ITERS[a.phase],
                    "bit_exact": True,
                    "note": "separate native MM invocation, NOT a decomposable phase inside stock+HCCL",
                }
            # 每个M验完立即单独落盘。若后续新形状故障，已完成形状证据不会消失；
            # partial只允许事后诊断，不允许通过U1总分析器的完整会话门禁。
            partial = a.out / "partial"
            partial.mkdir(exist_ok=True)
            with (partial / f"m{m}_rank{rank}.json").open("x") as fp:
                json.dump({
                    "experiment": "U1_PARTIAL_NOT_FOR_GO", "rank": rank,
                    "session": session_id(a.scope, a.stage), "scope": a.scope,
                    "phase": a.phase, "M": m, "arm": arm,
                    "device_so_sha256": os.environ["U1_DEVICE_SHA256"],
                    "result": results[str(m)],
                }, fp, ensure_ascii=False, indent=2)
                fp.write("\n")
            print(json.dumps({"rank": rank, "stage": a.stage, "arm": arm, "M": m,
                              "samples": len(samples), "status": "PASS"}), flush=True)
        # 额外回测首形状，识别单Worker生命周期漂移；不替代主体慢样本。
        anchor_m = order[0]
        anchor_xs, anchor_refs = all_inputs[anchor_m]
        anchor_output = outputs[anchor_m]
        anchor_e0 = torch.npu.Event(enable_timing=True)
        anchor_e1 = torch.npu.Event(enable_timing=True)
        anchor_samples = []
        for i in range(8):
            dist.barrier(group=cpu_group)
            x, ref = anchor_xs[i % 2], anchor_refs[i % 2]
            if arm == "stock_fp16":
                def anchor_op():
                    y = F.linear(x, w)
                    dist.all_reduce(y, op=dist.ReduceOp.SUM, group=hccl)
                    return y
            elif arm == "memfabric_t1":
                def anchor_op():
                    require(lib.mf310p_v2_trial_run_nz_direct_small(
                        handle, x.data_ptr(), w_nz.data_ptr(), anchor_output.data_ptr(),
                        anchor_m, MM_CORES, ADD_CORES, raw_stream), "anchor T1")
                    return anchor_output
            else:
                def anchor_op():
                    require(lib.mf310p_v2_trial_run(
                        handle, x.data_ptr(), w_nz.data_ptr(), anchor_output.data_ptr(),
                        anchor_m, 1, raw_stream), "anchor D4")
                    return anchor_output
            y, wall, event = measure_one(anchor_op, stream, anchor_e0, anchor_e1)
            if lib is not None:
                check_protocol(lib, handle)
            if not torch.equal(y, ref):
                raise AssertionError(f"U1 late anchor wrong rank{rank} M{anchor_m}")
            anchor_samples.append({"i": i, "variant": i % 2,
                                   "wall_us": wall, "event_us": event})
        results["_anchor_after"] = {"M": anchor_m, "samples": anchor_samples}
        data = {
            "experiment": "U1_SMOKE_ONLY" if a.phase == "smoke" else "U1_FULLCHAIN",
            "scope": a.scope, "phase": a.phase,
            "session": session_id(a.scope, a.stage), "stage": a.stage,
            "arm": arm, "rank": rank, "M_order": order, "TP": 2, "K": K, "N": N,
            "dtype": "fp16", "reference": "CPU_GLOO_FP32_SUM_TO_FP16",
            "stock_collective": "FP16_HCCL_no_FP32_conversion",
            "comm_isolation": "separate process; never HCCL with MemFabric pool",
            "device_so_sha256": os.environ["U1_DEVICE_SHA256"],
            "plugin_sha256": os.environ["U1_PLUGIN_SHA256"],
            "host_so_sha256": os.environ["U1_HOST_SHA256"],
            "kver11_sha256": os.environ["U1_KVER_SHA256"],
            "profile": False, "results": results,
            "event_semantics": "NPU stream bracketing, may include Host enqueue gaps, not pure kernel cycles",
        }
        out = a.out / f"{session_id(a.scope, a.stage)}_rank{rank}.json"
        with out.open("x") as f:
            json.dump(data, f, ensure_ascii=False, indent=2)
            f.write("\n")
        dist.barrier(group=cpu_group)
    except BaseException as ex:
        error = ex
        raise
    finally:
        if handle.value:
            try:
                stream.synchronize()
                require(lib.mf310p_v2_trial_destroy(handle), "destroy")
            except BaseException as ex:
                print(f"U1 cleanup FAILED rank={rank}: {ex!r}", flush=True)
                if error is None:
                    raise
        if dist.is_initialized():
            dist.destroy_process_group()


if __name__ == "__main__":
    main()
