#!/usr/bin/env python3
"""Capture one vllm serving profile: /start_profile -> 1 request -> /stop_profile.

The server must have been started with VLLM_TORCH_PROFILER_DIR set; worker
traces land there (vllm-ascend TorchNPUProfilerWrapper, Level1+PipeUtilization).
This script returns immediately after stop; traces may take a few more
seconds to flush to disk.
"""
from __future__ import annotations

import argparse
import json
import random
import time
import urllib.request

p = argparse.ArgumentParser()
p.add_argument("--base", default="http://127.0.0.1:8000")
p.add_argument("--model", default="qwen3.6-35b-a3b-w8a8")
p.add_argument("--input-len", type=int, required=True)
p.add_argument("--max-tokens", type=int, default=2)
p.add_argument("--seed", type=int, default=777_000)
p.add_argument("--out", required=True)
a = p.parse_args()


def post(path: str, body: dict, timeout: int = 120):
    req = urllib.request.Request(
        a.base + path, data=json.dumps(body).encode(),
        headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.status, r.read().decode()[:500]


rng = random.Random(a.seed + a.input_len)
body = {"model": a.model,
        "prompt": [rng.randrange(1000, 100000) for _ in range(a.input_len)],
        "max_tokens": a.max_tokens, "temperature": 0.0, "stream": False}

t0 = time.time()
st, resp = post("/start_profile", {})
print(f"start_profile -> {st} ({time.time()-t0:.1f}s)", flush=True)

t0 = time.time()
req = urllib.request.Request(a.base + "/v1/completions",
                             data=json.dumps(body).encode(),
                             headers={"Content-Type": "application/json"})
with urllib.request.urlopen(req, timeout=600) as r:
    r.read()
print(f"request done ({time.time()-t0:.1f}s)", flush=True)

t0 = time.time()
st, resp = post("/stop_profile", {})
print(f"stop_profile -> {st} ({time.time()-t0:.1f}s)", flush=True)

with open(a.out, "w") as f:
    json.dump({"input_len": a.input_len, "status": st}, f, indent=2)
