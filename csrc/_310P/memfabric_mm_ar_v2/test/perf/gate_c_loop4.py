#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Gate C：TP2 独立单变体正确性门禁和短窗口 profiler。

示例（仅在确认设备独占、KVER11 及诊断 SO 后执行）：
  ASCEND_RT_VISIBLE_DEVICES=0,1 torchrun --standalone --nproc-per-node=2 \
    csrc/_310P/memfabric_mm_ar_v2/test/perf/gate_c_loop4.py \
    --case formal --out /tmp/opencode/gate_c/formal --profile
  ... --case loop4 --out /tmp/opencode/gate_c/loop4 --profile

同一个进程只能运行一种变体。单 Chunk 诊断没有 4 Chunk 等价性；
仅使用 kernel_details/chrome trace 的设备 Kernel 耗时比较，禁止将本脚本
含同步的 Host 测量解释为真实 Qwen Forward 延迟。
"""

from __future__ import annotations

import argparse
import ctypes
import json
import os
from pathlib import Path

import torch
import torch.distributed as dist
import torch.nn.functional as F
import torch_npu  # noqa: F401
import vllm_ascend.vllm_ascend_C as extension

from vllm_ascend.utils import maybe_trans_nz

CASES = ("formal", "loop4", "diag0", "diag1", "diag3", "diag4", "diag5")
N = K = 2048
M = 4096
CHUNK = 1024


def parse_args():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--case", required=True, choices=CASES)
    p.add_argument("--out", required=True, type=Path)
    p.add_argument("--warmup", type=int, default=3)
    p.add_argument("--iters", type=int, default=6)
    p.add_argument("--profile", action="store_true")
    p.add_argument(
        "--aic-metrics",
        action="store_true",
        help="with --profile: additionally enable torch_npu PipeUtilization "
        "AIC metrics; raw msprof op_summary CSV lands under "
        "<out>/prof_rank<N>/profiling_data/. No in-kernel timestamps.",
    )
    p.add_argument(
        "--store-url", default=os.environ.get("VLLM_ASCEND_310P_MEMFABRIC_STORE_URL", "tcp://127.0.0.1:8581")
    )
    a = p.parse_args()
    if a.warmup < 2 or a.iters < 1:
        p.error("--warmup >= 2 and --iters >= 1 required")
    if int(os.getenv("WORLD_SIZE", "0")) != 2:
        p.error("TP WORLD_SIZE=2 is required")
    if a.aic_metrics and not a.profile:
        p.error("--aic-metrics requires --profile")
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
        "mf310p_v2_trial_diag": ([P, U64, U64, U64, U32, P], ctypes.c_int),
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
    local_one = F.linear(x.narrow(0, 0, CHUNK), weight)
    ref_one = local_one.float()
    dist.all_reduce(ref_one)
    ref_one = ref_one.half()
    stream.synchronize()
    return x, weight, local_one, ref, ref_one


def run_one(lib, handle, case, x, w, y, raw_stream):
    if case == "formal":
        must(lib.mf310p_v2_trial_run(handle, x.data_ptr(), w.data_ptr(), y.data_ptr(), M, 1, raw_stream), case)
    elif case == "loop4":
        must(lib.mf310p_v2_trial_diag_loop4(handle, x.data_ptr(), w.data_ptr(), y.data_ptr(), raw_stream), case)
    else:
        n = int(case[4:])
        must(lib.mf310p_v2_trial_diag(handle, x.data_ptr(), w.data_ptr(), y.data_ptr(), n, raw_stream), case)


def check_one(lib, handle, y, ref, case, rank):
    status = ctypes.c_uint64()
    must(lib.mf310p_v2_trial_check_status(handle, ctypes.byref(status)), "status D2H")
    if status.value:
        raise RuntimeError(f"{case} rank{rank}: protocol status={status.value}")
    if not torch.equal(y, ref):
        mask = (y != ref).reshape(-1)
        bad = int(mask.sum().item())
        raise AssertionError(f"{case} rank{rank}: bit-exact mismatch {bad} elements")


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
        x, w, local_one, ref, ref_one = build_inputs(rank, device, stream)
        is_single = a.case.startswith("diag")
        y = torch.empty((CHUNK if is_single else M, N), device=device, dtype=torch.float16)
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

        xx = x.narrow(0, 0, CHUNK) if is_single else x
        rr = local_one if a.case == "diag0" else ref_one if is_single else ref

        def run_and_check():
            run_one(lib, handle, a.case, xx, w, y, raw_stream)
            stream.synchronize()  # Only after the whole Kernel; never inside its phases.
            check_one(lib, handle, y, rr, a.case, rank)

        # 第一轮已包含符号首次发射、首次写保护，禁止计入稳态。
        for i in range(a.warmup):
            dist.barrier(group=group)
            run_and_check()
            print(f"[rank{rank}] {a.case} warmup={i} bit-exact / status=0", flush=True)
        dist.barrier(group=group)

        a.out.mkdir(parents=True, exist_ok=True)
        result_path = a.out / f"{a.case}_rank{rank}_result.json"
        if result_path.exists():
            raise FileExistsError(f"refusing to overwrite prior evidence {result_path}")
        trace_path = a.out / f"{a.case}_rank{rank}_trace.json"
        if a.profile and trace_path.exists():
            raise FileExistsError(f"refusing to overwrite prior trace {trace_path}")

        # Profiler 捕获的是独立算子设备时长；同步/校验放在窗口外，
        # 每个采样用双 Rank 对齐、在内核完成后确认位精确。
        if a.profile:
            from torch_npu.profiler import ProfilerActivity, profile

            exp_cfg = None
            prof_dir = None
            if a.aic_metrics:
                # 硬件 PMU 管道计数器（mte2/scalar/mac/vec），不是 Kernel 内埋点；
                # ASCEND_WORK_PATH 使原始 msprof 数据落入本 case 目录且不被清理。
                from torch_npu.profiler import AiCMetrics, ProfilerLevel
                from torch_npu.profiler.experimental_config import (
                    _ExperimentalConfig,
                )

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
                    dist.barrier(group=group)
                    run_one(lib, handle, a.case, xx, w, y, raw_stream)
                    stream.synchronize()
                    # 每一次采样都验证完整结果；宁可增加诊断 CPU 开销，
                    # 也不能只用末次校验掩盖偶发错误。设备 Kernel 时长单独提取。
                    check_one(lib, handle, y, rr, a.case, rank)
            prof.export_chrome_trace(str(trace_path))
        else:
            for _ in range(a.iters):
                dist.barrier(group=group)
                run_and_check()

        data = {
            "case": a.case,
            "rank": rank,
            "M": CHUNK if is_single else M,
            "chunk_rows": CHUNK,
            "chunks": 1 if is_single else 4,
            "overlap": 1 if a.case == "formal" else 0,
            "warmup": a.warmup,
            "iters": a.iters,
            "status": 0,
            "bit_exact": True,
            "profile": a.profile,
            "aic_metrics": a.aic_metrics,
            "trace": str(trace_path) if a.profile else None,
            "prof_dir": str(prof_dir) if a.aic_metrics else None,
            "note": (
                "diag0/1/3/4/5 are single-chunk probes; cannot be added to "
                "reconstruct the exact 4-chunk model path. loop4 is serial and "
                "not equivalent to formal overlap=1."
            ),
        }
        result_path.write_text(json.dumps(data, indent=2) + "\n")
        print(json.dumps(data), flush=True)
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
