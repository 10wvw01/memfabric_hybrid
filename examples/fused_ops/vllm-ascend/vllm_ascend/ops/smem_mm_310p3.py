# ----------------------------------------------------------------------------------------------------------
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# This program is free software; you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You should not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------
"""smem_mm_310p3: TP2 MatMul + AllReduce fusion over the MemFabric SDMA pool.

Replaces the MM + tensor-parallel-AR of qualifying RowParallelLinear layers
(full-attention o_proj / GDN O.out) when M >= 1280, on Ascend 310P3 with TP=2.

Two fusion operators are integrated (mutually exclusive per serve instance):
  smem_mm_ar_310p3  - local reduction (COPY full exchange + local ADD lanes)
  smem_mm_iar_310p3 - hardware reduction (ADD_FP16 half exchange + flag convergence)

Selection: additional_config.enable_smem_mm_ar_310p3 /
enable_smem_mm_iar_310p3 (or the deprecated env vars registered in envs.py).
Enabling both is refused loudly before any pool is created.

Chain per worker process:
  SmemMm310P3Session  - thin delegation to the session facade shipped inside
                      the operator wheel (pool bootstrap with a size derived
                      from the operator's exported layout floor, same-base
                      geometry checks, status/trace channels, epoch
                      bookkeeping); created at early bootstrap so the pool
                      window reservation is the FIRST device allocation of
                      the worker
  SmemMm310P3RowParallelOp - wraps a RowParallelLinear; native-M direct launch
                      (no-pad contract: no pad buffers, no staging copy,
                      y allocated per call); the fused kernel writes the AR
                      result directly into y.

Kernel contract (see prd_code/smem_mm_ar_310p3 README):
  x [M,K] fp16 ND contiguous, w [N,K] fp16 FRACTAL_NZ, y [M,N] fp16 ND;
  K == N == 2048; M % 256 == 0 within [1024, 4096] (partial tail MM group /
  partial last comm subgroup supported); 7 int64 attrs (SDMA packet size /
  comm granularity / AIC count are compile-time constants inside the kernel;
  the seat array lives in the pool's seats area — ibPtr was removed with the
  2026-11-19 pool-layout-ownership change).
"""

from __future__ import annotations

import logging
import os
import time
from dataclasses import dataclass, field
from typing import Optional

import torch
import torch_npu

from vllm.config import ModelConfig
from vllm.distributed import get_tensor_model_parallel_world_size
from vllm.logger import logger

# Kernel geometry constraints. Single source: the operator's tiling header,
# exported programmatically by the xops wheel and cross-checked at session
# creation (_check_op_contract). These local copies exist only for routing
# checks that run before the wheel is imported.
SMEM_MM_310P3_WIDTH = 2048           # K == N (hidden size per rank at TP2)
SMEM_MM_310P3_MAX_ROWS = 4096        # kernel work-area capacity
# Kernel-side lower bound of the no-pad contract: rows % 256 == 0 within
# [KERNEL_MIN_ROWS, MAX_ROWS] (partial tail MM group / partial last comm subgroup).
SMEM_MM_310P3_KERNEL_MIN_ROWS = 1024
# Engagement gate (rows). Default 1280: on the 310P Qwen3.6-A3B dflash deployment
# the mamba-block-aligned scheduler splits every prefill into 1280-row chunks
# (block 640 x 2), which is the first M the fusion is competitive at after the
# no-pad contract. With the native-M launch a 1280-row chunk runs as its own
# wave — no pad-to-2048 tax.
SMEM_MM_310P3_MIN_ROWS = int(os.environ.get("SMEM_MM_310P3_MIN_ROWS", 1280))
# Deprecated env aliases (bench convenience): explicit operator pick on top of
# the config keys.
SMEM_MM_310P3_OP = os.environ.get("SMEM_MM_310P3_OP", "")


