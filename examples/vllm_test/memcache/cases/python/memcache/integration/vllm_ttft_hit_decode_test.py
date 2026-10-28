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

"""vLLM TTFT tests for partial prefix hit (2k) and long decode (1.5k) scenarios.

Test specification: TP=2, prefill 4k, hit 2k, decode 1.5k

Four test scenarios:
  H-1  Partial prefix hit 2k TTFT        — 4k prompt, 2k prefix cached, 2k new, max_tokens=1
  H-2  Decode 1.5k full miss TTFT+ITL    — 4k unique prompt, max_tokens=1536
  H-3  Decode 1.5k full hit TTFT+ITL    — 4k cached prompt, max_tokens=1536
  H-4  Decode 1.5k partial hit 2k       — 4k prompt (2k prefix + 2k new), max_tokens=1536

Test design:
  - Prefill 4k:  PROMPT_TOKENS = 4096
  - Hit 2k:      first 2048 tokens share a cached prefix, last 2048 tokens are unique
  - Decode 1.5k: max_tokens = 1536, measure TTFT + ITL + total latency

Cache isolation:
  Same as vllm_ttft_test.py — reset_local_prefix_cache() clears vLLM's
  internal block cache, forcing hit requests through the MemCache connector.
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
from typing import Any, Dict, List, Optional, Tuple

import pytest

# ---------------------------------------------------------------------------
# Test parameters
# ---------------------------------------------------------------------------
PROMPT_TOKENS = 4096
PREFIX_HIT_TOKENS = 2048
SUFFIX_TOKENS = PROMPT_TOKENS - PREFIX_HIT_TOKENS
DECODE_MAX_TOKENS = 1536
MAX_TOKENS_TTFT = 1
WARMUP_REQUESTS = 1
MEASURED_REQUESTS_TTFT = 10
MEASURED_REQUESTS_DECODE = 5
VLLM_READY_TIMEOUT_S = 600
VLLM_STOP_TIMEOUT_S = 60
TTFT_REQUEST_TIMEOUT_S = 300

PARTIAL_HIT_TTFT_RATIO = 0.80
PARTIAL_HIT_IMPROVEMENT_MIN = 0.15
DECODE_ITL_P99_P50_RATIO = 3.0
DECODE_THROUGHPUT_MIN_TPS = 15.0
FULL_HIT_DECODE_TTFT_RATIO = 0.30

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


def _build_partial_prefix_prompt(
    prefix_len: int,
    suffix_len: int,
    prefix_seed: int,
    suffix_seed: int,
) -> List[int]:
    prefix_ids = _build_prompt_ids(prefix_len, seed=prefix_seed)
    suffix_ids = _build_prompt_ids(suffix_len, seed=suffix_seed)
    return prefix_ids + suffix_ids


# ---------------------------------------------------------------------------
# HitDecodeBenchmark
# ---------------------------------------------------------------------------
class HitDecodeBenchmark:
    """Manage vLLM serve lifecycle and run TTFT/ITL benchmarks."""

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
        log_path = f"/tmp/vllm_hit_decode_{int(time.time())}.log"
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
    # TTFT measurement (streaming API, single token)
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

    # ------------------------------------------------------------------
    # Multi-token latency measurement (TTFT + ITL + total)
    # ------------------------------------------------------------------
    def measure_e2e_full_latency(
        self,
        prompt_ids: List[int],
        max_tokens: int = DECODE_MAX_TOKENS,
    ) -> Dict[str, Any]:
        """Measure TTFT, per-token ITL, and total latency for multi-token generation.

        Returns a dict with:
          - ttft_ms: time to first token
          - itl_ms_list: list of inter-token latencies (max_tokens - 1 values)
          - total_ms: time from request to last token
          - token_count: actual number of tokens generated
        """
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
                (token_timestamps[i] - token_timestamps[i - 1]) * 1000.0
                for i in range(1, len(token_timestamps))
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
    # Cache control
    # ------------------------------------------------------------------
    def reset_local_prefix_cache(self):
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
    # Batch decode latency measurement
    # ------------------------------------------------------------------
    def measure_decode_batch(
        self,
        prompts: List[List[int]],
        warmup_prompts: Optional[List[List[int]]] = None,
        max_tokens: int = DECODE_MAX_TOKENS,
    ) -> Dict[str, Any]:
        if warmup_prompts is None:
            warmup_prompts = prompts[:WARMUP_REQUESTS]
            measure_prompts = prompts[WARMUP_REQUESTS:]
        else:
            measure_prompts = prompts

        for i, prompt in enumerate(warmup_prompts):
            self.measure_e2e_full_latency(prompt, max_tokens=max_tokens)
            print(f"  warmup {i + 1}/{len(warmup_prompts)} done")

        all_ttft: List[float] = []
        all_itl: List[float] = []
        all_total: List[float] = []
        valid_count = 0
        for i, prompt in enumerate(measure_prompts):
            result = self.measure_e2e_full_latency(prompt, max_tokens=max_tokens)
            print(
                f"  measured {i + 1}/{len(measure_prompts)}: "
                f"TTFT={result['ttft_ms']:.2f}ms, "
                f"tokens={result['token_count']}, "
                f"total={result['total_ms']:.2f}ms"
            )
            if result["token_count"] < max_tokens * 0.9:
                print(f"  [WARN] token_count={result['token_count']} < {max_tokens}×0.9, skipping")
                continue
            valid_count += 1
            all_ttft.append(result["ttft_ms"])
            all_itl.extend(result["itl_ms_list"])
            all_total.append(result["total_ms"])

        decode_tps = []
        for total_ms in all_total:
            if total_ms > 0:
                decode_tps.append(max_tokens / (total_ms / 1000.0))

        return {
            "ttft_p50_ms": _percentile(all_ttft, 0.50),
            "ttft_p90_ms": _percentile(all_ttft, 0.90),
            "ttft_p99_ms": _percentile(all_ttft, 0.99),
            "ttft_avg_ms": sum(all_ttft) / len(all_ttft) if all_ttft else 0.0,
            "itl_p50_ms": _percentile(all_itl, 0.50) if all_itl else 0.0,
            "itl_p90_ms": _percentile(all_itl, 0.90) if all_itl else 0.0,
            "itl_p99_ms": _percentile(all_itl, 0.99) if all_itl else 0.0,
            "itl_avg_ms": sum(all_itl) / len(all_itl) if all_itl else 0.0,
            "total_p50_ms": _percentile(all_total, 0.50),
            "total_avg_ms": sum(all_total) / len(all_total) if all_total else 0.0,
            "decode_tps_avg": sum(decode_tps) / len(decode_tps) if decode_tps else 0.0,
            "count": len(all_ttft),
            "valid_count": valid_count,
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

    bench = HitDecodeBenchmark(
        model_path=model_path,
        served_model_name=served_name,
        port=port,
        device_ids=device_ids,
        tensor_parallel_size=tp_size,
    )
    yield bench
    bench.stop_vllm()


# ---------------------------------------------------------------------------
# H-1  Partial prefix hit 2k TTFT
# ---------------------------------------------------------------------------
@pytest.mark.hitdecode
@pytest.mark.rank2
def test_partial_prefix_hit_2k_ttft(benchmark):
    """Partial prefix hit (2k out of 4k) E2E TTFT.

    Pre-populates MemCache with a 2048-token prefix, then sends 4096-token
    requests where the first 2048 tokens match the cached prefix and the
    last 2048 tokens are unique. Only the last 2048 tokens need prefill.

    Compares partial-hit TTFT against full-miss TTFT (all 4096 unique).
    """
    try:
        print("\n===== Starting vLLM with MemCache for partial prefix hit 2k test =====")
        benchmark.start_vllm_with_memcache()

        # --- Phase 1: full miss baseline (4096 unique tokens) ---
        print("\n--- Phase 1: Full miss TTFT (4096 unique tokens) ---")
        warmup_miss = [_build_prompt_ids(PROMPT_TOKENS, seed=-(i + 1)) for i in range(WARMUP_REQUESTS)]
        measure_miss = [_build_prompt_ids(PROMPT_TOKENS, seed=i) for i in range(MEASURED_REQUESTS_TTFT)]
        miss_result = benchmark.measure_ttft_batch(measure_miss, warmup_prompts=warmup_miss)
        print(f"  Full miss: {miss_result}")

        # --- Phase 2: pre-populate prefix (2048 tokens) ---
        print("\n--- Phase 2: Pre-populate 2048-token prefix ---")
        prefix_prompt = _build_prompt_ids(PREFIX_HIT_TOKENS, seed=3000)
        benchmark.measure_e2e_ttft_ms(prefix_prompt)
        print(f"  Pre-populated prefix ({PREFIX_HIT_TOKENS} tokens, seed=3000)")
        benchmark.reset_local_prefix_cache()
        print("  Cleared vLLM local cache (MemCache retains prefix KV)")

        # --- Phase 3: partial hit (2k prefix + 2k unique suffix) ---
        print("\n--- Phase 3: Partial hit TTFT (2k prefix cached + 2k new) ---")
        warmup_partial = [
            _build_partial_prefix_prompt(PREFIX_HIT_TOKENS, SUFFIX_TOKENS, 3000, 4000 + i)
            for i in range(WARMUP_REQUESTS)
        ]
        for wp in warmup_partial:
            benchmark.measure_e2e_ttft_ms(wp)
            benchmark.reset_local_prefix_cache()
        print(f"  Warmup {WARMUP_REQUESTS} partial-hit requests done")

        partial_latencies: List[float] = []
        for i in range(MEASURED_REQUESTS_TTFT):
            prompt = _build_partial_prefix_prompt(PREFIX_HIT_TOKENS, SUFFIX_TOKENS, 3000, 5000 + i)
            ttft = benchmark.measure_e2e_ttft_ms(prompt)
            partial_latencies.append(ttft)
            benchmark.reset_local_prefix_cache()
            print(f"  measured {i + 1}/{MEASURED_REQUESTS_TTFT}: TTFT={ttft:.2f}ms")

        partial_p50 = _percentile(partial_latencies, 0.50)
        partial_p90 = _percentile(partial_latencies, 0.90)
        partial_p99 = _percentile(partial_latencies, 0.99)
        partial_avg = sum(partial_latencies) / len(partial_latencies)

        miss_p50 = miss_result["p50_ms"]
        ratio = partial_p50 / miss_p50 if miss_p50 > 0 else 1.0
        improvement = 1.0 - ratio

        print(
            f"\n===== Partial Prefix Hit 2k TTFT =====\n"
            f"  Full miss    P50={miss_p50:.2f}ms P99={miss_result['p99_ms']:.2f}ms\n"
            f"  Partial hit  P50={partial_p50:.2f}ms P90={partial_p90:.2f}ms P99={partial_p99:.2f}ms\n"
            f"  Partial avg  ={partial_avg:.2f}ms\n"
            f"  Ratio (partial/miss)={ratio:.2%}\n"
            f"  Improvement={improvement:.2%}\n"
            f"  Threshold: ratio < {PARTIAL_HIT_TTFT_RATIO:.0%}, improvement > {PARTIAL_HIT_IMPROVEMENT_MIN:.0%}"
        )

        benchmark.print_cache_stats("H-1 Partial Hit 2k")

        assert ratio < PARTIAL_HIT_TTFT_RATIO, (
            f"Partial hit P50 {partial_p50:.2f}ms >= miss P50 * {PARTIAL_HIT_TTFT_RATIO} "
            f"({miss_p50 * PARTIAL_HIT_TTFT_RATIO:.2f}ms)"
        )
        assert improvement > PARTIAL_HIT_IMPROVEMENT_MIN, (
            f"Improvement {improvement:.2%} <= min expected {PARTIAL_HIT_IMPROVEMENT_MIN:.2%}"
        )
    finally:
        benchmark.stop_vllm()


# ---------------------------------------------------------------------------
# H-2  Decode 1.5k full miss TTFT+ITL
# ---------------------------------------------------------------------------
@pytest.mark.hitdecode
@pytest.mark.rank2
def test_decode_1_5k_full_miss(benchmark):
    """Decode 1.5k tokens from a full miss (4096 unique prompt).

    Measures TTFT, ITL distribution, and total generation latency for
    1536-token generation after a full 4096-token prefill.
    """
    try:
        print("\n===== Starting vLLM with MemCache for decode 1.5k full miss test =====")
        benchmark.start_vllm_with_memcache()

        warmup_prompts = [_build_prompt_ids(PROMPT_TOKENS, seed=-(i + 1)) for i in range(WARMUP_REQUESTS)]
        measure_prompts = [_build_prompt_ids(PROMPT_TOKENS, seed=i) for i in range(MEASURED_REQUESTS_DECODE)]

        print(f"\n--- Decode 1.5k full miss: {MEASURED_REQUESTS_DECODE} requests, {DECODE_MAX_TOKENS} tokens each ---")
        result = benchmark.measure_decode_batch(measure_prompts, warmup_prompts=warmup_prompts)

        itl_p99_p50 = result["itl_p99_ms"] / result["itl_p50_ms"] if result["itl_p50_ms"] > 0 else 999.0

        print(
            f"\n===== Decode 1.5k Full Miss =====\n"
            f"  TTFT  P50={result['ttft_p50_ms']:.2f}ms P90={result['ttft_p90_ms']:.2f}ms "
            f"P99={result['ttft_p99_ms']:.2f}ms avg={result['ttft_avg_ms']:.2f}ms\n"
            f"  ITL   P50={result['itl_p50_ms']:.2f}ms P90={result['itl_p90_ms']:.2f}ms "
            f"P99={result['itl_p99_ms']:.2f}ms avg={result['itl_avg_ms']:.2f}ms\n"
            f"  Total P50={result['total_p50_ms']:.2f}ms avg={result['total_avg_ms']:.2f}ms\n"
            f"  Decode throughput={result['decode_tps_avg']:.2f} tokens/s\n"
            f"  ITL P99/P50={itl_p99_p50:.2f} (threshold: < {DECODE_ITL_P99_P50_RATIO})"
        )

        benchmark.print_cache_stats("H-2 Decode 1.5k Miss")

        assert result["count"] > 0, "No decode measurements collected"
        assert result["token_count"] > 0 if "token_count" in result else True, "No tokens generated"
        assert itl_p99_p50 < DECODE_ITL_P99_P50_RATIO, (
            f"ITL P99/P50 ratio {itl_p99_p50:.2f} >= {DECODE_ITL_P99_P50_RATIO}"
        )
    finally:
        benchmark.stop_vllm()


# ---------------------------------------------------------------------------
# H-3  Decode 1.5k full hit TTFT+ITL
# ---------------------------------------------------------------------------
@pytest.mark.hitdecode
@pytest.mark.rank2
def test_decode_1_5k_full_hit(benchmark):
    """Decode 1.5k tokens from a full cache hit (4096 cached prompt).

    Pre-populates MemCache with a 4096-token prompt, then re-sends the same
    prompt with reset_local_prefix_cache() to force MemCache hit. Measures
    TTFT, ITL, and total latency for 1536-token generation.
    """
    try:
        print("\n===== Starting vLLM with MemCache for decode 1.5k full hit test =====")
        benchmark.start_vllm_with_memcache()

        # Pre-populate: cache the full 4096-token prompt
        cached_prompt = _build_prompt_ids(PROMPT_TOKENS, seed=7000)
        warmup_prompt = _build_prompt_ids(PROMPT_TOKENS, seed=7999)
        for i in range(WARMUP_REQUESTS):
            benchmark.measure_e2e_ttft_ms(warmup_prompt)
            print(f"  warmup {i + 1}/{WARMUP_REQUESTS} done")

        # Cache the target prompt
        benchmark.measure_e2e_ttft_ms(cached_prompt)
        print(f"  Pre-populated full prompt ({PROMPT_TOKENS} tokens, seed=7000)")
        benchmark.reset_local_prefix_cache()
        print("  Cleared vLLM local cache (MemCache retains full KV)")

        # Measure: re-send cached prompt with decode 1.5k
        hit_ttft_latencies: List[float] = []
        all_results: List[Dict[str, Any]] = []
        for i in range(MEASURED_REQUESTS_DECODE):
            benchmark.reset_local_prefix_cache()
            result = benchmark.measure_e2e_full_latency(cached_prompt, max_tokens=DECODE_MAX_TOKENS)
            print(
                f"  measured {i + 1}/{MEASURED_REQUESTS_DECODE}: "
                f"TTFT={result['ttft_ms']:.2f}ms, tokens={result['token_count']}, "
                f"total={result['total_ms']:.2f}ms"
            )
            if result["token_count"] < DECODE_MAX_TOKENS * 0.9:
                print(f"  [WARN] token_count={result['token_count']} < {DECODE_MAX_TOKENS}×0.9, skipping")
                continue
            all_results.append(result)
            hit_ttft_latencies.append(result["ttft_ms"])

        all_itl: List[float] = []
        all_total: List[float] = []
        for r in all_results:
            all_itl.extend(r["itl_ms_list"])
            all_total.append(r["total_ms"])

        ttft_p50 = _percentile(hit_ttft_latencies, 0.50)
        ttft_p99 = _percentile(hit_ttft_latencies, 0.99)
        itl_p50 = _percentile(all_itl, 0.50) if all_itl else 0.0
        itl_p99 = _percentile(all_itl, 0.99) if all_itl else 0.0
        itl_avg = sum(all_itl) / len(all_itl) if all_itl else 0.0
        total_p50 = _percentile(all_total, 0.50)
        total_avg = sum(all_total) / len(all_total) if all_total else 0.0
        decode_tps = sum(
            DECODE_MAX_TOKENS / (t / 1000.0) for t in all_total if t > 0
        ) / len(all_total) if all_total else 0.0
        itl_p99_p50 = itl_p99 / itl_p50 if itl_p50 > 0 else 999.0

        print(
            f"\n===== Decode 1.5k Full Hit =====\n"
            f"  TTFT  P50={ttft_p50:.2f}ms P99={ttft_p99:.2f}ms\n"
            f"  ITL   P50={itl_p50:.2f}ms P99={itl_p99:.2f}ms avg={itl_avg:.2f}ms\n"
            f"  Total P50={total_p50:.2f}ms avg={total_avg:.2f}ms\n"
            f"  Decode throughput={decode_tps:.2f} tokens/s\n"
            f"  ITL P99/P50={itl_p99_p50:.2f} (threshold: < {DECODE_ITL_P99_P50_RATIO})"
        )

        benchmark.print_cache_stats("H-3 Decode 1.5k Hit")

        assert len(hit_ttft_latencies) > 0, "No valid hit decode measurements collected (all had insufficient tokens)"
        assert itl_p99_p50 < DECODE_ITL_P99_P50_RATIO, (
            f"ITL P99/P50 ratio {itl_p99_p50:.2f} >= {DECODE_ITL_P99_P50_RATIO}"
        )
    finally:
        benchmark.stop_vllm()


# ---------------------------------------------------------------------------
# H-4  Decode 1.5k partial hit 2k (combined scenario)
# ---------------------------------------------------------------------------
@pytest.mark.hitdecode
@pytest.mark.rank2
def test_decode_1_5k_partial_hit_2k(benchmark):
    """Decode 1.5k tokens from a partial prefix hit (2k cached + 2k new).

    Pre-populates MemCache with a 2048-token prefix, then sends 4096-token
    requests (2k prefix + 2k unique suffix) with decode 1.5k. This is the
    combined scenario: partial prefix hit + long decode.

    Compares TTFT and total latency against full miss decode 1.5k.
    """
    try:
        print("\n===== Starting vLLM with MemCache for decode 1.5k partial hit 2k test =====")
        benchmark.start_vllm_with_memcache()

        # --- Phase 1: full miss decode baseline ---
        print("\n--- Phase 1: Full miss decode 1.5k baseline ---")
        warmup_miss = [_build_prompt_ids(PROMPT_TOKENS, seed=-(i + 1)) for i in range(WARMUP_REQUESTS)]
        miss_prompts = [_build_prompt_ids(PROMPT_TOKENS, seed=i) for i in range(MEASURED_REQUESTS_DECODE)]
        miss_result = benchmark.measure_decode_batch(miss_prompts, warmup_prompts=warmup_miss)
        print(f"  Full miss decode: TTFT P50={miss_result['ttft_p50_ms']:.2f}ms, "
              f"total P50={miss_result['total_p50_ms']:.2f}ms")

        # --- Phase 2: pre-populate 2k prefix ---
        print("\n--- Phase 2: Pre-populate 2048-token prefix ---")
        prefix_prompt = _build_prompt_ids(PREFIX_HIT_TOKENS, seed=3000)
        benchmark.measure_e2e_ttft_ms(prefix_prompt)
        print(f"  Pre-populated prefix ({PREFIX_HIT_TOKENS} tokens, seed=3000)")
        benchmark.reset_local_prefix_cache()
        print("  Cleared vLLM local cache (MemCache retains prefix KV)")

        # --- Phase 3: partial hit decode 1.5k ---
        print(f"\n--- Phase 3: Partial hit 2k decode 1.5k ({MEASURED_REQUESTS_DECODE} requests) ---")
        warmup_partial = [
            _build_partial_prefix_prompt(PREFIX_HIT_TOKENS, SUFFIX_TOKENS, 3000, 4000 + i)
            for i in range(WARMUP_REQUESTS)
        ]
        for wp in warmup_partial:
            benchmark.measure_e2e_full_latency(wp, max_tokens=DECODE_MAX_TOKENS)
            benchmark.reset_local_prefix_cache()
        print(f"  Warmup {WARMUP_REQUESTS} partial-hit decode requests done")

        partial_ttft: List[float] = []
        partial_itl: List[float] = []
        partial_total: List[float] = []
        for i in range(MEASURED_REQUESTS_DECODE):
            prompt = _build_partial_prefix_prompt(PREFIX_HIT_TOKENS, SUFFIX_TOKENS, 3000, 6000 + i)
            benchmark.reset_local_prefix_cache()
            result = benchmark.measure_e2e_full_latency(prompt, max_tokens=DECODE_MAX_TOKENS)
            partial_ttft.append(result["ttft_ms"])
            partial_itl.extend(result["itl_ms_list"])
            partial_total.append(result["total_ms"])
            print(
                f"  measured {i + 1}/{MEASURED_REQUESTS_DECODE}: "
                f"TTFT={result['ttft_ms']:.2f}ms, tokens={result['token_count']}, "
                f"total={result['total_ms']:.2f}ms"
            )

        p_ttft_p50 = _percentile(partial_ttft, 0.50)
        p_ttft_p99 = _percentile(partial_ttft, 0.99)
        p_itl_p50 = _percentile(partial_itl, 0.50) if partial_itl else 0.0
        p_itl_p99 = _percentile(partial_itl, 0.99) if partial_itl else 0.0
        p_itl_avg = sum(partial_itl) / len(partial_itl) if partial_itl else 0.0
        p_total_p50 = _percentile(partial_total, 0.50)
        p_total_avg = sum(partial_total) / len(partial_total) if partial_total else 0.0
        p_decode_tps = sum(
            DECODE_MAX_TOKENS / (t / 1000.0) for t in partial_total if t > 0
        ) / len(partial_total) if partial_total else 0.0
        p_itl_p99_p50 = p_itl_p99 / p_itl_p50 if p_itl_p50 > 0 else 999.0

        miss_ttft_p50 = miss_result["ttft_p50_ms"]
        miss_total_p50 = miss_result["total_p50_ms"]
        ttft_ratio = p_ttft_p50 / miss_ttft_p50 if miss_ttft_p50 > 0 else 1.0
        ttft_improvement = 1.0 - ttft_ratio
        total_ratio = p_total_p50 / miss_total_p50 if miss_total_p50 > 0 else 1.0
        total_improvement = 1.0 - total_ratio

        print(
            f"\n===== Decode 1.5k Partial Hit 2k =====\n"
            f"  Full miss    TTFT P50={miss_ttft_p50:.2f}ms, total P50={miss_total_p50:.2f}ms\n"
            f"  Partial hit  TTFT P50={p_ttft_p50:.2f}ms P99={p_ttft_p99:.2f}ms\n"
            f"               ITL  P50={p_itl_p50:.2f}ms P99={p_itl_p99:.2f}ms avg={p_itl_avg:.2f}ms\n"
            f"               Total P50={p_total_p50:.2f}ms avg={p_total_avg:.2f}ms\n"
            f"               Decode throughput={p_decode_tps:.2f} tokens/s\n"
            f"  TTFT improvement={ttft_improvement:.2%} (ratio={ttft_ratio:.2%})\n"
            f"  Total improvement={total_improvement:.2%} (ratio={total_ratio:.2%})\n"
            f"  ITL P99/P50={p_itl_p99_p50:.2f} (threshold: < {DECODE_ITL_P99_P50_RATIO})"
        )

        benchmark.print_cache_stats("H-4 Decode 1.5k Partial Hit 2k")

        assert len(partial_ttft) > 0, "No partial hit decode measurements collected"
        assert ttft_ratio < PARTIAL_HIT_TTFT_RATIO, (
            f"Partial hit TTFT P50 {p_ttft_p50:.2f}ms >= miss P50 * {PARTIAL_HIT_TTFT_RATIO} "
            f"({miss_ttft_p50 * PARTIAL_HIT_TTFT_RATIO:.2f}ms)"
        )
        assert p_itl_p99_p50 < DECODE_ITL_P99_P50_RATIO, (
            f"ITL P99/P50 ratio {p_itl_p99_p50:.2f} >= {DECODE_ITL_P99_P50_RATIO}"
        )
    finally:
        benchmark.stop_vllm()


# ---------------------------------------------------------------------------
# 4-rank (TP=4) variants — identical logic, separate naming for report clarity
# ---------------------------------------------------------------------------
# The TP size is controlled by MMC_TEST_TENSOR_PARALLEL_SIZE. These exist as
# separately-named tests so ``pytest -m rank4`` produces a clear 4-rank report.
#
# Run with:
#   MMC_TEST_DEVICE_IDS=2,3,6,7 MMC_TEST_TENSOR_PARALLEL_SIZE=4 pytest -m rank4


@pytest.mark.hitdecode
@pytest.mark.rank4
def test_partial_prefix_hit_2k_ttft_4rank(benchmark):
    """4-rank (TP=4): Partial prefix hit (2k out of 4k) E2E TTFT.

    Identical to test_partial_prefix_hit_2k_ttft but named for 4-rank
    report clarity. Set MMC_TEST_TENSOR_PARALLEL_SIZE=4.
    """
    try:
        print("\n===== [4-Rank] Starting vLLM with MemCache for partial prefix hit 2k test =====")
        benchmark.start_vllm_with_memcache()

        print("\n--- Phase 1: Full miss TTFT (4096 unique tokens) ---")
        warmup_miss = [_build_prompt_ids(PROMPT_TOKENS, seed=-(i + 1)) for i in range(WARMUP_REQUESTS)]
        measure_miss = [_build_prompt_ids(PROMPT_TOKENS, seed=i) for i in range(MEASURED_REQUESTS_TTFT)]
        miss_result = benchmark.measure_ttft_batch(measure_miss, warmup_prompts=warmup_miss)
        print(f"  Full miss: {miss_result}")

        print("\n--- Phase 2: Pre-populate 2048-token prefix ---")
        prefix_prompt = _build_prompt_ids(PREFIX_HIT_TOKENS, seed=3000)
        benchmark.measure_e2e_ttft_ms(prefix_prompt)
        print(f"  Pre-populated prefix ({PREFIX_HIT_TOKENS} tokens, seed=3000)")
        benchmark.reset_local_prefix_cache()
        print("  Cleared vLLM local cache (MemCache retains prefix KV)")

        print("\n--- Phase 3: Partial hit TTFT (2k prefix cached + 2k new) ---")
        warmup_partial = [
            _build_partial_prefix_prompt(PREFIX_HIT_TOKENS, SUFFIX_TOKENS, 3000, 4000 + i)
            for i in range(WARMUP_REQUESTS)
        ]
        for wp in warmup_partial:
            benchmark.measure_e2e_ttft_ms(wp)
            benchmark.reset_local_prefix_cache()
        print(f"  Warmup {WARMUP_REQUESTS} partial-hit requests done")

        partial_latencies: List[float] = []
        for i in range(MEASURED_REQUESTS_TTFT):
            prompt = _build_partial_prefix_prompt(PREFIX_HIT_TOKENS, SUFFIX_TOKENS, 3000, 5000 + i)
            ttft = benchmark.measure_e2e_ttft_ms(prompt)
            partial_latencies.append(ttft)
            benchmark.reset_local_prefix_cache()
            print(f"  measured {i + 1}/{MEASURED_REQUESTS_TTFT}: TTFT={ttft:.2f}ms")

        partial_p50 = _percentile(partial_latencies, 0.50)
        partial_p90 = _percentile(partial_latencies, 0.90)
        partial_p99 = _percentile(partial_latencies, 0.99)
        partial_avg = sum(partial_latencies) / len(partial_latencies)

        miss_p50 = miss_result["p50_ms"]
        ratio = partial_p50 / miss_p50 if miss_p50 > 0 else 1.0
        improvement = 1.0 - ratio

        print(
            f"\n===== [4-Rank] Partial Prefix Hit 2k TTFT =====\n"
            f"  Full miss    P50={miss_p50:.2f}ms P99={miss_result['p99_ms']:.2f}ms\n"
            f"  Partial hit  P50={partial_p50:.2f}ms P90={partial_p90:.2f}ms P99={partial_p99:.2f}ms\n"
            f"  Partial avg  ={partial_avg:.2f}ms\n"
            f"  Ratio (partial/miss)={ratio:.2%}\n"
            f"  Improvement={improvement:.2%}\n"
            f"  Threshold: ratio < {PARTIAL_HIT_TTFT_RATIO:.0%}, improvement > {PARTIAL_HIT_IMPROVEMENT_MIN:.0%}"
        )

        benchmark.print_cache_stats("4-Rank H-1 Partial Hit 2k")

        assert ratio < PARTIAL_HIT_TTFT_RATIO, (
            f"Partial hit P50 {partial_p50:.2f}ms >= miss P50 * {PARTIAL_HIT_TTFT_RATIO} "
            f"({miss_p50 * PARTIAL_HIT_TTFT_RATIO:.2f}ms)"
        )
        assert improvement > PARTIAL_HIT_IMPROVEMENT_MIN, (
            f"Improvement {improvement:.2%} <= min expected {PARTIAL_HIT_IMPROVEMENT_MIN:.2%}"
        )
    finally:
        benchmark.stop_vllm()


@pytest.mark.hitdecode
@pytest.mark.rank4
def test_decode_1_5k_full_miss_4rank(benchmark):
    """4-rank (TP=4): Decode 1.5k tokens from a full miss.

    Identical to test_decode_1_5k_full_miss but named for 4-rank
    report clarity. Set MMC_TEST_TENSOR_PARALLEL_SIZE=4.
    """
    try:
        print("\n===== [4-Rank] Starting vLLM with MemCache for decode 1.5k full miss test =====")
        benchmark.start_vllm_with_memcache()

        warmup_prompts = [_build_prompt_ids(PROMPT_TOKENS, seed=-(i + 1)) for i in range(WARMUP_REQUESTS)]
        measure_prompts = [_build_prompt_ids(PROMPT_TOKENS, seed=i) for i in range(MEASURED_REQUESTS_DECODE)]

        print(f"\n--- [4-Rank] Decode 1.5k full miss: {MEASURED_REQUESTS_DECODE} requests, {DECODE_MAX_TOKENS} tokens each ---")
        result = benchmark.measure_decode_batch(measure_prompts, warmup_prompts=warmup_prompts)

        itl_p99_p50 = result["itl_p99_ms"] / result["itl_p50_ms"] if result["itl_p50_ms"] > 0 else 999.0

        print(
            f"\n===== [4-Rank] Decode 1.5k Full Miss =====\n"
            f"  TTFT  P50={result['ttft_p50_ms']:.2f}ms P90={result['ttft_p90_ms']:.2f}ms "
            f"P99={result['ttft_p99_ms']:.2f}ms avg={result['ttft_avg_ms']:.2f}ms\n"
            f"  ITL   P50={result['itl_p50_ms']:.2f}ms P90={result['itl_p90_ms']:.2f}ms "
            f"P99={result['itl_p99_ms']:.2f}ms avg={result['itl_avg_ms']:.2f}ms\n"
            f"  Total P50={result['total_p50_ms']:.2f}ms avg={result['total_avg_ms']:.2f}ms\n"
            f"  Decode throughput={result['decode_tps_avg']:.2f} tokens/s\n"
            f"  ITL P99/P50={itl_p99_p50:.2f} (threshold: < {DECODE_ITL_P99_P50_RATIO})"
        )

        benchmark.print_cache_stats("4-Rank H-2 Decode 1.5k Miss")

        assert result["count"] > 0, "No decode measurements collected"
        assert itl_p99_p50 < DECODE_ITL_P99_P50_RATIO, (
            f"ITL P99/P50 ratio {itl_p99_p50:.2f} >= {DECODE_ITL_P99_P50_RATIO}"
        )
    finally:
        benchmark.stop_vllm()


@pytest.mark.hitdecode
@pytest.mark.rank4
def test_decode_1_5k_full_hit_4rank(benchmark):
    """4-rank (TP=4): Decode 1.5k tokens from a full cache hit.

    Identical to test_decode_1_5k_full_hit but named for 4-rank
    report clarity. Set MMC_TEST_TENSOR_PARALLEL_SIZE=4.
    """
    try:
        print("\n===== [4-Rank] Starting vLLM with MemCache for decode 1.5k full hit test =====")
        benchmark.start_vllm_with_memcache()

        cached_prompt = _build_prompt_ids(PROMPT_TOKENS, seed=7000)
        warmup_prompt = _build_prompt_ids(PROMPT_TOKENS, seed=7999)
        for i in range(WARMUP_REQUESTS):
            benchmark.measure_e2e_ttft_ms(warmup_prompt)
            print(f"  warmup {i + 1}/{WARMUP_REQUESTS} done")

        benchmark.measure_e2e_ttft_ms(cached_prompt)
        print(f"  Pre-populated full prompt ({PROMPT_TOKENS} tokens, seed=7000)")
        benchmark.reset_local_prefix_cache()
        print("  Cleared vLLM local cache (MemCache retains full KV)")

        hit_ttft_latencies: List[float] = []
        all_results: List[Dict[str, Any]] = []
        for i in range(MEASURED_REQUESTS_DECODE):
            benchmark.reset_local_prefix_cache()
            result = benchmark.measure_e2e_full_latency(cached_prompt, max_tokens=DECODE_MAX_TOKENS)
            print(
                f"  measured {i + 1}/{MEASURED_REQUESTS_DECODE}: "
                f"TTFT={result['ttft_ms']:.2f}ms, tokens={result['token_count']}, "
                f"total={result['total_ms']:.2f}ms"
            )
            if result["token_count"] < DECODE_MAX_TOKENS * 0.9:
                print(f"  [WARN] token_count={result['token_count']} < {DECODE_MAX_TOKENS}×0.9, skipping")
                continue
            all_results.append(result)
            hit_ttft_latencies.append(result["ttft_ms"])

        all_itl: List[float] = []
        all_total: List[float] = []
        for r in all_results:
            all_itl.extend(r["itl_ms_list"])
            all_total.append(r["total_ms"])

        ttft_p50 = _percentile(hit_ttft_latencies, 0.50)
        ttft_p99 = _percentile(hit_ttft_latencies, 0.99)
        itl_p50 = _percentile(all_itl, 0.50) if all_itl else 0.0
        itl_p99 = _percentile(all_itl, 0.99) if all_itl else 0.0
        itl_avg = sum(all_itl) / len(all_itl) if all_itl else 0.0
        total_p50 = _percentile(all_total, 0.50)
        total_avg = sum(all_total) / len(all_total) if all_total else 0.0
        decode_tps = sum(
            DECODE_MAX_TOKENS / (t / 1000.0) for t in all_total if t > 0
        ) / len(all_total) if all_total else 0.0
        itl_p99_p50 = itl_p99 / itl_p50 if itl_p50 > 0 else 999.0

        print(
            f"\n===== [4-Rank] Decode 1.5k Full Hit =====\n"
            f"  TTFT  P50={ttft_p50:.2f}ms P99={ttft_p99:.2f}ms\n"
            f"  ITL   P50={itl_p50:.2f}ms P99={itl_p99:.2f}ms avg={itl_avg:.2f}ms\n"
            f"  Total P50={total_p50:.2f}ms avg={total_avg:.2f}ms\n"
            f"  Decode throughput={decode_tps:.2f} tokens/s\n"
            f"  ITL P99/P50={itl_p99_p50:.2f} (threshold: < {DECODE_ITL_P99_P50_RATIO})"
        )

        benchmark.print_cache_stats("4-Rank H-3 Decode 1.5k Hit")

        assert len(hit_ttft_latencies) > 0, "No hit decode measurements collected"
        assert itl_p99_p50 < DECODE_ITL_P99_P50_RATIO, (
            f"ITL P99/P50 ratio {itl_p99_p50:.2f} >= {DECODE_ITL_P99_P50_RATIO}"
        )
    finally:
        benchmark.stop_vllm()


@pytest.mark.hitdecode
@pytest.mark.rank4
def test_decode_1_5k_partial_hit_2k_4rank(benchmark):
    """4-rank (TP=4): Decode 1.5k tokens from a partial prefix hit (2k cached + 2k new).

    Identical to test_decode_1_5k_partial_hit_2k but named for 4-rank
    report clarity. Set MMC_TEST_TENSOR_PARALLEL_SIZE=4.
    """
    try:
        print("\n===== [4-Rank] Starting vLLM with MemCache for decode 1.5k partial hit 2k test =====")
        benchmark.start_vllm_with_memcache()

        print("\n--- Phase 1: Full miss decode 1.5k baseline ---")
        warmup_miss = [_build_prompt_ids(PROMPT_TOKENS, seed=-(i + 1)) for i in range(WARMUP_REQUESTS)]
        miss_prompts = [_build_prompt_ids(PROMPT_TOKENS, seed=i) for i in range(MEASURED_REQUESTS_DECODE)]
        miss_result = benchmark.measure_decode_batch(miss_prompts, warmup_prompts=warmup_miss)
        print(f"  Full miss decode: TTFT P50={miss_result['ttft_p50_ms']:.2f}ms, "
              f"total P50={miss_result['total_p50_ms']:.2f}ms")

        print("\n--- Phase 2: Pre-populate 2048-token prefix ---")
        prefix_prompt = _build_prompt_ids(PREFIX_HIT_TOKENS, seed=3000)
        benchmark.measure_e2e_ttft_ms(prefix_prompt)
        print(f"  Pre-populated prefix ({PREFIX_HIT_TOKENS} tokens, seed=3000)")
        benchmark.reset_local_prefix_cache()
        print("  Cleared vLLM local cache (MemCache retains prefix KV)")

        print(f"\n--- Phase 3: Partial hit 2k decode 1.5k ({MEASURED_REQUESTS_DECODE} requests) ---")
        warmup_partial = [
            _build_partial_prefix_prompt(PREFIX_HIT_TOKENS, SUFFIX_TOKENS, 3000, 4000 + i)
            for i in range(WARMUP_REQUESTS)
        ]
        for wp in warmup_partial:
            benchmark.measure_e2e_full_latency(wp, max_tokens=DECODE_MAX_TOKENS)
            benchmark.reset_local_prefix_cache()
        print(f"  Warmup {WARMUP_REQUESTS} partial-hit decode requests done")

        partial_ttft: List[float] = []
        partial_itl: List[float] = []
        partial_total: List[float] = []
        for i in range(MEASURED_REQUESTS_DECODE):
            prompt = _build_partial_prefix_prompt(PREFIX_HIT_TOKENS, SUFFIX_TOKENS, 3000, 6000 + i)
            benchmark.reset_local_prefix_cache()
            result = benchmark.measure_e2e_full_latency(prompt, max_tokens=DECODE_MAX_TOKENS)
            partial_ttft.append(result["ttft_ms"])
            partial_itl.extend(result["itl_ms_list"])
            partial_total.append(result["total_ms"])
            print(
                f"  measured {i + 1}/{MEASURED_REQUESTS_DECODE}: "
                f"TTFT={result['ttft_ms']:.2f}ms, tokens={result['token_count']}, "
                f"total={result['total_ms']:.2f}ms"
            )

        p_ttft_p50 = _percentile(partial_ttft, 0.50)
        p_ttft_p99 = _percentile(partial_ttft, 0.99)
        p_itl_p50 = _percentile(partial_itl, 0.50) if partial_itl else 0.0
        p_itl_p99 = _percentile(partial_itl, 0.99) if partial_itl else 0.0
        p_itl_avg = sum(partial_itl) / len(partial_itl) if partial_itl else 0.0
        p_total_p50 = _percentile(partial_total, 0.50)
        p_total_avg = sum(partial_total) / len(partial_total) if partial_total else 0.0
        p_decode_tps = sum(
            DECODE_MAX_TOKENS / (t / 1000.0) for t in partial_total if t > 0
        ) / len(partial_total) if partial_total else 0.0
        p_itl_p99_p50 = p_itl_p99 / p_itl_p50 if p_itl_p50 > 0 else 999.0

        miss_ttft_p50 = miss_result["ttft_p50_ms"]
        miss_total_p50 = miss_result["total_p50_ms"]
        ttft_ratio = p_ttft_p50 / miss_ttft_p50 if miss_ttft_p50 > 0 else 1.0
        ttft_improvement = 1.0 - ttft_ratio
        total_ratio = p_total_p50 / miss_total_p50 if miss_total_p50 > 0 else 1.0
        total_improvement = 1.0 - total_ratio

        print(
            f"\n===== [4-Rank] Decode 1.5k Partial Hit 2k =====\n"
            f"  Full miss    TTFT P50={miss_ttft_p50:.2f}ms, total P50={miss_total_p50:.2f}ms\n"
            f"  Partial hit  TTFT P50={p_ttft_p50:.2f}ms P99={p_ttft_p99:.2f}ms\n"
            f"               ITL  P50={p_itl_p50:.2f}ms P99={p_itl_p99:.2f}ms avg={p_itl_avg:.2f}ms\n"
            f"               Total P50={p_total_p50:.2f}ms avg={p_total_avg:.2f}ms\n"
            f"               Decode throughput={p_decode_tps:.2f} tokens/s\n"
            f"  TTFT improvement={ttft_improvement:.2%} (ratio={ttft_ratio:.2%})\n"
            f"  Total improvement={total_improvement:.2%} (ratio={total_ratio:.2%})\n"
            f"  ITL P99/P50={p_itl_p99_p50:.2f} (threshold: < {DECODE_ITL_P99_P50_RATIO})"
        )

        benchmark.print_cache_stats("4-Rank H-4 Decode 1.5k Partial Hit 2k")

        assert len(partial_ttft) > 0, "No partial hit decode measurements collected"
        assert ttft_ratio < PARTIAL_HIT_TTFT_RATIO, (
            f"Partial hit TTFT P50 {p_ttft_p50:.2f}ms >= miss P50 * {PARTIAL_HIT_TTFT_RATIO} "
            f"({miss_ttft_p50 * PARTIAL_HIT_TTFT_RATIO:.2f}ms)"
        )
        assert p_itl_p99_p50 < DECODE_ITL_P99_P50_RATIO, (
            f"ITL P99/P50 ratio {p_itl_p99_p50:.2f} >= {DECODE_ITL_P99_P50_RATIO}"
        )
    finally:
        benchmark.stop_vllm()
