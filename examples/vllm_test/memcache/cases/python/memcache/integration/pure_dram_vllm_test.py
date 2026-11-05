#!/usr/bin/env python
# coding=utf-8
# Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
# MemCache_Hybrid is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#          http://license.coscl.org.cn/MulanPSL2
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE.
# See the Mulan PSL v2 for more details.

"""Pure-DRAM vLLM integration tests (10GB DRAM-only, no HBM, TP=4).

Mirrors vllm_integration_test.py's three 4-rank scenarios but runs MemCache
in host_shm mode — KV Cache goes directly to /dev/shm DRAM, bypassing HBM
entirely. This verifies the pure-DRAM data path without HBM fast-path or
HBM→DRAM eviction.

Test scenarios (4-rank only):

  PD-L3-1  Full miss baseline  — MemCache (pure-DRAM) vs no-MemCache overhead
  PD-L3-2  50% hit rate        — half prompts pre-populated, mixed hit/miss
  PD-L3-3  100% hit rate       — prompt pre-populated, re-sent for full hit

Config: cases/config/mmc-local-puredram-4rank.conf
  - protocol = host_shm (DRAM-only, no HBM)
  - dram.size = 10GB per rank
  - hbm.size  = 0

Requirements: Ascend NPU + CANN, vLLM-Ascend, MetaService, /dev/shm >= 50GB.

Run with:
  MMC_TEST_DEVICE_IDS=4,5,6,7 MMC_TEST_TENSOR_PARALLEL_SIZE=4 \\
      pytest -m rank4 pure_dram_vllm_test.py
"""

import multiprocessing
import os
import random
import time
from typing import List

import pytest

# Reuse VLLMBenchmark and helpers from the baseline integration test module.
# Both files live in the same package (integration/), so a package-relative
# import works under pytest's default sys.path insertion.
from integration.vllm_integration_test import (
    DEFAULT_DEVICE_IDS,
    DEFAULT_MODEL_PATH,
    DEFAULT_SERVED_MODEL_NAME,
    DEFAULT_TENSOR_PARALLEL_SIZE,
    DEFAULT_VLLM_PORT,
    HIT_TTFT_THRESHOLD_MS,
    MEMCACHE_OVERHEAD_RATIO,
    META_BOOTSTRAP_SECONDS,
    META_SERVICE_PORT,
    PREFIX_CACHE_HIT_RATIO,
    PROMPT_TOKENS,
    VLLMBenchmark,
    WARMUP_REQUESTS,
    MEASURED_REQUESTS,
    _build_prompt_ids,
    _is_meta_service_running,
    _kill_meta_service,
    _npu_available,
    _percentile,
    _project_root,
    _vllm_available,
)

# ---------------------------------------------------------------------------
# Pure-DRAM config selection
# ---------------------------------------------------------------------------
_PURE_DRAM_CONFIG = os.path.join(
    os.path.dirname(__file__), "..", "..", "..", "config", "mmc-local-puredram-4rank.conf"
)


def _set_puredram_config_env():
    """Point MMC_LOCAL_CONFIG_PATH at the pure-DRAM (host_shm) config."""
    root = _project_root()
    config_path = os.environ.get("MMC_PURE_DRAM_CONFIG_PATH") or os.path.abspath(_PURE_DRAM_CONFIG)
    if not os.path.exists(config_path):
        config_path = os.path.join(root, "config", "mmc-local-puredram-4rank.conf")
    os.environ["MMC_LOCAL_CONFIG_PATH"] = config_path
    os.environ.setdefault(
        "MMC_META_CONFIG_PATH", os.path.join(root, "config", "mmc-meta.conf")
    )
    print(f"[INFO] Pure-DRAM config: {os.environ['MMC_LOCAL_CONFIG_PATH']}")


# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------
@pytest.fixture(scope="session")
def meta_service():
    if not _npu_available():
        pytest.skip("NPU environment not available")
    _set_puredram_config_env()
    # If a MetaService is already running under a different protocol config
    # (e.g. device_sdma), restart it so it picks up host_shm for pure-DRAM.
    if _is_meta_service_running():
        _kill_meta_service()
        time.sleep(2)
    if not _is_meta_service_running():
        from memcache_hybrid import MetaService

        ctx = multiprocessing.get_context("fork")
        proc = ctx.Process(target=lambda: MetaService.main())
        proc.start()
        time.sleep(META_BOOTSTRAP_SECONDS)
        deadline = time.time() + 30
        while time.time() < deadline and not _is_meta_service_running():
            time.sleep(1)
        if not _is_meta_service_running():
            proc.terminate()
            proc.join(timeout=5)
            pytest.skip("MetaService failed to start for pure-DRAM tests")
        yield proc
        proc.terminate()
        proc.join(timeout=5)
    else:
        yield None


@pytest.fixture
def benchmark(meta_service):
    if not _vllm_available():
        pytest.skip("vLLM environment not available")
    model_path = os.environ.get("MMC_TEST_VLLM_MODEL_PATH", DEFAULT_MODEL_PATH)
    served_name = os.environ.get("MMC_TEST_VLLM_MODEL", DEFAULT_SERVED_MODEL_NAME)
    port = int(os.environ.get("MMC_TEST_VLLM_PORT", str(DEFAULT_VLLM_PORT)))
    device_ids = os.environ.get("MMC_TEST_DEVICE_IDS", DEFAULT_DEVICE_IDS)
    tp_size = int(os.environ.get("MMC_TEST_TENSOR_PARALLEL_SIZE", str(DEFAULT_TENSOR_PARALLEL_SIZE)))
    bench = VLLMBenchmark(
        model_path=model_path,
        served_model_name=served_name,
        port=port,
        device_ids=device_ids,
        tensor_parallel_size=tp_size,
    )
    # Ensure every (re)start inside this fixture uses the pure-DRAM config.
    os.environ["MMC_LOCAL_CONFIG_PATH"] = os.path.abspath(_PURE_DRAM_CONFIG)
    yield bench
    bench.stop_vllm()


# ---------------------------------------------------------------------------
# PD-L3-1: Full miss baseline — MemCache (pure-DRAM) vs no-MemCache overhead
# ---------------------------------------------------------------------------
@pytest.mark.rank4
def test_puredram_full_miss_baseline_4rank(benchmark):
    """4-rank (TP=4): Full miss baseline with pure-DRAM MemCache overhead comparison."""
    baseline_result = None
    memcache_result = None

    try:
        print("\n===== [PureDRAM] Phase 1: Baseline vLLM (no MemCache) =====")
        benchmark.start_vllm_baseline()
        warmup = [_build_prompt_ids(PROMPT_TOKENS, seed=-(i + 1)) for i in range(WARMUP_REQUESTS)]
        measure = [_build_prompt_ids(PROMPT_TOKENS, seed=i) for i in range(MEASURED_REQUESTS)]
        baseline_result = benchmark.measure_ttft_batch(measure, warmup_prompts=warmup)
        print(f"Baseline: {baseline_result}")
        benchmark.print_cache_stats("PureDRAM Baseline")
    finally:
        benchmark.stop_vllm()

    try:
        print("\n===== [PureDRAM] Phase 2: vLLM with MemCache (10GB DRAM-only) =====")
        benchmark.start_vllm_with_memcache()
        warmup = [_build_prompt_ids(PROMPT_TOKENS, seed=-(i + 1)) for i in range(WARMUP_REQUESTS)]
        measure = [_build_prompt_ids(PROMPT_TOKENS, seed=i) for i in range(MEASURED_REQUESTS)]
        memcache_result = benchmark.measure_ttft_batch(measure, warmup_prompts=warmup)
        print(f"MemCache: {memcache_result}")
        benchmark.print_cache_stats("PureDRAM MemCache")
    finally:
        benchmark.stop_vllm()

    assert baseline_result is not None, "baseline benchmark failed"
    assert memcache_result is not None, "memcache benchmark failed"

    baseline_p50 = baseline_result["p50_ms"]
    memcache_p50 = memcache_result["p50_ms"]
    overhead = (memcache_p50 - baseline_p50) / baseline_p50 if baseline_p50 > 0 else 1.0

    print(
        f"\n===== [PureDRAM] Full Miss Baseline Comparison =====\n"
        f"  Baseline P50={baseline_p50:.2f}ms P99={baseline_result['p99_ms']:.2f}ms\n"
        f"  MemCache P50={memcache_p50:.2f}ms P99={memcache_result['p99_ms']:.2f}ms\n"
        f"  Overhead={overhead:.2%} (threshold: {MEMCACHE_OVERHEAD_RATIO:.0%})"
    )

    assert overhead < MEMCACHE_OVERHEAD_RATIO, (
        f"Pure-DRAM MemCache E2E TTFT overhead {overhead:.2%} exceeds "
        f"{MEMCACHE_OVERHEAD_RATIO:.0%} threshold"
    )


