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

"""Large-capacity vLLM integration tests (TP=4).

Tests how configured MemCache HBM and DRAM pools affect
vLLM end-to-end TTFT under various cache-hit patterns. The large capacity
allows more KV cache entries to be retained, reducing eviction and improving
cache hit rates compared to the 1GB+1GB baseline.

Test scenarios:

  LC-L3-1  Large-capacity overhead      — MemCache vs Baseline TTFT (8GB+20GB)
  LC-L3-2  Large-scale cache hit         — 40-key pre-populate, then 100% hit
  LC-L3-3  Eviction-aware mixed workload — fill a 1GiB/rank HBM pool until DRAM residency is verified,
                                             then 30% / 70% hit rate mix
  LC-L3-4  Throughput bench              — sustained load with large cache

Cache Isolation Methodology
---------------------------
vLLM has its own GPU-resident block-level prefix cache (``--enable-prefix-
caching``). When MemCache is enabled (``--kv-transfer-config``), both caches
operate simultaneously: vLLM checks local cache first, and only falls through
to the MemCache connector if local cache misses.

To isolate MemCache's contribution in hit/miss tests (LC-L3-2, LC-L3-3), the
test calls ``POST /reset_prefix_cache`` (vLLM dev API, enabled via
``VLLM_SERVER_DEV_MODE=1``) to clear vLLM's local block hash → block mapping
*without* touching MemCache data. This forces subsequent requests to go
through the MemCache connector for cache lookups.

After each test scenario, cache statistics are printed showing both vLLM
prefix cache hit rate and MemCache (external) prefix cache hit rate, parsed
from vLLM's periodic engine metrics log.

Requires: Ascend NPU + CANN, vLLM-Ascend and an isolated MetaService.
"""

import json
import multiprocessing
import os
import random
import re
import socket
import subprocess
import time
from typing import Any, Dict, List, Optional

import pytest

from integration import eviction_checks
from integration.vllm_integration_test import VLLMBenchmark as BaseVLLMBenchmark

# ---------------------------------------------------------------------------
# Test parameters
# ---------------------------------------------------------------------------
PROMPT_TOKENS = 4096
MAX_TOKENS_TTFT = 1
WARMUP_REQUESTS = 2
MEASURED_REQUESTS = 20
VLLM_READY_TIMEOUT_S = 600
VLLM_STOP_TIMEOUT_S = 60
TTFT_REQUEST_TIMEOUT_S = 120


LARGE_PREPOPULATE_COUNT = 40
LARGE_PREPOP_SEED_BASE = 7000
LARGE_MISS_SEED_BASE = 9000


EVICT_HIT_RATE = 0.3
EVICT_HIGH_HIT_RATE = 0.7
EVICT_PREPOP_COUNT = 64

VOCAB_SIZE = 151936

DEFAULT_MODEL_PATH = "/home/models/Qwen3.6-35B-A3B-w8a8"
DEFAULT_SERVED_MODEL_NAME = "Qwen3.6-35B-A3B"
DEFAULT_VLLM_PORT = 4077
DEFAULT_DEVICE_IDS = "4,5,6,7"
DEFAULT_TENSOR_PARALLEL_SIZE = 4
DEFAULT_GPU_MEM_UTIL = "0.70"
META_SERVICE_PORT = 5000
META_BOOTSTRAP_SECONDS = 3

ADDITIONAL_CONFIG_JSON = (
    '{"enable_flashcomm1":false,'
    '"enable_flashcomm2_parallel_size":0,'
    '"enable_prefill_mc2":false,'
    '"enable_fused_mc2":0,'
    '"ascend_compilation_config":'
    '{"fuse_norm_quant":false,"enable_npugraph_ex":false}}'
)

KV_TRANSFER_CONFIG_MEMCACHE = json.dumps(
    {
        "kv_connector": "AscendStoreConnector",
        "kv_role": "kv_both",
        "kv_connector_extra_config": {"backend": "memcache"},
    }
)


# ---------------------------------------------------------------------------
# Helpers (shared with vllm_integration_test.py)
# ---------------------------------------------------------------------------
def _npu_available() -> bool:
    try:
        import acl  # noqa: F401
        import torch_npu  # noqa: F401

        return True
    except ImportError:
        return False


