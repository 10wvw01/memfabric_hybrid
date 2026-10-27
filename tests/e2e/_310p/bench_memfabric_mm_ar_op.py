# SPDX-License-Identifier-Identifier: Apache-2.0
# Copyright (c) 2026 Huawei Technologies Co., Ltd. All Rights Reserved.
"""310P3 MemFabric mm_ar operator-level A/B benchmark (TP=2, real hardware).

Head-to-head, same-process alternating timing:
    stock: NZ matmul (torch F.linear on the NZ-cast weight) + HCCL all-reduce
    fused: ABI v7 memfabric_mm_ar_allreduce (8-core cooperative MM + SDMA
           exchange + FP16 add), every M fused (no threshold)

The fused o_proj and GDN out_proj layers share one kernel and one stock
shape contract (K_local=2048, N=2048), so a single synthetic layer covers
both layer families.

Run (q defaults to the env var / 2; sweep q by re-running with --batch-basem-count):

    ASCEND_RT_VISIBLE_DEVICES=0,1 VLLM_ASCEND_310P_ENABLE_MEMFABRIC_MM_AR=1 \
    torchrun --standalone --nproc-per-node=2 \
      tests/e2e/_310p/bench_memfabric_mm_ar_op.py \
      --rows 1 16 32 64 256 512 1024 2048 4096 --iters 60 --csv out.csv
"""

from __future__ import annotations

import argparse
import csv
import os
import statistics
import time
import types

import torch
import torch.distributed as dist
import torch.nn.functional as F
import torch_npu  # noqa: F401
import vllm_ascend.vllm_ascend_C  # noqa: F401

from vllm_ascend._310p.ops.memfabric_mm_ar import memfabric_mm_ar_allreduce
from vllm_ascend.utils import maybe_trans_nz

K_LOCAL = 2048
N_OUT = 2048
DEFAULT_ROWS = [1, 16, 32, 64, 256, 512, 1024, 2048, 4096]


def timeit(fn, iters: int) -> float:
    ts = []
    for _ in range(iters):
        t0 = time.perf_counter()
        fn()
        torch.npu.current_stream().synchronize()
        ts.append((time.perf_counter() - t0) * 1e3)
    ts.sort()
    return ts[len(ts) // 2]


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--rows", type=int, nargs="+", default=DEFAULT_ROWS)
    parser.add_argument(
        "--batch-basem-count",
        type=int,
        choices=(1, 2, 4),
        default=int(os.getenv("VLLM_ASCEND_310P_MEMFABRIC_MM_AR_BATCH_BASEM_COUNT", "1")),
    )
    parser.add_argument("--iters", type=int, default=60)
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--csv", type=str, default="")
    args = parser.parse_args()

    rank = int(os.environ["RANK"])
    local_rank = int(os.environ["LOCAL_RANK"])
    if int(os.environ["WORLD_SIZE"]) != 2:
        raise RuntimeError("This benchmark requires exactly TP=2")

    # The CLI flag must reach every rank (batch_m/mail expectations are
    # per-process); a rank-0-only override desynchronizes the two sides.
    os.environ["VLLM_ASCEND_310P_MEMFABRIC_MM_AR_BATCH_BASEM_COUNT"] = str(args.batch_basem_count)

    torch.npu.set_device(local_rank)
    device = torch.device(f"npu:{local_rank}")
    dist.init_process_group(backend="hccl")
    cpu_group = dist.new_group(backend="gloo")

    layer = types.SimpleNamespace()
    torch.manual_seed(7)
    w = torch.randn(N_OUT, K_LOCAL, dtype=torch.float16, device=device)
    layer.weight = types.SimpleNamespace(data=maybe_trans_nz(w))
    layer.params_dtype = torch.float16

    xs = {}
    for m in args.rows:
        torch.manual_seed(42 + m)
        xs[m] = torch.randn(m, K_LOCAL, dtype=torch.float16, device=device)
    dist.barrier(group=cpu_group)

    rows = []
    if rank == 0:
        print(
            f"q={args.batch_basem_count} batch_m={256 * args.batch_basem_count} iters={args.iters} rounds={args.rounds}"
        )
        print("rows\tstock_ms\tfused_ms\tdelta_ms\ttime_delta\tthroughput_gain")
    for m in args.rows:
        x = xs[m]

        def stock(x=x) -> None:
            out = F.linear(x, layer.weight.data)
            dist.all_reduce(out)

        # First fused call at the biggest shape creates the pool; warm each
        # shape once before timing.
        _ = memfabric_mm_ar_allreduce(layer=layer, x=x, tp_rank=rank)
        torch.npu.current_stream().synchronize()
        dist.barrier(group=cpu_group)

        stock_meds, fused_meds = [], []
        for _ in range(args.rounds):
            stock_meds.append(timeit(stock, max(1, args.iters // args.rounds)))
            fused_meds.append(
                timeit(
                    lambda x=x: memfabric_mm_ar_allreduce(layer=layer, x=x, tp_rank=rank),
                    max(1, args.iters // args.rounds),
                )
            )
        s, f = statistics.median(stock_meds), statistics.median(fused_meds)
        gain = (s - f) / f * 100 if f > 0 else 0.0
        rows.append(
            {
                "q": args.batch_basem_count,
                "batch_m": 256 * args.batch_basem_count,
                "rows": m,
                "stock_ms": f"{s:.4f}",
                "fused_ms": f"{f:.4f}",
                "delta_ms": f"{s - f:.4f}",
                "time_delta_pct": f"{(s - f) / s * 100:+.1f}",
                "throughput_gain_pct": f"{gain:+.1f}",
            }
        )
        if rank == 0:
            print(f"{m}\t{s:.3f}\t{f:.3f}\t{s - f:.3f}\t{(s - f) / s * 100:+.1f}%\t{gain:+.1f}%")

    dist.barrier(group=cpu_group)
    torch.ops._C_ascend.memfabric_mm_ar_shutdown()
    dist.barrier(group=cpu_group)

    if rank == 0 and args.csv:
        with open(args.csv, "w", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
            writer.writeheader()
            writer.writerows(rows)
        print(f"csv written to {args.csv}")


if __name__ == "__main__":
    main()
