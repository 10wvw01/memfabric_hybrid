#!/usr/bin/env python
# coding=utf-8
# Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
# MemCache_Hybrid is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#          http://license.coscl.org.cn/MulanPSL2
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
# See the Mulan PSL v2 for more details.

"""vLLM end-to-end TTFT integration tests (Layer 3).

These tests measure full-pipeline TTFT (request → first token) using a real
vLLM-Ascend server, comparing:

  3.7  MemCache backend vs no-cache baseline TTFT overhead
  3.8  Prefix cache hit vs miss E2E TTFT reduction (with MemCache)
  3.9  Partial cache hit rate E2E TTFT (30% and 50%)
  3.10 Throughput benchmark via ``vllm bench serve`` (optional, --run-throughput)

The TP size and device IDs are controlled by environment variables, so the
same tests cover both 2-rank (TP=2) and 4-rank (TP=4) scenarios:

  MMC_TEST_DEVICE_IDS=0,1 MMC_TEST_TENSOR_PARALLEL_SIZE=2  → 2-rank
  MMC_TEST_DEVICE_IDS=0,1,2,3 MMC_TEST_TENSOR_PARALLEL_SIZE=4 → 4-rank

For 4-rank tests, use ``pytest -k 4rank`` to select the 4-rank-only subset
(stored as test_3.7_4rank / test_3.8_4rank). These are duplicates of 3.7/3.8
with explicit 4-rank naming for report clarity.

Cache Isolation Methodology
---------------------------
vLLM has its own GPU-resident block-level prefix cache (``--enable-prefix-
caching``). When MemCache is enabled (``--kv-transfer-config``), both caches
operate simultaneously: vLLM checks local cache first, and only falls through
to the MemCache connector if local cache misses.

To isolate MemCache's contribution in hit/miss tests (3.8, 3.9), the test
calls ``POST /reset_prefix_cache`` (vLLM dev API, enabled via
``VLLM_SERVER_DEV_MODE=1``) to clear vLLM's local block hash → block mapping
*without* touching MemCache data. This forces subsequent requests to go
through the MemCache connector for cache lookups.

Three cache sources are distinguished in logs:

- **vLLM local hit**: ``local_computed >= token_len`` → connector skipped
- **MemCache hit**: ``local_computed = 0``, ``external_hit > 0``
- **Full miss**: ``local_computed = 0``, ``external_hit = 0``

After each test scenario, cache statistics are printed showing both vLLM
prefix cache hit rate and MemCache (external) prefix cache hit rate, parsed
from vLLM's periodic engine metrics log.

Requirements: Ascend NPU + CANN, vLLM-Ascend environment, MetaService running
(for MemCache mode), and a W8A8 quantized model available for serving.
"""

import http.client
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

MEMCACHE_OVERHEAD_RATIO = 0.2
PREFIX_CACHE_HIT_RATIO = 0.7

PARTIAL_HIT_RATES = [0.3, 0.5]
PARTIAL_HIT_PREPOP_SEED_BASE = 5000
PARTIAL_HIT_MISS_SEED_BASE = 6000
HIT_TTFT_THRESHOLD_MS = 1500.0
PARTIAL_HIT_MIN_IMPROVEMENT_FACTOR = 0.2

VOCAB_SIZE = 151936

# ---------------------------------------------------------------------------
# Environment defaults
# ---------------------------------------------------------------------------
DEFAULT_MODEL_PATH = "/home/models/Qwen3.6-35B-A3B-w8a8"
DEFAULT_SERVED_MODEL_NAME = "Qwen3.6-35B-A3B"
DEFAULT_VLLM_PORT = 4077
DEFAULT_DEVICE_IDS = "0,1"
DEFAULT_TENSOR_PARALLEL_SIZE = 2
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
# Helpers
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
            shell=True, capture_output=True, text=True, timeout=5,
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
    integration_config = os.path.join(root, "config", "mmc-local-integration.conf")
    if os.path.exists(integration_config):
        os.environ.setdefault("MMC_LOCAL_CONFIG_PATH", integration_config)
    else:
        os.environ.setdefault("MMC_LOCAL_CONFIG_PATH", os.path.join(root, "config", "mmc-local.conf"))
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


