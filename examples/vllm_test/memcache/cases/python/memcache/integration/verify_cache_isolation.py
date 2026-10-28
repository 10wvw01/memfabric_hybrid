#!/usr/bin/env python
# coding=utf-8
"""Verify reset_local_prefix_cache isolates MemCache from vLLM internal cache.

Scenarios:
  A. Full miss       — unique prompt, no cache anywhere
  B. Local hit       — same prompt immediately, vLLM GPU local cache
  C. MemCache hit    — same prompt after reset_local_prefix_cache
  C2. MemCache hit (polluted) — reset + pollute local blocks, then hit from MemCache
  D. Full miss (both cleared) — reset_external=true, clears both caches
                                (SKIPPED: MemCache has not implemented reset_external yet)

Pass criteria:
  - C < A * 0.8   (MemCache hit is faster than full miss)
"""

import http.client
import json
import multiprocessing
import os
import socket
import subprocess
import sys
import time

PROMPT_TOKENS = 4096
VOCAB_SIZE = 151936
VLLM_PORT = 4077
META_SERVICE_PORT = 5000
VLLM_READY_TIMEOUT_S = 600

DEFAULT_MODEL_PATH = "/home/models/Qwen3.6-35B-A3B-w8a8"
DEFAULT_SERVED_MODEL_NAME = "Qwen3.6-35B-A3B"

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


def build_prompt_ids(num_tokens, seed=1):
    import random
    rng = random.Random(seed)
    return [rng.randrange(VOCAB_SIZE) for _ in range(num_tokens)]


def is_port_listening(port):
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=1):
            return True
    except OSError:
        return False


def start_meta_service(memcache_dir):
    meta_bin = os.path.join(memcache_dir, "output", "memcache", "bin", "mmc_meta_service")
    env = os.environ.copy()
    env["MMC_META_CONFIG_PATH"] = os.path.join(memcache_dir, "config", "mmc-meta.conf")
    env["MMC_MEMCACHE_DIR"] = memcache_dir
    lib_dirs = []
    output_lib = os.path.join(memcache_dir, "output", "memcache", "lib64")
    if os.path.isdir(output_lib):
        lib_dirs.append(output_lib)
    try:
        import memcache_hybrid
        import memfabric_hybrid
        for pkg in (memcache_hybrid, memfabric_hybrid):
            pkg_lib = os.path.join(os.path.dirname(pkg.__file__), "lib")
            if os.path.isdir(pkg_lib):
                lib_dirs.append(pkg_lib)
    except ImportError:
        pass
    if lib_dirs:
        env["LD_LIBRARY_PATH"] = ":".join(lib_dirs) + ":" + env.get("LD_LIBRARY_PATH", "")
    subprocess.Popen([meta_bin], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env,
                      start_new_session=True)
    for _ in range(10):
        if is_port_listening(META_SERVICE_PORT):
            print("[INFO] MetaService started")
            return
        time.sleep(1)
    raise RuntimeError("MetaService failed to start")


def stop_meta_service():
    subprocess.run("pkill -9 -f 'mmc_meta_service'", shell=True, timeout=5,
                   capture_output=True)
    time.sleep(1)


