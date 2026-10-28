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

"""Layer 3 large-capacity vLLM end-to-end TTFT tests (8GB HBM + 20GB DRAM, TP=4).

Tests how large-capacity MemCache (8GB HBM + 20GB DRAM per rank) affects
vLLM end-to-end TTFT under various cache-hit patterns. The large capacity
allows more KV cache entries to be retained, reducing eviction and improving
cache hit rates compared to the 1GB+1GB baseline.

Test scenarios:

  LC-L3-1  Large-capacity overhead      — MemCache vs Baseline TTFT (8GB+20GB)
  LC-L3-2  Large-scale cache hit         — 40-key pre-populate, then 100% hit
  LC-L3-3  Eviction-aware mixed workload — pre-populate 30 keys (exceeds HBM),
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

Requires: Ascend NPU + CANN, vLLM-Ascend, MetaService, 8GB HBM + 20GB DRAM config.
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

LARGE_CAP_OVERHEAD_RATIO = 0.25
PREFIX_CACHE_HIT_RATIO = 0.7

LARGE_PREPOPULATE_COUNT = 40
LARGE_PREPOP_SEED_BASE = 7000
LARGE_MISS_SEED_BASE = 9000

HIT_TTFT_THRESHOLD_MS = 1500.0
PARTIAL_HIT_MIN_IMPROVEMENT_FACTOR = 0.2

EVICT_HIT_RATE = 0.3
EVICT_HIGH_HIT_RATE = 0.7
EVICT_PREPOP_COUNT = 30

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
# Helpers (shared with vllm_ttft_test.py)
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
class VLLMTTFTBenchmark:
    def __init__(self, model_path, served_model_name, port, device_ids, tensor_parallel_size):
        self._model_path = model_path
        self._served_model_name = served_model_name
        self._port = port
        self._device_ids = device_ids
        self._tensor_parallel_size = tensor_parallel_size
        self._process: Optional[subprocess.Popen] = None
        self._base_url = f"http://127.0.0.1:{port}"

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
        _kill_meta_service()
        time.sleep(15)
        _start_meta_service()
        time.sleep(5)

    @staticmethod
    def _cleanup_vllm_only():
        try:
            subprocess.run(
                "pkill -9 -f 'python.*forkserver' 2>/dev/null; "
                "pkill -9 -f 'python.*resource_tracker' 2>/dev/null; "
                "pkill -9 -f 'python.*spawn_main' 2>/dev/null; "
                "pkill -9 -f 'VLLM::' 2>/dev/null",
                shell=True, timeout=10,
            )
        except subprocess.TimeoutExpired:
            pass
        time.sleep(10)

    def restart_vllm_preserve_memcache(self):
        self.stop_vllm_preserve_memcache()
        time.sleep(3)

    def stop_vllm_preserve_memcache(self):
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
            self._cleanup_vllm_only()

    def _build_serve_cmd(self, with_memcache: bool) -> list:
        gpu_mem_util = os.environ.get("MMC_TEST_GPU_MEM_UTIL", DEFAULT_GPU_MEM_UTIL)
        cmd = [
            "vllm", "serve", self._model_path,
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
            "--gpu-memory-utilization", gpu_mem_util,
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
        test_config = os.path.join(os.path.dirname(__file__), "..", "..", "config", "mmc-local-largecap-4rank.conf")
        local_config = os.environ.get("MMC_LOCAL_CONFIG_PATH", "")
        if not local_config or local_config == os.path.join(root, "config", "mmc-local.conf"):
            if os.path.exists(test_config):
                local_config = os.path.abspath(test_config)
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
        gpu_util = os.environ.get("MMC_TEST_GPU_MEM_UTIL", DEFAULT_GPU_MEM_UTIL)
        print(f"[INFO] Starting vLLM: {' '.join(cmd[:6])}... (devices={self._device_ids}, gpu_mem_util={gpu_util})")
        print(f"[INFO] MMC_LOCAL_CONFIG_PATH={env['MMC_LOCAL_CONFIG_PATH']}")
        log_path = f"/tmp/vllm_largecap_{int(time.time())}.log"
        print(f"[INFO] vLLM log: {log_path}")
        log_file = open(log_path, "w")
        self._process = subprocess.Popen(cmd, stdout=log_file, stderr=subprocess.STDOUT, env=env)
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

    def measure_e2e_ttft_ms(self, prompt_ids: List[int], max_tokens: int = MAX_TOKENS_TTFT,
                             skip_prefix_cache: bool = False) -> float:
        body_dict = {
            "model": self._served_model_name,
            "prompt": prompt_ids,
            "max_tokens": max_tokens,
            "stream": True,
            "temperature": 0,
            "ignore_eos": True,
        }
        if skip_prefix_cache:
            body_dict["skip_reading_prefix_cache"] = True
        body = json.dumps(body_dict)
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
        """Clear vLLM's internal GPU block cache, preserving MemCache data."""
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

    def measure_ttft_batch(self, num_requests=MEASURED_REQUESTS, prompt_tokens=PROMPT_TOKENS,
                           warmup=WARMUP_REQUESTS, use_unique_prompts=True) -> Dict[str, float]:
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
        model_path=model_path, served_model_name=served_name, port=port,
        device_ids=device_ids, tensor_parallel_size=tp_size,
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
            num_requests=MEASURED_REQUESTS, prompt_tokens=PROMPT_TOKENS,
            warmup=WARMUP_REQUESTS, use_unique_prompts=True,
        )
        print(f"Baseline: {baseline_result}")
        benchmark.print_cache_stats("LargeCap Baseline")
    finally:
        benchmark.stop_vllm()

    try:
        print("\n===== [LargeCap] Starting vLLM with MemCache (8GB HBM + 20GB DRAM) =====")
        benchmark.start_vllm_with_memcache()
        memcache_result = benchmark.measure_ttft_batch(
            num_requests=MEASURED_REQUESTS, prompt_tokens=PROMPT_TOKENS,
            warmup=WARMUP_REQUESTS, use_unique_prompts=True,
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
        f"  Overhead={overhead:.2%} (threshold: {LARGE_CAP_OVERHEAD_RATIO:.0%})"
    )

    assert overhead < LARGE_CAP_OVERHEAD_RATIO, (
        f"LargeCap overhead {overhead:.2%} exceeds {LARGE_CAP_OVERHEAD_RATIO:.0%} threshold"
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
            miss_ttft = benchmark.measure_e2e_ttft_ms(prompt)
            miss_latencies.append(miss_ttft)
            benchmark.reset_local_prefix_cache()
            hit_ttft = benchmark.measure_e2e_ttft_ms(prompt)
            hit_latencies.append(hit_ttft)
            print(f"  pair {i + 1}/{MEASURED_REQUESTS}: miss={miss_ttft:.2f}ms, hit={hit_ttft:.2f}ms (seed={seed})")

        miss_p50 = _percentile(miss_latencies, 0.50)
        miss_p99 = _percentile(miss_latencies, 0.99)
        hit_p50 = _percentile(hit_latencies, 0.50)
        hit_p99 = _percentile(hit_latencies, 0.99)
        hit_avg = sum(hit_latencies) / len(hit_latencies) if hit_latencies else 0.0

        actual_hits = sum(1 for t in hit_latencies if t < HIT_TTFT_THRESHOLD_MS)
        ratio = hit_p50 / miss_p50 if miss_p50 > 0 else 1.0

        print(
            f"\n===== [LargeCap] Large-scale cache hit result =====\n"
            f"  Pre-populated: {LARGE_PREPOPULATE_COUNT} keys\n"
            f"  Cache miss P50={miss_p50:.2f}ms P99={miss_p99:.2f}ms\n"
            f"  Cache hit  P50={hit_p50:.2f}ms P99={hit_p99:.2f}ms avg={hit_avg:.2f}ms\n"
            f"  Hit/Miss ratio={ratio:.2%} (threshold: < {PREFIX_CACHE_HIT_RATIO:.0%})\n"
            f"  Actual hits (TTFT<{HIT_TTFT_THRESHOLD_MS:.0f}ms): {actual_hits}/{MEASURED_REQUESTS}"
        )

        benchmark.print_cache_stats("LC-L3-2 Cache Hit")

        assert actual_hits >= MEASURED_REQUESTS * 0.7, (
            f"Only {actual_hits}/{MEASURED_REQUESTS} hits achieved"
        )
        assert hit_p50 < miss_p50 * PREFIX_CACHE_HIT_RATIO, (
            f"Hit P50 {hit_p50:.2f}ms >= miss P50 * {PREFIX_CACHE_HIT_RATIO} "
            f"({miss_p50 * PREFIX_CACHE_HIT_RATIO:.2f}ms)"
        )
    finally:
        benchmark.stop_vllm()


# ---------------------------------------------------------------------------
# LC-L3-3: Eviction-aware mixed workload — pre-populate 30 keys (exceeds HBM),
#           then mix hit/miss at 30% and 70% rates
# ---------------------------------------------------------------------------
@pytest.mark.rank4
@pytest.mark.parametrize(
    "hit_rate",
    [EVICT_HIT_RATE, EVICT_HIGH_HIT_RATE],
    ids=[f"{int(r * 100)}pct" for r in [EVICT_HIT_RATE, EVICT_HIGH_HIT_RATE]],
)
def test_eviction_aware_mixed_4rank(benchmark, hit_rate):
    try:
        print(f"\n===== [LargeCap] Eviction-aware mixed ({hit_rate:.0%} hit) =====")
        benchmark.start_vllm_with_memcache()

        warmup_prompt = _build_prompt_ids(PROMPT_TOKENS, seed=99)
        for i in range(WARMUP_REQUESTS):
            benchmark.measure_e2e_ttft_ms(warmup_prompt)
            print(f"  warmup {i + 1}/{WARMUP_REQUESTS} done")

        num_requests = MEASURED_REQUESTS
        hit_count = round(hit_rate * num_requests)
        miss_count = num_requests - hit_count

        hit_seeds = [LARGE_PREPOP_SEED_BASE + 1000 + i for i in range(EVICT_PREPOP_COUNT)]
        print(f"  Pre-populating {EVICT_PREPOP_COUNT} keys (exceeds HBM, triggers eviction to DRAM)...")
        for i, seed in enumerate(hit_seeds[:EVICT_PREPOP_COUNT]):
            prompt = _build_prompt_ids(PROMPT_TOKENS, seed=seed)
            benchmark.measure_e2e_ttft_ms(prompt)
            if (i + 1) % 10 == 0:
                print(f"  pre-populate {i + 1}/{EVICT_PREPOP_COUNT} done")

        benchmark.reset_local_prefix_cache()

        miss_seeds = [LARGE_MISS_SEED_BASE + i for i in range(miss_count)]
        requests = []
        for i in range(hit_count):
            seed = hit_seeds[i % EVICT_PREPOP_COUNT]
            requests.append((_build_prompt_ids(PROMPT_TOKENS, seed=seed), True))
        for seed in miss_seeds:
            requests.append((_build_prompt_ids(PROMPT_TOKENS, seed=seed), False))

        rng = random.Random(42)
        rng.shuffle(requests)

        print(f"  Measuring {num_requests} mixed requests (hit={hit_count}, miss={miss_count})...")
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
        hit_avg = sum(hit_latencies) / len(hit_latencies) if hit_latencies else 0.0
        hit_p50 = _percentile(hit_latencies, 0.50) if hit_latencies else 0.0
        miss_avg = sum(miss_latencies) / len(miss_latencies) if miss_latencies else 0.0
        miss_p50 = _percentile(miss_latencies, 0.50) if miss_latencies else 0.0
        actual_hit_count = sum(1 for t in hit_latencies if t < HIT_TTFT_THRESHOLD_MS)
        improvement = (miss_avg - all_avg) / miss_avg if miss_avg > 0 else 0.0
        min_expected = hit_rate * PARTIAL_HIT_MIN_IMPROVEMENT_FACTOR

        print(
            f"\n===== [LargeCap] Eviction-aware mixed ({hit_rate:.0%}) result =====\n"
            f"  Pre-populated: {EVICT_PREPOP_COUNT} keys (HBM eviction triggered)\n"
            f"  Expected hit/total: {hit_count}/{num_requests} ({hit_rate:.0%})\n"
            f"  Actual hit (TTFT<{HIT_TTFT_THRESHOLD_MS:.0f}ms): {actual_hit_count}/{hit_count}\n"
            f"  All   avg={all_avg:.2f}ms\n"
            f"  Hit   avg={hit_avg:.2f}ms P50={hit_p50:.2f}ms (count={len(hit_latencies)})\n"
            f"  Miss  avg={miss_avg:.2f}ms P50={miss_p50:.2f}ms (count={len(miss_latencies)})\n"
            f"  Improvement: {improvement:.2%} (min expected: {min_expected:.2%})"
        )

        benchmark.print_cache_stats(f"LC-L3-3 {hit_rate:.0%}")

        assert actual_hit_count >= hit_count * 0.6, (
            f"Only {actual_hit_count}/{hit_count} expected hits had TTFT < {HIT_TTFT_THRESHOLD_MS:.0f}ms"
        )
        assert improvement > min_expected, (
            f"Improvement {improvement:.2%} <= {min_expected:.2%}"
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
        "--result-filename", f"largecap_bench_{mode_label}.json",
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
            benchmark._port, benchmark._served_model_name,
            BENCH_INPUT_LEN, BENCH_OUTPUT_LEN,
            BENCH_NUM_PROMPTS, BENCH_REQUEST_RATE, BENCH_NUM_WARMUPS,
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
            benchmark._port, benchmark._served_model_name,
            BENCH_INPUT_LEN, BENCH_OUTPUT_LEN,
            BENCH_NUM_PROMPTS, BENCH_REQUEST_RATE, BENCH_NUM_WARMUPS,
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