def _build_prompt_ids(num_tokens: int, token_id: int = 1, seed: Optional[int] = None) -> List[int]:
    if seed is not None:
        rng = random.Random(seed)
        return [rng.randrange(VOCAB_SIZE) for _ in range(num_tokens)]
    return [token_id] * num_tokens


# ---------------------------------------------------------------------------
# VLLMTTFTBenchmark
# ---------------------------------------------------------------------------
class VLLMTTFTBenchmark:
    """Manage vLLM serve lifecycle and run TTFT benchmarks."""

    def __init__(
        self,
        model_path: str,
        served_model_name: str,
        port: int,
        device_ids: str,
        tensor_parallel_size: int,
    ):
        self._model_path = model_path
        self._served_model_name = served_model_name
        self._port = port
        self._device_ids = device_ids
        self._tensor_parallel_size = tensor_parallel_size
        self._process: Optional[subprocess.Popen] = None
        self._base_url = f"http://127.0.0.1:{port}"

    # ------------------------------------------------------------------
    # vLLM lifecycle
    # ------------------------------------------------------------------
    def start_vllm_baseline(self):
        cmd = self._build_serve_cmd(with_memcache=False)
        self._start_vllm(cmd)

    def start_vllm_with_memcache(self):
        cmd = self._build_serve_cmd(with_memcache=True)
        self._start_vllm(cmd)

    def stop_vllm(self):
        had_process = self._process is not None
        if self._process is not None:
            self._process.terminate()
            try:
                self._process.wait(timeout=VLLM_STOP_TIMEOUT_S)
            except subprocess.TimeoutExpired:
                self._process.kill()
                self._process.wait(timeout=VLLM_STOP_TIMEOUT_S)
            self._process = None
        if hasattr(self, "_log_file") and self._log_file is not None:
            self._log_file.close()
            self._log_file = None
        if had_process:
            self._cleanup_stale_processes()

    @staticmethod
    def _cleanup_stale_processes():
        import signal as sig_module
        try:
            subprocess.run(
                "pkill -9 -f 'python.*forkserver' 2>/dev/null; "
                "pkill -9 -f 'python.*resource_tracker' 2>/dev/null; "
                "pkill -9 -f 'python.*spawn_main' 2>/dev/null; "
                "pkill -9 -f 'VLLM::' 2>/dev/null; "
                "rm -f /dev/shm/mf_dram_* /dev/shm/memfabric_hybrid_* "
                "/dev/shm/psm_* /dev/shm/sem.mp-* 2>/dev/null",
                shell=True, timeout=10,
            )
        except subprocess.TimeoutExpired:
            pass
        meta_pid = _kill_meta_service()
        time.sleep(2)
        _start_meta_service()
        time.sleep(3)

    def _build_serve_cmd(self, with_memcache: bool) -> list:
        cmd = [
            "vllm",
            "serve",
            self._model_path,
            "--host", "127.0.0.1",
            "--port", str(self._port),
            "--tensor-parallel-size", str(self._tensor_parallel_size),
            "--quantization", "ascend",
            "--served-model-name", self._served_model_name,
            "--trust-remote-code",
            "--dtype", "float16",
            "--max-num-seqs", "10",
            "--max-num-batched-tokens", "4096",
            "--max-model-len", "8192",
            "--gpu-memory-utilization", os.environ.get("MMC_TEST_GPU_MEM_UTIL", "0.80"),
            "--enforce-eager",
            "--enable-chunked-prefill",
            "--mamba-ssm-cache-dtype", "float16",
            "--mamba-cache-mode", "align",
            "--additional-config", ADDITIONAL_CONFIG_JSON,
        ]
        if with_memcache:
            cmd.append("--enable-prefix-caching")
            cmd.extend(["--kv-transfer-config", KV_TRANSFER_CONFIG_MEMCACHE])
        else:
            cmd.append("--no-enable-prefix-caching")
        return cmd

    def _start_vllm(self, cmd: list):
        self.stop_vllm()
        env = os.environ.copy()
        env["ASCEND_RT_VISIBLE_DEVICES"] = self._device_ids
        root = _project_root()
        integration_config = os.path.join(root, "config", "mmc-local-integration.conf")
        local_config = os.environ.get("MMC_LOCAL_CONFIG_PATH", "")
        if not local_config or local_config == os.path.join(root, "config", "mmc-local.conf"):
            if os.path.exists(integration_config):
                local_config = integration_config
            else:
                local_config = os.path.join(root, "config", "mmc-local.conf")
        env["MMC_LOCAL_CONFIG_PATH"] = local_config
        env["MMC_META_CONFIG_PATH"] = os.environ.get(
            "MMC_META_CONFIG_PATH", os.path.join(root, "config", "mmc-meta.conf")
        )
        try:
            import memcache_hybrid
            import memfabric_hybrid

            for pkg in (memcache_hybrid, memfabric_hybrid):
                pkg_lib = os.path.join(os.path.dirname(pkg.__file__), "lib")
                if os.path.isdir(pkg_lib) and pkg_lib not in env.get("LD_LIBRARY_PATH", ""):
                    env["LD_LIBRARY_PATH"] = pkg_lib + ":" + env.get("LD_LIBRARY_PATH", "")
        except ImportError:
            pass
        env["VLLM_SERVER_DEV_MODE"] = "1"
        print(f"[INFO] Starting vLLM: {' '.join(cmd[:6])}... (devices={self._device_ids})")
        print(f"[INFO] MMC_LOCAL_CONFIG_PATH={env['MMC_LOCAL_CONFIG_PATH']}")
        log_path = f"/tmp/vllm_test_{int(time.time())}.log"
        print(f"[INFO] vLLM log: {log_path}")
        log_file = open(log_path, "w")
        self._process = subprocess.Popen(
            cmd,
            stdout=log_file,
            stderr=subprocess.STDOUT,
            env=env,
        )
        self._log_file = log_file
        self._log_path = log_path
        self._wait_for_ready()

    def _wait_for_ready(self, timeout: int = VLLM_READY_TIMEOUT_S):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self._process is not None and self._process.poll() is not None:
                stdout_tail = ""
                try:
                    if hasattr(self, "_log_path") and os.path.exists(self._log_path):
                        with open(self._log_path, "r") as f:
                            lines = f.readlines()
                        stdout_tail = "".join(lines[-30:])
                except OSError:
                    pass
                raise RuntimeError(f"vLLM process exited (code={self._process.returncode})\n{stdout_tail}")
            try:
                conn = http.client.HTTPConnection("127.0.0.1", self._port, timeout=5)
                conn.request("GET", "/health")
                resp = conn.getresponse()
                conn.close()
                if resp.status == 200:
                    print(f"[INFO] vLLM is ready on port {self._port}")
                    return
            except (OSError, http.client.HTTPException, ConnectionRefusedError):
                pass
            time.sleep(5)
        raise TimeoutError(f"vLLM did not become ready within {timeout}s on port {self._port}")

    # ------------------------------------------------------------------
    # TTFT measurement (streaming API)
    # ------------------------------------------------------------------
    def measure_e2e_ttft_ms(self, prompt_ids: List[int], max_tokens: int = MAX_TOKENS_TTFT) -> float:
        body = json.dumps(
            {
                "model": self._served_model_name,
                "prompt": prompt_ids,
                "max_tokens": max_tokens,
                "stream": True,
                "temperature": 0,
                "ignore_eos": True,
            }
        )
        conn = http.client.HTTPConnection("127.0.0.1", self._port, timeout=TTFT_REQUEST_TIMEOUT_S)
        try:
            start = time.perf_counter()
            conn.request("POST", "/v1/completions", body=body, headers={"Content-Type": "application/json"})
            resp = conn.getresponse()
            ttft_ms = 0.0
            for line in resp:
                if line.startswith(b"data: ") and b"[DONE]" not in line:
                    ttft_ms = (time.perf_counter() - start) * 1000.0
                    break
            else:
                ttft_ms = (time.perf_counter() - start) * 1000.0
            resp.close()
            return ttft_ms
        finally:
            conn.close()

    def reset_local_prefix_cache(self):
        """Clear vLLM's internal GPU block cache, preserving MemCache data.

        Calls POST /reset_prefix_cache (without reset_external) so that
        subsequent cache hits must come from MemCache, not vLLM's local
        block-level prefix cache.
        """
        conn = http.client.HTTPConnection("127.0.0.1", self._port, timeout=30)
        try:
            conn.request("POST", "/reset_prefix_cache")
            resp = conn.getresponse()
            body = resp.read().decode()
            resp.close()
            if resp.status != 200:
                print(f"  [WARN] reset_prefix_cache returned {resp.status}: {body}")
            return resp.status == 200
        except (OSError, http.client.HTTPException) as e:
            print(f"  [WARN] reset_prefix_cache failed: {e}")
            return False
        finally:
            conn.close()

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
        num_requests: int = MEASURED_REQUESTS,
        prompt_tokens: int = PROMPT_TOKENS,
        warmup: int = WARMUP_REQUESTS,
        use_unique_prompts: bool = True,
    ) -> Dict[str, float]:
        if use_unique_prompts:
            warmup_prompts = [_build_prompt_ids(prompt_tokens, seed=-(i + 1)) for i in range(warmup)]
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
            ttft = self.measure_e2e_ttft_ms(prompt)
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
# 3.7  MemCache vs Baseline E2E TTFT overhead
# ---------------------------------------------------------------------------
@pytest.mark.rank2
def test_memcache_vs_baseline_e2e_ttft(benchmark):
    baseline_result = None
    memcache_result = None

    try:
        print("\n===== Starting baseline vLLM (no MemCache) =====")
        benchmark.start_vllm_baseline()
        baseline_result = benchmark.measure_ttft_batch(
            num_requests=MEASURED_REQUESTS,
            prompt_tokens=PROMPT_TOKENS,
            warmup=WARMUP_REQUESTS,
            use_unique_prompts=True,
        )
        print(f"Baseline: {baseline_result}")
        benchmark.print_cache_stats("Baseline")
    finally:
        benchmark.stop_vllm()

    try:
        print("\n===== Starting vLLM with MemCache backend =====")
        benchmark.start_vllm_with_memcache()
        memcache_result = benchmark.measure_ttft_batch(
            num_requests=MEASURED_REQUESTS,
            prompt_tokens=PROMPT_TOKENS,
            warmup=WARMUP_REQUESTS,
            use_unique_prompts=True,
        )
        print(f"MemCache: {memcache_result}")
        benchmark.print_cache_stats("MemCache")
    finally:
        benchmark.stop_vllm()

    assert baseline_result is not None, "baseline benchmark failed"
    assert memcache_result is not None, "memcache benchmark failed"

    baseline_p50 = baseline_result["p50_ms"]
    memcache_p50 = memcache_result["p50_ms"]
    overhead = (memcache_p50 - baseline_p50) / baseline_p50 if baseline_p50 > 0 else 1.0

    print(
        f"\n===== TTFT Overhead Comparison =====\n"
        f"  Baseline P50={baseline_p50:.2f}ms P99={baseline_result['p99_ms']:.2f}ms\n"
        f"  MemCache P50={memcache_p50:.2f}ms P99={memcache_result['p99_ms']:.2f}ms\n"
        f"  Overhead={overhead:.2%} (threshold: {MEMCACHE_OVERHEAD_RATIO:.0%})"
    )

    assert overhead < MEMCACHE_OVERHEAD_RATIO, (
        f"MemCache E2E TTFT overhead {overhead:.2%} exceeds {MEMCACHE_OVERHEAD_RATIO:.0%} threshold"
    )


