# SPDX-License-Identifier-Identifier: Apache-2.0
# Copyright (c) 2026 Huawei Technologies Co., Ltd. All Rights Reserved.
"""310P3 MemFabric mm_ar serve-level functional test (real hardware, TP=2).

Verifies, for the Qwen3.6-35B-A3B-w8a8 stack:
1. routing: the fused server enables all 10 full-attention ``o_proj`` and
   all 30 GDN ``linear_attn.out_proj`` layers (per-layer enable log);
2. compute + communication accuracy: greedy outputs of the fused server are
   identical to the stock server (NZ matmul + HCCL all-reduce) for a
   deterministic prompt set, in both eager and ACL-Graph (FULL_DECODE_ONLY,
   capture size 10) modes;
3. batched decode: 10 concurrent greedy requests (decode M up to 10, the
   graph capture size) match stock outputs.

Prerequisites (see tests/e2e/_310p/README.md): Ascend 310P3 die pair, the
wgm-dev-310p MemFabric run package installed with its aicpu kernel deployed,
a feature-on build of vllm_ascend_C (VLLM_ASCEND_310P_ENABLE_MEMFABRIC_MM_AR=1).

Run:
    pytest -sv tests/e2e/_310p/test_memfabric_mm_ar_functional.py
"""

from __future__ import annotations

import os
from concurrent.futures import ThreadPoolExecutor

import pytest

from tests.e2e._310p.memfabric_mm_ar_serve_lib import (
    ServeProcess,
    build_prompt_set,
    completions,
)

pytestmark = pytest.mark.skipif(
    os.getenv("MEMFABRIC_MM_AR_E2E_ENABLED", "0") != "1",
    reason="set MEMFABRIC_MM_AR_E2E_ENABLED=1 plus a 310P3 die pair to run",
)

EXPECTED_O_PROJ_LAYERS = 10
EXPECTED_OUT_PROJ_LAYERS = 30


def _collect_greedy(
    port: int,
    prompts: dict[str, str],
    short_max_tokens: int = 8,
    long_max_tokens: int = 8,
) -> dict[str, str]:
    outputs: dict[str, str] = {}
    for name, prompt in prompts.items():
        max_tokens = long_max_tokens if name.startswith("long") else short_max_tokens
        outputs[name] = completions(prompt, max_tokens=max_tokens, port=port)
    return outputs


def _collect_batched(port: int, prompts: list[str], max_tokens: int = 32, concurrency: int = 10) -> list[str]:
    with ThreadPoolExecutor(max_workers=concurrency) as pool:
        futures = [pool.submit(completions, p, max_tokens, port) for p in prompts]
        return [f.result() for f in futures]


def _run_combo(graph: bool, tmpdir: str) -> None:
    prompts = build_prompt_set()
    batched_prompts = [f"Count from one to twenty, step {i}: " for i in range(10)]

    stock_log = os.path.join(tmpdir, f"stock_{'graph' if graph else 'eager'}.log")
    fused_log = os.path.join(tmpdir, f"fused_{'graph' if graph else 'eager'}.log")

    with ServeProcess("stock", graph=graph, log_path=stock_log) as stock:
        stock_out = _collect_greedy(stock.port, prompts)
        stock_batched = _collect_batched(stock.port, batched_prompts)

    with ServeProcess("fused", graph=graph, log_path=fused_log) as fused:
        o_proj, out_proj = fused.count_enabled_layers()
        assert o_proj == EXPECTED_O_PROJ_LAYERS, (
            f"fused server enabled {o_proj} o_proj layers, expected {EXPECTED_O_PROJ_LAYERS}"
        )
        assert out_proj == EXPECTED_OUT_PROJ_LAYERS, (
            f"fused server enabled {out_proj} out_proj layers, expected {EXPECTED_OUT_PROJ_LAYERS}"
        )
        fused_out = _collect_greedy(fused.port, prompts)
        fused_batched = _collect_batched(fused.port, batched_prompts)

    mismatches = [name for name in prompts if stock_out[name] != fused_out[name]]
    assert not mismatches, (
        f"greedy output mismatch for {mismatches}: "
        f"stock={[stock_out[m] for m in mismatches]} "
        f"fused={[fused_out[m] for m in mismatches]}"
    )

    batched_mismatches = [i for i, (s, f) in enumerate(zip(stock_batched, fused_batched)) if s != f]
    assert not batched_mismatches, f"batched decode mismatch at indices {batched_mismatches}"


def test_fused_matches_stock_eager(tmp_path_factory: pytest.TempPathFactory) -> None:
    _run_combo(graph=False, tmpdir=str(tmp_path_factory.mktemp("mm_ar_eager")))


def test_fused_matches_stock_graph(tmp_path_factory: pytest.TempPathFactory) -> None:
    _run_combo(graph=True, tmpdir=str(tmp_path_factory.mktemp("mm_ar_graph")))
