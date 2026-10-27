#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""U1大M机制诊断：单会话仅一个Case，gloo参考，禁止HCCL/MemFabric共存。
formal=4Chunk overlap1，loop4=4Chunk serial，diag0/4/5=1024行单Chunk诊断；
Profiler单独短会话，不参与正式Stock/Fused性能评估。
"""
from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import math
import os
from pathlib import Path
import statistics
import time

import torch
import torch.distributed as dist
import torch.nn.functional as F
import torch_npu  # noqa: F401
import vllm_ascend.vllm_ascend_C as extension

from u1_spec import CHUNK, D4_DEVICE_SHA, K, N, PLUGIN_SHA, MEMFABRIC_HOST_SHA, KVER_SHA

M = 4096
CASES = ("formal", "loop4", "diag0", "diag4", "diag5")


def must(rc, label):
    if rc:
        raise RuntimeError(f"{label}: rc={rc}")


def bind():
    lib = ctypes.CDLL(os.environ["V2_HOST_LIB"])
    P, U32, U64 = ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint64
    api = {
        "mf310p_v2_trial_create": ([ctypes.c_int, ctypes.c_char_p, U32, ctypes.POINTER(P)], ctypes.c_int),
        "mf310p_v2_trial_ib_probe": ([P, U32, P], ctypes.c_int),
        "mf310p_v2_trial_run": ([P, U64, U64, U64, U32, U32, P], ctypes.c_int),
        "mf310p_v2_trial_diag": ([P, U64, U64, U64, U32, P], ctypes.c_int),
        "mf310p_v2_trial_diag_loop4": ([P, U64, U64, U64, P], ctypes.c_int),
        "mf310p_v2_trial_check_status": ([P, ctypes.POINTER(U64)], ctypes.c_int),
        "mf310p_v2_trial_destroy": ([P], ctypes.c_int),
    }
    for name, (types, rt) in api.items():
        f = getattr(lib, name)
        f.argtypes, f.restype = types, rt
    return lib


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--case", choices=CASES, required=True)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--warmup", type=int, default=3)
    p.add_argument("--iters", type=int, default=8)
    p.add_argument("--profile", action="store_true")
    p.add_argument("--store-url", default=os.getenv(
        "VLLM_ASCEND_310P_MEMFABRIC_STORE_URL", "tcp://127.0.0.1:8581"))
    a = p.parse_args()
    if not a.out.is_dir() or a.warmup < 2 or a.iters < 3:
        p.error("precreated output directory, warmup>=2 and iters>=3 required")
    if a.iters > 30:
        p.error("diagnostic short session only: iters<=30")
    if int(os.environ.get("WORLD_SIZE", "0")) != 2:
        p.error("TP2 only")
    if "MF_KFC_BYPASS" in os.environ or "MF310P_V2_NOWARMUP" in os.environ:
        p.error("no safety bypass")
    identities = (("U1_DEVICE_SHA256", D4_DEVICE_SHA),
                  ("U1_PLUGIN_SHA256", PLUGIN_SHA),
                  ("U1_HOST_SHA256", MEMFABRIC_HOST_SHA),
                  ("U1_KVER_SHA256", KVER_SHA))
    if any(os.environ.get(key) != value for key, value in identities):
        p.error("D4/Host/MemFabric/KVER identity missing; complete G0")
    host_path = os.environ.get("V2_HOST_LIB")
    if not host_path or Path(host_path).resolve() != Path(extension.__file__).resolve():
        p.error("actually imported diagnostic Host SO differs from specified library")
    if hashlib.sha256(Path(extension.__file__).read_bytes()).hexdigest() != PLUGIN_SHA:
        p.error("imported Python plugin SHA mismatch")
    device_path = os.environ.get("U1_DEVICE_SO_PATH")
    if not device_path or hashlib.sha256(Path(device_path).read_bytes()).hexdigest() != D4_DEVICE_SHA:
        p.error("pinned device SO mismatch")
    json_path = os.environ.get("MF_SDMA_ORCH_JSON", "")
    if not json_path.endswith("/libmf_sdma_orch_v11.json") or not Path(json_path).is_file():
        p.error("KVER11 JSON missing")

    rank = int(os.environ["RANK"])
    local = int(os.environ["LOCAL_RANK"])
    torch.npu.set_device(local)
    device = torch.device(f"npu:{local}")
    dist.init_process_group(backend="gloo")
    group = dist.group.WORLD
    stream = torch.npu.current_stream()
    ptr = ctypes.c_void_p(stream.npu_stream)
    handle = ctypes.c_void_p()
    lib = None
    error = None
    try:
        torch.manual_seed(86420 + rank)
        w = torch.randn(N, K, dtype=torch.float16, device=device)
        w_nz = torch_npu.npu_format_cast(w, 29)
        torch.manual_seed(74320 + rank * 101)
        x = torch.randn(M, K, dtype=torch.float16, device=device)
        first_x = x.narrow(0, 0, CHUNK)
        local_mm = F.linear(first_x, w)
        one = local_mm.float().cpu()
        dist.all_reduce(one, group=group)
        one = one.to(device=device, dtype=torch.float16)
        full = F.linear(x, w).float().cpu()
        dist.all_reduce(full, group=group)
        full = full.to(device=device, dtype=torch.float16)
        single = a.case.startswith("diag")
        xx = first_x if single else x
        ref = local_mm if a.case == "diag0" else one if single else full
        y = torch.zeros((CHUNK if single else M, N), device=device, dtype=torch.float16)
        stream.synchronize()
        lib = bind()
        dist.barrier()
        must(lib.mf310p_v2_trial_create(rank, a.store_url.encode(), CHUNK,
                                         ctypes.byref(handle)), "pool create")
        if not handle.value:
            raise RuntimeError("NULL pool")
        must(lib.mf310p_v2_trial_ib_probe(handle, 1000, ptr), "IB probe")
        stream.synchronize()

        def launch():
            if a.case == "formal":
                must(lib.mf310p_v2_trial_run(
                    handle, xx.data_ptr(), w_nz.data_ptr(), y.data_ptr(), M, 1, ptr), "formal")
            elif a.case == "loop4":
                must(lib.mf310p_v2_trial_diag_loop4(
                    handle, xx.data_ptr(), w_nz.data_ptr(), y.data_ptr(), ptr), "loop4")
            else:
                must(lib.mf310p_v2_trial_diag(
                    handle, xx.data_ptr(), w_nz.data_ptr(), y.data_ptr(),
                    int(a.case[4:]), ptr), "diag")
            return y

        def verified_sample(index, timing=True):
            if timing:
                ev0 = torch.npu.Event(enable_timing=True)
                ev1 = torch.npu.Event(enable_timing=True)
                t0 = time.perf_counter_ns()
                ev0.record(stream)
            result = launch()
            if timing:
                ev1.record(stream)
            stream.synchronize()
            event = ev0.elapsed_time(ev1) * 1000 if timing else None
            wall = (time.perf_counter_ns() - t0) / 1000 if timing else None
            status = ctypes.c_uint64()
            must(lib.mf310p_v2_trial_check_status(handle, ctypes.byref(status)), "protocol status")
            if status.value:
                raise RuntimeError(f"diag {a.case} protocol status={status.value}")
            if not torch.equal(result, ref):
                raise AssertionError(f"diag {a.case} rank{rank} sample{index}: wrong result")
            if timing and (not math.isfinite(event) or event <= 0 or not math.isfinite(wall) or wall <= 0):
                raise ValueError("invalid diag timing")
            return {"i": index, "event_us": round(event, 3),
                    "wall_us": round(wall, 3)} if timing else None

        for i in range(a.warmup):
            dist.barrier()
            verified_sample(i, timing=False)
        dist.barrier()
        samples = []
        trace = None
        if a.profile:
            from torch_npu.profiler import ProfilerActivity, profile
            with profile(activities=[ProfilerActivity.CPU, ProfilerActivity.NPU],
                         record_shapes=False, with_stack=False) as prof:
                for i in range(a.iters):
                    dist.barrier()
                    verified_sample(i, timing=False)
            trace = a.out / f"{a.case}_rank{rank}_trace.json"
            if trace.exists():
                raise FileExistsError(trace)
            prof.export_chrome_trace(str(trace))
        else:
            for i in range(a.iters):
                dist.barrier()
                samples.append(verified_sample(i))
        data = {
            "experiment": "U1_LARGE_MECHANISM_DIAG", "case": a.case,
            "rank": rank, "M": CHUNK if single else M,
            "chunks": 1 if single else 4, "chunk_rows": CHUNK,
            "bit_exact": True, "status": 0, "profile": a.profile,
            "iters": a.iters, "warmup": a.warmup, "samples": samples,
            "trace": trace.name if trace else None,
            "event_p50_us": round(statistics.median(s["event_us"] for s in samples), 3)
                if samples else None,
            "note": "diag0/4/5 are SINGLE chunk (1024) with non-equivalent protocol; "
                    "loop4 serial is not four-chunk formal overlap. "
                    "Never subtract profiler diagnostics to claim actual kernel phase durations.",
        }
        with (a.out / f"{a.case}_rank{rank}_result.json").open("x") as fp:
            json.dump(data, fp, ensure_ascii=False, indent=2)
            fp.write("\n")
        dist.barrier()
    except BaseException as exc:
        error = exc
        raise
    finally:
        if handle.value:
            try:
                stream.synchronize()
                must(lib.mf310p_v2_trial_destroy(handle), "pool destroy")
            except BaseException as exc:
                print(f"U1 diag cleanup failed rank{rank}: {exc!r}", flush=True)
                if error is None:
                    raise
        if dist.is_initialized():
            dist.destroy_process_group()


if __name__ == "__main__":
    main()
