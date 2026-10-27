#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""D4-3：Tile2048 vs Tile1024 正式核 TP2 受控性能 A/B（external 无 Profiler）。

计时口径与 D1-A gate_d1a_perturbation.py 完全一致：
  wall_us  = host perf_counter 包住 (launch + stream sync)；
  event_us = 双 NPU Event 夹 Kernel 的设备流区间（主验收口径）。
唯一差异（D4 环境偏差，已记录）：进程组与参考 all-reduce 由 HCCL 改为
gloo(CPU)——TP2 两操作数 FP32 加法与顺序无关，参考位精确等价；
计时窗口内不含任何集合通信操作，口径不受影响。

纪律：一会话一条件；每样本逐次 torch.equal 位精确 + status=0，失败即停；
MF310P_V2_NOWARMUP 必须 unset；KVER11 显式绝对路径；禁止长压测。
"""

from __future__ import annotations

import argparse
import ctypes
import json
import os
import time
from pathlib import Path

import torch
import torch.distributed as dist
import torch.nn.functional as F
import torch_npu  # noqa: F401
import vllm_ascend.vllm_ascend_C as extension

from vllm_ascend.utils import maybe_trans_nz

N = K = 2048
M = 4096
CHUNK = 1024
STALL_LOG_US = 5000.0


def parse_args():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--label", required=True, help="baseline|candidate + rep, e.g. cand_r1")
    p.add_argument("--out", required=True, type=Path)
    p.add_argument("--warmup", type=int, default=3)
    p.add_argument("--iters", type=int, default=30)
    p.add_argument(
        "--store-url", default=os.environ.get("VLLM_ASCEND_310P_MEMFABRIC_STORE_URL", "tcp://127.0.0.1:8581")
    )
    a = p.parse_args()
    if a.warmup < 2 or a.iters < 1:
        p.error("--warmup >= 2 and --iters >= 1 required")
    if int(os.getenv("WORLD_SIZE", "0")) != 2:
        p.error("TP WORLD_SIZE=2 is required")
    if "MF310P_V2_NOWARMUP" in os.environ:
        p.error("MF310P_V2_NOWARMUP must be unset: never bypass first-write protection")
    orch = os.environ.get("MF_SDMA_ORCH_JSON", "")
    if not orch.endswith("/libmf_sdma_orch_v11.json"):
        p.error("MF_SDMA_ORCH_JSON must explicitly select an absolute KVER11 JSON path")
    if not Path(orch).is_file():
        p.error(f"missing MemFabric KVER11 JSON: {orch}")
    return a


def bind():
    lib = ctypes.CDLL(extension.__file__)
    P, U32, U64 = ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint64
    sigs = {
        "mf310p_v2_trial_create": ([ctypes.c_int, ctypes.c_char_p, U32, ctypes.POINTER(P)], ctypes.c_int),
        "mf310p_v2_trial_ib_probe": ([P, U32, P], ctypes.c_int),
        "mf310p_v2_trial_run": ([P, U64, U64, U64, U32, U32, P], ctypes.c_int),
        "mf310p_v2_trial_check_status": ([P, ctypes.POINTER(U64)], ctypes.c_int),
        "mf310p_v2_trial_destroy": ([P], ctypes.c_int),
    }
    for name, (args, ret) in sigs.items():
        fn = getattr(lib, name)
        fn.argtypes, fn.restype = args, ret
    return lib


def must(rc, name):
    if rc:
        raise RuntimeError(f"{name}: rc={rc}")


def build_inputs(rank, device, stream):
    # 与 D1-A 相同种子；参考经 CPU gloo FP32 reduce（TP2 加法与顺序无关）。
    torch.manual_seed(86420 + rank)
    weight = maybe_trans_nz(torch.randn(N, K, dtype=torch.float16, device=device))
    torch.manual_seed(74320 + 101 * rank)
    x = torch.randn(M, K, dtype=torch.float16, device=device)
    ref = F.linear(x, weight).float().cpu()
    dist.all_reduce(ref)
    ref = ref.to(device).half()
    stream.synchronize()
    return x, weight, ref


def check_one(lib, handle, y, ref, rank, label):
    status = ctypes.c_uint64()
    must(lib.mf310p_v2_trial_check_status(handle, ctypes.byref(status)), "status D2H")
    if status.value:
        raise RuntimeError(f"{label} rank{rank}: protocol status={status.value}")
    if not torch.equal(y, ref):
        bad = int((y != ref).sum().item())
        raise AssertionError(f"{label} rank{rank}: bit-exact mismatch {bad} elements")


def timed_sample(lib, handle, x, w, y, raw_stream, stream):
    ev0 = torch.npu.Event(enable_timing=True)
    ev1 = torch.npu.Event(enable_timing=True)
    t0 = time.perf_counter_ns()
    ev0.record(stream)
    must(lib.mf310p_v2_trial_run(handle, x.data_ptr(), w.data_ptr(), y.data_ptr(), M, 1, raw_stream), "run")
    ev1.record(stream)
    stream.synchronize()
    t1 = time.perf_counter_ns()
    wall_us = (t1 - t0) / 1000.0
    event_us = ev0.elapsed_time(ev1) * 1000.0
    return wall_us, event_us


def main():
    a = parse_args()
    rank = int(os.environ["RANK"])
    local = int(os.environ["LOCAL_RANK"])
    torch.npu.set_device(local)
    device = torch.device(f"npu:{local}")
    dist.init_process_group(backend="gloo")
    stream = torch.npu.current_stream()
    raw_stream = ctypes.c_void_p(stream.npu_stream)
    handle = ctypes.c_void_p()

    try:
        x, w, ref = build_inputs(rank, device, stream)
        y = torch.empty((M, N), device=device, dtype=torch.float16)
        y.zero_()
        stream.synchronize()
        lib = bind()

        dist.barrier()
        must(lib.mf310p_v2_trial_create(rank, a.store_url.encode(), CHUNK, ctypes.byref(handle)), "create")
        if not handle.value:
            raise RuntimeError("MemFabric handle unexpectedly NULL")
        must(lib.mf310p_v2_trial_ib_probe(handle, 64, raw_stream), "ib_probe")
        dist.barrier()

        for i in range(a.warmup):
            dist.barrier()
            wall_us, event_us = timed_sample(lib, handle, x, w, y, raw_stream, stream)
            check_one(lib, handle, y, ref, rank, a.label)
            print(
                f"[rank{rank}] {a.label} warmup={i} bit-exact wall={wall_us:.0f}us event={event_us:.0f}us", flush=True
            )
        dist.barrier()

        a.out.mkdir(parents=True, exist_ok=True)
        result_path = a.out / f"{a.label}_rank{rank}_d4.json"
        if result_path.exists():
            raise FileExistsError(f"refusing to overwrite prior evidence {result_path}")

        samples = []
        for _ in range(a.iters):
            dist.barrier()
            wall_us, event_us = timed_sample(lib, handle, x, w, y, raw_stream, stream)
            check_one(lib, handle, y, ref, rank, a.label)
            samples.append({"i": len(samples), "wall_us": round(wall_us, 1), "event_us": round(event_us, 1)})
            if wall_us > STALL_LOG_US or event_us > STALL_LOG_US:
                print(
                    f"[rank{rank}] {a.label} sample={len(samples) - 1} SLOW "
                    f"wall={wall_us:.0f}us event={event_us:.0f}us",
                    flush=True,
                )

        data = {
            "gate": "D4-3 ABBA perf (external, no profiler)",
            "label": a.label,
            "rank": rank,
            "M": M,
            "chunk_rows": CHUNK,
            "chunks": 4,
            "overlap": 1,
            "warmup": a.warmup,
            "iters": a.iters,
            "status": 0,
            "bit_exact": True,
            "profile": False,
            "samples": samples,
            "device_so": os.environ.get("D4_DEVICE_SO_SHA", "unset"),
            "timing_semantics": {
                "wall_us": "host perf_counter around (launch + stream.synchronize)",
                "event_us": "device stream interval between two NPU events bracketing the kernel; primary metric",
            },
            "note": (
                "D4 deviation: gloo process group/reference "
                "(HCCL port leaked by killed session); timing window contains no collectives"
            ),
        }
        result_path.write_text(json.dumps(data, indent=2) + "\n")
        print(json.dumps({k: data[k] for k in ("label", "rank", "iters", "status", "bit_exact")}), flush=True)
        dist.barrier()
    finally:
        if handle.value:
            stream.synchronize()
            try:
                must(bind().mf310p_v2_trial_destroy(handle), "destroy")
            except Exception as exc:
                print(f"[rank{rank}] destroy error: {exc}", flush=True)
        if dist.is_initialized():
            dist.destroy_process_group()


if __name__ == "__main__":
    main()
