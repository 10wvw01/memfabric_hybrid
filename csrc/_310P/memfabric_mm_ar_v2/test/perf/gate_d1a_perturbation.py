#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""D1-A：V2 Profiler 扰动排除与稳态基线（TP2，M=4096/chunk=1024/KVER11）。

对 formal（overlap=1）与 loop4（串行）各以三种模式独立 torchrun 会话测量：
  --mode external  完全无 Profiler；每样本用双口径外部计时：
                   wall_us  = host perf_counter 包住 (launch + stream sync)，
                              含 host 发射、设备执行、同步返回全部成本；
                   event_us = 双 NPU Event 夹住 Kernel 的设备流区间，
                              不含 host 发射延迟，含 event record 本身开销。
                   两口径都不是 msprof Kernel Duration，禁止混用。
  --mode profile   torch_npu CPU/NPU profiler（无 AIC 指标），另存 chrome trace；
                   同一窗口内仍逐样本记录 wall/event 双口径。
  --mode aic       profile + PipeUtilization/Level1 硬件 PMU（同 Gate C）。

纪律（继承 Gate C，不放松）：
  - 一个会话只跑一个 case + mode；每次采样逐次位精确 + status=0，失败即停；
  - MF_SDMA_ORCH_JSON 必须显式指向绝对 KVER11 JSON；MF310P_V2_NOWARMUP 必须 unset；
  - 不修改正式 Kernel / MemFabric 协议 / KVER11 部署；禁止长压测。

示例：
  ASCEND_RT_VISIBLE_DEVICES=0,1 torchrun --standalone --nproc-per-node=2 \
    csrc/_310P/memfabric_mm_ar_v2/test/perf/gate_d1a_perturbation.py \
    --case formal --mode external --out /tmp/opencode/gate_d1a/sessions/formal_ext_r1
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

CASES = ("formal", "loop4")
MODES = ("external", "profile", "aic")
N = K = 2048
M = 4096
CHUNK = 1024
STALL_LOG_US = 5000.0  # 只做现场告警，不改变判定；统计离线做


def parse_args():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--case", required=True, choices=CASES)
    p.add_argument("--mode", required=True, choices=MODES)
    p.add_argument("--out", required=True, type=Path)
    p.add_argument("--warmup", type=int, default=3)
    p.add_argument("--iters", type=int, default=16)
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
        "mf310p_v2_trial_diag_loop4": ([P, U64, U64, U64, P], ctypes.c_int),
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
    # 两个 Rank 各自固定输入/权重；先完成 Stock 参考值，再建立 MemFabric 池。
    torch.manual_seed(86420 + rank)
    weight = maybe_trans_nz(torch.randn(N, K, dtype=torch.float16, device=device))
    torch.manual_seed(74320 + 101 * rank)
    x = torch.randn(M, K, dtype=torch.float16, device=device)
    local = F.linear(x, weight)
    ref = local.float()
    dist.all_reduce(ref)
    ref = ref.half()
    stream.synchronize()
    return x, weight, ref


def run_one(lib, handle, case, x, w, y, raw_stream):
    if case == "formal":
        must(lib.mf310p_v2_trial_run(handle, x.data_ptr(), w.data_ptr(), y.data_ptr(), M, 1, raw_stream), case)
    else:
        must(lib.mf310p_v2_trial_diag_loop4(handle, x.data_ptr(), w.data_ptr(), y.data_ptr(), raw_stream), case)


def check_one(lib, handle, y, ref, case, rank):
    status = ctypes.c_uint64()
    must(lib.mf310p_v2_trial_check_status(handle, ctypes.byref(status)), "status D2H")
    if status.value:
        raise RuntimeError(f"{case} rank{rank}: protocol status={status.value}")
    if not torch.equal(y, ref):
        mask = (y != ref).reshape(-1)
        bad = int(mask.sum().item())
        raise AssertionError(f"{case} rank{rank}: bit-exact mismatch {bad} elements")