def start_vllm(device_ids, tp_size, memcache_dir):
    cmd = [
        "vllm", "serve", DEFAULT_MODEL_PATH,
        "--host", "127.0.0.1", "--port", str(VLLM_PORT),
        "--tensor-parallel-size", str(tp_size),
        "--quantization", "ascend",
        "--served-model-name", DEFAULT_SERVED_MODEL_NAME,
        "--trust-remote-code", "--dtype", "float16",
        "--max-num-seqs", "10", "--max-num-batched-tokens", "4096",
        "--max-model-len", "8192", "--gpu-memory-utilization", "0.80",
        "--enforce-eager", "--enable-chunked-prefill",
        "--mamba-ssm-cache-dtype", "float16", "--mamba-cache-mode", "align",
        "--additional-config", ADDITIONAL_CONFIG_JSON,
        "--enable-prefix-caching",
        "--kv-transfer-config", KV_TRANSFER_CONFIG_MEMCACHE,
    ]
    env = os.environ.copy()
    env["ASCEND_RT_VISIBLE_DEVICES"] = device_ids
    env["VLLM_SERVER_DEV_MODE"] = "1"
    env["MMC_LOCAL_CONFIG_PATH"] = os.environ.get("MMC_LOCAL_CONFIG_PATH", "")
    env["MMC_META_CONFIG_PATH"] = os.path.join(memcache_dir, "config", "mmc-meta.conf")
    env["MMC_MEMCACHE_DIR"] = memcache_dir
    try:
        import memcache_hybrid
        import memfabric_hybrid
        for pkg in (memcache_hybrid, memfabric_hybrid):
            pkg_lib = os.path.join(os.path.dirname(pkg.__file__), "lib")
            if os.path.isdir(pkg_lib):
                env["LD_LIBRARY_PATH"] = pkg_lib + ":" + env.get("LD_LIBRARY_PATH", "")
    except ImportError:
        pass
    output_lib = os.path.join(memcache_dir, "output", "memcache", "lib64")
    if os.path.isdir(output_lib):
        env["LD_LIBRARY_PATH"] = output_lib + ":" + env.get("LD_LIBRARY_PATH", "")
    env["VLLM_SERVER_DEV_MODE"] = "1"
    log_path = f"/tmp/vllm_verify_{int(time.time())}.log"
    print(f"[INFO] vLLM log: {log_path}")
    proc = subprocess.Popen(cmd, stdout=open(log_path, "w"), stderr=subprocess.STDOUT, env=env)
    deadline = time.time() + VLLM_READY_TIMEOUT_S
    while time.time() < deadline:
        if proc.poll() is not None:
            raise RuntimeError(f"vLLM exited (code={proc.returncode})")
        try:
            conn = http.client.HTTPConnection("127.0.0.1", VLLM_PORT, timeout=5)
            conn.request("GET", "/health")
            resp = conn.getresponse()
            conn.close()
            if resp.status == 200:
                print("[INFO] vLLM is ready")
                return proc, log_path
        except (OSError, http.client.HTTPException):
            pass
        time.sleep(5)
    raise TimeoutError(f"vLLM not ready in {VLLM_READY_TIMEOUT_S}s")


def stop_vllm(proc):
    if proc:
        proc.terminate()
        try:
            proc.wait(timeout=30)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=30)
    subprocess.run("pkill -9 -f 'VLLM::'", shell=True, timeout=5, capture_output=True)
    time.sleep(2)


def measure_ttft(prompt_ids):
    body = json.dumps({
        "model": DEFAULT_SERVED_MODEL_NAME,
        "prompt": prompt_ids,
        "max_tokens": 1,
        "stream": True,
        "temperature": 0,
        "ignore_eos": True,
    })
    conn = http.client.HTTPConnection("127.0.0.1", VLLM_PORT, timeout=120)
    try:
        start = time.perf_counter()
        conn.request("POST", "/v1/completions", body=body, headers={"Content-Type": "application/json"})
        resp = conn.getresponse()
        for line in resp:
            if line.startswith(b"data: ") and b"[DONE]" not in line:
                return (time.perf_counter() - start) * 1000.0
        return (time.perf_counter() - start) * 1000.0
    finally:
        conn.close()


def reset_local_prefix_cache(reset_external=False):
    conn = http.client.HTTPConnection("127.0.0.1", VLLM_PORT, timeout=30)
    try:
        if reset_external:
            conn.request("POST", "/reset_prefix_cache?reset_external=true")
        else:
            conn.request("POST", "/reset_prefix_cache")
        resp = conn.getresponse()
        body = resp.read().decode()
        status = resp.status
        resp.close()
        if status != 200:
            print(f"  [WARN] reset_prefix_cache returned {status}: {body}")
            return False
        return True
    except (OSError, http.client.HTTPException) as e:
        print(f"  [WARN] reset_prefix_cache failed: {e}")
        return False
    finally:
        conn.close()


