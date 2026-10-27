#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""F1-C 单臂独立进程 TP2 真正 FP16 Stock MM+HCCL 对照 T1 MM+MemFabric。
一进程仅启用一种通信后端；绝不在 MemFabric 池进程中创建 HCCL group。
Stock 的主要计时路线是 F.linear(fp16) + HCCL fp16 all_reduce，绝不混入 FP32 转换。
相同固定种子、两份轮换输入和 CPU-gloo FP32参考，仅在窗口外计算参考。
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


N = K = 2048
MS = (1, 8, 10, 16, 32)
ARMS = ("stock_fp16", "memfabric_t1")
DEVICE_SHA = "2a19390c83c3fe6b0bdb8946d88990cca66b331f765ca445dbda2e787c2d2a19"
EXT_SHA = "8eb65de98c3d8664ff10b421c1d7ad31318dc720c3f5679cd4d34e187203bea1"
HOST_SHA = "1dc46d7dcbe583a5dbb78b4c65a0b5e22f9f352976c53ed31b761fa744f91a30"
KVER_SHA = "25e16b0af2f16989231ff0443698261257bd8142834783a0c1e4ad57cd9ebc78"


def args_parse():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--arm", choices=ARMS, required=True)
    p.add_argument("--session", required=True)
    p.add_argument("--m", type=int, choices=MS, required=True)
    p.add_argument("--mm-cores", type=int, default=8, choices=(4, 8))
    p.add_argument("--add-cores", type=int, default=4, choices=(1, 4, 8))
    p.add_argument("--warmup", type=int, default=5)
    p.add_argument("--iters", type=int, default=50)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--store-url", default=os.environ.get(
        "VLLM_ASCEND_310P_MEMFABRIC_STORE_URL", "tcp://127.0.0.1:8581"))
    a = p.parse_args()
    if a.warmup < 3 or a.iters < 30:
        p.error("warmup>=3 and iters>=30, no silent small samples")
    if not a.output.is_dir():
        p.error("session log folder must exist; do not create it inside distributed ranks")
    if int(os.getenv("WORLD_SIZE", "0")) != 2:
        p.error("torchrun TP2 required")
    if "MF_KFC_BYPASS" in os.environ or "MF310P_V2_NOWARMUP" in os.environ:
        p.error("do not bypass KFC or MTE3 guards")
    if not os.environ.get("MF_SDMA_ORCH_JSON", "").endswith("/libmf_sdma_orch_v11.json"):
        p.error("KVER11 must be selected even on Stock arm for environment parity")
    for key, expected in (("F1C_DEVICE_SHA256", DEVICE_SHA),
                          ("F1C_PLUGIN_SHA256", EXT_SHA),
                          ("F1C_HOST_SHA256", HOST_SHA),
                          ("F1C_KVER_SHA256", KVER_SHA)):
        if os.environ.get(key) != expected:
            p.error(f"binary identity mismatch or missing {key}")
    plugin_actual = hashlib.sha256(Path(extension.__file__).read_bytes()).hexdigest()
    if plugin_actual != EXT_SHA:
        p.error("currently imported Python plugin differs from Gate-C diagnostic ON binary")
    return a


def _trial_bind():
    lib_path = os.environ["V2_HOST_LIB"]
    lib = ctypes.CDLL(lib_path)
    P, U32, U64 = ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint64
    specs = {
        "mf310p_v2_trial_create": ([ctypes.c_int, ctypes.c_char_p, U32, ctypes.POINTER(P)], ctypes.c_int),
        "mf310p_v2_trial_ib_probe": ([P, U32, P], ctypes.c_int),
        "mf310p_v2_trial_run_nz_direct_small":
            ([P, U64, U64, U64, U32, U32, U32, P], ctypes.c_int),
        "mf310p_v2_trial_check_status": ([P, ctypes.POINTER(U64)], ctypes.c_int),
        "mf310p_v2_trial_destroy": ([P], ctypes.c_int),
    }
    for key, (types, ret) in specs.items():
        fn = getattr(lib, key)
        fn.argtypes, fn.restype = types, ret
    return lib


def _must(rc, op):
    if rc:
        raise RuntimeError(f"{op} returned {rc}")


def _inputs(rank, m, device, cpu_group):
    torch.manual_seed(4242)
    w = torch.randn(N, K, dtype=torch.float16, device=device)
    # T1 小M诊断核的历史合同要求无条件 FRACTAL_NZ，不能受全局开关影响。
    w_nz = torch_npu.npu_format_cast(w, 29)
    xs, refs = [], []
    for sample in range(2):
        torch.manual_seed(1200 + rank * 17 + m + sample * 1009)
        x = torch.randn(m, K, dtype=torch.float16, device=device)
        # 参考始终通过CPU gloo做FP32求和并转换成FP16，与T1历史bit-exact合同相同。
        ref = F.linear(x, w).float().cpu()
        dist.all_reduce(ref, group=cpu_group)
        refs.append(ref.to(device=device, dtype=torch.float16))
        xs.append(x)
    if torch.equal(refs[0], refs[1]):
        raise ValueError("two alternating inputs produced identical outputs")
    torch.npu.current_stream().synchronize()
    return w, w_nz, xs, refs


def _launch_stock(x, w, hccl):
    # 严格FP16原生Stock，不允许float()/to(half)污染性能窗口。
    y = F.linear(x, w)
    if y.dtype != torch.float16:
        raise AssertionError("F.linear stock lost FP16")
    dist.all_reduce(y, op=dist.ReduceOp.SUM, group=hccl)
    return y


