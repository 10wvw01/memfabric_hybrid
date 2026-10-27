# SPDX-License-Identifier: Apache-2.0
"""310P3 V2 Serving 的可选路由实验：2048≤M≤4096 走融合，其余走 Stock。

关闭特性时 V1 Serving 不变；大 M 使用已测过的 V2 T0 单 Kernel，
在 C++ 层管理有界持久输出缓冲和精确 M 的尾块补齐。
当前安全范围仅允许 FULL_DECODE_ONLY Graph 捕获小 M，
大 M 融合 Prefill 采用 Eager。生产启用仍需实机证明首写、
KFC 初始化与 TP 偏斜稳定性。

每种尾块形状类别（对齐和补齐）的首次融合调用都会以
原生 MM+HCCL 参考进行位精确校验。若不匹配便 Fail-stop，
阻止冷池首波坏数据进入模型结果。2026-10-18 的冷
orchestrator 事件中，新部署 Kernel 的首次读取产生
垃圾输出而协议 status 仍为 0。
"""

from __future__ import annotations

import torch
from vllm.logger import logger

from vllm_ascend import envs

MIN_FUSED_M = 2048
MAX_FUSED_M = 4096  # 服务配置的 4096-token 单次 Forward 预算
_pool_started = False
_logged_stock = False
_logged_fused = False
_logged_oversize_stock = False
_verified_aligned = False
_verified_padded = False


def v2_pool_started() -> bool:
    return _pool_started


def v2_threshold_route(m: int) -> str:
    """纯 M 区间路由判断，与 V1 的 q 和 batch 配置独立。"""
    if m < 0:
        raise ValueError(f"negative M: {m}")
    # 即使本服务预期 Forward M≤4096，也可能收到更大的 M，
    # 此时 Stock 必须提供安全的非融合回退，不能
    # 把输入拆成多波 V2，也不能直接终止正常推理。
    return "v2" if MIN_FUSED_M <= m <= MAX_FUSED_M else "stock"