# ---------------------------------------------------------------------------
# PD-L3-2: 50% hit rate — half prompts pre-populated, mixed hit/miss
# ---------------------------------------------------------------------------
@pytest.mark.rank4
def test_puredram_50pct_hit_4rank(benchmark):
    """4-rank (TP=4): 50% cache hit rate with pure-DRAM MemCache."""
    try:
        print("\n===== [PureDRAM] Starting vLLM with MemCache for 50% hit test =====")
        benchmark.start_vllm_with_memcache()

        warmup_prompt = _build_prompt_ids(PROMPT_TOKENS, seed=99)
        for i in range(WARMUP_REQUESTS):
            benchmark.measure_e2e_ttft_ms(warmup_prompt)
            print(f"  warmup {i + 1}/{WARMUP_REQUESTS} done")

        num_requests = MEASURED_REQUESTS
        hit_count = round(0.5 * num_requests)
        miss_count = num_requests - hit_count

        hit_seeds = [5000 + i for i in range(hit_count)]
        for i, seed in enumerate(hit_seeds):
            prompt = _build_prompt_ids(PROMPT_TOKENS, seed=seed)
            benchmark.measure_e2e_ttft_ms(prompt)
            print(f"  pre-populate {i + 1}/{hit_count} (seed={seed}) done")

        time.sleep(3)

        miss_seeds = [6000 + i for i in range(miss_count)]
        requests = []
        for seed in hit_seeds:
            requests.append((_build_prompt_ids(PROMPT_TOKENS, seed=seed), True))
        for seed in miss_seeds:
            requests.append((_build_prompt_ids(PROMPT_TOKENS, seed=seed), False))

        rng = random.Random(42)
        rng.shuffle(requests)

        all_latencies: List[float] = []
        hit_latencies: List[float] = []
        miss_latencies: List[float] = []
        for i, (prompt, is_expected_hit) in enumerate(requests):
            ttft = benchmark.measure_e2e_ttft_ms(prompt)
            all_latencies.append(ttft)
            if is_expected_hit:
                hit_latencies.append(ttft)
            else:
                miss_latencies.append(ttft)
            label = "hit " if is_expected_hit else "miss"
            print(f"  measured {i + 1}/{num_requests}: TTFT={ttft:.2f}ms ({label})")

        all_avg = sum(all_latencies) / len(all_latencies)
        all_p50 = _percentile(all_latencies, 0.50)
        all_p99 = _percentile(all_latencies, 0.99)
        hit_avg = sum(hit_latencies) / len(hit_latencies) if hit_latencies else 0.0
        hit_p50 = _percentile(hit_latencies, 0.50) if hit_latencies else 0.0
        miss_avg = sum(miss_latencies) / len(miss_latencies) if miss_latencies else 0.0
        miss_p50 = _percentile(miss_latencies, 0.50) if miss_latencies else 0.0

        actual_hit_count = sum(1 for t in hit_latencies if t < HIT_TTFT_THRESHOLD_MS)
        improvement = (miss_avg - all_avg) / miss_avg if miss_avg > 0 else 0.0

        print(
            f"\n===== [PureDRAM] 50% Hit Rate E2E TTFT =====\n"
            f"  Expected hit/total: {hit_count}/{num_requests} (50%)\n"
            f"  Actual hit (TTFT<{HIT_TTFT_THRESHOLD_MS:.0f}ms): {actual_hit_count}/{hit_count}\n"
            f"  All   avg={all_avg:.2f}ms P50={all_p50:.2f}ms P99={all_p99:.2f}ms\n"
            f"  Hit   avg={hit_avg:.2f}ms P50={hit_p50:.2f}ms (count={len(hit_latencies)})\n"
            f"  Miss  avg={miss_avg:.2f}ms P50={miss_p50:.2f}ms (count={len(miss_latencies)})\n"
            f"  Improvement (avg): {improvement:.2%}"
        )

        benchmark.print_cache_stats("PureDRAM 50% Hit")

        assert actual_hit_count >= hit_count * 0.5, (
            f"Only {actual_hit_count}/{hit_count} expected hits had "
            f"TTFT < {HIT_TTFT_THRESHOLD_MS:.0f}ms"
        )
        assert hit_p50 < miss_p50 * PREFIX_CACHE_HIT_RATIO, (
            f"Hit P50 {hit_p50:.2f}ms >= miss P50 * {PREFIX_CACHE_HIT_RATIO} "
            f"({miss_p50 * PREFIX_CACHE_HIT_RATIO:.2f}ms)"
        )
    finally:
        benchmark.stop_vllm()


