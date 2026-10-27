# SPDX-License-Identifier: Apache-2.0
"""T2 Profiler 分解测试：采集 Chrome 时间线，对比
A（Stock）、B（原生 MM+暂存+AR-only Kernel）和可选的
C（融合 Kernel）。

逐次输出设备时间统计：MatMul Kernel、D2D 拷贝、
AR-only Kernel 和 HCCL Kernel 的耗时。
"""

from __future__ import annotations

import argparse
import atexit
import contextlib
import ctypes
import json
import os

import torch
import torch.distributed as dist
import torch.nn.functional as F
import torch_npu  # noqa: F401
import vllm_ascend.vllm_ascend_C as extension
from torch_npu.profiler import ProfilerActivity, profile

from vllm_ascend.utils import maybe_trans_nz


def bind_library():
    lib = ctypes.CDLL(extension.__file__)
    P, U32, U64 = ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint64
    signatures = {
        "mf310p_v2_trial_create": ([ctypes.c_int, ctypes.c_char_p, U32, ctypes.POINTER(P)], ctypes.c_int),
        "mf310p_v2_trial_ib_probe": ([P, U32, P], ctypes.c_int),
        "mf310p_v2_trial_run_ar_only": ([P, U64, U64, U32, U32, P], ctypes.c_int),
        "mf310p_v2_trial_check_status": ([P, ctypes.POINTER(U64)], ctypes.c_int),
        "mf310p_v2_trial_destroy": ([P], ctypes.c_int),
    }
    for name, (args, result) in signatures.items():
        fn = getattr(lib, name)
        fn.argtypes, fn.restype = args, result
    return lib


def optional_test_kfc_bypass():
    if os.environ.get("MF_KFC_BYPASS") != "1":
        return
    paths = []
    for index in range(16):
        path = f"/tmp/mf_kfc_{os.getpid()}_{index}.json"
        try:
            fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_RDWR, 0o600)
            os.close(fd)
            paths.append(path)
        except FileExistsError:
            continue

    def cleanup():
        for path in paths:
            with contextlib.suppress(FileNotFoundError):
                os.unlink(path)

    atexit.register(cleanup)


