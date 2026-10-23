# SPDX-License-Identifier: Apache-2.0
"""Opt-in 310P3 V2 serving route: 2048<=M<=4096 fused, otherwise Stock.

V1 serving remains unchanged when this feature is disabled. The native
large-M op uses the already-tested V2 T0 one-kernel path, with a bounded
pre-pool output buffer lease and exact-M tail padding at the C++ boundary.

Current safety contract: only FULL_DECODE_ONLY graph (capture sizes <2048)
is supported; large-M fused prefill is eager. Production enablement still
requires G3 first-write, KFC and TP skew evidence from real 310P3 tests.

The first fused call of each tail shape class (chunk-aligned vs padded)
is verified bit-exact against the stock MM + HCCL path. A mismatch raises
immediately (fail-stop) instead of letting a cold-pool wave silently
corrupt model outputs: the 2026-10-18 cold-orchestrator incident produced
garbage outputs with protocol status 0 on the first loads of a freshly
deployed kernel.
"""
from __future__ import annotations

import torch
from vllm.logger import logger

from vllm_ascend import envs

MIN_FUSED_M = 2048
MAX_FUSED_M = 4096  # the configured 4096-token Forward budget
_pool_started = False
_logged_stock = False
_logged_fused = False
_logged_oversize_stock = False
_verified_aligned = False
_verified_padded = False


def v2_pool_started() -> bool:
    return _pool_started


def v2_threshold_route(m: int) -> str:
    """Pure route decision, intentionally independent from V1's q/batch size."""
    if m < 0:
        raise ValueError(f"negative M: {m}")
    # Forward M may exceed this serving profile's expected 4096-token
    # budget. Stock must remain a safe, non-fused escape hatch: do NOT
    # split it into multiple V2 waves or abort normal inference.
    return "v2" if MIN_FUSED_M <= m <= MAX_FUSED_M else "stock"


def make_memfabric_mm_ar_v2_linear_method():
    from vllm_ascend.ops.linear import AscendUnquantizedLinearMethod
    from vllm_ascend._310p.ops.memfabric_mm_ar import (
        configure_memfabric_mm_ar,
        is_memfabric_mm_ar_configured,
        memfabric_mm_ar_warmup_fallback_active,
    )

    class MemFabricMmArV2LinearMethod310(AscendUnquantizedLinearMethod):
        """Only selected FP16 RowParallelLinear o_proj/out_proj are routed."""

        def process_weights_after_loading(self, layer: torch.nn.Module) -> None:
            super().process_weights_after_loading(layer)
            configure_memfabric_mm_ar(layer)

        def apply(
            self,
            layer: torch.nn.Module,
            x: torch.Tensor,
            bias: torch.Tensor | None = None,
        ) -> torch.Tensor:
            global _pool_started, _logged_stock, _logged_fused
            global _logged_oversize_stock, _verified_aligned, _verified_padded

            if not is_memfabric_mm_ar_configured(layer):
                return super().apply(layer, x, bias)
            if bias is not None:
                raise RuntimeError("310P V2 fused o_proj does not support bias")

            # Profile/dummy-run and any M outside [2048,4096] go through
            # the original native MM + HCCL. Decode can still be captured
            # by the existing FULL_DECODE_ONLY graph route.
            if memfabric_mm_ar_warmup_fallback_active() or (
                v2_threshold_route(int(x.shape[0])) == "stock"
            ):
                if x.shape[0] > MAX_FUSED_M and not _logged_oversize_stock:
                    logger.warning(
                        "[mf310p-v2] unexpected Forward M=%d > %d; "
                        "falling back to original MM + HCCL AR (not V2)",
                        x.shape[0], MAX_FUSED_M,
                    )
                    _logged_oversize_stock = True
                if not _logged_stock:
                    logger.info("[mf310p-v2] M=%d outside [2048,4096] (or dummy warmup): native MM + HCCL", x.shape[0])
                    _logged_stock = True
                from vllm.distributed import tensor_model_parallel_all_reduce
                partial = super().apply(layer, x, bias)
                return tensor_model_parallel_all_reduce(partial)

            op = getattr(torch.ops._C_ascend, "memfabric_mm_ar_v2_allreduce", None)
            if op is None:
                raise RuntimeError(
                    "310P V2 serving op missing. Rebuild with both "
                    "VLLM_ASCEND_310P_ENABLE_MEMFABRIC_MM_AR=1 and "
                    "VLLM_ASCEND_310P_ENABLE_MEMFABRIC_MM_AR_V2=1."
                )
            from vllm.distributed import get_tensor_model_parallel_rank

            chunk = int(envs.VLLM_ASCEND_310P_MEMFABRIC_V2_CHUNK_ROWS)
            if chunk not in (256, 512, 1024):
                raise ValueError(f"invalid V2 chunk_rows={chunk}")
            out = op(
                x.contiguous(), layer.weight.data,
                int(get_tensor_model_parallel_rank()), chunk,
            )
            _pool_started = True
            if not _logged_fused:
                logger.info("[mf310p-v2] M=%d within [2048,4096]: V2 MM+MemFabric (chunk=%d)", x.shape[0], chunk)
                _logged_fused = True

            # Numerical gate for the first fused call of each tail shape
            # class. Both ranks run this symmetrically; the 2-rank FP16 sum
            # is order-independent, so bit-exact equality with the stock
            # path is the expected steady-state behavior (proven by the
            # TP=2 mixed-route test). A cold pool/transport must fail
            # loudly here instead of silently corrupting model outputs.
            aligned = (int(x.shape[0]) % chunk) == 0
            needs_verify = (aligned and not _verified_aligned) or (
                (not aligned) and not _verified_padded)
            if needs_verify:
                from vllm.distributed import tensor_model_parallel_all_reduce
                reference = tensor_model_parallel_all_reduce(
                    super().apply(layer, x, bias))
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
                    "[mf310p-v2] first fused call verified bit-exact vs "
                    "stock (M=%d, aligned=%s)", x.shape[0], aligned)
            # The V2 op already performs a TP=2 reduction. Generic HCCL
            # MUST NOT run again on this branch.
            return out

    return MemFabricMmArV2LinearMethod310
