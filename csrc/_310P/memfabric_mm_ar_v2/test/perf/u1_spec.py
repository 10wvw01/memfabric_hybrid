#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""U1统一实验协议：纯Python，可在没有NPU环境下静态审计。"""
from __future__ import annotations

SMALL_MS = (1, 8, 10, 16, 17, 64, 255, 256)
LARGE_MS = (1024, 2048, 4096)
SHAPES = {"small": SMALL_MS, "large": LARGE_MS}
FUSED_ARM = {"small": "memfabric_t1", "large": "memfabric_d4"}
BASE_ARM = "stock_fp16"
SCHEDULE = (BASE_ARM, "fused", "fused", BASE_ARM) * 2
D4_DEVICE_SHA = "2a19390c83c3fe6b0bdb8946d88990cca66b331f765ca445dbda2e787c2d2a19"
PLUGIN_SHA = "8eb65de98c3d8664ff10b421c1d7ad31318dc720c3f5679cd4d34e187203bea1"
MEMFABRIC_HOST_SHA = "1dc46d7dcbe583a5dbb78b4c65a0b5e22f9f352976c53ed31b761fa744f91a30"
KVER_SHA = "25e16b0af2f16989231ff0443698261257bd8142834783a0c1e4ad57cd9ebc78"
K = N = 2048
CHUNK = 1024
MM_CORES = 8
ADD_CORES = 4
WARMUP = {"smoke": 3, "screen": 3, "confirm": 5}
ITERS = {"smoke": 6, "screen": 12, "confirm": 40}
MIN_SPEEDUP_PCT = 5.0
ANCHOR_PCT_MAX = 5.0
MAX_TAIL_DEGRADE = 1.05


def arm_for(scope: str, stage: int) -> str:
    if scope not in SHAPES or not 1 <= stage <= 8:
        raise ValueError("scope/stage invalid")
    a = SCHEDULE[stage - 1]
    return FUSED_ARM[scope] if a == "fused" else BASE_ARM


def order_for(ms: tuple[int, ...], stage: int) -> tuple[int, ...]:
    """Odd/even session cyclic shift; all shapes retained, last includes first anchor repeated externally."""
    if not ms or len(set(ms)) != len(ms) or not 1 <= stage <= 8:
        raise ValueError("invalid shapes/stage")
    k = (stage - 1) % len(ms)
    return ms[k:] + ms[:k]


def eligible(scope: str, m: int) -> bool:
    if scope == "small":
        return m in SMALL_MS
    return scope == "large" and m in LARGE_MS and m % CHUNK == 0


def session_id(scope: str, stage: int) -> str:
    return f"{stage:02d}_{scope}_{arm_for(scope, stage)}"


def plan(scope: str, phase: str) -> list[dict]:
    if scope not in SHAPES or phase not in WARMUP:
        raise ValueError("scope/phase invalid")
    stages = (1, 2) if phase in ("smoke", "screen") else tuple(range(1, 9))
    return [
        {"scope": scope, "phase": phase, "stage": i, "session": session_id(scope, i),
         "arm": arm_for(scope, i),
         "M_order": ((10,) if scope == "small" else (2048,)) if phase == "smoke"
                    else order_for(SHAPES[scope], i),
         "warmup": WARMUP[phase], "iters": ITERS[phase]}
        for i in stages
    ]