def _selected_op() -> Optional[str]:
    """Resolve which fusion operator this instance runs: 'ar' / 'iar' / None.

    Priority: additional_config via AscendConfig (with deprecated env fallback
    registered in envs.py) > explicit SMEM_MM_310P3_OP. Enabling both is an
    error (loud refusal before any pool exists).
    """
    from vllm_ascend.ascend_config import get_ascend_config
    cfg = get_ascend_config()
    ar = bool(getattr(cfg, "enable_smem_mm_ar_310p3", False))
    iar = bool(getattr(cfg, "enable_smem_mm_iar_310p3", False))
    if ar and iar:
        raise RuntimeError(
            "smem_mm_ar_310p3 and smem_mm_iar_310p3 are mutually exclusive per "
            "serve instance (both enabled) — pick exactly one")
    if ar:
        return "ar"
    if iar:
        return "iar"
    if SMEM_MM_310P3_OP in ("ar", "iar"):
        return SMEM_MM_310P3_OP
    return None


def smem_mm_310p3_enable() -> bool:
    return _selected_op() is not None


def _deployment_supported(model_config, parallel_config) -> tuple[bool, str]:
    """Route check that only needs configs, valid before distributed init."""
    if parallel_config.tensor_parallel_size != 2:
        return False, "requires TP=2 (smem pool spans 2 ranks)"
    if parallel_config.pipeline_parallel_size != 1:
        return False, "requires PP=1"
    dp = getattr(parallel_config, "data_parallel_size", 1) or 1
    if dp != 1:
        return False, "requires DP=1"
    if model_config.dtype != torch.float16:
        return False, "requires --dtype float16"
    return True, ""


def early_bootstrap_smem_mm_310p3(device: int, rank: int, vllm_config) -> None:
    """Create the smem session as the FIRST device allocation of the worker.

    The 310P ACL pool reservation (AclrtReserveMemAddress) picks its window
    base from the process VA state, and the peer import requires BOTH TP
    workers to have reserved at the same base ("310P import gva out of
    window" otherwise). Fresh worker processes have identical VA state, so
    reserving right after set_device — before HCCL init, torch allocator
    growth, and weight loading — keeps the two bases symmetric. The 2GB
    window reservation survives everything allocated afterwards.
    """
    op = _selected_op()
    if op is None:
        # AscendConfig may not have consumed additional_config yet at this
        # point of worker startup; fall back to the raw vllm_config.
        add = getattr(vllm_config, "additional_config", None) or {}
        ops = [k for k in ("enable_smem_mm_ar_310p3", "enable_smem_mm_iar_310p3")
               if bool(add.get(k, False))]
        if len(ops) > 1:
            raise RuntimeError(
                "smem_mm_ar_310p3 and smem_mm_iar_310p3 are mutually exclusive "
                "per serve instance (both enabled) — pick exactly one")
        if ops:
            op = "ar" if ops[0] == "enable_smem_mm_ar_310p3" else "iar"
    print(f"[smem_mm_310p3] early hook: dev={device} rank={rank} op={op}",
          flush=True)
    if op is None:
        return
    ok, why = _deployment_supported(vllm_config.model_config,
                                    vllm_config.parallel_config)
    if not ok:
        logger.warning("smem_mm_310p3 disabled: %s.", why)
        return
    SmemMm310P3Session.get_or_create(device, rank, op)
    logger.info("smem_mm_310p3 early session bootstrap done (dev=%d rank=%d op=%s)",
                device, rank, op)


def _check_op_contract(xops) -> None:
    """Version-skew guard: fail loudly if this module's routing constants
    disagree with the operator package's exported kernel contract (the single
    source of these numbers is the operator's tiling header, exported
    programmatically by the wheel)."""
    mismatch = []
    if xops.KERNEL_WIDTH != SMEM_MM_310P3_WIDTH:
        mismatch.append(f"width {xops.KERNEL_WIDTH} != {SMEM_MM_310P3_WIDTH}")
    if xops.KERNEL_MAX_ROWS != SMEM_MM_310P3_MAX_ROWS:
        mismatch.append(
            f"max_rows {xops.KERNEL_MAX_ROWS} != {SMEM_MM_310P3_MAX_ROWS}")
    if xops.KERNEL_MIN_ROWS != SMEM_MM_310P3_KERNEL_MIN_ROWS:
        mismatch.append(
            f"min_rows {xops.KERNEL_MIN_ROWS} != "
            f"{SMEM_MM_310P3_KERNEL_MIN_ROWS}")
    if mismatch:
        raise RuntimeError(
            "smem_mm_310p3 contract mismatch between vllm wiring and the "
            f"operator wheel: {'; '.join(mismatch)} — reinstall the xops "
            "wheel matching this vllm-ascend build")