def _vllm_available() -> bool:
    try:
        import vllm  # noqa: F401

        return True
    except ImportError:
        return False


def _kill_meta_service() -> Optional[int]:
    try:
        result = subprocess.run(
            "pgrep -f 'mmc_meta_service\\|MetaService.main'",
            shell=True,
            capture_output=True,
            text=True,
            timeout=5,
        )
        for line in result.stdout.strip().split():
            pid = int(line.strip())
            try:
                os.kill(pid, 9)
                print(f"[INFO] Killed MetaService PID {pid}")
            except (ProcessLookupError, PermissionError):
                pass
    except (subprocess.TimeoutExpired, ValueError):
        pass
    return None


def _start_meta_service():
    root = _project_root()
    meta_bin = os.path.join(root, "output", "memcache", "bin", "mmc_meta_service")
    env = os.environ.copy()
    lib_dirs = []
    try:
        import memcache_hybrid
        import memfabric_hybrid

        for pkg in (memcache_hybrid, memfabric_hybrid):
            pkg_lib = os.path.join(os.path.dirname(pkg.__file__), "lib")
            if os.path.isdir(pkg_lib):
                lib_dirs.append(pkg_lib)
    except ImportError:
        pass
    output_lib = os.path.join(root, "output", "memcache", "lib64")
    if os.path.isdir(output_lib):
        lib_dirs.append(output_lib)
    if lib_dirs:
        env["LD_LIBRARY_PATH"] = ":".join(lib_dirs) + ":" + env.get("LD_LIBRARY_PATH", "")
    env["MMC_META_CONFIG_PATH"] = os.path.join(root, "config", "mmc-meta.conf")
    subprocess.Popen(
        [meta_bin],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        env=env,
        start_new_session=True,
    )
    print("[INFO] Started fresh MetaService")


def _is_meta_service_running(host: str = "127.0.0.1", port: int = META_SERVICE_PORT) -> bool:
    try:
        with socket.create_connection((host, port), timeout=1):
            return True
    except (OSError, ConnectionRefusedError):
        return False


def _project_root() -> str:
    env_root = os.environ.get("MMC_MEMCACHE_DIR")
    if env_root and os.path.isdir(env_root):
        return env_root
    p = os.path.abspath(__file__)
    for _ in range(5):
        p = os.path.dirname(p)
    return p


def _set_config_env():
    root = _project_root()
    test_config = os.path.join(os.path.dirname(__file__), "..", "..", "config", "mmc-local-largecap-4rank.conf")
    if not os.path.exists(test_config):
        test_config = os.path.join(root, "config", "mmc-local.conf")
    os.environ.setdefault("MMC_LOCAL_CONFIG_PATH", os.path.abspath(test_config))
    os.environ.setdefault("MMC_META_CONFIG_PATH", os.path.join(root, "config", "mmc-meta.conf"))


def _percentile(values: List[float], pct: float) -> float:
    if not values:
        return 0.0
    sorted_values = sorted(values)
    n = len(sorted_values)
    rank = pct * (n - 1)
    lower = int(rank)
    upper = min(lower + 1, n - 1)
    fraction = rank - lower
    return sorted_values[lower] * (1 - fraction) + sorted_values[upper] * fraction


def _build_prompt_ids(num_tokens: int, seed: Optional[int] = None) -> List[int]:
    if seed is not None:
        rng = random.Random(seed)
        return [rng.randrange(VOCAB_SIZE) for _ in range(num_tokens)]
    return [1] * num_tokens