# ---------------------------------------------------------------------------
# 3.8  Prefix cache hit E2E TTFT reduction (with MemCache)
# ---------------------------------------------------------------------------
@pytest.mark.rank2
def test_prefix_cache_hit_e2e_ttft(benchmark):
    """Prefix cache hit vs miss E2E TTFT reduction (100% hit, via MemCache).

    For each measured request pair:
      1. Send a unique prompt (cache miss) → stores KV in MemCache.
      2. Call ``reset_local_prefix_cache()`` to clear vLLM's GPU-local
         block cache, so the repeat request must go through MemCache.
      3. Re-send the same prompt (cache hit from MemCache).

    This isolates MemCache's contribution: without the reset, vLLM's local
    prefix cache would absorb the hit, making it impossible to distinguish
    MemCache's TTFT reduction from vLLM's own block-level cache.
    """
    try:
        print("\n===== Starting vLLM with MemCache for prefix cache test =====")
        benchmark.start_vllm_with_memcache()

        prompt_ids = _build_prompt_ids(PROMPT_TOKENS, seed=42)

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

            benchmark.reset_local_prefix_cache()

            hit_ttft = benchmark.measure_e2e_ttft_ms(miss_prompt)
            hit_latencies.append(hit_ttft)

            print(f"  pair {i + 1}/{MEASURED_REQUESTS}: miss={miss_ttft:.2f}ms, hit={hit_ttft:.2f}ms")

        benchmark.print_cache_stats("3.8 Hit/Miss")

        miss_p50 = _percentile(miss_latencies, 0.50)
        hit_p50 = _percentile(hit_latencies, 0.50)
        miss_p99 = _percentile(miss_latencies, 0.99)
        hit_p99 = _percentile(hit_latencies, 0.99)

        ratio = hit_p50 / miss_p50 if miss_p50 > 0 else 1.0

        print(
            f"\n===== Prefix Cache Hit vs Miss E2E TTFT =====\n"
            f"  Cache miss P50={miss_p50:.2f}ms P99={miss_p99:.2f}ms\n"
            f"  Cache hit  P50={hit_p50:.2f}ms P99={hit_p99:.2f}ms\n"
            f"  Hit/Miss ratio={ratio:.2%} (threshold: < {PREFIX_CACHE_HIT_RATIO:.0%})"
        )

        assert hit_p50 < miss_p50 * PREFIX_CACHE_HIT_RATIO, (
            f"Cache hit P50 {hit_p50:.2f}ms >= miss P50 * {PREFIX_CACHE_HIT_RATIO} "
            f"({miss_p50 * PREFIX_CACHE_HIT_RATIO:.2f}ms)"
        )
    finally:
        benchmark.stop_vllm()


