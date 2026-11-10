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

"""vLLM integration tests: MemCache TTFT under full miss, partial hit, and full hit.

Three test scenarios (each with 1-rank, 2-rank and 4-rank variants):

  1. Full miss (baseline)  — all unique prompts; also compares with/without MemCache overhead
  2. 50% hit               — half prompts pre-populated in MemCache, mixed hit/miss
  3. 100% hit              — prompt pre-populated, re-sent for full cache hit

Default spec: prefill 4096 tokens, decode 1536 tokens.
Adjust via environment variables:
  MMC_TEST_PROMPT_TOKENS     (default 4096)  — prefill length
  MMC_TEST_PREFIX_HIT_TOKENS (default 2048)  — cached prefix length
  MMC_TEST_DECODE_MAX_TOKENS (default 1536)  — decode length

The TP size and device IDs are controlled by environment variables:

  MMC_TEST_DEVICE_IDS=0 MMC_TEST_TENSOR_PARALLEL_SIZE=1      -> 1-rank
  MMC_TEST_DEVICE_IDS=0,1 MMC_TEST_TENSOR_PARALLEL_SIZE=2    -> 2-rank
  MMC_TEST_DEVICE_IDS=0,1,2,3 MMC_TEST_TENSOR_PARALLEL_SIZE=4 -> 4-rank

For 1-rank tests, use ``pytest -m rank1`` to select the 1-rank-only subset.
For 4-rank tests, use ``pytest -m rank4`` to select the 4-rank-only subset.

Cache Isolation Methodology
---------------------------
Local prefix caching stays enabled so original vLLM 0.24 can use Mamba align
checkpoints. External-cache latency measurements reset only the local prefix
cache after each completed request and verify the engine reset acknowledgement.
The separate cache-source correctness test retains local state deliberately and
checks local, external, and mixed hits using metrics and baseline token IDs.

Requirements: Ascend NPU + CANN, vLLM-Ascend environment, MetaService running
(for MemCache mode), and a W8A8 quantized model available for serving.
"""

import http.client
import json
import multiprocessing
import os
import random
import re
import signal
import socket
import subprocess
import time
from typing import Any, Dict, List, Optional

import pytest

# ---------------------------------------------------------------------------
# Test parameters (defaults: small values for quick verification)
# ---------------------------------------------------------------------------
PROMPT_TOKENS = int(os.environ.get("MMC_TEST_PROMPT_TOKENS", "4096"))
PREFIX_HIT_TOKENS = int(os.environ.get("MMC_TEST_PREFIX_HIT_TOKENS", "2048"))
SUFFIX_TOKENS = PROMPT_TOKENS - PREFIX_HIT_TOKENS
DECODE_MAX_TOKENS = int(os.environ.get("MMC_TEST_DECODE_MAX_TOKENS", "1536"))
MAX_TOKENS_TTFT = 1

WARMUP_REQUESTS = 1
MEASURED_REQUESTS = 10

VLLM_READY_TIMEOUT_S = 600
VLLM_STOP_TIMEOUT_S = 60
TTFT_REQUEST_TIMEOUT_S = 300

# Thresholds
MEMCACHE_OVERHEAD_RATIO = 0.2
PREFIX_CACHE_HIT_RATIO = 0.7
HIT_TTFT_THRESHOLD_MS = 1500.0

VOCAB_SIZE = 151936