# ---------------------------------------------------------------------------
# VLLMTTFTBenchmark (large-capacity variant)
# ---------------------------------------------------------------------------
class VLLMTTFTBenchmark(BaseVLLMBenchmark):
    def stop_vllm(self):
        had_process = self._process is not None
        super().stop_vllm()
        if had_process:
            _kill_meta_service()
            _start_meta_service()

    def stop_vllm_preserve_memcache(self):
        super().stop_vllm()

    def restart_vllm_preserve_memcache(self):
        self.stop_vllm_preserve_memcache()
        time.sleep(3)

    def measure_e2e_ttft_ms(
        self, prompt_ids: List[int], max_tokens: int = MAX_TOKENS_TTFT, reset_local: bool = True
    ) -> float:
        return super().measure_e2e_ttft_ms(prompt_ids, max_tokens=max_tokens, reset_local=reset_local)

    def get_cache_stats(self) -> Dict[str, Any]:
        """Return cache hit statistics from the vLLM engine metrics log.

        Parses the last 'Prefix cache hit rate' / 'External prefix cache
        hit rate' lines from the vLLM log file.

        Returns:
            dict with keys: local_hit_rate, external_hit_rate, gpu_kv_usage,
            raw_line
        """
        stats: Dict[str, Any] = {
            "local_hit_rate": None,
            "external_hit_rate": None,
            "gpu_kv_usage": None,
            "raw_line": "",
        }
        log_path = getattr(self, "_log_path", None)
        if not log_path or not os.path.exists(log_path):
            return stats
        try:
            with open(log_path, "r") as f:
                lines = f.readlines()
        except OSError:
            return stats
        for line in reversed(lines):
            if "Prefix cache hit rate:" in line and "External prefix cache hit rate:" in line:
                stats["raw_line"] = line.strip()
                m_local = re.search(r"Prefix cache hit rate:\s*([\d.]+)%", line)
                m_ext = re.search(r"External prefix cache hit rate:\s*([\d.]+)%", line)
                m_kv = re.search(r"GPU KV cache usage:\s*([\d.]+)%", line)
                if m_local:
                    stats["local_hit_rate"] = float(m_local.group(1))
                if m_ext:
                    stats["external_hit_rate"] = float(m_ext.group(1))
                if m_kv:
                    stats["gpu_kv_usage"] = float(m_kv.group(1))
                break
        return stats

    def print_cache_stats(self, label: str = ""):
        """Print cache hit statistics with an optional label."""
        stats = self.get_cache_stats()
        local = stats["local_hit_rate"]
        ext = stats["external_hit_rate"]
        kv = stats["gpu_kv_usage"]
        local_s = f"{local:.1f}%" if local is not None else "N/A"
        ext_s = f"{ext:.1f}%" if ext is not None else "N/A"
        kv_s = f"{kv:.1f}%" if kv is not None else "N/A"
        prefix = f"  [{label}] " if label else "  "
        print(f"{prefix}Cache stats: vLLM local hit={local_s}, MemCache external hit={ext_s}, GPU KV usage={kv_s}")

    def measure_ttft_batch(
        self,
        num_requests=MEASURED_REQUESTS,
        prompt_tokens=PROMPT_TOKENS,
        warmup=WARMUP_REQUESTS,
        use_unique_prompts=True,
    ) -> Dict[str, float]:
        if use_unique_prompts:
            warmup_prompts = [_build_prompt_ids(prompt_tokens, seed=10_000_000 + i) for i in range(warmup)]
            measure_prompts = [_build_prompt_ids(prompt_tokens, seed=i) for i in range(num_requests)]
        else:
            shared_prompt = _build_prompt_ids(prompt_tokens)
            warmup_prompts = [shared_prompt] * warmup
            measure_prompts = [shared_prompt] * num_requests
        for i, prompt in enumerate(warmup_prompts):
            self.measure_e2e_ttft_ms(prompt)
            print(f"  warmup {i + 1}/{warmup} done")
        latencies: List[float] = []
        for i, prompt in enumerate(measure_prompts):
            ttft = (
                self.measure_request_with_cache_source(prompt, False)
                if use_unique_prompts
                else self.measure_e2e_ttft_ms(prompt)
            )
            latencies.append(ttft)
            print(f"  measured {i + 1}/{num_requests}: TTFT={ttft:.2f}ms")
        return {
            "p50_ms": _percentile(latencies, 0.50),
            "p90_ms": _percentile(latencies, 0.90),
            "p99_ms": _percentile(latencies, 0.99),
            "avg_ms": sum(latencies) / len(latencies) if latencies else 0.0,
            "min_ms": min(latencies) if latencies else 0.0,
            "max_ms": max(latencies) if latencies else 0.0,
            "count": len(latencies),
        }


# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------
@pytest.fixture(scope="session")
def meta_service():
    if not _npu_available():
        pytest.skip("NPU environment not available")
    _set_config_env()
    if not _is_meta_service_running():
        from memcache_hybrid import MetaService

        ctx = multiprocessing.get_context("fork")
        proc = ctx.Process(target=lambda: MetaService.main())
        proc.start()
        time.sleep(META_BOOTSTRAP_SECONDS)
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
    bench = VLLMTTFTBenchmark(
        model_path=model_path,
        served_model_name=served_name,
        port=port,
        device_ids=device_ids,
        tensor_parallel_size=tp_size,
    )
    yield bench
    bench.stop_vllm()


# ---------------------------------------------------------------------------
# LC-L3-1: Large-capacity overhead — MemCache vs Baseline (8GB HBM + 20GB DRAM)
# ---------------------------------------------------------------------------
@pytest.mark.rank4
def test_large_cap_overhead_4rank(benchmark):
    baseline_result = None
    memcache_result = None

    try:
        print("\n===== [LargeCap] Starting baseline vLLM (no MemCache) =====")
        benchmark.start_vllm_baseline()
        baseline_result = benchmark.measure_ttft_batch(
            num_requests=MEASURED_REQUESTS,
            prompt_tokens=PROMPT_TOKENS,
            warmup=WARMUP_REQUESTS,
            use_unique_prompts=True,
        )
        print(f"Baseline: {baseline_result}")
        benchmark.print_cache_stats("LargeCap Baseline")
    finally:
        benchmark.stop_vllm()

    try:
        print("\n===== [LargeCap] Starting vLLM with MemCache (8GB HBM + 20GB DRAM) =====")
        benchmark.start_vllm_with_memcache()
        memcache_result = benchmark.measure_ttft_batch(
            num_requests=MEASURED_REQUESTS,
            prompt_tokens=PROMPT_TOKENS,
            warmup=WARMUP_REQUESTS,
            use_unique_prompts=True,
        )
        print(f"MemCache: {memcache_result}")
        benchmark.print_cache_stats("LargeCap MemCache")
    finally:
        benchmark.stop_vllm()

    assert baseline_result is not None, "baseline benchmark failed"
    assert memcache_result is not None, "memcache benchmark failed"

    baseline_p50 = baseline_result["p50_ms"]
    memcache_p50 = memcache_result["p50_ms"]
    overhead = (memcache_p50 - baseline_p50) / baseline_p50 if baseline_p50 > 0 else 1.0

    print(
        f"\n===== [LargeCap] TTFT Overhead Comparison =====\n"
        f"  Baseline P50={baseline_p50:.2f}ms P99={baseline_result['p99_ms']:.2f}ms\n"
        f"  MemCache P50={memcache_p50:.2f}ms P99={memcache_result['p99_ms']:.2f}ms\n"
        f"  Overhead={overhead:.2%}"
    )


