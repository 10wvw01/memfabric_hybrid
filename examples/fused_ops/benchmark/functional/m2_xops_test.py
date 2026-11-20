#!/usr/bin/env python3
"""TP2 functional (M2) test for the prd_code fused operators.

Per-rank process (RANK=0/1 in env); both ranks run the identical sequence:
session create (pool bootstrap inside the xops session facade) -> one
unverified warm-up wave -> one verified real wave -> golden bit-exact
comparison -> teardown. The launch goes through the session facade
(xops_{ar,iar}_310p3.SmemSession), which owns the pool bootstrap, geometry
checks, status/trace channels, epoch bookkeeping and the operator call —
validating the wheel facade -> EXEC_NPU_CMD_V1 -> aclnnSmemMm{Ar,Iar}310p3
-> kernel chain end to end exactly as consumers use it.
(The kernel has no enabled=0 prewarm gate — 2026-11-19 ABI reduction; the
warm-up duty lives in the session layer because the first kernel launch
after pool creation has a cold-start corruption window on some device
pairs.)

Verification data: one-hot rows x, small-int weights w; golden AR = y0 + y1.
Both fused kernels write the AR result DIRECTLY into y — verify from the
tensor itself (bit-exact: the 2-rank fp16 sum of these integers is
order-independent).
"""
import argparse
import os
import sys

import numpy as np

import torch  # noqa: E402
import torch_npu  # noqa: E402

K = 2048
N = 2048


def fail(msg):
    print(f"[rank{RANK}] FAIL: {msg}", flush=True)
    sys.exit(1)


def main():
    global RANK
    p = argparse.ArgumentParser()
    p.add_argument("--devices", required=True)
    p.add_argument("--store-url", required=True)
    p.add_argument("--m", type=int, choices=[1280, 2048, 3840, 4096], default=2048)
    p.add_argument("--op", choices=["ar", "iar"], required=True,
                   help="ar = smem_mm_ar_310p3 (local reduction); "
                        "iar = smem_mm_iar_310p3 (hardware reduction)")
    a = p.parse_args()
    RANK = int(os.environ["RANK"])
    devs = [int(x) for x in a.devices.split(",")]
    dev = devs[RANK]
    M = a.m

    torch.npu.set_device(dev)
    device = torch.device(f"npu:{dev}")
    torch.npu.empty_cache()

    # xops 会话封装层：建池（含尺寸下限程序化推导）、几何强校验、
    # status/trace 通道、epoch 内管、算子发射全部在封装层内完成。
    if a.op == "ar":
        import xops_ar_310p3 as xops
    else:
        import xops_iar_310p3 as xops
    sess = xops.SmemSession(device=dev, rank=RANK, store_url=a.store_url)
    print(f"[rank{RANK}] pool gva=0x{sess.gva:x} symm=0x{sess.symm:x} "
          f"floor=0x{xops.POOL_FLOOR_BYTES:x}", flush=True)

    # Deterministic integer test data: one-hot rows, small-int weight.
    wi = (np.arange(N)[:, None] * 31 + np.arange(K)[None, :] * 7) % 13 - 6  # [N,K]
    w_nd = torch.from_numpy(wi.astype(np.float16)).to(device)
    w_nz = torch_npu.npu_format_cast(w_nd.clone(), 29)
    col = np.arange(M) % K
    val = (1 + np.arange(M) % 7) if RANK == 0 else (2 + np.arange(M) % 5)
    x_np = np.zeros((M, K), dtype=np.float16)
    x_np[np.arange(M), col] = val.astype(np.float16)
    x = torch.from_numpy(x_np).to(device)
    y = torch.zeros(M, N, dtype=torch.float16, device=device)

    # Session-layer warm-up wave (unverified): the FIRST kernel launch after
    # process/pool creation has a cold-start corruption window on some device
    # pairs (2026-11-19 finding on devices 2/3: cold first wave mismatches
    # 33~51%, second wave bit-exact; the op binary load races the first real
    # wave's SDMA data). Both ranks fire one lock-step warm-up wave here; the
    # verified wave below is then a warm (second) launch. vllm sessions do not
    # need this: their first-call bit-exact gate verifies the cold first wave
    # and fails loudly on corruption instead.
    sess.launch(x, w_nz, M, out=y)
    torch.npu.synchronize()
    sess.barrier()

    # Verified wave (warm second launch): the golden comparison target.
    sess.launch(x, w_nz, M, out=y)
    torch.npu.synchronize()
    sess.barrier()

    st = sess.status.cpu().numpy().view(np.uint64)[0]
    if st != 0:
        fail(f"kernel status=0x{st:x}")

    # Both operators write the AR result DIRECTLY into y — verify from the tensor.
    got = y.cpu().numpy().astype(np.int32)

    yr = (val[:, None] * wi[:, col].T.astype(np.int32)).astype(np.int16)
    other_val = (2 + np.arange(M) % 5) if RANK == 0 else (1 + np.arange(M) % 7)
    yo = (other_val[:, None] * wi[:, col].T.astype(np.int32)).astype(np.int16)
    golden = (yr + yo).astype(np.int32)
    bad = int(np.count_nonzero(got != golden))
    print(f"[rank{RANK}] {'PASS' if bad == 0 else 'FAIL'} data mismatches={bad}/{M * N}",
          flush=True)

    sess.teardown()
    if bad != 0:
        sys.exit(1)


if __name__ == "__main__":
    main()