@torch.library.custom_op("vllm_ascend::memfabric_v2_routed_linear", mutates_args=())
def memfabric_v2_routed_linear(x: torch.Tensor, weight: torch.Tensor, tp_rank: int, chunk: int) -> torch.Tensor:
    """Stock 与 V2 融合之间逐次调用的不透明路由边界。

    torch.compile/ACL Graph 不能把该路由条件静态固化：
    其编译 backbone 采用符号化 M，dummy 追踪时预热回退生效，
    若关闭动态 Guard，Stock 分支会覆盖所有回放 M。
    2026-10-19 发现编译产物含大量 all_reduce 调用却没有
    MemFabric Op，所谓融合对比实际上变成 Stock 对 Stock。
    custom-op 对编译器不透明，使 Python Body 在运行期
    每次调用都重新判断 M。FULL_DECODE_ONLY 捕获时仅记录
    Stock 分支（unquantized_gemm+HCCL），从而保持 Decode
    Graph 捕获安全。
    """
    global _pool_started, _logged_stock, _logged_fused
    global _logged_oversize_stock, _verified_aligned, _verified_padded
    from vllm.distributed import tensor_model_parallel_all_reduce

    from vllm_ascend._310p.ops.memfabric_mm_ar import (
        memfabric_mm_ar_warmup_fallback_active,
    )

    # Eager 启动期的 dummy/profile 与全部区间外 M 都
    # 执行原生 MM+HCCL。
    if memfabric_mm_ar_warmup_fallback_active() or v2_threshold_route(int(x.shape[0])) == "stock":
        if x.shape[0] > MAX_FUSED_M and not _logged_oversize_stock:
            logger.warning(
                "[mf310p-v2] unexpected Forward M=%d > %d; falling back to original MM + HCCL AR (not V2)",
                x.shape[0],
                MAX_FUSED_M,
            )
            _logged_oversize_stock = True
        if not _logged_stock:
            logger.info("[mf310p-v2] M=%d outside [2048,4096] (or dummy warmup): native MM + HCCL", x.shape[0])
            _logged_stock = True
        partial = torch.ops.vllm.unquantized_gemm(x, weight, None)
        return tensor_model_parallel_all_reduce(partial)

    op = getattr(torch.ops._C_ascend, "memfabric_mm_ar_v2_allreduce", None)
    if op is None:
        raise RuntimeError(
            "310P V2 serving op missing. Rebuild with both "
            "VLLM_ASCEND_310P_ENABLE_MEMFABRIC_MM_AR=1 and "
            "VLLM_ASCEND_310P_ENABLE_MEMFABRIC_MM_AR_V2=1."
        )
    out = op(x.contiguous(), weight, tp_rank, chunk)
    _pool_started = True
    if not _logged_fused:
        logger.info("[mf310p-v2] M=%d within [2048,4096]: V2 MM+MemFabric (chunk=%d)", x.shape[0], chunk)
        _logged_fused = True

    # 针对不同尾块形状类别，首次 V2 调用设置数值校验门。
    # 两个 Rank 对称执行；TP=2 下两卡 FP16 求和
    # 与输入顺序无关，预期与 Stock 位精确一致，
    # 已经由混合路由测试实机证明。冷池或传输故障
    # 须在此立即显式失败，而不是让坏数据静默
    # 进入模型输出。custom-op 的 Python 实现在真实 Prefill 时
    # 逐次调用，因此编译模式下也会执行该校验。
    aligned = (int(x.shape[0]) % chunk) == 0
    needs_verify = (aligned and not _verified_aligned) or ((not aligned) and not _verified_padded)
    if needs_verify:
        reference = tensor_model_parallel_all_reduce(torch.ops.vllm.unquantized_gemm(x, weight, None))
        if not torch.equal(out, reference):
            bad = int((out != reference).sum().item())
            raise RuntimeError(
                "[mf310p-v2] first fused call verification FAILED "
                f"(M={x.shape[0]}, aligned={aligned}, mismatched "
                f"elements={bad}). The MemFabric pool is in a cold/"
                "corrupted state; restart BOTH TP=2 workers (and "
                "re-check the orchestrator kernel deployment)."
            )
        if aligned:
            _verified_aligned = True
        else:
            _verified_padded = True
        logger.info(
            "[mf310p-v2] first fused call verified bit-exact vs stock (M=%d, aligned=%s)",
            x.shape[0],
            aligned,
        )
    # V2 Op 内已完成 TP=2 AllReduce，不允许在
    # 此分支再次执行框架 HCCL，避免重复规约。
    return out


@memfabric_v2_routed_linear.register_fake
def _(x: torch.Tensor, weight: torch.Tensor, tp_rank: int, chunk: int) -> torch.Tensor:
    return torch.empty_like(x)


def make_memfabric_mm_ar_v2_linear_method():
    from vllm_ascend._310p.ops.memfabric_mm_ar import (
        configure_memfabric_mm_ar,
        is_memfabric_mm_ar_configured,
    )
    from vllm_ascend.ops.linear import AscendUnquantizedLinearMethod

    class MemFabricMmArV2LinearMethod310(AscendUnquantizedLinearMethod):
        """仅对选定 FP16 RowParallelLinear o_proj/out_proj 执行路由。"""

        def process_weights_after_loading(self, layer: torch.nn.Module) -> None:
            super().process_weights_after_loading(layer)
            configure_memfabric_mm_ar(layer)

        def apply(
            self,
            layer: torch.nn.Module,
            x: torch.Tensor,
            bias: torch.Tensor | None = None,
        ) -> torch.Tensor:
            if not is_memfabric_mm_ar_configured(layer):
                return super().apply(layer, x, bias)
            if bias is not None:
                raise RuntimeError("310P V2 fused o_proj does not support bias")
            from vllm.distributed import get_tensor_model_parallel_rank

            chunk = int(envs.VLLM_ASCEND_310P_MEMFABRIC_V2_CHUNK_ROWS)
            if chunk not in (256, 512, 1024):
                raise ValueError(f"invalid V2 chunk_rows={chunk}")
            return memfabric_v2_routed_linear(x, layer.weight.data, int(get_tensor_model_parallel_rank()), chunk)

    return MemFabricMmArV2LinearMethod310