# ---------------------------------------------------------------------------
# LC-L3-2: Large-scale cache hit — 40-key pre-populate, then 100% hit
# ---------------------------------------------------------------------------
@pytest.mark.rank4
def test_large_scale_cache_hit_4rank(benchmark):
    try:
        print(f"\n===== [LargeCap] Large-scale cache hit (pre-populate {LARGE_PREPOPULATE_COUNT}) =====")
        benchmark.start_vllm_with_memcache()

        warmup_prompt = _build_prompt_ids(PROMPT_TOKENS, seed=99)
        for i in range(WARMUP_REQUESTS):
            benchmark.measure_e2e_ttft_ms(warmup_prompt)
            print(f"  warmup {i + 1}/{WARMUP_REQUESTS} done")

        hit_seeds = [LARGE_PREPOP_SEED_BASE + i for i in range(LARGE_PREPOPULATE_COUNT)]

        print(f"  Pre-populating {LARGE_PREPOPULATE_COUNT} unique prompts (miss→hit pairs)...")
        miss_latencies: List[float] = []
        hit_latencies: List[float] = []
        for i in range(MEASURED_REQUESTS):
            seed = hit_seeds[i % LARGE_PREPOPULATE_COUNT]
            prompt = _build_prompt_ids(PROMPT_TOKENS, seed=seed)
            miss_ttft = benchmark.measure_request_with_cache_source(prompt, False)
            miss_latencies.append(miss_ttft)
            time.sleep(1)
            hit_ttft = benchmark.measure_request_with_cache_source(prompt, True)
            hit_latencies.append(hit_ttft)
            print(f"  pair {i + 1}/{MEASURED_REQUESTS}: miss={miss_ttft:.2f}ms, hit={hit_ttft:.2f}ms (seed={seed})")

        miss_p50 = _percentile(miss_latencies, 0.50)
        miss_p99 = _percentile(miss_latencies, 0.99)
        hit_p50 = _percentile(hit_latencies, 0.50)
        hit_p99 = _percentile(hit_latencies, 0.99)
        hit_avg = sum(hit_latencies) / len(hit_latencies) if hit_latencies else 0.0

        actual_hits = len(hit_latencies)
        ratio = hit_p50 / miss_p50 if miss_p50 > 0 else 1.0

        print(
            f"\n===== [LargeCap] Large-scale cache hit result =====\n"
            f"  Pre-populated: {LARGE_PREPOPULATE_COUNT} keys\n"
            f"  Cache miss P50={miss_p50:.2f}ms P99={miss_p99:.2f}ms\n"
            f"  Cache hit  P50={hit_p50:.2f}ms P99={hit_p99:.2f}ms avg={hit_avg:.2f}ms\n"
            f"  Hit/Miss ratio={ratio:.2%}\n"
            f"  Verified external hits: {actual_hits}/{MEASURED_REQUESTS}"
        )

        benchmark.print_cache_stats("LC-L3-2 Cache Hit")

    finally:
        benchmark.stop_vllm()


def _prefill_for_eviction(benchmark):
    initial = eviction_checks.meta_summary()
    assert not eviction_checks.all_keys(), "Eviction scenario requires an empty MemCache"
    samples = []
    deadline = time.monotonic() + 600
    for index in range(EVICT_PREPOP_COUNT):
        assert time.monotonic() < deadline, f"Eviction prefill timed out: {eviction_checks.meta_summary()}"
        seed = LARGE_PREPOP_SEED_BASE + 1000 + index
        prompt = eviction_checks.eviction_prompt(benchmark._model_path, seed, PROMPT_TOKENS)
        before = eviction_checks.all_keys()
        _, tokens = eviction_checks.checked_completion(benchmark, prompt, False)
        keys = eviction_checks.wait_saved_keys(before, benchmark._tensor_parallel_size)
        if index == 0:
            _, hbm_tokens = eviction_checks.checked_completion(benchmark, prompt, True)
            assert hbm_tokens == tokens, f"Unstable HBM reference: expected={tokens}, actual={hbm_tokens}"
        samples.append({"prompt": prompt, "tokens": tokens, "keys": keys, "seed": seed})
        summary = eviction_checks.meta_summary()
        print(f"  pre-populate {index + 1}/{EVICT_PREPOP_COUNT}: {summary}")
        if summary["evict"] > initial["evict"] and summary["dram_used"] > 0:
            candidates = eviction_checks.dram_candidates(samples, benchmark._tensor_parallel_size)
            if candidates:
                return candidates, len(samples), summary
    candidates = eviction_checks.wait_dram_candidates(samples, benchmark._tensor_parallel_size, initial["evict"])
    return candidates, len(samples), eviction_checks.meta_summary()


