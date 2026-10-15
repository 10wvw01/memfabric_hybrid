# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Huawei Technologies Co., Ltd. All Rights Reserved.

"""310P3 TP=2 MemFabric fused matmul+allreduce (mm_ar) helpers.

The fused path is intentionally narrow. It is only enabled for Qwen3.5/3.6
MoE layers whose RowParallelLinear matches the fused geometry contract:
full-attention ``self_attn.o_proj`` and GDN (linear-attention)
``linear_attn.out_proj``. Both are *unquantized* in the real
Qwen3.6-35B-A3B-w8a8 checkpoint (kept FLOAT) and share the identical shape
K_global=4096 / K_local=2048 / N=2048 at TP=2, so one ABI v7 kernel serves
both. All other layers keep the existing RowParallelLinear implementation.
The checkpoint carries no torch_dtype, so vLLM runs it as FP16 on 310P
(BF16 NZ linear is unsupported by this CANN anyway); the fused path
therefore exchanges FP16 partial results.

There is no M threshold: once the fused path is selected at startup every
M is fused. Tail batches are zero-padded to the fixed communication batch
``batch_m``; the producer scratch is zeroed once at allocation and tail
batches only copy their valid rows (padded rows are computed and exchanged
but never reduced into the output).

The fused op treats wgm-dev-310p MemFabric as an opaque public transport.
All 8 AI cores cooperatively execute each batch MM; core0 becomes the sole
data ``signal()`` owner after every core publishes a generation-tagged
ready cell. Per batch the dataflow is MM -> cache clean -> ready -> signal,
allowing MM of later batches to overlap SDMA of earlier batches. Public
``wait()`` joins each peer batch payload, a repo-owned FP16 add reduces the
two TP partials, and a fixed FIFO credit protects arena reuse across waves
and graph replays.
"""

from __future__ import annotations

import threading
import time
from collections import deque
from contextlib import contextmanager
from dataclasses import dataclass

import regex as re
import torch
from vllm.config import get_current_vllm_config
from vllm.logger import logger

from vllm_ascend import envs

_QWEN35_MOE_TEXT_MODEL_TYPE = "qwen3_5_moe_text"
_EXPECTED_INPUT_SIZE = 4096
_EXPECTED_INPUT_SIZE_PER_PARTITION = 2048
_EXPECTED_OUTPUT_SIZE = 2048
_EXPECTED_TP_SIZE = 2
_BASE_M = 256
_LAYER_INDEX_RE = re.compile(r"(?:^|\.)layers\.(\d+)\.")


@dataclass(frozen=True)
class MemFabricMmArPlan:
    """Static ABI v7 contract for the 310P3 fused implementation."""

    batch_basem_count: int
    base_m: int = _BASE_M
    input_size_per_partition: int = _EXPECTED_INPUT_SIZE_PER_PARTITION
    output_size: int = _EXPECTED_OUTPUT_SIZE
    tp_size: int = _EXPECTED_TP_SIZE

    @property
    def batch_m(self) -> int:
        return self.base_m * self.batch_basem_count

    @property
    def batch_bytes(self) -> int:
        # FP16 [batch_m, 2048]; 2 MiB at the default q=2 is derived.
        return self.batch_m * self.output_size * 2

    def batches_for_tokens(self, num_tokens: int) -> int:
        if num_tokens < 0:
            raise ValueError(f"num_tokens must be non-negative, got {num_tokens}")
        return (num_tokens + self.batch_m - 1) // self.batch_m


def get_memfabric_mm_ar_plan() -> MemFabricMmArPlan:
    q = int(envs.VLLM_ASCEND_310P_MEMFABRIC_MM_AR_BATCH_BASEM_COUNT)
    if q not in (1, 2, 4):
        raise ValueError(f"VLLM_ASCEND_310P_MEMFABRIC_MM_AR_BATCH_BASEM_COUNT must be one of 1/2/4, got {q}")
    return MemFabricMmArPlan(batch_basem_count=q)


