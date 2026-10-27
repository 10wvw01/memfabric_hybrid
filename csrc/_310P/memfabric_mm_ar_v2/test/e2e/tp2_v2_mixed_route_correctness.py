# SPDX-License-Identifier: Apache-2.0
"""G3 TP=2 混合路由：M<2048 或 M>4096 走 Stock，2048～4096 走 V2。

覆盖 V2 MemFabric 建池后仍执行 HCCL、区间内对齐及补齐尾块，
以及 M 超过 4096 时的 Stock 安全回退。仅支持 TP=2。

本脚本直接验证 C++ V2 Serving Op 和运行时协议状态；
vLLM 模型层路由另由设置 MEMFABRIC_MM_AR_E2E_V2=1 的
Serving 功能测试验证。
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

from vllm_ascend.utils import maybe_trans_nz
from vllm_ascend._310p.ops.memfabric_mm_ar_v2_serve import v2_threshold_route


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--rows", type=int, nargs="+",
                   default=[2047, 2048, 2049, 3000, 3073, 4095, 4096, 4097, 5000])
    p.add_argument("--chunk-rows", type=int, choices=(256, 512, 1024),
                   default=1024)
    p.add_argument("--reps", type=int, default=3)
    args = p.parse_args()
    if any(m < 1 for m in args.rows) or args.reps < 1:
        p.error("M must be positive and reps >=1")
    if int(os.environ.get("WORLD_SIZE", "0")) != 2:
        p.error("torchrun --nproc-per-node=2 is required")
    rank, local = int(os.environ["RANK"]), int(os.environ["LOCAL_RANK"])
    torch.npu.set_device(local)
    dev = torch.device(f"npu:{local}")
    dist.init_process_group("hccl")
    group = dist.new_group(backend="gloo")

    torch.manual_seed(812 + rank)
    w = maybe_trans_nz(torch.randn(2048, 2048, dtype=torch.float16, device=dev))
    all_m = sorted(set(args.rows + [1, 10, 2047, 2048, 2049, 3000, 3073, 4095, 4096, 4097, 5000]))
    xs, fp32_refs, stock_refs = {}, {}, {}
    # 全部 FP32/HCCL 正确性参考均在 MemFabric 建池之前生成。
    # 同时预先生成 FP16 Stock 基线，供稍后在池已存在时
    # 检查 Stock/HCCL 的共存和输出一致性。
    for m in all_m:
        torch.manual_seed(6300 + rank * 67 + m)
        x = torch.randn(m, 2048, dtype=torch.float16, device=dev)
        xs[m] = x
        part = F.linear(x, w)
        fp32_ref = part.float()
        dist.all_reduce(fp32_ref)
        fp32_refs[m] = fp32_ref.half()
        stock = F.linear(x, w)
        dist.all_reduce(stock)
        stock_refs[m] = stock
    torch.npu.current_stream().synchronize()
    dist.barrier(group=group)

    plan = [2048, 1, 2049, 10, 2047, 3000, 3073, 4095, 4096, 4097, 5000]
    plan.extend(args.rows)
    plan *= args.reps
    ok = 0
    try:
        for iteration, m in enumerate(plan):
            x = xs[m]
            if v2_threshold_route(m) == "stock":
                out = F.linear(x, w)
                dist.all_reduce(out)
                expected = stock_refs[m]
                path = "stock_hccl_after_pool"
            else:
                out = torch.ops._C_ascend.memfabric_mm_ar_v2_allreduce(
                    x, w, rank, args.chunk_rows
                )
                expected = fp32_refs[m]
                path = "v2_fused"
            torch.npu.current_stream().synchronize()
            if v2_threshold_route(m) == "v2":
                status = int(torch.ops._C_ascend.memfabric_mm_ar_v2_debug_status())
                assert status == 0, (
                    f"protocol status={status} rank={rank} m={m} i={iteration}"
                )
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
        dist.barrier(group=group)
        if rank == 0:
            print(f"TP2 V2 mixed-route PASS: {ok} calls including live HCCL", flush=True)
    finally:
        torch.npu.current_stream().synchronize()
        # 该进程未创建 V1 Serving 上下文。
        torch.ops._C_ascend.memfabric_mm_ar_v2_shutdown()
        dist.destroy_process_group()


if __name__ == "__main__":
    main()
