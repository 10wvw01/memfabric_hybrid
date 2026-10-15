# SPDX-License-Identifier-Identifier: Apache-2.0
# Copyright (c) 2026 Huawei Technologies Co., Ltd. All Rights Reserved.
"""Shared helpers for the 310P3 MemFabric mm_ar serve-level e2e tests/benchmarks.

Launches a real ``vllm serve`` subprocess on the TP=2 310P3 die pair and
collects greedy outputs / benchmark artifacts. The MemFabric transport is
treated as an external black box; the fused feature is toggled purely
through its public environment variables.
"""

from __future__ import annotations

import contextlib
import json
import os
import shutil
import signal
import subprocess
import time
import urllib.request

DEFAULT_MODEL = os.getenv("MEMFABRIC_MM_AR_E2E_MODEL", "/home/models/Qwen/Qwen3.6-35B-A3B-w8a8")
DEFAULT_SERVED_NAME = "qwen3.6-35b-a3b-w8a8"
DEFAULT_PORT = int(os.getenv("MEMFABRIC_MM_AR_E2E_PORT", "8000"))
DEFAULT_STORE_PORT = int(os.getenv("MEMFABRIC_MM_AR_E2E_STORE_PORT", "8581"))
DEFAULT_BATCH_BASEM_COUNT = os.getenv("MEMFABRIC_MM_AR_E2E_BATCH_BASEM_COUNT", "1")
HEALTH_TIMEOUT_S = float(os.getenv("MEMFABRIC_MM_AR_E2E_HEALTH_TIMEOUT", "2400"))
MEMFABRIC_SET_ENV = os.getenv("MEMFABRIC_MM_AR_E2E_SET_ENV", "/usr/local/memfabric_hybrid/set_env.sh")
MEMFABRIC_ORCH_JSON = os.getenv(
    "MEMFABRIC_MM_AR_E2E_ORCH_JSON",
    "/usr/local/memfabric_hybrid/1.2.0/aarch64-linux/hybm/aicpu_kernel/libmf_sdma_orch_v10.json",
)

FUSED_ENV_KEYS = (
    "VLLM_ASCEND_310P_ENABLE_MEMFABRIC_MM_AR",
    "VLLM_ASCEND_310P_MEMFABRIC_STORE_URL",
    "VLLM_ASCEND_310P_MEMFABRIC_LOCAL_BYTES",
    "VLLM_ASCEND_310P_MEMFABRIC_MM_AR_BATCH_BASEM_COUNT",
    "VLLM_ASCEND_310P_MEMFABRIC_MM_AR_WARMUP_FALLBACK",
    "VLLM_ASCEND_310P_MEMFABRIC_MM_AR_TRACE",
)

GRAPH_COMPILATION_CONFIG = json.dumps({"cudagraph_mode": "FULL_DECODE_ONLY", "cudagraph_capture_sizes": [10]})


def build_serve_cmd(
    port: int = DEFAULT_PORT,
    model: str = DEFAULT_MODEL,
    graph: bool = False,
) -> list[str]:
    """The canonical serve command for the 310P3 Qwen3.6-35B-A3B-w8a8 stack."""

    cmd = [
        "vllm",
        "serve",
        model,
        "--host",
        "127.0.0.1",
        "--port",
        str(port),
        "--tensor-parallel-size",
        "2",
        "--quantization",
        "ascend",
        "--served-model-name",
        DEFAULT_SERVED_NAME,
        "--trust-remote-code",
        "--dtype",
        "float16",
        "--max-num-seqs",
        "16",
        "--max-num-batched-tokens",
        "4096",
        "--max-model-len",
        "8192",
        "--gpu-memory-utilization",
        "0.90",
        "--no-enable-prefix-caching",
        "--enable-chunked-prefill",
        "--mamba-ssm-cache-dtype",
        "float16",
        "--mamba-cache-mode",
        "align",
        "--additional-config",
        json.dumps(
            {
                "enable_flashcomm1": False,
                "enable_flashcomm2_parallel_size": 0,
                "enable_prefill_mc2": False,
                "enable_fused_mc2": 0,
                "ascend_compilation_config": {
                    "fuse_norm_quant": True,
                    "enable_npugraph_ex": False,
                },
            }
        ),
    ]
    if graph:
        cmd += ["--compilation-config", GRAPH_COMPILATION_CONFIG]
    else:
        cmd += ["--enforce-eager"]
    return cmd