def _target_text_config():
    try:
        vllm_config = get_current_vllm_config()
    except Exception:
        return None
    text_config = vllm_config.model_config.hf_text_config
    if getattr(text_config, "model_type", None) != _QWEN35_MOE_TEXT_MODEL_TYPE:
        return None
    return text_config


_FUSED_LAYER_PREFIXES = {
    # full-attention attn output projection
    ".self_attn.o_proj": "full_attention",
    # GDN (linear-attention) attn output projection
    ".linear_attn.out_proj": "linear_attention",
}


def _is_fused_layer_prefix(prefix: str, text_config) -> bool:
    """Cross-check prefix against Qwen3.5/3.6's configured hybrid layer type."""

    suffix = next((s for s in _FUSED_LAYER_PREFIXES if prefix.endswith(s)), None)
    if suffix is None:
        return False

    match = _LAYER_INDEX_RE.search(prefix)
    if match is None:
        return False
    layer_idx = int(match.group(1))

    layer_types = getattr(text_config, "layer_types", None)
    if layer_types is None or layer_idx >= len(layer_types):
        return False
    return layer_types[layer_idx] == _FUSED_LAYER_PREFIXES[suffix]


def should_enable_memfabric_mm_ar(layer: torch.nn.Module) -> bool:
    """Return whether ``layer`` matches the deliberately narrow fused contract."""

    if not envs.VLLM_ASCEND_310P_ENABLE_MEMFABRIC_MM_AR:
        return False

    text_config = _target_text_config()
    if text_config is None:
        return False

    prefix = getattr(layer, "prefix", "")
    if not _is_fused_layer_prefix(prefix, text_config):
        return False

    if getattr(layer, "tp_size", None) != _EXPECTED_TP_SIZE:
        return False
    if getattr(layer, "input_size", None) != _EXPECTED_INPUT_SIZE:
        return False
    if getattr(layer, "input_size_per_partition", None) != _EXPECTED_INPUT_SIZE_PER_PARTITION:
        return False
    if getattr(layer, "output_size", None) != _EXPECTED_OUTPUT_SIZE:
        return False
    # The model runs FP16 on 310P (checkpoint has no torch_dtype and BF16 NZ
    # linear is unsupported here); the exchange/reduce kernel is FP16-only.
    if getattr(layer, "params_dtype", None) != torch.float16:
        return False

    # Owner decision A (2026-09-18): the target checkpoint's fused-eligible
    # o_proj/out_proj layers are unquantized, routed through the 310P
    # unquantized linear method. Accept either that method or the MemFabric
    # dispatch subclass of it (during quant-method routing quant_method may
    # still be None).
    quant_method = getattr(layer, "quant_method", None)
    if quant_method is not None:
        method_name = type(quant_method).__name__
        if method_name not in ("AscendUnquantizedLinearMethod", "MemFabricMmArLinearMethod310"):
            return False
        nested = getattr(quant_method, "quant_method", None)
        if nested is not None and type(nested).__name__ != "AscendUnquantizedLinearMethod":
            return False

    return True


def configure_memfabric_mm_ar(layer: torch.nn.Module) -> bool:
    """Mark a target RowParallelLinear so the fused op owns the TP reduction."""

    enabled = should_enable_memfabric_mm_ar(layer)
    layer._ascend_310p_memfabric_mm_ar = enabled
    if not enabled:
        return False

    if not getattr(layer, "reduce_results", False):
        raise RuntimeError(
            "MemFabric mm_ar fusion expects RowParallelLinear(reduce_results=True) "
            f"before patching, got {getattr(layer, 'prefix', '<unknown>')}"
        )

    # The fused path returns an already-reduced TP=2 result, so the generic
    # RowParallelLinear must not launch its HCCL all-reduce afterwards.
    layer.reduce_results = False
    plan = get_memfabric_mm_ar_plan()
    logger.info(
        "Enable 310P3 TP=2 MemFabric unquantized mm_ar ABI v7 for %s "
        "(base_m=%d, batch_basem_count=%d, batch_m=%d, batch_bytes=%d).",
        getattr(layer, "prefix", "<unknown>"),
        plan.base_m,
        plan.batch_basem_count,
        plan.batch_m,
        plan.batch_bytes,
    )
    return True