# ---------------------------------------------------------------------------
# 3.9  Partial cache hit rate E2E TTFT (30% and 50%)
# ---------------------------------------------------------------------------
@pytest.mark.parametrize(
    "hit_rate",
    PARTIAL_HIT_RATES,
    ids=[f"{int(r * 100)}pct" for r in PARTIAL_HIT_RATES],
)
@pytest.mark.rank2
def test_partial_cache_hit_e2e_ttft(benchmark, hit_rate):
    """Measure E2E TTFT under a mixed workload with partial cache hit rate.

    Pre-populates MemCache with a subset of prompts, then calls
    ``reset_local_prefix_cache()`` to clear vLLM's local block cache. A
    shuffled mix of cache-hit (repeated) and cache-miss (unique) requests
    is then sent, so that hit requests are served by MemCache, not vLLM's
    local cache. This interpolates between Test 3.7 (0% hit) and Test 3.8
    (100% hit), showing how TTFT improves with partial MemCache hit rates.
    """
    try:
        print(f"\n===== Starting vLLM with MemCache for partial cache hit ({hit_rate:.0%}) test =====")
        benchmark.start_vllm_with_memcache()

        warmup_prompt = _build_prompt_ids(PROMPT_TOKENS, seed=99)
        for i in range(WARMUP_REQUESTS):
            benchmark.measure_e2e_ttft_ms(warmup_prompt)
            print(f"  warmup {i + 1}/{WARMUP_REQUESTS} done")

        num_requests = MEASURED_REQUESTS
        hit_count = round(hit_rate * num_requests)
        miss_count = num_requests - hit_count

        hit_seeds = [PARTIAL_HIT_PREPOP_SEED_BASE + i for i in range(hit_count)]
        for i, seed in enumerate(hit_seeds):
            prompt = _build_prompt_ids(PROMPT_TOKENS, seed=seed)
            benchmark.measure_e2e_ttft_ms(prompt)
            print(f"  pre-populate {i + 1}/{hit_count} (seed={seed}) done")

        benchmark.reset_local_prefix_cache()

        miss_seeds = [PARTIAL_HIT_MISS_SEED_BASE + i for i in range(miss_count)]
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
        all_p90 = _percentile(all_latencies, 0.90)
        all_p99 = _percentile(all_latencies, 0.99)
        hit_avg = sum(hit_latencies) / len(hit_latencies) if hit_latencies else 0.0
        hit_p50 = _percentile(hit_latencies, 0.50) if hit_latencies else 0.0
        miss_avg = sum(miss_latencies) / len(miss_latencies) if miss_latencies else 0.0
        miss_p50 = _percentile(miss_latencies, 0.50) if miss_latencies else 0.0

        actual_hit_count = sum(1 for t in hit_latencies if t < HIT_TTFT_THRESHOLD_MS)
        improvement = (miss_avg - all_avg) / miss_avg if miss_avg > 0 else 0.0
        min_expected_improvement = hit_rate * PARTIAL_HIT_MIN_IMPROVEMENT_FACTOR

        print(
            f"\n===== Partial Cache Hit ({hit_rate:.0%}) E2E TTFT =====\n"
            f"  Expected hit/total: {hit_count}/{num_requests} ({hit_rate:.0%})\n"
            f"  Actual hit (TTFT<{HIT_TTFT_THRESHOLD_MS:.0f}ms): {actual_hit_count}/{hit_count}\n"
            f"  All   avg={all_avg:.2f}ms P50={all_p50:.2f}ms P90={all_p90:.2f}ms P99={all_p99:.2f}ms\n"
            f"  Hit   avg={hit_avg:.2f}ms P50={hit_p50:.2f}ms (count={len(hit_latencies)})\n"
            f"  Miss  avg={miss_avg:.2f}ms P50={miss_p50:.2f}ms (count={len(miss_latencies)})\n"
            f"  Improvement (avg): {improvement:.2%} (min expected: {min_expected_improvement:.2%})"
        )

        benchmark.print_cache_stats(f"3.9 {hit_rate:.0%}")

        assert actual_hit_count >= hit_count * 0.7, (
            f"Only {actual_hit_count}/{hit_count} expected hits had TTFT < {HIT_TTFT_THRESHOLD_MS:.0f}ms"
        )
        assert hit_p50 < miss_p50 * PREFIX_CACHE_HIT_RATIO, (
            f"Hit P50 {hit_p50:.2f}ms >= miss P50 * {PREFIX_CACHE_HIT_RATIO} "
            f"({miss_p50 * PREFIX_CACHE_HIT_RATIO:.2f}ms)"
        )
        assert improvement > min_expected_improvement, (
            f"Improvement {improvement:.2%} <= min expected {min_expected_improvement:.2%} "
            f"(mixed avg {all_avg:.2f}ms vs miss avg {miss_avg:.2f}ms)"
        )
    finally:
        benchmark.stop_vllm()