@dataclass
class SmemMm310P3Session:
    """Per-worker smem session. Create once via get_or_create().

    Thin delegation to the session facade shipped inside the operator wheel
    (xops_{ar,iar}_310p3.SmemSession): pool bootstrap with a size derived
    from the operator's exported layout floor, same-base geometry checks,
    status/trace channels and epoch bookkeeping all live in the facade. This
    wrapper keeps the vllm-side concerns (operator selection, device/rank
    routing, early-bootstrap timing) and nothing else — no second copy of
    pool or ABI knowledge lives here.
    """

    device: int
    rank: int                      # smem rank id (0/1), == TP rank in pool
    op: str                        # "ar" (local reduction) / "iar" (hardware reduction)
    _inner: object = field(default=None, repr=False)

    @classmethod
    def get_or_create(cls, device: int, rank: int,
                      op: str = "ar") -> "SmemMm310P3Session":
        global _SESSION
        if _SESSION is None:
            # The xops wheels register torch_npu.npu_smem_mm_{ar,iar}_310p3 at
            # import time and ship the session facade; the worker process does
            # not import them anywhere else.
            if op == "ar":
                import xops_ar_310p3 as xops
            elif op == "iar":
                import xops_iar_310p3 as xops
            else:
                raise RuntimeError(f"smem_mm_310p3: unknown op {op!r}")
            _check_op_contract(xops)
            inner = xops.SmemSession.get_or_create(device=device, rank=rank)
            _SESSION = cls(device=device, rank=rank, op=op)
            _SESSION._inner = inner
            # Marker line required by the E2E takeover-verification
            # discipline (6 markers = 2 workers x session up / installed
            # on 40 / verified bit-exact; grep must not depend on /health).
            logger.info("smem_mm_310p3 session up: op=%s rank=%d dev=%d "
                        "gva=0x%x symm=0x%x", op, rank, device,
                        inner.gva, inner.symm)
        return _SESSION

    @property
    def gva(self) -> int:
        """Local symmetric-pool base VA (diagnostics)."""
        return self._inner.gva

    def launch(self, x: torch.Tensor, w_nz: torch.Tensor, y: torch.Tensor,
               rows: int) -> None:
        """Fire one fused wave; x/y are [rows, 2048] fp16 contiguous (native M, no padding)."""
        self._inner.launch(x, w_nz, rows, out=y)

    def teardown(self) -> None:
        self._inner.teardown()


_SESSION: Optional[SmemMm310P3Session] = None
# First fused call verified bit-exact against the native MM+AR path (the
# 2-rank FP16 sum is order-independent, so bit-exact equality is expected;
# a cold/corrupted pool must fail loudly here instead of corrupting output).
_FUSED_VERIFIED = [False]
# One-shot fallback observability: log each distinct (layer, reason) once
# per process so a silent native-path fallback cannot go unnoticed.
_FALLBACK_LOGGED: set = set()