def _measure_evicted_mixed(benchmark, candidates, hit_count):
    requests = [(candidates[index % len(candidates)], True) for index in range(hit_count)]
    requests += [
        (
            {
                "prompt": eviction_checks.eviction_prompt(
                    benchmark._model_path, LARGE_MISS_SEED_BASE + index, PROMPT_TOKENS
                )
            },
            False,
        )
        for index in range(MEASURED_REQUESTS - hit_count)
    ]
    random.Random(42).shuffle(requests)
    latencies, verified_hits = [], 0
    for index, (sample, expected_hit) in enumerate(requests):
        if expected_hit:
            metadata = eviction_checks.query_keys(sample["keys"])
            assert eviction_checks.readable(metadata, benchmark._tensor_parallel_size, "DRAM"), metadata
        ttft, tokens = eviction_checks.checked_completion(benchmark, sample["prompt"], expected_hit)
        if expected_hit:
            assert tokens == sample["tokens"], (
                f"DRAM hit output differs from miss reference: seed={sample.get('seed')}, "
                f"expected={sample['tokens']}, actual={tokens}"
            )
            verified_hits += 1
        latencies.append(ttft)
        print(f"  measured {index + 1}/{MEASURED_REQUESTS}: TTFT={ttft:.2f}ms, external={expected_hit}")
    assert verified_hits == hit_count
    return latencies, verified_hits


@pytest.mark.rank4
@pytest.mark.parametrize(
    "hit_rate",
    [EVICT_HIT_RATE, EVICT_HIGH_HIT_RATE],
    ids=[f"{int(rate * 100)}pct" for rate in [EVICT_HIT_RATE, EVICT_HIGH_HIT_RATE]],
)
def test_eviction_aware_mixed_4rank(benchmark, tmp_path, monkeypatch, record_property, hit_rate):
    assert benchmark._tensor_parallel_size == 4, "Eviction scenario requires TP=4"
    config_path = os.environ["MMC_LOCAL_CONFIG_PATH"]
    with open(config_path) as source:
        config, count = re.subn(
            r"(?m)^ock\.mmc\.local_service\.hbm\.size\s*=.*$",
            "ock.mmc.local_service.hbm.size = 1GB",
            source.read(),
        )
    assert count == 1, f"Expected one HBM pool size in {config_path}"
    eviction_config = tmp_path / "mmc-local-eviction-4rank.conf"
    eviction_config.write_text(config)
    monkeypatch.setenv("MMC_LOCAL_CONFIG_PATH", str(eviction_config))
    try:
        benchmark.start_vllm_with_memcache()
        candidates, prefilled, summary = _prefill_for_eviction(benchmark)
        print(f"  DRAM eviction verified: {summary}, complete prompts={len(candidates)}")
        hit_count = round(hit_rate * MEASURED_REQUESTS)
        record_property("eviction_summary", json.dumps(summary))
        record_property("dram_prompts", len(candidates))
        record_property("prefilled_requests", prefilled)
        latencies, verified_hits = _measure_evicted_mixed(benchmark, candidates, hit_count)
        record_property("verified_external_hits", verified_hits)
        print(
            f"\n===== [LargeCap] Verified DRAM mixed ({hit_rate:.0%}) =====\n"
            f"  Pre-populated requests: {prefilled}, DRAM prompts: {len(candidates)}\n"
            f"  Verified external hits and matching output: {verified_hits}/{MEASURED_REQUESTS}\n"
            f"  Average TTFT={sum(latencies) / len(latencies):.2f}ms"
        )
    finally:
        benchmark.stop_vllm()


# ---------------------------------------------------------------------------
# LC-L3-4: Throughput bench — sustained load with large cache
# ---------------------------------------------------------------------------
BENCH_INPUT_LEN = 4096
BENCH_OUTPUT_LEN = 128
BENCH_NUM_PROMPTS = 100
BENCH_REQUEST_RATE = 10
BENCH_NUM_WARMUPS = 10
BENCH_RESULT_DIR = "/tmp/vllm_largecap_bench_results"

pytest_marker_throughput = pytest.mark.throughput


def _build_bench_cmd(port, served_name, input_len, output_len, num_prompts, request_rate, num_warmups, mode_label):
    return [
        "vllm",
        "bench",
        "serve",
        "--backend",
        "vllm",
        "--host",
        "127.0.0.1",
        "--port",
        str(port),
        "--model",
        served_name,
        "--dataset-name",
        "random",
        "--input-len",
        str(input_len),
        "--output-len",
        str(output_len),
        "--num-prompts",
        str(num_prompts),
        "--request-rate",
        str(request_rate),
        "--num-warmups",
        str(num_warmups),
        "--seed",
        "42",
        "--ignore-eos",
        "--save-result",
        "--result-dir",
        BENCH_RESULT_DIR,
        "--result-filename",
        f"largecap_bench_{mode_label}.json",
    ]