# ---------------------------------------------------------------------------
# Environment defaults
# ---------------------------------------------------------------------------
DEFAULT_MODEL_PATH = "/workspace/models/Qwen3.5-4B-W8A8-310P"
DEFAULT_SERVED_MODEL_NAME = "Qwen3.5-4B"
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
# VLLMBenchmark
# ---------------------------------------------------------------------------
class VLLMBenchmark:
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
        if self._process is not None:
            try:
                os.killpg(self._process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                self._process.wait(timeout=VLLM_STOP_TIMEOUT_S)
            except subprocess.TimeoutExpired:
                os.killpg(self._process.pid, signal.SIGKILL)
                self._process.wait(timeout=VLLM_STOP_TIMEOUT_S)
            self._process = None
        if hasattr(self, "_log_file") and self._log_file is not None:
            self._log_file.close()
            self._log_file = None

    def _build_serve_cmd(self, with_memcache: bool) -> list:
        cmd = [
            "vllm",
            "serve",
            self._model_path,
            "--host",
            "127.0.0.1",
            "--port",
            str(self._port),
            "--tensor-parallel-size",
            str(self._tensor_parallel_size),
            "--quantization",
            "ascend",
            "--served-model-name",
            self._served_model_name,
            "--trust-remote-code",
            "--limit-mm-per-prompt",
            '{"image": 0, "video": 0}',
            "--dtype",
            "float16",
            "--max-num-seqs",
            "10",
            "--max-num-batched-tokens",
            "4096",
            "--max-model-len",
            "8192",
            "--gpu-memory-utilization",
            os.environ.get("MMC_TEST_GPU_MEM_UTIL", "0.80"),
            "--enforce-eager",
            "--enable-chunked-prefill",
            "--mamba-ssm-cache-dtype",
            "float16",
            "--mamba-cache-mode",
            "align",
            "--additional-config",
            ADDITIONAL_CONFIG_JSON,
        ]
        cmd.append("--enable-prefix-caching")
        if with_memcache:
            cmd.extend(["--kv-transfer-config", KV_TRANSFER_CONFIG_MEMCACHE])
        return cmd

    def _wait_for_device_memory(self, timeout: int = 120):
        device_ids = {int(device) for device in self._device_ids.split(",")}
        deadline = time.monotonic() + timeout
        pattern = re.compile(r"^\|\s*\d+\s+(\d+)\s*\|.*\|\s*\d+\s+(\d+)\s*/\s*(\d+)\s*\|")
        while time.monotonic() < deadline:
            result = subprocess.run(["npu-smi", "info"], capture_output=True, text=True, timeout=15, check=True)
            memory = {}
            for line in result.stdout.splitlines():
                match = pattern.match(line)
                if match:
                    device, used, total = map(int, match.groups())
                    if device in device_ids:
                        memory[device] = (used, total)
            if set(memory) != device_ids:
                raise RuntimeError(f"npu-smi did not report memory for devices {sorted(device_ids)}: {memory}")
            if all(used <= total * 0.25 for used, total in memory.values()):
                return
            time.sleep(2)
        raise RuntimeError(f"NPU memory did not return to an idle level: {memory}")

    def _start_vllm(self, cmd: list):
        self.stop_vllm()
        self._wait_for_device_memory()
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
                    env["LD_LIBRARY_PATH"] = env.get("LD_LIBRARY_PATH", "") + ":" + pkg_lib
        except ImportError:
            pass
        env["VLLM_SERVER_DEV_MODE"] = "1"
        print(f"[INFO] Starting vLLM: {' '.join(cmd[:6])}... (devices={self._device_ids})")
        print(f"[INFO] MMC_LOCAL_CONFIG_PATH={env['MMC_LOCAL_CONFIG_PATH']}")
        log_path = f"/tmp/vllm_integration_{int(time.time())}.log"
        print(f"[INFO] vLLM log: {log_path}")
        log_file = open(log_path, "w")
        self._process = subprocess.Popen(
            cmd,
            stdout=log_file,
            stderr=subprocess.STDOUT,
            env=env,
            start_new_session=True,
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
    # TTFT measurement (streaming API, single token)
    # ------------------------------------------------------------------
    def measure_e2e_ttft_ms(self, prompt_ids: List[int], max_tokens: int = MAX_TOKENS_TTFT) -> float:
        body = json.dumps(
            {
                "model": self._served_model_name,
                "prompt": prompt_ids,
                "max_tokens": max_tokens,
                "stream": True,
                "return_token_ids": True,
                "temperature": 0,
                "ignore_eos": True,
            }
        )
        conn = http.client.HTTPConnection("127.0.0.1", self._port, timeout=TTFT_REQUEST_TIMEOUT_S)
        try:
            start = time.perf_counter()
            conn.request("POST", "/v1/completions", body=body, headers={"Content-Type": "application/json"})
            resp = conn.getresponse()
            if resp.status != 200:
                detail = resp.read().decode(errors="replace")
                raise RuntimeError(f"completion request failed: HTTP {resp.status}: {detail}")
            ttft_ms = 0.0
            for line in resp:
                if not line.startswith(b"data: ") or b"[DONE]" in line:
                    continue
                event = json.loads(line[6:])
                if "error" in event:
                    raise RuntimeError(f"completion stream error: {event['error']}")
                choices = event.get("choices", [])
                if choices and (choices[0].get("token_ids") or choices[0].get("text")):
                    ttft_ms = (time.perf_counter() - start) * 1000.0
                    break
            else:
                raise RuntimeError("completion stream ended without a token")
            resp.close()
            self.reset_local_prefix_cache()
            return ttft_ms
        finally:
            conn.close()

    def completion_tokens(self, prompt: str, reset_local: bool = True) -> List[int]:
        body = json.dumps(
            {
                "model": self._served_model_name,
                "prompt": prompt,
                "max_tokens": 8,
                "temperature": 0,
                "ignore_eos": False,
                "return_token_ids": True,
            }
        )
        conn = http.client.HTTPConnection("127.0.0.1", self._port, timeout=TTFT_REQUEST_TIMEOUT_S)
        try:
            conn.request("POST", "/v1/completions", body=body, headers={"Content-Type": "application/json"})
            resp = conn.getresponse()
            payload = resp.read()
            if resp.status != 200:
                raise RuntimeError(f"completion request failed: HTTP {resp.status}: {payload.decode(errors='replace')}")
            token_ids = json.loads(payload)["choices"][0]["token_ids"]
            if reset_local:
                self.reset_local_prefix_cache()
            return token_ids
        finally:
            conn.close()

    def reset_local_prefix_cache(self):
        # A completed HTTP response may precede final request cleanup in the engine.
        time.sleep(0.2)
        offset = os.path.getsize(self._log_path)
        conn = http.client.HTTPConnection("127.0.0.1", self._port, timeout=30)
        try:
            conn.request("POST", "/reset_prefix_cache?reset_external=false")
            resp = conn.getresponse()
            payload = resp.read()
            if resp.status != 200:
                raise RuntimeError(f"local prefix reset failed: HTTP {resp.status}: {payload!r}")
        finally:
            conn.close()
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            with open(self._log_path, "rb") as log_file:
                log_file.seek(offset)
                if b"Successfully reset prefix cache" in log_file.read():
                    return
            time.sleep(0.1)
        raise RuntimeError("Engine did not confirm local prefix cache reset")

    def cache_hit_counts(self) -> tuple[float, float]:
        conn = http.client.HTTPConnection("127.0.0.1", self._port, timeout=30)
        try:
            conn.request("GET", "/metrics")
            resp = conn.getresponse()
            payload = resp.read().decode()
            if resp.status != 200:
                raise RuntimeError(f"metrics request failed: HTTP {resp.status}")
        finally:
            conn.close()
        counts = []
        for name in ("vllm:prefix_cache_hits_total", "vllm:external_prefix_cache_hits_total"):
            counts.append(
                sum(
                    float(line.split()[-1])
                    for line in payload.splitlines()
                    if line.startswith(name + "{") or line.startswith(name + " ")
                )
            )
        return tuple(counts)

    def check_cache_source(self, before, local: bool, external: bool):
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            after = self.cache_hit_counts()
            delta = tuple(end - start for start, end in zip(before, after))
            if (delta[0] > 0) == local and (delta[1] > 0) == external:
                print(f"Cache source verified: local_hits={delta[0]}, external_hits={delta[1]}")
                return
            time.sleep(0.5)
        raise AssertionError(f"Expected local={local}, external={external}; hit deltas={delta}")

    # ------------------------------------------------------------------
    # Multi-token latency measurement (TTFT + ITL + total)
    # ------------------------------------------------------------------
    def measure_e2e_full_latency(
        self,
        prompt_ids: List[int],
        max_tokens: int = DECODE_MAX_TOKENS,
    ) -> Dict[str, Any]:
        """Measure TTFT, per-token ITL, and total latency for multi-token generation."""
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

            token_timestamps: List[float] = []
            for line in resp:
                if line.startswith(b"data: ") and b"[DONE]" not in line:
                    token_timestamps.append(time.perf_counter())
            resp.close()

            if not token_timestamps:
                return {
                    "ttft_ms": (time.perf_counter() - start) * 1000.0,
                    "itl_ms_list": [],
                    "total_ms": (time.perf_counter() - start) * 1000.0,
                    "token_count": 0,
                }

            ttft_ms = (token_timestamps[0] - start) * 1000.0
            itl_ms_list = [
                (token_timestamps[i] - token_timestamps[i - 1]) * 1000.0 for i in range(1, len(token_timestamps))
            ]
            total_ms = (token_timestamps[-1] - start) * 1000.0
            return {
                "ttft_ms": ttft_ms,
                "itl_ms_list": itl_ms_list,
                "total_ms": total_ms,
                "token_count": len(token_timestamps),
            }
        finally:
            conn.close()

    # ------------------------------------------------------------------
    # Batch TTFT measurement
    # ------------------------------------------------------------------
    def measure_ttft_batch(
        self,
        prompts: List[List[int]],
        warmup_prompts: Optional[List[List[int]]] = None,
    ) -> Dict[str, float]:
        if warmup_prompts is None:
            warmup_prompts = prompts[:WARMUP_REQUESTS]
            measure_prompts = prompts[WARMUP_REQUESTS:]
        else:
            measure_prompts = prompts

        for i, prompt in enumerate(warmup_prompts):
            self.measure_e2e_ttft_ms(prompt)
            print(f"  warmup {i + 1}/{len(warmup_prompts)} done")

        latencies: List[float] = []
        for i, prompt in enumerate(measure_prompts):
            ttft = self.measure_e2e_ttft_ms(prompt)
            latencies.append(ttft)
            print(f"  measured {i + 1}/{len(measure_prompts)}: TTFT={ttft:.2f}ms")

        return {
            "p50_ms": _percentile(latencies, 0.50),
            "p90_ms": _percentile(latencies, 0.90),
            "p99_ms": _percentile(latencies, 0.99),
            "avg_ms": sum(latencies) / len(latencies) if latencies else 0.0,
            "min_ms": min(latencies) if latencies else 0.0,
            "max_ms": max(latencies) if latencies else 0.0,
            "count": len(latencies),
        }

    # ------------------------------------------------------------------
    # Cache stats
    # ------------------------------------------------------------------
    def get_cache_stats(self) -> Dict[str, Any]:
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
        stats = self.get_cache_stats()
        local = stats["local_hit_rate"]
        ext = stats["external_hit_rate"]
        kv = stats["gpu_kv_usage"]
        local_s = f"{local:.1f}%" if local is not None else "N/A"
        ext_s = f"{ext:.1f}%" if ext is not None else "N/A"
        kv_s = f"{kv:.1f}%" if kv is not None else "N/A"
        prefix = f"  [{label}] " if label else "  "
        print(f"{prefix}Cache stats: vLLM local hit={local_s}, MemCache external hit={ext_s}, GPU KV usage={kv_s}")
        if hasattr(self, "_log_path"):
            with open(self._log_path, encoding="utf-8") as log_file:
                log_text = log_file.read()
            assert "Failed to put" not in log_text, "MemCache reported a write failure; inspect the vLLM log"
            assert "Failed to get" not in log_text, "MemCache reported a read failure; inspect the vLLM log"


# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------
@pytest.fixture
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
        if proc.is_alive():
            proc.kill()
            proc.join(timeout=5)
    else:
        pytest.fail("MetaService port is already in use; configure an isolated test environment")


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
    yield bench
    bench.stop_vllm()


# ---------------------------------------------------------------------------
# 1. Full miss baseline (with MemCache overhead comparison)
# ---------------------------------------------------------------------------
@pytest.mark.rank2
def test_full_miss_baseline(benchmark):
    """Full miss baseline: compare TTFT with and without MemCache.

    Phase 1: vLLM baseline (no MemCache) — all unique prompts, measure TTFT.
    Phase 2: vLLM with MemCache — all unique prompts, measure TTFT.
    Asserts MemCache overhead < threshold.
    """
    baseline_result = None
    memcache_result = None

    try:
        print("\n===== Phase 1: Baseline vLLM (no MemCache) =====")
        benchmark.start_vllm_baseline()
        warmup = [_build_prompt_ids(PROMPT_TOKENS, seed=10_000_000 + i) for i in range(WARMUP_REQUESTS)]
        measure = [_build_prompt_ids(PROMPT_TOKENS, seed=i) for i in range(MEASURED_REQUESTS)]
        baseline_result = benchmark.measure_ttft_batch(measure, warmup_prompts=warmup)
        print(f"Baseline: {baseline_result}")
        benchmark.print_cache_stats("Baseline")
    finally:
        benchmark.stop_vllm()

    try:
        print("\n===== Phase 2: vLLM with MemCache =====")
        benchmark.start_vllm_with_memcache()
        warmup = [_build_prompt_ids(PROMPT_TOKENS, seed=10_000_000 + i) for i in range(WARMUP_REQUESTS)]
        measure = [_build_prompt_ids(PROMPT_TOKENS, seed=i) for i in range(MEASURED_REQUESTS)]
        memcache_result = benchmark.measure_ttft_batch(measure, warmup_prompts=warmup)
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
        f"\n===== Full Miss Baseline Comparison =====\n"
        f"  Baseline P50={baseline_p50:.2f}ms P99={baseline_result['p99_ms']:.2f}ms\n"
        f"  MemCache P50={memcache_p50:.2f}ms P99={memcache_result['p99_ms']:.2f}ms\n"
        f"  Overhead={overhead:.2%} (threshold: {MEMCACHE_OVERHEAD_RATIO:.0%})"
    )

    assert overhead < MEMCACHE_OVERHEAD_RATIO, (
        f"MemCache E2E TTFT overhead {overhead:.2%} exceeds {MEMCACHE_OVERHEAD_RATIO:.0%} threshold"
    )


# ---------------------------------------------------------------------------
# 2. 50% hit rate
# ---------------------------------------------------------------------------
@pytest.mark.rank2
def test_50pct_hit(benchmark):
    """50% cache hit rate: half prompts pre-populated in MemCache.

    Pre-populates MemCache with 50% of prompts, then sends a shuffled mix
    of cache-hit (repeated) and cache-miss (unique) requests.
    Compares hit vs miss TTFT to verify MemCache provides reduction.
    """
    try:
        print("\n===== Starting vLLM with MemCache for 50% hit test =====")
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
            f"\n===== 50% Hit Rate E2E TTFT =====\n"
            f"  Expected hit/total: {hit_count}/{num_requests} (50%)\n"
            f"  Actual hit (TTFT<{HIT_TTFT_THRESHOLD_MS:.0f}ms): {actual_hit_count}/{hit_count}\n"
            f"  All   avg={all_avg:.2f}ms P50={all_p50:.2f}ms P99={all_p99:.2f}ms\n"
            f"  Hit   avg={hit_avg:.2f}ms P50={hit_p50:.2f}ms (count={len(hit_latencies)})\n"
            f"  Miss  avg={miss_avg:.2f}ms P50={miss_p50:.2f}ms (count={len(miss_latencies)})\n"
            f"  Improvement (avg): {improvement:.2%}"
        )

        benchmark.print_cache_stats("50% Hit")

        assert actual_hit_count >= hit_count * 0.5, (
            f"Only {actual_hit_count}/{hit_count} expected hits had TTFT < {HIT_TTFT_THRESHOLD_MS:.0f}ms"
        )
        assert hit_p50 < miss_p50 * PREFIX_CACHE_HIT_RATIO, (
            f"Hit P50 {hit_p50:.2f}ms >= miss P50 * {PREFIX_CACHE_HIT_RATIO} "
            f"({miss_p50 * PREFIX_CACHE_HIT_RATIO:.2f}ms)"
        )
    finally:
        benchmark.stop_vllm()