def is_memfabric_mm_ar_configured(layer: torch.nn.Module) -> bool:
    return bool(getattr(layer, "_ascend_310p_memfabric_mm_ar", False))


_pool_protocol_started = False


def memfabric_mm_ar_pool_started() -> bool:
    """Whether the MemFabric SDMA pool exists in this process.

    The pool (and its supervised epoch kernel on the orchestrator's own
    launch stream) is created inside the first fused call and lives until
    worker shutdown.
    """
    return _pool_protocol_started


_warmup_fallback_depth = 0


@contextmanager
def memfabric_mm_ar_warmup_fallback():
    """Opt-in D2 workaround: defer pool creation past profile/warmup runs.

    While active (and only when
    ``VLLM_ASCEND_310P_MEMFABRIC_MM_AR_WARMUP_FALLBACK`` is set), routed
    o_proj layers fall back to stock matmul + HCCL all-reduce so the first
    fused call - and with it the MemFabric pool - happens at the first real
    request instead of during engine-init dummy runs.
    """
    global _warmup_fallback_depth
    if envs.VLLM_ASCEND_310P_MEMFABRIC_MM_AR_WARMUP_FALLBACK:
        _warmup_fallback_depth += 1
        try:
            yield
        finally:
            _warmup_fallback_depth -= 1
    else:
        yield


def memfabric_mm_ar_warmup_fallback_active() -> bool:
    return _warmup_fallback_depth > 0


def memfabric_mm_ar_allreduce(
    layer: torch.nn.Module,
    x: torch.Tensor,
    tp_rank: int,
) -> torch.Tensor:
    """Run the fused MM/SDMA/reduce pipeline (unquantized, FP16).

    One opaque custom op per call: each wave consumes a fixed application
    credit, runs 8-core cooperative batch MM, pipelines public MemFabric
    wait/reduce per batch, drains outbound transfers with one quiet(), and
    returns the wave credit after local arena reads complete.
    """
    global _pool_protocol_started

    if tp_rank not in (0, 1):
        raise RuntimeError(f"MemFabric mm_ar fusion only supports TP rank 0/1, got {tp_rank}")
    if x.dtype != torch.float16:
        raise TypeError(f"MemFabric mm_ar expects FP16 activation, got {x.dtype}")
    if x.dim() != 2 or x.shape[1] != _EXPECTED_INPUT_SIZE_PER_PARTITION:
        raise ValueError(f"MemFabric mm_ar expects x=[M, 2048], got {tuple(x.shape)}")
    if layer.params_dtype != torch.float16:
        raise TypeError(f"MemFabric mm_ar reduction currently requires FP16 output, got {layer.params_dtype}")

    plan = get_memfabric_mm_ar_plan()

    direct = getattr(torch.ops._C_ascend, "memfabric_mm_ar_allreduce", None)
    if direct is None:
        raise RuntimeError("vllm_ascend_C was built without the fused 310P MemFabric mm_ar op")
    if envs.VLLM_ASCEND_310P_MEMFABRIC_MM_AR_TRACE:
        ev0, ev1 = _record_fused_call_events()
        out = direct(
            x,
            layer.weight.data,
            int(tp_rank),
            int(plan.batch_basem_count),
        )
        _finish_fused_call_events(ev0, ev1, x.shape[0])
    else:
        out = direct(
            x,
            layer.weight.data,
            int(tp_rank),
            int(plan.batch_basem_count),
        )
    _pool_protocol_started = True
    return out


_DEVICE_MONITOR_LOCK = threading.Lock()
_DEVICE_MONITOR_EVENTS: deque = deque(maxlen=128)
_DEVICE_MONITOR_SEQ = 0
_DEVICE_MONITOR_STARTED = False


def _record_fused_call_events():
    global _DEVICE_MONITOR_SEQ
    ev0 = torch.npu.Event(enable_timing=True)
    ev0.record()
    return ev0, torch.npu.Event(enable_timing=True)