# ---------------------------------------------------------------------------
# 3.7-4rank / 3.8-4rank  — 4-rank (TP=4) variants for report clarity
# ---------------------------------------------------------------------------
# These are functionally identical to 3.7/3.8 above; the TP size is already
# controlled by MMC_TEST_TENSOR_PARALLEL_SIZE. They exist as separately-named
# tests so that ``pytest -k 4rank`` produces a clear 4-rank-only report.
#
# Run with:
#   MMC_TEST_DEVICE_IDS=0,1,2,3 MMC_TEST_TENSOR_PARALLEL_SIZE=4 pytest -k 4rank


@pytest.mark.rank4
def test_memcache_vs_baseline_e2e_ttft_4rank(benchmark):
    """4-rank (TP=4): MemCache vs Baseline E2E TTFT overhead (0% hit).

    Identical to test_memcache_vs_baseline_e2e_ttft but named for 4-rank
    report clarity. Set MMC_TEST_TENSOR_PARALLEL_SIZE=4.
    """
    baseline_result = None
    memcache_result = None

    try:
        print("\n===== [4-Rank] Starting baseline vLLM (no MemCache) =====")
        benchmark.start_vllm_baseline()
        baseline_result = benchmark.measure_ttft_batch(
            num_requests=MEASURED_REQUESTS,
            prompt_tokens=PROMPT_TOKENS,
            warmup=WARMUP_REQUESTS,
            use_unique_prompts=True,
        )
        print(f"Baseline: {baseline_result}")
        benchmark.print_cache_stats("4-Rank Baseline")
    finally:
        benchmark.stop_vllm()

    try:
        print("\n===== [4-Rank] Starting vLLM with MemCache backend =====")
        benchmark.start_vllm_with_memcache()
        memcache_result = benchmark.measure_ttft_batch(
            num_requests=MEASURED_REQUESTS,
            prompt_tokens=PROMPT_TOKENS,
            warmup=WARMUP_REQUESTS,
            use_unique_prompts=True,
        )
        print(f"MemCache: {memcache_result}")
        benchmark.print_cache_stats("4-Rank MemCache")
    finally:
        benchmark.stop_vllm()

    assert baseline_result is not None, "baseline benchmark failed"
    assert memcache_result is not None, "memcache benchmark failed"

    baseline_p50 = baseline_result["p50_ms"]
    memcache_p50 = memcache_result["p50_ms"]
    overhead = (memcache_p50 - baseline_p50) / baseline_p50 if baseline_p50 > 0 else 1.0

    print(
        f"\n===== [4-Rank] TTFT Overhead Comparison =====\n"
        f"  Baseline P50={baseline_p50:.2f}ms P99={baseline_result['p99_ms']:.2f}ms\n"
        f"  MemCache P50={memcache_p50:.2f}ms P99={memcache_result['p99_ms']:.2f}ms\n"
        f"  Overhead={overhead:.2%} (threshold: {MEMCACHE_OVERHEAD_RATIO:.0%})"
    )

    assert overhead < MEMCACHE_OVERHEAD_RATIO, (
        f"MemCache E2E TTFT overhead {overhead:.2%} exceeds {MEMCACHE_OVERHEAD_RATIO:.0%} threshold"
    )