# ---------------------------------------------------------------------------
# 3. 100% hit rate
# ---------------------------------------------------------------------------
@pytest.mark.rank2
def test_100pct_hit(benchmark):
    """100% cache hit rate: prompt pre-populated, re-sent for full hit.

    For each measured request pair:
      1. Send a unique prompt (cache miss) -> stores KV in MemCache.
      2. Re-send the same prompt (cache hit from MemCache).
    Compares hit vs miss TTFT to verify MemCache provides reduction.
    """
    try:
        print("\n===== Starting vLLM with MemCache for 100% hit test =====")
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

        check_prompt = (
            "Paris is the capital of France. Berlin is the capital of Germany. " * 400
            + "\nQuestion: What is 2 + 2? Answer:"
        )
        miss_tokens = benchmark.completion_tokens(check_prompt)
        time.sleep(1)
        hit_tokens = benchmark.completion_tokens(check_prompt)
        assert miss_tokens and hit_tokens == miss_tokens, "Cached completion differs from the original completion"

        benchmark.print_cache_stats("100% Hit")

        miss_p50 = _percentile(miss_latencies, 0.50)
        hit_p50 = _percentile(hit_latencies, 0.50)
        miss_p99 = _percentile(miss_latencies, 0.99)
        hit_p99 = _percentile(hit_latencies, 0.99)
        ratio = hit_p50 / miss_p50 if miss_p50 > 0 else 1.0

        print(
            f"\n===== 100% Hit Rate E2E TTFT =====\n"
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
# 1-rank (TP=1) variants
# ---------------------------------------------------------------------------
# Identical logic, separate naming for report clarity.
#
# Run with:
#   MMC_TEST_DEVICE_IDS=0 MMC_TEST_TENSOR_PARALLEL_SIZE=1 pytest -m rank1


@pytest.mark.rank1
def test_full_miss_baseline_1rank(benchmark):
    """1-rank (TP=1): Full miss baseline with MemCache overhead comparison."""
    baseline_result = None
    memcache_result = None

    try:
        print("\n===== [1-Rank] Phase 1: Baseline vLLM (no MemCache) =====")
        benchmark.start_vllm_baseline()
        warmup = [_build_prompt_ids(PROMPT_TOKENS, seed=10_000_000 + i) for i in range(WARMUP_REQUESTS)]
        measure = [_build_prompt_ids(PROMPT_TOKENS, seed=i) for i in range(MEASURED_REQUESTS)]
        baseline_result = benchmark.measure_ttft_batch(measure, warmup_prompts=warmup)
        print(f"Baseline: {baseline_result}")
        benchmark.print_cache_stats("1-Rank Baseline")
    finally:
        benchmark.stop_vllm()

    try:
        print("\n===== [1-Rank] Phase 2: vLLM with MemCache =====")
        benchmark.start_vllm_with_memcache()
        warmup = [_build_prompt_ids(PROMPT_TOKENS, seed=10_000_000 + i) for i in range(WARMUP_REQUESTS)]
        measure = [_build_prompt_ids(PROMPT_TOKENS, seed=i) for i in range(MEASURED_REQUESTS)]
        memcache_result = benchmark.measure_ttft_batch(measure, warmup_prompts=warmup)
        print(f"MemCache: {memcache_result}")
        benchmark.print_cache_stats("1-Rank MemCache")
    finally:
        benchmark.stop_vllm()

    assert baseline_result is not None, "baseline benchmark failed"
    assert memcache_result is not None, "memcache benchmark failed"

    baseline_p50 = baseline_result["p50_ms"]
    memcache_p50 = memcache_result["p50_ms"]
    overhead = (memcache_p50 - baseline_p50) / baseline_p50 if baseline_p50 > 0 else 1.0

    print(
        f"\n===== [1-Rank] Full Miss Baseline Comparison =====\n"
        f"  Baseline P50={baseline_p50:.2f}ms P99={baseline_result['p99_ms']:.2f}ms\n"
        f"  MemCache P50={memcache_p50:.2f}ms P99={memcache_result['p99_ms']:.2f}ms\n"
        f"  Overhead={overhead:.2%} (threshold: {MEMCACHE_OVERHEAD_RATIO:.0%})"
    )

    assert overhead < MEMCACHE_OVERHEAD_RATIO, (
        f"MemCache E2E TTFT overhead {overhead:.2%} exceeds {MEMCACHE_OVERHEAD_RATIO:.0%} threshold"
    )


@pytest.mark.rank1
def test_50pct_hit_1rank(benchmark):
    """1-rank (TP=1): 50% cache hit rate."""
    try:
        print("\n===== [1-Rank] Starting vLLM with MemCache for 50% hit test =====")
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
            f"\n===== [1-Rank] 50% Hit Rate E2E TTFT =====\n"
            f"  Expected hit/total: {hit_count}/{num_requests} (50%)\n"
            f"  Actual hit (TTFT<{HIT_TTFT_THRESHOLD_MS:.0f}ms): {actual_hit_count}/{hit_count}\n"
            f"  All   avg={all_avg:.2f}ms P50={all_p50:.2f}ms P99={all_p99:.2f}ms\n"
            f"  Hit   avg={hit_avg:.2f}ms P50={hit_p50:.2f}ms (count={len(hit_latencies)})\n"
            f"  Miss  avg={miss_avg:.2f}ms P50={miss_p50:.2f}ms (count={len(miss_latencies)})\n"
            f"  Improvement (avg): {improvement:.2%}"
        )

        benchmark.print_cache_stats("1-Rank 50% Hit")

        assert actual_hit_count >= hit_count * 0.5, (
            f"Only {actual_hit_count}/{hit_count} expected hits had TTFT < {HIT_TTFT_THRESHOLD_MS:.0f}ms"
        )
        assert hit_p50 < miss_p50 * PREFIX_CACHE_HIT_RATIO, (
            f"Hit P50 {hit_p50:.2f}ms >= miss P50 * {PREFIX_CACHE_HIT_RATIO} "
            f"({miss_p50 * PREFIX_CACHE_HIT_RATIO:.2f}ms)"
        )
    finally:
        benchmark.stop_vllm()


