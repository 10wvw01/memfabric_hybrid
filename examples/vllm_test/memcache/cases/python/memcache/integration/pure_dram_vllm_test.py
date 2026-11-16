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
import time

import pytest

from integration import vllm_integration_test as integration_cases

# Reuse VLLMBenchmark and helpers from the baseline integration test module.
# Both files live in the same package (integration/), so a package-relative
# import works under pytest's default sys.path insertion.
from integration.vllm_integration_test import (
    DEFAULT_DEVICE_IDS,
    DEFAULT_MODEL_PATH,
    DEFAULT_SERVED_MODEL_NAME,
    DEFAULT_TENSOR_PARALLEL_SIZE,
    DEFAULT_VLLM_PORT,
    META_BOOTSTRAP_SECONDS,
    VLLMBenchmark,
    _is_meta_service_running,
    _kill_meta_service,
    _npu_available,
    _project_root,
    _vllm_available,
)

# ---------------------------------------------------------------------------
# Pure-DRAM config selection
# ---------------------------------------------------------------------------
_PURE_DRAM_CONFIG = os.path.join(os.path.dirname(__file__), "..", "..", "..", "config", "mmc-local-puredram-4rank.conf")


def _set_puredram_config_env():
    """Point MMC_LOCAL_CONFIG_PATH at the pure-DRAM (host_shm) config."""
    root = _project_root()
    config_path = os.environ.get("MMC_PURE_DRAM_CONFIG_PATH") or os.path.abspath(_PURE_DRAM_CONFIG)
    if not os.path.exists(config_path):
        config_path = os.path.join(root, "config", "mmc-local-puredram-4rank.conf")
    os.environ["MMC_LOCAL_CONFIG_PATH"] = config_path
    os.environ.setdefault("MMC_META_CONFIG_PATH", os.path.join(root, "config", "mmc-meta.conf"))
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


# Run the shared functional checks with the pure-DRAM fixture and configuration.


@pytest.mark.rank4
def test_puredram_full_miss_baseline_4rank(benchmark):
    integration_cases.test_full_miss_baseline_4rank(benchmark)


@pytest.mark.rank4
def test_puredram_50pct_hit_4rank(benchmark):
    integration_cases.test_50pct_hit_4rank(benchmark)


@pytest.mark.rank4
def test_puredram_100pct_hit_4rank(benchmark):
    integration_cases.test_100pct_hit_4rank(benchmark)