@pytest.mark.rank4
def test_prefix_cache_hit_e2e_ttft_4rank(benchmark):
    """4-rank (TP=4): Prefix cache hit E2E TTFT reduction (100% hit).

    Identical to test_prefix_cache_hit_e2e_ttft but named for 4-rank
    report clarity. Set MMC_TEST_TENSOR_PARALLEL_SIZE=4.

    Uses ``reset_local_prefix_cache()`` to isolate MemCache hits from
    vLLM's local prefix cache. See test_prefix_cache_hit_e2e_ttft
    docstring for full methodology.
    """
    try:
        print("\n===== [4-Rank] Starting vLLM with MemCache for prefix cache test =====")
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

            benchmark.reset_local_prefix_cache()

            hit_ttft = benchmark.measure_e2e_ttft_ms(miss_prompt)
            hit_latencies.append(hit_ttft)

            print(f"  pair {i + 1}/{MEASURED_REQUESTS}: miss={miss_ttft:.2f}ms, hit={hit_ttft:.2f}ms")

        miss_p50 = _percentile(miss_latencies, 0.50)
        hit_p50 = _percentile(hit_latencies, 0.50)
        miss_p99 = _percentile(miss_latencies, 0.99)
        hit_p99 = _percentile(hit_latencies, 0.99)
        ratio = hit_p50 / miss_p50 if miss_p50 > 0 else 1.0

        print(
            f"\n===== [4-Rank] Prefix Cache Hit vs Miss E2E TTFT =====\n"
            f"  Cache miss P50={miss_p50:.2f}ms P99={miss_p99:.2f}ms\n"
            f"  Cache hit  P50={hit_p50:.2f}ms P99={hit_p99:.2f}ms\n"
            f"  Hit/Miss ratio={ratio:.2%} (threshold: < {PREFIX_CACHE_HIT_RATIO:.0%})"
        )

        benchmark.print_cache_stats("4-Rank Hit/Miss")

        assert hit_p50 < miss_p50 * PREFIX_CACHE_HIT_RATIO, (
            f"Cache hit P50 {hit_p50:.2f}ms >= miss P50 * {PREFIX_CACHE_HIT_RATIO} "
            f"({miss_p50 * PREFIX_CACHE_HIT_RATIO:.2f}ms)"
        )
    finally:
        benchmark.stop_vllm()