@pytest.mark.rank1
def test_100pct_hit_1rank(benchmark):
    """1-rank (TP=1): 100% cache hit rate."""
    try:
        print("\n===== [1-Rank] Starting vLLM with MemCache for 100% hit test =====")
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
            f"\n===== [1-Rank] 100% Hit Rate E2E TTFT =====\n"
            f"  Cache miss P50={miss_p50:.2f}ms P99={miss_p99:.2f}ms\n"
            f"  Cache hit  P50={hit_p50:.2f}ms P99={hit_p99:.2f}ms\n"
            f"  Hit/Miss ratio={ratio:.2%} (threshold: < {PREFIX_CACHE_HIT_RATIO:.0%})"
        )

        check_prompt = (
            "Paris is the capital of France. Berlin is the capital of Germany. " * 400
            + "\nQuestion: What is 2 + 2? Answer:"
        )
        miss_tokens = benchmark.completion_tokens(check_prompt)
        time.sleep(1)
        hit_tokens = benchmark.completion_tokens(check_prompt)
        assert miss_tokens and hit_tokens == miss_tokens, "Cached completion differs from the original completion"

        benchmark.print_cache_stats("1-Rank 100% Hit")

        assert hit_p50 < miss_p50 * PREFIX_CACHE_HIT_RATIO, (
            f"Cache hit P50 {hit_p50:.2f}ms >= miss P50 * {PREFIX_CACHE_HIT_RATIO} "
            f"({miss_p50 * PREFIX_CACHE_HIT_RATIO:.2f}ms)"
        )
    finally:
        benchmark.stop_vllm()