def build_serve_env(fused: bool, batch_basem_count: str = DEFAULT_BATCH_BASEM_COUNT) -> dict[str, str]:
    """Serve environment: identical for stock/fused except the fused feature."""

    env = dict(os.environ)
    env["ASCEND_RT_VISIBLE_DEVICES"] = os.getenv("MEMFABRIC_MM_AR_E2E_DEVICES", "0,1")
    env["VLLM_WORKER_MULTIPROC_METHOD"] = "spawn"
    for key in FUSED_ENV_KEYS:
        env.pop(key, None)
    env.pop("MF_SDMA_ORCH_JSON", None)

    # The MemFabric run package must be on the library path for the fused
    # build regardless of any pip-installed copy.
    if os.path.isfile(MEMFABRIC_SET_ENV):
        parsed = subprocess.run(
            ["bash", "-c", f". {MEMFABRIC_SET_ENV} && env"],
            capture_output=True,
            text=True,
            check=True,
        ).stdout
        for line in parsed.splitlines():
            if "=" in line:
                key, _, value = line.partition("=")
                if key in ("MEMFABRIC_HYBRID_HOME_PATH", "MEMFABRIC_HYBRID_EXTEND_LIB_PATH", "LD_LIBRARY_PATH", "PATH"):
                    env[key] = value

    if fused:
        env["VLLM_ASCEND_310P_ENABLE_MEMFABRIC_MM_AR"] = "1"
        env["VLLM_ASCEND_310P_MEMFABRIC_STORE_URL"] = f"tcp://127.0.0.1:{DEFAULT_STORE_PORT}"
        env["VLLM_ASCEND_310P_MEMFABRIC_LOCAL_BYTES"] = str(96 * 1024 * 1024)
        env["VLLM_ASCEND_310P_MEMFABRIC_MM_AR_BATCH_BASEM_COUNT"] = str(batch_basem_count)
        env["VLLM_ASCEND_310P_MEMFABRIC_MM_AR_WARMUP_FALLBACK"] = "1"
        # The installed run package may be shadowed by a pip site-packages
        # copy inside the serve process; pin the launch json explicitly.
        if os.path.isfile(MEMFABRIC_ORCH_JSON):
            env["MF_SDMA_ORCH_JSON"] = MEMFABRIC_ORCH_JSON
    return env