# Optional instrumentation / A-B switches (perf-analysis only):
#   SMEM_MM_310P3_TIME=1          per-op device timing (fused / native)
#   SMEM_MM_310P3_FORCE_NATIVE=1  wrappers installed but always route to native path
# WARNING: SMEM_MM_310P3_TIME=1 DEADLOCKED startup when tested in-situ (per-call
# torch.npu.synchronize() interleaved with the wave protocol + HCCL
# collectives). Only use SMEM_MM_310P3_TIME in offline harnesses. With the
# pool resident, per-op host syncs degrade SDMA completions to ~500ms
# watchdogs.
_TIMING = bool(os.environ.get("SMEM_MM_310P3_TIME"))
_FORCE_NATIVE = bool(os.environ.get("SMEM_MM_310P3_FORCE_NATIVE"))
_stat = {"fused_ms": 0.0, "fused_n": 0,
         "native_ms": 0.0, "native_n": 0}


def _sync_t() -> float:
    torch.npu.synchronize()
    return time.perf_counter()


def _report(tag: str) -> None:
    s = _stat
    logger.info("[smem_mm_310p3:%s] fused=%.2fms(x%d) "
                "native=%.2fms(x%d)",
                tag, s["fused_ms"], s["fused_n"],
                s["native_ms"], s["native_n"])


class SmemMm310P3RowParallelOp:
    """Wraps a qualifying RowParallelLinear: fused MM+AR for M >= 1280 (native M)."""

    def __init__(self, layer, session: SmemMm310P3Session):
        self.layer = layer
        self.session = session
        self.w_nz: Optional[torch.Tensor] = None
        self._prepare_weight()

    # -- weight -----------------------------------------------------------

    def _prepare_weight(self) -> None:
        w = self.layer.weight
        quant = getattr(self.layer, "quant_method", None)
        if quant is not None and w.dtype == torch.int8:
            # W8A8 checkpoint: dequantize to fp16 (exact per-channel), then NZ.
            scale = getattr(self.layer, "weight_scale", None)
            if scale is None:
                raise RuntimeError(
                    f"smem_mm_310p3: quantized layer {self.layer.prefix} has no "
                    "weight_scale; cannot dequantize")
            scale = scale.reshape(-1, 1).to(torch.float32)
            w = (w.to(torch.float32) * scale).to(torch.float16)
        else:
            w = w.to(torch.float16)
        self.w_nz = torch_npu.npu_format_cast(w.contiguous(), 29)

    # -- forward ----------------------------------------------------------

    def apply(self, x: torch.Tensor):
        layer = self.layer
        M = x.shape[0]
        reason = None
        if _FORCE_NATIVE:
            reason = "force-native switch"
        elif M < SMEM_MM_310P3_MIN_ROWS:
            reason = f"M={M} < MIN_ROWS={SMEM_MM_310P3_MIN_ROWS}"
        elif not x.is_contiguous():
            reason = "input not contiguous"
        elif layer.bias is not None:
            reason = "layer has bias"
        elif x.dtype != torch.float16:
            reason = f"dtype={x.dtype}"
        else:
            # No-pad contract: the kernel takes native M directly —
            # rows % 256 == 0 within [KERNEL_MIN_ROWS, MAX_ROWS] (partial tail
            # MM group / partial last comm subgroup run as-is, no pad-to-1024
            # wave tax). Anything off-contract (vllm chunked prefill bounds M by
            # max_num_batched_tokens; M > MAX_ROWS would need wave splitting)
            # falls back to the native path rather than splitting here.
            if (M < SMEM_MM_310P3_KERNEL_MIN_ROWS
                    or M > SMEM_MM_310P3_MAX_ROWS):
                reason = (f"M={M} outside kernel rows range "
                          f"[{SMEM_MM_310P3_KERNEL_MIN_ROWS}, "
                          f"{SMEM_MM_310P3_MAX_ROWS}]")
            elif M % 256 != 0:
                reason = f"M={M} not 256-aligned (no-pad kernel contract)"
        if reason is not None:
            # Fallback: native path (plain MM + TP allreduce). Logged once
            # per reason per process — a silent fallback would otherwise
            # silently erase the fusion (observed in practice).
            key = f"{layer.prefix}:{reason}"
            if key not in _FALLBACK_LOGGED:
                _FALLBACK_LOGGED.add(key)
                logger.info("[smem_mm_310p3] FALLBACK to native on %s: %s",
                            layer.prefix, reason)
            return self._native_forward(x)

        # Native-M direct launch: x is used as-is (contiguity already
        # checked) and y is allocated per call — no pad buffers, no staging
        # copy, no persistent pad residency.
        y = torch.empty(M, SMEM_MM_310P3_WIDTH,
                        dtype=torch.float16, device=x.device)
        if _TIMING:
            t1 = _sync_t()
        if not _FUSED_VERIFIED[0]:
            # One-shot gate: compute the stock MM + HCCL AR reference on
            # this exact input, then the fused wave, and require bit-exact
            # equality. Both ranks run this symmetrically.
            ref = self._native_forward(x)[0]
            self.session.launch(x, self.w_nz, y, M)
            if not torch.equal(ref, y):
                bad = int((ref != y).sum().item())
                raise RuntimeError(
                    f"smem_mm_310p3 first fused call verification FAILED "
                    f"(M={M}, mismatched elements={bad}); pool may be cold/"
                    "corrupted — restart BOTH TP workers")
            _FUSED_VERIFIED[0] = True
            logger.info("[smem_mm_310p3] first fused call verified bit-exact vs "
                        "native MM+AR (M=%d)", M)
            return y, None
        self.session.launch(x, self.w_nz, y, M)
        if _TIMING:
            _stat["fused_ms"] += (_sync_t() - t1) * 1e3
            _stat["fused_n"] += 1
            if _stat["fused_n"] % 100 == 0:
                _report("fused")
        return y, None

    def _native_forward(self, x: torch.Tensor):
        layer = self.layer
        if _TIMING:
            t0 = _sync_t()
        out = layer.quant_method.apply(layer, x)
        if layer.reduce_results:
            from vllm.distributed import tensor_model_parallel_all_reduce
            out = tensor_model_parallel_all_reduce(out)
        if _TIMING:
            _stat["native_ms"] += (_sync_t() - t0) * 1e3
            _stat["native_n"] += 1
            if _stat["native_n"] % 100 == 0:
                _report("native")
        return out, None