# ---------------------------------------------------------------------------
# 4-rank (TP=4) variants
# ---------------------------------------------------------------------------
# Identical logic, separate naming for report clarity.
#
# Run with:
#   MMC_TEST_DEVICE_IDS=0,1,2,3 MMC_TEST_TENSOR_PARALLEL_SIZE=4 pytest -m rank4


@pytest.mark.rank4
def test_full_miss_baseline_4rank(benchmark):
    """4-rank (TP=4): Full miss baseline with MemCache overhead comparison."""
    baseline_result = None
    memcache_result = None

    try:
        print("\n===== [4-Rank] Phase 1: Baseline vLLM (no MemCache) =====")
        benchmark.start_vllm_baseline()
        warmup = [_build_prompt_ids(PROMPT_TOKENS, seed=10_000_000 + i) for i in range(WARMUP_REQUESTS)]
        measure = [_build_prompt_ids(PROMPT_TOKENS, seed=i) for i in range(MEASURED_REQUESTS)]
        baseline_result = benchmark.measure_ttft_batch(measure, warmup_prompts=warmup)
        print(f"Baseline: {baseline_result}")
        benchmark.print_cache_stats("4-Rank Baseline")
    finally:
        benchmark.stop_vllm()

    try:
        print("\n===== [4-Rank] Phase 2: vLLM with MemCache =====")
        benchmark.start_vllm_with_memcache()
        warmup = [_build_prompt_ids(PROMPT_TOKENS, seed=10_000_000 + i) for i in range(WARMUP_REQUESTS)]
        measure = [_build_prompt_ids(PROMPT_TOKENS, seed=i) for i in range(MEASURED_REQUESTS)]
        memcache_result = benchmark.measure_ttft_batch(measure, warmup_prompts=warmup)
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
        f"\n===== [4-Rank] Full Miss Baseline Comparison =====\n"
        f"  Baseline P50={baseline_p50:.2f}ms P99={baseline_result['p99_ms']:.2f}ms\n"
        f"  MemCache P50={memcache_p50:.2f}ms P99={memcache_result['p99_ms']:.2f}ms\n"
        f"  Overhead={overhead:.2%} (threshold: {MEMCACHE_OVERHEAD_RATIO:.0%})"
    )

    assert overhead < MEMCACHE_OVERHEAD_RATIO, (
        f"MemCache E2E TTFT overhead {overhead:.2%} exceeds {MEMCACHE_OVERHEAD_RATIO:.0%} threshold"
    )