# ---------------------------------------------------------------------------
# 3.10  Throughput benchmark via ``vllm bench serve`` (optional)
# ---------------------------------------------------------------------------
# Run with: pytest -k throughput -m throughput
# Or:       python3 vllm_ttft_test.py --throughput-only
#
# This test is marked ``throughput`` so it can be excluded from default runs:
#   pytest vllm_ttft_test.py -m "not throughput"

BENCH_INPUT_LEN = 4096
BENCH_OUTPUT_LEN = 128
BENCH_NUM_PROMPTS = 100
BENCH_REQUEST_RATE = 10
BENCH_NUM_WARMUPS = 10
BENCH_RESULT_DIR = "/tmp/vllm_bench_results"

pytest_marker_throughput = pytest.mark.throughput


def _build_bench_cmd(port, served_name, input_len, output_len, num_prompts, request_rate, num_warmups, mode_label):
    return [
        "vllm", "bench", "serve",
        "--backend", "vllm",
        "--host", "127.0.0.1",
        "--port", str(port),
        "--model", served_name,
        "--dataset-name", "random",
        "--input-len", str(input_len),
        "--output-len", str(output_len),
        "--num-prompts", str(num_prompts),
        "--request-rate", str(request_rate),
        "--num-warmups", str(num_warmups),
        "--seed", "42",
        "--ignore-eos",
        "--save-result",
        "--result-dir", BENCH_RESULT_DIR,
        "--result-filename", f"bench_{mode_label}.json",
    ]