# ---------------------------------------------------------------------------
# PD-L3-3: 100% hit rate — prompt pre-populated, re-sent for full hit
# ---------------------------------------------------------------------------
@pytest.mark.rank4
def test_puredram_100pct_hit_4rank(benchmark):
    """4-rank (TP=4): 100% cache hit rate with pure-DRAM MemCache."""
    try:
        print("\n===== [PureDRAM] Starting vLLM with MemCache for 100% hit test =====")
        benchmark.start_vllm_with_memcache()

        warmup_prompt = _build_prompt_ids(PROMPT_TOKENS, seed=99)
        for i in range(WARMUP_REQUESTS):
            benchmark.measure_e2e_ttft_ms(warmup_prompt)
            print(f"  warmup {i + 1}/{WARMUP_REQUESTS} done")

        miss_latencies: List[float] = []
        hit_latencies: List[float] = []
        for i in range(MEASURED_REQUESTS):
            miss_prompt = _build_prompt_ids(PROMPT_TOKENS, seed=1000 + i)
            miss_ttft = benchmark.measure_e2e_ttft_ms(miss_prompt)
            miss_latencies.append(miss_ttft)

            time.sleep(1)

            hit_ttft = benchmark.measure_e2e_ttft_ms(miss_prompt)
            hit_latencies.append(hit_ttft)

            print(f"  pair {i + 1}/{MEASURED_REQUESTS}: miss={miss_ttft:.2f}ms, hit={hit_ttft:.2f}ms")

        miss_p50 = _percentile(miss_latencies, 0.50)
        hit_p50 = _percentile(hit_latencies, 0.50)
        miss_p99 = _percentile(miss_latencies, 0.99)
        hit_p99 = _percentile(hit_latencies, 0.99)
        ratio = hit_p50 / miss_p50 if miss_p50 > 0 else 1.0

        print(
            f"\n===== [PureDRAM] 100% Hit Rate E2E TTFT =====\n"
            f"  Cache miss P50={miss_p50:.2f}ms P99={miss_p99:.2f}ms\n"
            f"  Cache hit  P50={hit_p50:.2f}ms P99={hit_p99:.2f}ms\n"
            f"  Hit/Miss ratio={ratio:.2%} (threshold: < {PREFIX_CACHE_HIT_RATIO:.0%})"
        )

        benchmark.print_cache_stats("PureDRAM 100% Hit")

        assert hit_p50 < miss_p50 * PREFIX_CACHE_HIT_RATIO, (
            f"Cache hit P50 {hit_p50:.2f}ms >= miss P50 * {PREFIX_CACHE_HIT_RATIO} "
            f"({miss_p50 * PREFIX_CACHE_HIT_RATIO:.2f}ms)"
        )
    finally:
        benchmark.stop_vllm()