@pytest.mark.rank4
def test_50pct_hit_4rank(benchmark):
    """4-rank (TP=4): 50% cache hit rate."""
    try:
        print("\n===== [4-Rank] Starting vLLM with MemCache for 50% hit test =====")
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
            f"\n===== [4-Rank] 50% Hit Rate E2E TTFT =====\n"
            f"  Expected hit/total: {hit_count}/{num_requests} (50%)\n"
            f"  Actual hit (TTFT<{HIT_TTFT_THRESHOLD_MS:.0f}ms): {actual_hit_count}/{hit_count}\n"
            f"  All   avg={all_avg:.2f}ms P50={all_p50:.2f}ms P99={all_p99:.2f}ms\n"
            f"  Hit   avg={hit_avg:.2f}ms P50={hit_p50:.2f}ms (count={len(hit_latencies)})\n"
            f"  Miss  avg={miss_avg:.2f}ms P50={miss_p50:.2f}ms (count={len(miss_latencies)})\n"
            f"  Improvement (avg): {improvement:.2%}"
        )

        benchmark.print_cache_stats("4-Rank 50% Hit")

        assert actual_hit_count >= hit_count * 0.5, (
            f"Only {actual_hit_count}/{hit_count} expected hits had TTFT < {HIT_TTFT_THRESHOLD_MS:.0f}ms"
        )
        assert hit_p50 < miss_p50 * PREFIX_CACHE_HIT_RATIO, (
            f"Hit P50 {hit_p50:.2f}ms >= miss P50 * {PREFIX_CACHE_HIT_RATIO} "
            f"({miss_p50 * PREFIX_CACHE_HIT_RATIO:.2f}ms)"
        )
    finally:
        benchmark.stop_vllm()