def _load_bench_result(mode_label):
    path = os.path.join(BENCH_RESULT_DIR, f"bench_{mode_label}.json")
    if not os.path.exists(path):
        return {}
    with open(path) as f:
        return json.load(f)


@pytest_marker_throughput
def test_throughput_bench(benchmark):
    """Throughput benchmark: Baseline vs MemCache via ``vllm bench serve``.

    Measures request throughput (req/s), input/output token throughput,
    TTFT/ITL percentiles under a sustained load of 100 requests at 10 req/s.
    """
    os.makedirs(BENCH_RESULT_DIR, exist_ok=True)

    # --- Baseline ---
    print("\n===== Throughput Phase 1: Baseline (no MemCache) =====")
    _kill_meta_service()
    time.sleep(2)
    benchmark.start_vllm_baseline()
    try:
        cmd = _build_bench_cmd(
            benchmark._port, benchmark._served_model_name,
            BENCH_INPUT_LEN, BENCH_OUTPUT_LEN,
            BENCH_NUM_PROMPTS, BENCH_REQUEST_RATE, BENCH_NUM_WARMUPS,
            "baseline",
        )
        print(f"[INFO] Running vllm bench serve (baseline)...")
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=1800)
        if result.stdout:
            print(result.stdout[-2000:] if len(result.stdout) > 2000 else result.stdout)
        if result.returncode != 0 and result.stderr:
            print(f"[WARN] stderr: {result.stderr[-1000:]}")
        baseline = _load_bench_result("baseline")
    finally:
        benchmark.stop_vllm()

    # --- MemCache ---
    print("\n===== Throughput Phase 2: MemCache =====")
    benchmark.start_vllm_with_memcache()
    try:
        cmd = _build_bench_cmd(
            benchmark._port, benchmark._served_model_name,
            BENCH_INPUT_LEN, BENCH_OUTPUT_LEN,
            BENCH_NUM_PROMPTS, BENCH_REQUEST_RATE, BENCH_NUM_WARMUPS,
            "memcache",
        )
        print(f"[INFO] Running vllm bench serve (memcache)...")
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=1800)
        if result.stdout:
            print(result.stdout[-2000:] if len(result.stdout) > 2000 else result.stdout)
        if result.returncode != 0 and result.stderr:
            print(f"[WARN] stderr: {result.stderr[-1000:]}")
        memcache = _load_bench_result("memcache")
    finally:
        benchmark.stop_vllm()

    # --- Compare ---
    print("\n===== Throughput Comparison =====")
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
    print(f"  {'-'*35} {'-'*15} {'-'*15} {'-'*10}")
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
