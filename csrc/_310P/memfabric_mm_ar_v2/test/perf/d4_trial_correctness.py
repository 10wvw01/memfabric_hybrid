#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""D4-2：Tile2048 候选正式核 TP2 正确性矩阵（trial 直调路径）。

形状矩阵覆盖全部三个 tiling 实例（CHUNK 256/512/1024）与多 Chunk overlap：
  4096x1024（D1-A 基线形状，先行）, 256x256, 512x512, 1024x1024,
  2048x512, 2048x1024, 4096x256, 4096x512
每个形状独立 handle：ib_probe → 2 次预热（含首写保护）→ 4 次采样，
每次采样逐样本 status=0 + FP16 位精确（参考在 MemFabric 建池前生成）。
输出缓冲区复用：固定两个长生命周期 y 交替；另含新地址首写检查。
纪律：一会话一候选；MF310P_V2_NOWARMUP 必须 unset；KVER11 显式绝对路径。
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

N = K = 2048
SHAPES = [(4096, 1024), (256, 256), (512, 512), (1024, 1024), (2048, 512), (2048, 1024), (4096, 256), (4096, 512)]


def parse_args():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--out", required=True, type=Path)
    p.add_argument("--reps", type=int, default=4)
    p.add_argument(
        "--store-url", default=os.environ.get("VLLM_ASCEND_310P_MEMFABRIC_STORE_URL", "tcp://127.0.0.1:8581")
    )
    a = p.parse_args()
    if a.reps < 1:
        p.error("--reps >= 1 required")
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


def check_one(lib, handle, y, ref, tag, rank):
    status = ctypes.c_uint64()
    must(lib.mf310p_v2_trial_check_status(handle, ctypes.byref(status)), "status D2H")
    if status.value:
        raise RuntimeError(f"{tag} rank{rank}: protocol status={status.value}")
    if not torch.equal(y, ref):
        bad = int((y != ref).sum().item())
        raise AssertionError(f"{tag} rank{rank}: bit-exact mismatch {bad} elements")


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
        # D4 偏差：宿主 dev2/3 wedge 期间设备侧 HCCL 端口 16666 被一次误杀会话泄露，
        # 本会话改用 gloo。参考 = 双 Rank FP32 偏导之和（TP2 两操作数加法与顺序无关，
        # CPU gloo 与 NPU HCCL 位精确等价）。计时/校验路径本就不含 HCCL。
        torch.manual_seed(86420 + rank)
        weight = maybe_trans_nz(torch.randn(N, K, dtype=torch.float16, device=device))
        xs, refs = {}, {}
        for m in sorted({m for m, _ in SHAPES}):
            torch.manual_seed(74320 + 101 * rank + m)
            x = torch.randn(m, K, dtype=torch.float16, device=device)
            xs[m] = x
            r = F.linear(x, weight).float().cpu()
            dist.all_reduce(r)
            refs[m] = r.to(device).half()
        torch.manual_seed(24680 + rank)
        ys = {
            m: [
                torch.zeros(m, N, device=device, dtype=torch.float16),
                torch.zeros(m, N, device=device, dtype=torch.float16),
            ]
            for m in sorted({m for m, _ in SHAPES})
        }
        stream.synchronize()
        lib = bind()

        results = []
        for si, (m, chunk) in enumerate(SHAPES):
            handle = ctypes.c_void_p()
            dist.barrier()
            must(
                lib.mf310p_v2_trial_create(rank, a.store_url.encode(), chunk, ctypes.byref(handle)),
                f"create {m}x{chunk}",
            )
            if not handle.value:
                raise RuntimeError("MemFabric handle unexpectedly NULL")
            must(lib.mf310p_v2_trial_ib_probe(handle, 64, raw_stream), "ib_probe")
            dist.barrier()
            tag = f"M{m}xC{chunk}"
            # 预热 2 次：首次含新符号首写保护 + 新输出地址设备级同步。
            for i in range(2):
                dist.barrier()
                y = ys[m][i % 2]
                must(
                    lib.mf310p_v2_trial_run(
                        handle, xs[m].data_ptr(), weight.data_ptr(), y.data_ptr(), m, 1, raw_stream
                    ),
                    f"{tag} warmup{i}",
                )
                check_one(lib, handle, y, refs[m], f"{tag} warmup{i}", rank)
            # 采样：双输出缓冲交替（输出槽复用），逐样本位精确。
            for i in range(a.reps):
                dist.barrier()
                y = ys[m][i % 2]
                must(
                    lib.mf310p_v2_trial_run(
                        handle, xs[m].data_ptr(), weight.data_ptr(), y.data_ptr(), m, 1, raw_stream
                    ),
                    f"{tag} rep{i}",
                )
                check_one(lib, handle, y, refs[m], f"{tag} rep{i}", rank)
            # 新地址首写：全新分配的输出，仅一次，含首写同步守护。
            dist.barrier()
            y_fresh = torch.zeros(m, N, device=device, dtype=torch.float16)
            must(
                lib.mf310p_v2_trial_run(
                    handle, xs[m].data_ptr(), weight.data_ptr(), y_fresh.data_ptr(), m, 1, raw_stream
                ),
                f"{tag} fresh-y",
            )
            check_one(lib, handle, y_fresh, refs[m], f"{tag} fresh-y", rank)
            del y_fresh
            results.append({"m": m, "chunk": chunk, "reps": a.reps, "status": 0, "bit_exact": True, "fresh_y": True})
            print(f"[rank{rank}] PASS {tag} (reps={a.reps} + fresh-y)", flush=True)
            dist.barrier()
            stream.synchronize()
            must(lib.mf310p_v2_trial_destroy(handle), f"destroy {tag}")
            handle = ctypes.c_void_p()

        a.out.mkdir(parents=True, exist_ok=True)
        result_path = a.out / f"d4_correctness_rank{rank}.json"
        if result_path.exists():
            raise FileExistsError(f"refusing to overwrite prior evidence {result_path}")
        data = {
            "gate": "D4-2 trial-path correctness matrix",
            "rank": rank,
            "shapes": [list(s) for s in SHAPES],
            "reps": a.reps,
            "status": 0,
            "bit_exact": True,
            "results": results,
            "device_so": os.environ.get("D4_DEVICE_SO_SHA", "unset"),
        }
        result_path.write_text(json.dumps(data, indent=2) + "\n")
        dist.barrier()
        print(f"[rank{rank}] D4-2 TRIAL CORRECTNESS MATRIX PASS", flush=True)
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