@pytest.mark.rank4
def test_100pct_hit_4rank(benchmark):
    """4-rank (TP=4): 100% cache hit rate."""
    try:
        print("\n===== [4-Rank] Starting vLLM with MemCache for 100% hit test =====")
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
            f"\n===== [4-Rank] 100% Hit Rate E2E TTFT =====\n"
            f"  Cache miss P50={miss_p50:.2f}ms P99={miss_p99:.2f}ms\n"
            f"  Cache hit  P50={hit_p50:.2f}ms P99={hit_p99:.2f}ms\n"
            f"  Hit/Miss ratio={ratio:.2%} (threshold: < {PREFIX_CACHE_HIT_RATIO:.0%})"
        )

        check_prompt = (
            "Paris is the capital of France. Berlin is the capital of Germany. " * 400
            + "\nQuestion: What is 2 + 2? Answer:"
        )
        miss_tokens = benchmark.completion_tokens(check_prompt)
        time.sleep(1)
        hit_tokens = benchmark.completion_tokens(check_prompt)
        assert miss_tokens and hit_tokens == miss_tokens, "Cached completion differs from the original completion"

        benchmark.print_cache_stats("4-Rank 100% Hit")

        assert hit_p50 < miss_p50 * PREFIX_CACHE_HIT_RATIO, (
            f"Cache hit P50 {hit_p50:.2f}ms >= miss P50 * {PREFIX_CACHE_HIT_RATIO} "
            f"({miss_p50 * PREFIX_CACHE_HIT_RATIO:.2f}ms)"
        )
    finally:
        benchmark.stop_vllm()