def summarize_trace(path, rank):
    with open(path) as f:
        data = json.load(f)
    if isinstance(data, dict):
        raw = data.get("traceEvents", [])
    else:
        raw = data
    events = [e for e in raw if isinstance(e, dict) and e.get("ph") == "X" and isinstance(e.get("dur"), (int, float))]
    buckets = {"matmul": [], "memcpy": [], "ar_kernel": [], "hccl": [], "memset": [], "other_kernel": []}
    for e in events:
        name = e.get("name", "")
        cat = e.get("cat", "")
        if "Memcpy" in name or "memcpy" in name:
            buckets["memcpy"].append(e)
        elif "V2MemfabricArOnlyKernel" in name:
            buckets["ar_kernel"].append(e)
        elif "hccl" in name.lower() or "AllReduce" in name or "allreduce" in name.lower():
            buckets["hccl"].append(e)
        elif "Matmul" in name or "matmul" in name or "GEMM" in name.upper():
            buckets["matmul"].append(e)
        elif "Memset" in name or "memset" in name:
            buckets["memset"].append(e)
        elif cat == "kernel":
            buckets["other_kernel"].append(e)
    out = {}
    for k, evs in buckets.items():
        durs = sorted(round(e["dur"], 2) for e in evs)
        if durs:
            n = len(durs)
            out[k] = {
                "count": n,
                "median_us": durs[n // 2],
                "p95_us": durs[min(n - 1, int(0.95 * (n - 1)))],
                "min_us": durs[0],
                "max_us": durs[-1],
                "total_ms": round(sum(durs) / 1000, 3),
            }
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--m", type=int, default=8192)
    ap.add_argument("--add", type=int, default=8)
    ap.add_argument("--chunk-rows", type=int, default=1024)
    ap.add_argument("--iters", type=int, default=20)
    ap.add_argument("--out", default="/tmp/opencode/t2/prof")
    a = ap.parse_args()

    rank = int(os.environ["RANK"])
    local_rank = int(os.environ["LOCAL_RANK"])
    torch.npu.set_device(local_rank)
    device = torch.device(f"npu:{local_rank}")
    dist.init_process_group(backend="hccl")
    cpu_group = dist.new_group(backend="gloo")
    stream = torch.npu.current_stream()
    raw_stream = ctypes.c_void_p(stream.npu_stream)

    torch.manual_seed(7865 + rank)
    weight = maybe_trans_nz(torch.randn(2048, 2048, dtype=torch.float16, device=device))
    torch.manual_seed(2500 + rank * 103 + a.m)
    x = torch.randn(a.m, 2048, device=device, dtype=torch.float16)
    partial = F.linear(x, weight)
    ref = partial.float()
    dist.all_reduce(ref)
    ref = ref.half()
    stream.synchronize()
    torch.npu.synchronize()
    dist.barrier(group=cpu_group)

    # A 路径：建池前采集 Stock 的设备时间线。
    if rank == 0:
        print(f"=== profiling A stock M={a.m} ===", flush=True)
    with profile(
        activities=[ProfilerActivity.CPU, ProfilerActivity.NPU], with_stack=False, record_shapes=False
    ) as prof:
        for _ in range(a.iters):
            local = F.linear(x, weight)
            dist.all_reduce(local)
            stream.synchronize()
            del local
    pa = f"{a.out}_stock_rank{rank}.json"
    os.makedirs(a.out, exist_ok=True)
    prof.export_chrome_trace(pa)
    dist.barrier(group=cpu_group)
    print(f"[rank{rank}] A summary: {json.dumps(summarize_trace(pa, rank))}", flush=True)

    optional_test_kfc_bypass()
    lib = bind_library()
    handle = ctypes.c_void_p()
    rc = lib.mf310p_v2_trial_create(
        rank,
        os.environ.get("VLLM_ASCEND_310P_MEMFABRIC_STORE_URL", "tcp://127.0.0.1:8581").encode("utf-8"),
        a.chunk_rows,
        ctypes.byref(handle),
    )
    assert rc == 0 and handle.value
    assert lib.mf310p_v2_trial_ib_probe(handle, 1000, raw_stream) == 0
    y = torch.empty_like(ref)

    def run_b():
        p = F.linear(x, weight)
        # 生产者到暂存 Kernel 之间必须同步 Stream（参见 bench_ar_only.py）。
        stream.synchronize()
        rc = lib.mf310p_v2_trial_run_ar_only(handle, p.data_ptr(), y.data_ptr(), a.m, a.add, raw_stream)
        assert rc == 0
        stream.synchronize()
        return p

    run_b()  # 预热并执行首写保护
    run_b()
    status = ctypes.c_uint64(0)
    assert lib.mf310p_v2_trial_check_status(handle, ctypes.byref(status)) == 0
    assert status.value == 0
    assert torch.equal(y, ref), "profiling warmup not bit-exact"
    dist.barrier(group=cpu_group)

    if rank == 0:
        print(f"=== profiling B M={a.m} add={a.add} chunk={a.chunk_rows} ===", flush=True)
    with profile(
        activities=[ProfilerActivity.CPU, ProfilerActivity.NPU], with_stack=False, record_shapes=False
    ) as prof:
        for _ in range(a.iters):
            run_b()
    pb = f"{a.out}_b_rank{rank}.json"
    prof.export_chrome_trace(pb)
    dist.barrier(group=cpu_group)
    print(f"[rank{rank}] B summary: {json.dumps(summarize_trace(pb, rank))}", flush=True)

    stream.synchronize()
    lib.mf310p_v2_trial_destroy(handle)
    dist.barrier(group=cpu_group)
    dist.destroy_process_group()


if __name__ == "__main__":
    main()