def _get_parallel_config():
    from vllm.config import get_current_vllm_config
    return get_current_vllm_config().parallel_config


def _layer_qualifies(layer) -> bool:
    try:
        from vllm.model_executor.layers.linear import RowParallelLinear
    except ImportError:
        return False
    if not isinstance(layer, RowParallelLinear):
        return False
    if layer.input_size_per_partition != SMEM_MM_310P3_WIDTH:
        return False
    if layer.output_size != SMEM_MM_310P3_WIDTH:
        return False
    if layer.bias is not None:
        return False
    if not getattr(layer, "input_is_parallel", True):
        return False
    w = layer.weight
    if w.dtype not in (torch.float16, torch.int8):
        return False
    # w8a8 layers need a per-channel weight_scale to dequantize exactly.
    if w.dtype == torch.int8 and not hasattr(layer, "weight_scale"):
        return False
    return True


def maybe_install_smem_mm_310p3(model: torch.nn.Module, device: int,
                                model_config: ModelConfig) -> list[str]:
    """Post-load hook: create the session and wrap qualifying layers.

    Returns the list of wrapped layer names (empty if disabled/unsuitable).
    """
    op = _selected_op()
    if op is None:
        return []
    ok, why = _deployment_supported(model_config, _get_parallel_config())
    if not ok:
        logger.warning("smem_mm_310p3 disabled: %s.", why)
        return []

    from vllm.distributed import get_tensor_model_parallel_rank
    rank = get_tensor_model_parallel_rank()
    session = SmemMm310P3Session.get_or_create(device, rank, op)

    wrapped = []
    for name, module in model.named_modules():
        if _layer_qualifies(module):
            module.forward = SmemMm310P3RowParallelOp(module, session).apply
            wrapped.append(name)
    logger.info("smem_mm_310p3[%s] installed on %d layers: %s", op, len(wrapped),
                ", ".join(wrapped[:8]) + ("..." if len(wrapped) > 8 else ""))
    return wrapped
