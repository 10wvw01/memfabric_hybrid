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

"""Large-capacity vLLM integration tests (3GB HBM + 10GB DRAM, TP=4).

Mirrors vllm_integration_test.py's three 4-rank scenarios but runs MemCache
with large HBM + DRAM pools (device_sdma protocol) to verify the large-capacity
data path under full miss, 50% hit, and 100% hit patterns.

Test scenarios (4-rank only):

  LC-L3-1  Full miss baseline  — MemCache vs no-MemCache overhead
  LC-L3-2  50% hit rate        — half prompts pre-populated, mixed hit/miss
  LC-L3-3  100% hit rate       — prompt pre-populated, re-sent for full hit

Config: cases/config/mmc-local-largecap-4rank.conf
  - protocol = device_sdma (HBM + DRAM hybrid)
  - hbm.size  = 3GB per rank
  - dram.size = 10GB per rank

Requirements: Ascend NPU + CANN, vLLM-Ascend, MetaService, /dev/shm >= 50GB.

Run with:
  MMC_TEST_DEVICE_IDS=4,5,6,7 MMC_TEST_TENSOR_PARALLEL_SIZE=4 \\
      pytest -m rank4 large_capacity_vllm_test.py
"""

import multiprocessing
import os
import sys
import time

import pytest

from integration import vllm_integration_test as integration_cases
from integration.vllm_integration_test import (
    DEFAULT_DEVICE_IDS,
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
# Large-capacity config selection
# ---------------------------------------------------------------------------
_LARGE_CAP_CONFIG = os.path.join(
    os.path.dirname(__file__), "..", "..", "..", "config", "mmc-local-largecap-4rank.conf"
)


def _set_largecap_config_env():
    """Point MMC_LOCAL_CONFIG_PATH at the large-capacity (device_sdma) config."""
    root = _project_root()
    config_path = os.environ.get("MMC_LARGE_CAP_CONFIG_PATH") or os.path.abspath(_LARGE_CAP_CONFIG)
    if not os.path.exists(config_path):
        config_path = os.path.join(root, "config", "mmc-local-largecap-4rank.conf")
    os.environ["MMC_LOCAL_CONFIG_PATH"] = config_path
    os.environ.setdefault("MMC_META_CONFIG_PATH", os.path.join(root, "config", "mmc-meta.conf"))
    print(f"[INFO] Large-capacity config: {os.environ['MMC_LOCAL_CONFIG_PATH']}")


# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------
@pytest.fixture(scope="session")
def meta_service():
    if not _npu_available():
        pytest.skip("NPU environment not available")
    _set_largecap_config_env()
    # 容量与 integration-4rank 不同（3GB+10GB vs 2GB+2GB），重启 MetaService 以应用新配置。
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
            pytest.skip("MetaService failed to start for large-capacity tests")
        yield proc
        proc.terminate()
        proc.join(timeout=5)
    else:
        yield None


@pytest.fixture
def benchmark(meta_service):
    if not _vllm_available():
        pytest.skip("vLLM environment not available")
    # model path 为必填项，不设默认路径，避免误用不存在的路径。
    model_path = os.environ.get("MMC_TEST_VLLM_MODEL_PATH", "")
    if not model_path:
        print(f"[ERROR] MMC_TEST_VLLM_MODEL_PATH is not set, deviceIds="
              f"{os.environ.get('MMC_TEST_DEVICE_IDS', DEFAULT_DEVICE_IDS)}, "
              f"tpSize={os.environ.get('MMC_TEST_TENSOR_PARALLEL_SIZE', str(DEFAULT_TENSOR_PARALLEL_SIZE))}; "
              "set it to the W8A8 model path before running large-capacity tests", file=sys.stderr)
        pytest.skip("MMC_TEST_VLLM_MODEL_PATH not set; set it to the W8A8 model path")
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
    # Ensure every (re)start inside this fixture uses the large-capacity config.
    os.environ["MMC_LOCAL_CONFIG_PATH"] = os.path.abspath(_LARGE_CAP_CONFIG)
    yield bench
    bench.stop_vllm()


# Run the shared functional checks with the large-capacity fixture and configuration.


@pytest.mark.rank4
def test_large_cap_full_miss_baseline_4rank(benchmark):
    integration_cases.test_full_miss_baseline_4rank(benchmark)


@pytest.mark.rank4
def test_large_cap_50pct_hit_4rank(benchmark):
    integration_cases.test_50pct_hit_4rank(benchmark)


@pytest.mark.rank4
def test_large_cap_100pct_hit_4rank(benchmark):
    integration_cases.test_100pct_hit_4rank(benchmark)