def _load_bench_result(mode_label):
    path = os.path.join(BENCH_RESULT_DIR, f"largecap_bench_{mode_label}.json")
    if not os.path.exists(path):
        return {}
    with open(path) as f:
        return json.load(f)


@pytest_marker_throughput
def test_large_cap_throughput_bench(benchmark):
    os.makedirs(BENCH_RESULT_DIR, exist_ok=True)

    print("\n===== [LargeCap] Throughput Phase 1: Baseline (no MemCache) =====")
    _kill_meta_service()
    time.sleep(2)
    benchmark.start_vllm_baseline()
    try:
        cmd = _build_bench_cmd(
            benchmark._port,
            benchmark._served_model_name,
            BENCH_INPUT_LEN,
            BENCH_OUTPUT_LEN,
            BENCH_NUM_PROMPTS,
            BENCH_REQUEST_RATE,
            BENCH_NUM_WARMUPS,
            "baseline",
        )
        print("[INFO] Running vllm bench serve (baseline)...")
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=1800)
        if result.stdout:
            print(result.stdout[-2000:] if len(result.stdout) > 2000 else result.stdout)
        if result.returncode != 0 and result.stderr:
            print(f"[WARN] stderr: {result.stderr[-1000:]}")
        baseline = _load_bench_result("baseline")
    finally:
        benchmark.stop_vllm()

    print("\n===== [LargeCap] Throughput Phase 2: MemCache (8GB HBM + 20GB DRAM) =====")
    benchmark.start_vllm_with_memcache()
    try:
        cmd = _build_bench_cmd(
            benchmark._port,
            benchmark._served_model_name,
            BENCH_INPUT_LEN,
            BENCH_OUTPUT_LEN,
            BENCH_NUM_PROMPTS,
            BENCH_REQUEST_RATE,
            BENCH_NUM_WARMUPS,
            "memcache",
        )
        print("[INFO] Running vllm bench serve (memcache largecap)...")
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=1800)
        if result.stdout:
            print(result.stdout[-2000:] if len(result.stdout) > 2000 else result.stdout)
        if result.returncode != 0 and result.stderr:
            print(f"[WARN] stderr: {result.stderr[-1000:]}")
        memcache = _load_bench_result("memcache")
    finally:
        benchmark.stop_vllm()

    print("\n===== [LargeCap] Throughput Comparison =====")
    metrics = [
        ("Request throughput (req/s)", "request_throughput"),
        ("Input token throughput (tok/s)", "input_throughput"),
        ("Output token throughput (tok/s)", "output_throughput"),
        ("Mean TTFT (ms)", "mean_ttft_ms"),
        ("Median TTFT (ms)", "median_ttft_ms"),
        ("P99 TTFT (ms)", "p99_ttft_ms"),
        ("Mean ITL (ms)", "mean_itl_ms"),
        ("Median ITL (ms)", "median_itl_ms"),
        ("P99 ITL (ms)", "p99_itl_ms"),
    ]
    print(f"  {'Metric':<35} {'Baseline':>15} {'MemCache':>15} {'Delta':>10}")
    print(f"  {'-' * 35} {'-' * 15} {'-' * 15} {'-' * 10}")
    for label, key in metrics:
        b_val = baseline.get(key, "N/A")
        m_val = memcache.get(key, "N/A")
        if isinstance(b_val, (int, float)) and isinstance(m_val, (int, float)):
            pct = f"{(m_val - b_val) / b_val * 100:+.1f}%" if b_val != 0 else "N/A"
            print(f"  {label:<35} {b_val:>15.2f} {m_val:>15.2f} {pct:>10}")
        else:
            print(f"  {label:<35} {str(b_val):>15} {str(m_val):>15} {'N/A':>10}")

    assert baseline, "Baseline result not found"
    assert memcache, "MemCache result not found"