def timed_sample(lib, handle, case, x, w, y, raw_stream, stream):
    """barrier 对齐后单样本：双口径计时窗口只包 launch+sync，校验在窗口外。"""
    ev0 = torch.npu.Event(enable_timing=True)
    ev1 = torch.npu.Event(enable_timing=True)
    t0 = time.perf_counter_ns()
    ev0.record(stream)
    run_one(lib, handle, case, x, w, y, raw_stream)
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
    dist.init_process_group(backend="hccl")
    group = dist.new_group(backend="gloo")
    stream = torch.npu.current_stream()
    raw_stream = ctypes.c_void_p(stream.npu_stream)
    handle = ctypes.c_void_p()

    try:
        x, w, ref = build_inputs(rank, device, stream)
        y = torch.empty((M, N), device=device, dtype=torch.float16)
        # 提前初始化设备输出地址；整个 session 持续存活，禁止释放/重分配。
        y.zero_()
        stream.synchronize()
        lib = bind()

        dist.barrier(group=group)
        must(lib.mf310p_v2_trial_create(rank, a.store_url.encode(), CHUNK, ctypes.byref(handle)), "create")
        if not handle.value:
            raise RuntimeError("MemFabric handle unexpectedly NULL")
        must(lib.mf310p_v2_trial_ib_probe(handle, 64, raw_stream), "ib_probe")
        dist.barrier(group=group)

        # 预热含首次符号/首写保护，禁止计入稳态。
        for i in range(a.warmup):
            dist.barrier(group=group)
            wall_us, event_us = timed_sample(lib, handle, a.case, x, w, y, raw_stream, stream)
            check_one(lib, handle, y, ref, a.case, rank)
            print(
                f"[rank{rank}] {a.case}/{a.mode} warmup={i} bit-exact wall={wall_us:.0f}us event={event_us:.0f}us",
                flush=True,
            )
        dist.barrier(group=group)

        a.out.mkdir(parents=True, exist_ok=True)
        result_path = a.out / f"{a.case}_{a.mode}_rank{rank}_d1a.json"
        if result_path.exists():
            raise FileExistsError(f"refusing to overwrite prior evidence {result_path}")
        trace_path = a.out / f"{a.case}_{a.mode}_rank{rank}_trace.json"
        if a.mode != "external" and trace_path.exists():
            raise FileExistsError(f"refusing to overwrite prior trace {trace_path}")

        samples = []

        def one_sample():
            dist.barrier(group=group)
            wall_us, event_us = timed_sample(lib, handle, a.case, x, w, y, raw_stream, stream)
            # 校验在计时窗口外；每次采样逐次位精确，失败立即抛出终止会话。
            check_one(lib, handle, y, ref, a.case, rank)
            samples.append({"i": len(samples), "wall_us": round(wall_us, 1), "event_us": round(event_us, 1)})
            if wall_us > STALL_LOG_US or event_us > STALL_LOG_US:
                print(
                    f"[rank{rank}] {a.case}/{a.mode} sample={len(samples) - 1} "
                    f"SLOW wall={wall_us:.0f}us event={event_us:.0f}us",
                    flush=True,
                )

        if a.mode == "external":
            for _ in range(a.iters):
                one_sample()
        else:
            from torch_npu.profiler import ProfilerActivity, profile

            exp_cfg = None
            prof_dir = None
            if a.mode == "aic":
                # 硬件 PMU 管道计数器，非 Kernel 内埋点；与 Gate C 完全同参。
                from torch_npu.profiler import AiCMetrics, ProfilerLevel
                from torch_npu.profiler.experimental_config import _ExperimentalConfig

                prof_dir = a.out / f"prof_rank{rank}"
                os.environ["ASCEND_WORK_PATH"] = str(prof_dir)
                exp_cfg = _ExperimentalConfig(
                    profiler_level=ProfilerLevel.Level1,
                    aic_metrics=AiCMetrics.PipeUtilization,
                )
            with profile(
                activities=[ProfilerActivity.CPU, ProfilerActivity.NPU],
                experimental_config=exp_cfg,
                record_shapes=False,
                with_stack=False,
            ) as prof:
                for _ in range(a.iters):
                    one_sample()
            prof.export_chrome_trace(str(trace_path))

        data = {
            "case": a.case,
            "mode": a.mode,
            "rank": rank,
            "M": M,
            "chunk_rows": CHUNK,
            "chunks": 4,
            "overlap": 1 if a.case == "formal" else 0,
            "warmup": a.warmup,
            "iters": a.iters,
            "status": 0,
            "bit_exact": True,
            "profile": a.mode != "external",
            "aic_metrics": a.mode == "aic",
            "trace": str(trace_path) if a.mode != "external" else None,
            "prof_dir": str(prof_dir) if a.mode == "aic" else None,
            "samples": samples,
            "timing_semantics": {
                "wall_us": (
                    "host perf_counter around (kernel launch + stream.synchronize); "
                    "includes host launch overhead, full device execution and sync completion cost"
                ),
                "event_us": (
                    "device stream interval between two NPU event records bracketing the kernel; "
                    "excludes host launch latency, includes event record overhead; "
                    "NOT identical to msprof Task Duration"
                ),
            },
            "note": (
                "D1-A profiler perturbation check; per-sample correctness verified "
                "outside timing window; loop4 is serial and not equivalent to formal overlap=1."
            ),
        }
        result_path.write_text(json.dumps(data, indent=2) + "\n")
        print(json.dumps({k: data[k] for k in ("case", "mode", "rank", "iters", "status", "bit_exact")}), flush=True)
        dist.barrier(group=group)
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