class ServeProcess:
    """A managed ``vllm serve`` subprocess with health polling and log capture."""

    def __init__(
        self,
        mode: str,
        graph: bool,
        port: int = DEFAULT_PORT,
        log_path: str | None = None,
        batch_basem_count: str = DEFAULT_BATCH_BASEM_COUNT,
    ) -> None:
        if mode not in ("stock", "fused"):
            raise ValueError(f"mode must be stock|fused, got {mode}")
        self.mode = mode
        self.graph = graph
        self.port = port
        self.log_path = log_path
        self.batch_basem_count = batch_basem_count
        self.proc: subprocess.Popen | None = None

    def start(self) -> None:
        if shutil.which("vllm") is None:
            raise RuntimeError("vllm CLI not found on PATH")
        env = build_serve_env(fused=self.mode == "fused", batch_basem_count=self.batch_basem_count)
        cmd = build_serve_cmd(port=self.port, graph=self.graph)
        # Long-lived handle: closed in stop() after the server exits.
        self._log_file = open(self.log_path, "wb")  # noqa: SIM115
        try:
            self.proc = subprocess.Popen(
                cmd,
                stdout=self._log_file,
                stderr=subprocess.STDOUT,
                env=env,
                start_new_session=True,
            )
        except Exception:
            self._log_file.close()
            raise

    def wait_healthy(self, timeout: float = HEALTH_TIMEOUT_S) -> None:
        deadline = time.time() + timeout
        url = f"http://127.0.0.1:{self.port}/health"
        while time.time() < deadline:
            if self.proc is not None and self.proc.poll() is not None:
                self.dump_log_tail(40)
                raise RuntimeError(f"{self.tag} server exited with rc={self.proc.returncode}")
            try:
                with urllib.request.urlopen(url, timeout=5) as resp:
                    if resp.status == 200:
                        return
            except Exception:
                pass
            time.sleep(5)
        self.dump_log_tail(40)
        raise TimeoutError(f"{self.tag} server not healthy after {timeout}s")

    def dump_log_tail(self, lines: int) -> None:
        if self.log_path and os.path.exists(self.log_path):
            with open(self.log_path, errors="replace") as f:
                tail = f.readlines()[-lines:]
            print("".join(tail), flush=True)

    @property
    def tag(self) -> str:
        return f"{self.mode}_{'graph' if self.graph else 'eager'}"

    def stop(self) -> None:
        if self.proc is None:
            return
        with contextlib.suppress(ProcessLookupError):
            os.killpg(self.proc.pid, signal.SIGTERM)
        try:
            self.proc.wait(timeout=120)
        except subprocess.TimeoutExpired:
            with contextlib.suppress(ProcessLookupError):
                os.killpg(self.proc.pid, signal.SIGKILL)
            self.proc.wait(timeout=60)
        finally:
            self._log_file.close()
            self.proc = None

    def __enter__(self) -> ServeProcess:
        self.start()
        self.wait_healthy()
        return self

    def __exit__(self, *exc) -> None:
        self.stop()

    def count_enabled_layers(self) -> tuple[int, int]:
        """Count distinct fused-enabled o_proj / out_proj layers from the log.

        The serve log interleaves both TP workers, so each enabled layer
        appears once per rank; layer indices are de-duplicated here.
        """

        def layer_index(line: str) -> str | None:
            for marker in (".self_attn.o_proj", ".linear_attn.out_proj"):
                pos = line.find(marker)
                if pos == -1:
                    continue
                head = line[:pos]
                start = head.rfind("layers.")
                if start == -1:
                    return None
                digits = head[start + len("layers.") :]
                return digits.split(".")[0]
            return None

        o_proj: set[str] = set()
        out_proj: set[str] = set()
        with open(self.log_path, errors="replace") as f:
            for line in f:
                if "Enable 310P3 TP=2 MemFabric unquantized mm_ar ABI v7 for" not in line:
                    continue
                idx = layer_index(line)
                if idx is None:
                    continue
                if ".self_attn.o_proj" in line:
                    o_proj.add(idx)
                else:
                    out_proj.add(idx)
        return len(o_proj), len(out_proj)


def completions(
    prompt: str | list[int],
    max_tokens: int,
    port: int = DEFAULT_PORT,
    temperature: float = 0.0,
    seed: int = 1234,
    ignore_eos: bool = True,
    timeout: float = 600.0,
) -> str:
    payload = json.dumps(
        {
            "model": DEFAULT_SERVED_NAME,
            "prompt": prompt,
            "max_tokens": max_tokens,
            "temperature": temperature,
            "seed": seed,
            "ignore_eos": ignore_eos,
        }
    ).encode()
    req = urllib.request.Request(
        f"http://127.0.0.1:{port}/v1/completions",
        data=payload,
        headers={"Content-Type": "application/json"},
    )
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        body = json.loads(resp.read().decode())
    return body["choices"][0]["text"]


def build_prompt_set() -> dict[str, str | list[int]]:
    """Deterministic greedy prompt set exercising prefill/decode M regimes.

    - short: decode M=1 per request after a small prefill;
    - long_4095: a 4095-token prefill (one full chunked-prefill wave with a
      511-row tail batch) built from token ids. Natural-language ~4k prompts
      are NOT deterministic on this stack (the greedy token flips between
      two attractors across runs on the stock server itself, 2026-09-29
      rerun confirmed the 2026-09-22 finding); the repeated-token id prompt
      below was validated deterministic on both ends.
    - batched decode is driven by the caller issuing concurrent requests.
    """

    prompts: dict[str, str | list[int]] = {
        "short_0": "The capital of France is",
        "short_1": "Hello, my name is",
        "short_2": "1 2 3 4 5 6 7 8 9 10 ",
        "short_3": "The president of the United States is",
        "long_4095": [64] * 4095,
    }
    return prompts