def _finish_fused_call_events(ev0, ev1, rows):
    global _DEVICE_MONITOR_SEQ
    ev1.record()
    with _DEVICE_MONITOR_LOCK:
        _DEVICE_MONITOR_EVENTS.append((_DEVICE_MONITOR_SEQ, ev0, ev1, rows, time.time()))
        if _DEVICE_MONITOR_SEQ == 0:
            logger.info("[mf310p-monitor] first fused call wall=%.3f", time.time())
        _DEVICE_MONITOR_SEQ += 1
        _maybe_start_device_monitor_locked()


def _maybe_start_device_monitor_locked():
    global _DEVICE_MONITOR_STARTED
    if _DEVICE_MONITOR_STARTED:
        return
    _DEVICE_MONITOR_STARTED = True
    threading.Thread(target=_device_monitor_loop, daemon=True).start()


def _device_monitor_loop():
    while True:
        time.sleep(0.5)
        while True:
            with _DEVICE_MONITOR_LOCK:
                if not _DEVICE_MONITOR_EVENTS:
                    break
                seq, ev0, ev1, rows, t_enq = _DEVICE_MONITOR_EVENTS[0]
                if not ev1.query():
                    break
                _DEVICE_MONITOR_EVENTS.popleft()
            try:
                elapsed_ms = ev0.elapsed_time(ev1)
            except Exception:
                elapsed_ms = -1.0
            if elapsed_ms > 500.0:
                logger.warning(
                    "[mf310p-slow] seq=%d rows=%d device_ms=%.1f enq_wall=%.3f",
                    seq,
                    rows,
                    elapsed_ms,
                    t_enq,
                )


def make_memfabric_mm_ar_linear_method():
    """Build the MemFabric dispatch linear method (lazy import, no cycles).

    The returned class subclasses the 310P unquantized linear method so the
    FLOAT-routed o_proj keeps its exact weight handling (NZ cast) while its
    apply() takes over matmul+TP-reduction for eligible layers.
    """
    from vllm_ascend.ops.linear import AscendUnquantizedLinearMethod

    class MemFabricMmArLinearMethod310(AscendUnquantizedLinearMethod):
        """Unquantized linear method that owns the eligible matmul + TP reduction.

        Selected by the 310P modelslim router for layers matching the
        MemFabric mm_ar prefix/geometry contract (full-attention o_proj and
        GDN out_proj); configure-time checks in
        ``configure_memfabric_mm_ar`` remain the single eligibility source
        and can still reject this layer (leaving the stock behavior).
        """

        def process_weights_after_loading(self, layer: torch.nn.Module) -> None:
            super().process_weights_after_loading(layer)
            configure_memfabric_mm_ar(layer)

        def apply(
            self,
            layer: torch.nn.Module,
            x: torch.Tensor,
            bias: torch.Tensor | None = None,
        ) -> torch.Tensor:
            if is_memfabric_mm_ar_configured(layer):
                if bias is not None:
                    # The fused-eligible o_proj/out_proj layers are
                    # bias-free; a biased RowParallel layer would need
                    # explicit rank0-only handling before fusion.
                    raise RuntimeError("MemFabric mm_ar fusion does not support biased layers")
                if memfabric_mm_ar_warmup_fallback_active():
                    # Stock path (warmup dummy runs only): profile/warmup
                    # dummy runs fall back to stock matmul + HCCL all-reduce
                    # so the first fused call - and with it the MemFabric
                    # pool - happens at the first real request.
                    # reduce_results was disabled by the routing, so the
                    # stock path must be followed by the TP reduction the
                    # fused op would have provided. Real requests have no
                    # M threshold: every M is fused (tail batches are
                    # zero-padded to the communication batch).
                    from vllm.distributed import (
                        tensor_model_parallel_all_reduce,
                    )

                    out = super().apply(layer, x, bias)
                    return tensor_model_parallel_all_reduce(out)
                from vllm.distributed import get_tensor_model_parallel_rank

                x = x.contiguous()
                return memfabric_mm_ar_allreduce(
                    layer=layer,
                    x=x,
                    tp_rank=get_tensor_model_parallel_rank(),
                )
            return super().apply(layer, x, bias)

    return MemFabricMmArLinearMethod310