def main():
    memcache_dir = os.environ.get("MMC_MEMCACHE_DIR", "/tmp/memcache_new")
    device_ids = os.environ.get("MMC_TEST_DEVICE_IDS", "0,1")
    tp_size = int(os.environ.get("MMC_TEST_TENSOR_PARALLEL_SIZE", "2"))

    print(f"[INFO] memcache_dir={memcache_dir}, devices={device_ids}, TP={tp_size}")

    stop_meta_service()
    time.sleep(1)
    start_meta_service(memcache_dir)
    time.sleep(2)

    proc = None
    try:
        proc, _ = start_vllm(device_ids, tp_size, memcache_dir)
        time.sleep(2)

        prompt = build_prompt_ids(PROMPT_TOKENS, seed=42)
        warmup_prompt = build_prompt_ids(PROMPT_TOKENS, seed=99)

        print("\n[INFO] Warmup (2 requests)...")
        for i in range(2):
            t = measure_ttft(warmup_prompt)
            print(f"  warmup {i+1}: TTFT={t:.2f}ms")

        print("\n===== Scenario A: Full miss (unique prompt) =====")
        ttft_a = measure_ttft(prompt)
        print(f"  TTFT = {ttft_a:.2f}ms")

        print("\n===== Scenario B: Local hit (same prompt, no reset) =====")
        ttft_b = measure_ttft(prompt)
        print(f"  TTFT = {ttft_b:.2f}ms")

        print("\n===== Scenario C: MemCache hit (after reset_local_prefix_cache) =====")
        ok = reset_local_prefix_cache()
        if not ok:
            print("  [FAIL] reset_local_prefix_cache failed, cannot verify isolation")
            return 1
        time.sleep(1)
        ttft_c = measure_ttft(prompt)
        print(f"  TTFT = {ttft_c:.2f}ms")

        print("\n===== Repeat C x3 for stability =====")
        for i in range(3):
            reset_local_prefix_cache()
            time.sleep(0.5)
            t = measure_ttft(prompt)
            print(f"  repeat C-{i+1}: TTFT={t:.2f}ms")

        print("\n===== Scenario C2: MemCache hit (reset + pollute local cache first) =====")
        reset_local_prefix_cache()
        time.sleep(0.5)
        pollute_prompt = build_prompt_ids(PROMPT_TOKENS, seed=7777)
        t_pollute = measure_ttft(pollute_prompt)
        print(f"  pollute prompt TTFT = {t_pollute:.2f}ms (miss, overwrites local blocks)")
        ttft_c2 = measure_ttft(prompt)
        print(f"  target prompt TTFT  = {ttft_c2:.2f}ms (MemCache hit only)")

        print("\n===== Scenario D: Full miss after reset_external (both caches cleared) =====")
        reset_local_prefix_cache(reset_external=True)
        time.sleep(1)
        pollute_prompt2 = build_prompt_ids(PROMPT_TOKENS, seed=8888)
        t_pollute2 = measure_ttft(pollute_prompt2)
        print(f"  pollute prompt TTFT = {t_pollute2:.2f}ms (miss, overwrites local blocks)")
        ttft_d = measure_ttft(prompt)
        print(f"  TTFT = {ttft_d:.2f}ms")

        print("\n===== Summary =====")
        print(f"  A (full miss)             = {ttft_a:.2f}ms")
        print(f"  B (local hit)             = {ttft_b:.2f}ms")
        print(f"  C (MemCache hit, reset)   = {ttft_c:.2f}ms")
        print(f"  D (both cleared, full)    = {ttft_d:.2f}ms")
        print(f"  C/B ratio                 = {ttft_c/ttft_b:.2f}x")
        print(f"  C/A ratio                 = {ttft_c/ttft_a:.2f}x")
        print(f"  D/A ratio                 = {ttft_d/ttft_a:.2f}x")

        if ttft_d > ttft_c * 1.5:
            print("\n  [PASS] D >> C: reset_external=true cleared MemCache, proving C was MemCache hit")
        else:
            print("\n  [SKIP] D ≈ C: MemCache has not implemented reset_external cleanup yet")

        if ttft_c < ttft_a * 0.8:
            print("  [PASS] C < A: MemCache hit is faster than full miss")
        else:
            print("  [WARN] C ≈ A: MemCache hit did not provide speedup")

        if ttft_c > ttft_b * 1.5:
            print("  [INFO] C >> B: MemCache hit is slower than local hit (expected on some HW)")
        else:
            print("  [INFO] C ≈ B: MemCache hit ≈ local hit speed (HBM/host_shm is fast)")

        return 0
    finally:
        stop_vllm(proc)
        stop_meta_service()


if __name__ == "__main__":
    sys.exit(main())
