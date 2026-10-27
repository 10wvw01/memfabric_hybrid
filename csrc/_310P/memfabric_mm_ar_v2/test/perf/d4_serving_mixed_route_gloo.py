#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""D4-2 serving 路径正确性（gloo 变体，候选 Tile2048）。

原版 test/e2e/tp2_v2_mixed_route_correctness.py 的 D4 环境适配：
dev2/3 wedge 期间设备侧 HCCL 端口 16666 被泄露，本变体以 gloo(CPU) 生成参考
与 stock 路径（TP2 两操作数 FP32 加法与顺序无关，位精确等价）。
v2 serving op（memfabric_mm_ar_v2_allreduce）与协议状态检查完全不变。
覆盖：非整 Chunk tail 2049/3073/4095、阈值边界 2048/4096、
stock 回退 2047/1/10/4097/5000、输出槽复用（逐次 del+gc）、
建池后 stock 与 v2 交错、逐次 status=0。
"""

from __future__ import annotations

import argparse
import gc
import os

import torch
import torch.distributed as dist
import torch.nn.functional as F
import torch_npu  # noqa: F401
import vllm_ascend.vllm_ascend_C  # noqa: F401

from vllm_ascend._310p.ops.memfabric_mm_ar_v2_serve import v2_threshold_route
from vllm_ascend.utils import maybe_trans_nz


def cpu_gloo_mm_ar(x, w):
    """Stock 等价：本地线性 + CPU gloo FP32 all-reduce（与 NPU HCCL 位精确等价）。"""
    out = F.linear(x, w).float().cpu()
    dist.all_reduce(out)
    return out.to(x.device).half()


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--rows", type=int, nargs="+", default=[2047, 2048, 2049, 3000, 3073, 4095, 4096, 4097, 5000])
    p.add_argument("--chunk-rows", type=int, choices=(256, 512, 1024), default=1024)
    p.add_argument("--reps", type=int, default=3)
    p.add_argument(
        "--store-url",
        default=os.environ.get("VLLM_ASCEND_310P_MEMFABRIC_STORE_URL", "tcp://127.0.0.1:8581"),
        help="runner 统一传参；serving 池实际读取 VLLM_ASCEND_310P_MEMFABRIC_STORE_URL",
    )
    p.add_argument("--out", default=".", help="证据输出目录（runner 统一传参）")
    args = p.parse_args()
    if any(m < 1 for m in args.rows) or args.reps < 1:
        p.error("M must be positive and reps >=1")
    if int(os.environ.get("WORLD_SIZE", "0")) != 2:
        p.error("torchrun --nproc-per-node=2 is required")
    if "MF310P_V2_NOWARMUP" in os.environ:
        p.error("MF310P_V2_NOWARMUP must be unset")
    rank, local = int(os.environ["RANK"]), int(os.environ["LOCAL_RANK"])
    torch.npu.set_device(local)
    dev = torch.device(f"npu:{local}")
    dist.init_process_group("gloo")

    torch.manual_seed(812 + rank)
    w = maybe_trans_nz(torch.randn(2048, 2048, dtype=torch.float16, device=dev))
    all_m = sorted(set(args.rows + [1, 10, 2047, 2048, 2049, 3000, 3073, 4095, 4096, 4097, 5000]))
    xs, fp32_refs, stock_refs = {}, {}, {}
    # 全部参考在建池前生成（原版纪律）。
    for m in all_m:
        torch.manual_seed(6300 + rank * 67 + m)
        x = torch.randn(m, 2048, dtype=torch.float16, device=dev)
        xs[m] = x
        fp32_refs[m] = cpu_gloo_mm_ar(x, w)
        stock_refs[m] = cpu_gloo_mm_ar(x, w)
    torch.npu.current_stream().synchronize()
    dist.barrier()

    plan = [2048, 1, 2049, 10, 2047, 3000, 3073, 4095, 4096, 4097, 5000]
    plan.extend(args.rows)
    plan *= args.reps
    ok = 0
    v2_calls = 0
    try:
        for iteration, m in enumerate(plan):
            x = xs[m]
            if v2_threshold_route(m) == "stock":
                out = cpu_gloo_mm_ar(x, w)
                expected = stock_refs[m]
                path = "stock_gloo_after_pool"
            else:
                out = torch.ops._C_ascend.memfabric_mm_ar_v2_allreduce(x, w, rank, args.chunk_rows)
                expected = fp32_refs[m]
                path = "v2_fused"
                v2_calls += 1
            torch.npu.current_stream().synchronize()
            status = int(torch.ops._C_ascend.memfabric_mm_ar_v2_debug_status())
            assert status == 0, f"protocol status={status} rank={rank} m={m} i={iteration}"
            if not torch.equal(out, expected):
                delta = (out.float() - expected.float()).abs()
                raise AssertionError(
                    f"rank={rank} i={iteration} M={m} path={path}: "
                    f"bad={(out != expected).sum().item()} "
                    f"max_abs={delta.max().item()}"
                )
            ok += 1
            if rank == 0:
                print(f"PASS i={iteration} M={m} route={path}", flush=True)
            del out
            # 确保之前返回的池内输出视图已释放，槽位才可复用。
            gc.collect()
        dist.barrier()
        if rank == 0:
            print(f"D4-2 SERVING MIXED-ROUTE PASS: {ok} calls, v2_calls={v2_calls}", flush=True)
        from pathlib import Path

        out_dir = Path(args.out)
        out_dir.mkdir(parents=True, exist_ok=True)
        result_path = out_dir / f"d4_serving_rank{rank}.json"
        if not result_path.exists():
            import json

            result_path.write_text(
                json.dumps(
                    {
                        "gate": "D4-2 serving mixed-route (gloo variant)",
                        "rank": rank,
                        "ok_calls": ok,
                        "v2_calls": v2_calls,
                        "chunk_rows": args.chunk_rows,
                        "reps": args.reps,
                        "status": 0,
                        "bit_exact": True,
                        "device_so": os.environ.get("D4_DEVICE_SO_SHA", "unset"),
                    },
                    indent=2,
                )
                + "\n"
            )
    finally:
        torch.npu.current_stream().synchronize()
        torch.ops._C_ascend.memfabric_mm_ar_v2_shutdown()
        dist.destroy_process_group()


if __name__ == "__main__":
    main()
