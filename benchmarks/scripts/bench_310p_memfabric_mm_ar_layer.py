# SPDX-License-Identifier: Apache-2.0
"""310P3 TP=2 FP16 mm_ar single-layer correctness benchmark.

The target checkpoint keeps the fused-eligible o_proj/out_proj as unquantized FLOAT
layer; this 310P3 path runs FP16.

Reference:
    local F.linear -> FP32 TP sum -> FP16

Fused ABI v7:
    8-core cooperative MM; core0 is the sole signal owner
    -> per-batch MemFabric public wait
    -> per-batch local FP16 add
    -> one public quiet + wave credit at drain

This benchmark intentionally computes the HCCL reference before the first fused
call so correctness measurement does not depend on HCCL/MemFabric coexistence.
Coexistence is a separate hardware-validation gate.

Run:

  ASCEND_RT_VISIBLE_DEVICES=0,1 \
  VLLM_ASCEND_310P_ENABLE_MEMFABRIC_MM_AR=1 \
  torchrun --standalone --nproc-per-node=2 \
    benchmarks/scripts/bench_310p_memfabric_mm_ar_layer.py \
    --rows 255 256 257 511 512 513 1024 2048 4096 6144 8192 --repeat 20
"""

from __future__ import annotations

import argparse
import os
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


def _make_layer(rank: int, device: torch.device) -> types.SimpleNamespace:
    layer = types.SimpleNamespace()
    weight = torch.randn(N_OUT, K_LOCAL, dtype=torch.float16, device=device)
    layer.weight = types.SimpleNamespace(data=maybe_trans_nz(weight))
    layer.params_dtype = torch.float16
    return layer


def _reference(layer, x):
    y = F.linear(x, layer.weight.data).float()
    dist.all_reduce(y)
    return y.to(torch.float16)


def _stream_sync() -> None:
    torch.npu.current_stream().synchronize()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--rows",
        type=int,
        nargs="+",
        default=[
            1,
            10,
            16,
            17,
            32,
            33,
            64,
            128,
            255,
            256,
            257,
            511,
            512,
            513,
            1024,
            2048,
            4096,
            6144,
            8192,
        ],
    )
    parser.add_argument(
        "--batch-basem-count",
        type=int,
        choices=(1, 2, 4),
        default=int(
            os.getenv(
                "VLLM_ASCEND_310P_MEMFABRIC_MM_AR_BATCH_BASEM_COUNT",
                "1",
            )
        ),
        help="ABI v7 communication batch multiplier q over baseM=256.",
    )
    parser.add_argument("--repeat", type=int, default=20)
    parser.add_argument("--atol", type=float, default=0.0)
    parser.add_argument("--rtol", type=float, default=0.0)
    parser.add_argument(
        "--diverge-va",
        action="store_true",
        help=(
            "Rank 1 allocates 4GB of device memory before the pool is "
            "created so the per-process smem GVA bases diverge. Regression "
            "guard for the peer-space mail dst validation: mail dst fields "
            "carry sender-space GVAs, which only coincide with receiver-"
            "space addresses when both processes were assigned the same "
            "mapping base."
        ),
    )
    args = parser.parse_args()

    local_rank = int(os.environ["LOCAL_RANK"])
    rank = int(os.environ["RANK"])
    if int(os.environ["WORLD_SIZE"]) != 2:
        raise RuntimeError("This benchmark requires exactly TP=2")

    # The CLI flag must reach every rank: the plan (and with it batch_m /
    # batch_bytes / mail imm-len expectations) is read per process, and a
    # rank-0-only override silently desynchronizes the two sides when the
    # ambient env var differs from --batch-basem-count.
    os.environ["VLLM_ASCEND_310P_MEMFABRIC_MM_AR_BATCH_BASEM_COUNT"] = str(args.batch_basem_count)

    torch.npu.set_device(local_rank)
    device = torch.device(f"npu:{local_rank}")
    dist.init_process_group(backend="hccl")
    # Host-only rendezvous used after MemFabric starts. This avoids turning
    # the correctness benchmark into an HCCL/MemFabric coexistence test.
    cpu_group = dist.new_group(backend="gloo")

    if rank == 0:
        print(
            f"base_m=256 batch_basem_count={args.batch_basem_count} "
            f"batch_m={256 * args.batch_basem_count} repeat={args.repeat}"
        )
        print("rows\tverdict\tmax_abs_diff\tn_bad\tfirst_ms\tmin_ms\tmed_ms\tmax_ms")

    layer = _make_layer(rank, device)

    # Diverge the per-process device-VA state before MemFabric pool creation.
    va_junk: list[torch.Tensor] = []
    if args.diverge_va and rank == 1:
        for _ in range(8):
            va_junk.append(torch.empty(256 * 1024 * 1024, dtype=torch.float16, device=device))
            va_junk[-1].fill_(1.0)

    # Materialize all HCCL references before MemFabric context creation.
    inputs: dict[int, torch.Tensor] = {}
    refs: dict[int, torch.Tensor] = {}
    for rows in args.rows:
        if rows <= 0:
            raise ValueError(f"rows must be positive, got {rows}")
        torch.manual_seed(42 + rows)
        x = torch.randn(rows, K_LOCAL, dtype=torch.float16, device=device)
        inputs[rows] = x
        refs[rows] = _reference(layer, x)
    torch.npu.synchronize()
    dist.barrier(group=cpu_group)

    for rows in args.rows:
        x = inputs[rows]
        ref = refs[rows]

        t0 = time.perf_counter()
        fused = memfabric_mm_ar_allreduce(layer=layer, x=x, tp_rank=rank)
        _stream_sync()
        first_ms = (time.perf_counter() - t0) * 1e3

        diff = (fused.float() - ref.float()).abs()
        max_diff = diff.max().item()
        n_bad = (diff > (args.atol + args.rtol * ref.float().abs())).sum().item()
        ok = torch.allclose(fused, ref, rtol=args.rtol, atol=args.atol)

        steady_ms = []
        for _ in range(args.repeat - 1):
            t1 = time.perf_counter()
            out = memfabric_mm_ar_allreduce(layer=layer, x=x, tp_rank=rank)
            _stream_sync()
            steady_ms.append((time.perf_counter() - t1) * 1e3)
            if not torch.allclose(out, ref, rtol=args.rtol, atol=args.atol):
                ok = False
                n_bad += ((out.float() - ref.float()).abs() > (args.atol + args.rtol * ref.float().abs())).sum().item()

        steady_ms.sort()
        verdict = "PASS" if ok else "FAIL"
        if rank == 0:
            if steady_ms:
                med = steady_ms[len(steady_ms) // 2]
                print(
                    f"{rows}\t{verdict}\t{max_diff:.6f}\t{n_bad}"
                    f"\t{first_ms:.3f}\t{steady_ms[0]:.3f}"
                    f"\t{med:.3f}\t{steady_ms[-1]:.3f}"
                )
            else:
                print(f"{rows}\t{verdict}\t{max_diff:.6f}\t{n_bad}\t{first_ms:.3f}")

        if not ok:
            raise AssertionError(f"FP16 mm_ar fused mismatch rows={rows} max_abs_diff={max_diff}")

    if rank == 0:
        print("ALL PASS", flush=True)

    # Both ranks must finish their last wave before either destroys the
    # symmetric pool. Use host-only Gloo, not HCCL, for this rendezvous.
    dist.barrier(group=cpu_group)
    torch.ops._C_ascend.memfabric_mm_ar_shutdown()
    dist.barrier(group=cpu_group)


if __name__ == "__main__":
    main()