def _time_one(op, stream):
    ev_start = torch.npu.Event(enable_timing=True)
    ev_end = torch.npu.Event(enable_timing=True)
    t0 = time.perf_counter_ns()
    ev_start.record(stream)
    result = op()
    ev_end.record(stream)
    stream.synchronize()
    wall_us = (time.perf_counter_ns() - t0) / 1000.0
    event_us = ev_start.elapsed_time(ev_end) * 1000.0
    if not all(math.isfinite(x) and x > 0 for x in (wall_us, event_us)):
        raise ValueError("invalid timings")
    return result, round(wall_us, 2), round(event_us, 2)


def main():
    a = args_parse()
    rank = int(os.environ["RANK"])
    local_rank = int(os.environ["LOCAL_RANK"])
    torch.npu.set_device(local_rank)
    device = torch.device(f"npu:{local_rank}")
    # 沿用已在T1实机验证的group初始化顺序：
    # Stock默认HCCL、另建CPU-gloo参考/屏障；T1仅初始化gloo、绝不创建HCCL组。
    if a.arm == "stock_fp16":
        dist.init_process_group(backend="hccl")
        cpu_group = dist.new_group(backend="gloo")
        hccl = dist.group.WORLD
    else:
        dist.init_process_group(backend="gloo")
        cpu_group = dist.group.WORLD
        hccl = None
    stream = torch.npu.current_stream()
    lib, handle = None, ctypes.c_void_p()
    data = None
    try:
        w, w_nz, xs, refs = _inputs(rank, a.m, device, cpu_group)
        output = torch.empty((a.m, N), device=device, dtype=torch.float16)
        output.zero_()
        stream.synchronize()
        if a.arm == "memfabric_t1":
            lib = _trial_bind()
            dist.barrier(group=cpu_group)
            _must(lib.mf310p_v2_trial_create(rank, a.store_url.encode(), 256,
                                              ctypes.byref(handle)), "create")
            if not handle.value:
                raise RuntimeError("pool handle NULL")
            _must(lib.mf310p_v2_trial_ib_probe(handle, 1000,
                     ctypes.c_void_p(stream.npu_stream)), "ib_probe")
            stream.synchronize()
            status = ctypes.c_uint64()
            _must(lib.mf310p_v2_trial_check_status(handle, ctypes.byref(status)), "status after probe")
            if status.value:
                raise RuntimeError(f"ib_probe device status={status.value}")

        def single(i):
            x, ref = xs[i % 2], refs[i % 2]
            if a.arm == "stock_fp16":
                op = lambda: _launch_stock(x, w, hccl)
            else:
                def op():
                    _must(lib.mf310p_v2_trial_run_nz_direct_small(
                        handle, x.data_ptr(), w_nz.data_ptr(), output.data_ptr(),
                        a.m, a.mm_cores, a.add_cores, ctypes.c_void_p(stream.npu_stream)), "T1 run")
                    return output

            got, wall_us, event_us = _time_one(op, stream)
            if a.arm == "memfabric_t1":
                st = ctypes.c_uint64()
                _must(lib.mf310p_v2_trial_check_status(handle, ctypes.byref(st)), "T1 status")
                if st.value:
                    raise RuntimeError(f"rank{rank} device protocol status={st.value}")
            if not torch.equal(got, ref):
                mismatch = int((got != ref).sum().item())
                raise AssertionError(f"rank{rank} {a.arm} m={a.m} sample={i}: {mismatch} bit-exact mismatches")
            return wall_us, event_us

        for i in range(a.warmup):
            dist.barrier(group=cpu_group)
            single(i)
        stream.synchronize()
        dist.barrier(group=cpu_group)
        rows = []
        for i in range(a.iters):
            dist.barrier(group=cpu_group)
            wall, ev = single(i + a.warmup)
            rows.append({"i": i, "input_variant": (i + a.warmup) % 2,
                         "wall_us": wall, "event_us": ev})
        dist.barrier(group=cpu_group)
        data = {
            "experiment": "F1C_SEPARATE_PROCESS_CROSS_ABBA",
            "rank": rank, "arm": a.arm, "session": a.session,
            "M": a.m, "K": K, "N": N, "TP": 2,
            "dtype": "fp16", "stock_collective_dtype": "fp16",
            "reference": "CPU_GLOO_FP32_SUM_TO_FP16",
            "communication_isolation": "HCCL group only Stock; MemFabric pool only T1",
            "host_f32_conversion_in_timed_stock": False,
            "mm_cores": a.mm_cores if a.arm == "memfabric_t1" else None,
            "add_cores": a.add_cores if a.arm == "memfabric_t1" else None,
            "warmup": a.warmup, "iters": a.iters, "bit_exact": True,
            "protocol_status": 0 if a.arm == "memfabric_t1" else None,
            "device_so_sha256": os.environ["F1C_DEVICE_SHA256"],
            "plugin_sha256": os.environ["F1C_PLUGIN_SHA256"],
            "host_so_sha256": os.environ["F1C_HOST_SHA256"],
            "kver11_sha256": os.environ["F1C_KVER_SHA256"],
            "samples": rows,
            "metric_semantics": "paired per-index max of both ranks; stream events vs host wall; HCCL multi-stream must be audited on device",
        }
        a.output.mkdir(parents=True, exist_ok=True)
        path = a.output / f"{a.session}_rank{rank}.json"
        if path.exists():
            raise FileExistsError(path)
        path.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n")
        print(json.dumps({"session": a.session, "rank": rank, "arm": a.arm,
                          "samples": len(rows), "bit_exact": True}), flush=True)
    finally:
        if handle.value:
            try:
                stream.synchronize()
                _must(lib.mf310p_v2_trial_destroy(handle), "destroy")
            except Exception as e:
                print(f"rank{rank} cleanup_error {e}", flush=True)
        if dist.is_initialized():
            dist.destroy_process_group()


if __name__ == "__main__":
    main()