@pytest.mark.rank1
@pytest.mark.rank2
@pytest.mark.rank4
def test_cache_source_correctness(benchmark):
    """Compare local, external and mixed cache hits with an independent model run."""
    from transformers import AutoTokenizer

    tokenizer = AutoTokenizer.from_pretrained(benchmark._model_path, trust_remote_code=True)
    sentence = "Paris is the capital of France. Berlin is the capital of Germany. "
    user_short = sentence * 200 + "\nCopy this sentence exactly: The sum of two and two is four."
    user_long = sentence * 200 + "\nCopy this sentence exactly: The sum of three and three is six."
    messages = [{"role": "user", "content": user_short}]
    short_prompt = tokenizer.apply_chat_template(
        messages, tokenize=False, add_generation_prompt=True, enable_thinking=False
    )
    long_prompt = tokenizer.apply_chat_template(
        messages
        + [{"role": "assistant", "content": "The sum of two and two is four."}, {"role": "user", "content": user_long}],
        tokenize=False,
        add_generation_prompt=True,
        enable_thinking=False,
    )
    try:
        benchmark.start_vllm_baseline()
        short_reference = benchmark.completion_tokens(short_prompt)
        long_reference = benchmark.completion_tokens(long_prompt)
        assert short_reference and long_reference
        long_repeat = benchmark.completion_tokens(long_prompt)
        benchmark.completion_tokens(short_prompt, reset_local=False)
        long_local_reference = benchmark.completion_tokens(long_prompt)
        print(
            f"Reference tokens: short={short_reference}, long={long_reference}, "
            f"long_repeat={long_repeat}, long_local={long_local_reference}"
        )
        observed = []
        benchmark.stop_vllm()
        benchmark.start_vllm_with_memcache()
        # Align-mode Mamba checkpoints are saved at completed prefill boundaries.
        # Seed both boundaries instead of assuming the long prompt stores the short one.
        observed.append(("seed-short", benchmark.completion_tokens(short_prompt), short_reference))
        observed.append(("seed-long", benchmark.completion_tokens(long_prompt), long_reference))

        before = benchmark.cache_hit_counts()
        observed.append(("short-hit", benchmark.completion_tokens(short_prompt, reset_local=False), short_reference))
        benchmark.check_cache_source(before, local=False, external=True)

        before = benchmark.cache_hit_counts()
        observed.append(("short-hit", benchmark.completion_tokens(short_prompt, reset_local=False), short_reference))
        benchmark.check_cache_source(before, local=True, external=False)

        before = benchmark.cache_hit_counts()
        observed.append(("mixed-hit", benchmark.completion_tokens(long_prompt, reset_local=False), long_reference))
        benchmark.check_cache_source(before, local=True, external=True)
        benchmark.print_cache_stats("Cache source correctness")
        print(f"Observed cache tokens: {observed}")
        assert long_repeat == long_reference, "Uncached model output is not repeatable"
        assert long_local_reference == long_reference, "Local prefix reuse changed baseline output"
        for stage, actual, expected in observed:
            assert actual == expected, f"{stage}: actual={actual}, expected={expected}"
    finally:
        benchmark.stop_vllm()
