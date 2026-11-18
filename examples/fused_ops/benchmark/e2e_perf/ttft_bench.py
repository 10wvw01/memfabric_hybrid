#!/usr/bin/env python3
"""E2E TTFT benchmark for stock-vs-fused serving (M=2048/4096 prompts).

Per invocation, against one already-running server:
  1 cold request   (first request of this server lifetime — only meaningful
                    for the FIRST ttft_bench invocation after server start)
  N warmup requests (absorb allocator/pipeline settling)
  R measured requests (unique 4096/2048-token prompts, one at a time,
                    streaming — TTFT = time to first SSE chunk)

Writes a JSON result file (--out).
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
p.add_argument("--input-len", type=int, required=True,
               help="prompt token count (2048 or 4096)")
p.add_argument("--requests", type=int, default=10)
p.add_argument("--warmup", type=int, default=2)
p.add_argument("--max-tokens", type=int, default=8)
p.add_argument("--seed-base", type=int, default=0,
               help="offset so seeds never repeat across invocations")
p.add_argument("--out", required=True)
a = p.parse_args()


def gen_tokens(seed: int) -> list[int]:
    rng = random.Random(seed)
    return [rng.randrange(1000, 100000) for _ in range(a.input_len)]


def one_request(seed: int):
    body = json.dumps({
        "model": a.model,
        "prompt": gen_tokens(seed),   # list of ints == token ids
        "max_tokens": a.max_tokens,
        "temperature": 0.0,
        "stream": True,
    }).encode()
    req = urllib.request.Request(a.base + "/v1/completions", data=body,
                                 headers={"Content-Type": "application/json"})
    t0 = time.perf_counter()
    ttft = None
    with urllib.request.urlopen(req, timeout=600) as r:
        for line in r:
            if line.startswith(b"data:") and b"[DONE]" not in line and ttft is None:
                ttft = time.perf_counter() - t0
    return ttft, time.perf_counter() - t0


def main():
    sb = a.seed_base + a.input_len * 1000
    cold_ttft, cold_total = one_request(sb + 999_000)
    print(f"cold   ttft={cold_ttft:.3f}s total={cold_total:.3f}s", flush=True)

    for i in range(a.warmup):
        ttft, total = one_request(sb + 900_000 + i)
        print(f"warmup[{i}] ttft={ttft:.3f}s", flush=True)

    ttfts, totals = [], []
    for i in range(a.requests):
        ttft, total = one_request(sb + i)
        ttfts.append(ttft)
        totals.append(total)
        print(f"req[{i:2d}] ttft={ttft:.3f}s total={total:.3f}s", flush=True)

    s = sorted(ttfts)
    n = len(s)

    def q(p):
        import math
        k = (n - 1) * p
        f, c = math.floor(k), math.ceil(k)
        return s[f] if f == c else s[f] * (c - k) + s[c] * (k - f)

    tpots = [(t - f) / a.max_tokens for f, t in zip(ttfts, totals)]
    ts = sorted(tpots)

    def tq(p):
        import math
        k = (len(ts) - 1) * p
        f, c = math.floor(k), math.ceil(k)
        return ts[f] if f == c else ts[f] * (c - k) + ts[c] * (k - f)

    result = {
        "input_len": a.input_len,
        "requests": n,
        "cold_ttft_s": cold_ttft,
        "ttft_p50_s": round(q(0.50), 4),
        "ttft_p90_s": round(q(0.90), 4),
        "ttft_p95_s": round(q(0.95), 4),
        "ttft_mean_s": round(sum(s) / n, 4),
        "ttft_min_s": s[0],
        "ttft_max_s": s[-1],
        "ttfts_s": [round(v, 4) for v in s],
        "tpot_p50_s": round(tq(0.50), 5),
        "tpot_p90_s": round(tq(0.90), 5),
        "tpot_p95_s": round(tq(0.95), 5),
    }
    with open(a.out, "w") as f:
        json.dump(result, f, indent=2)
    print(f"TTFT P50/P90/P95 = {result['ttft_p50_s']:.3f}/"
          f"{result['ttft_p90_s']:.3f}/{result['ttft_p95_s']:.3f}s "
          f"-> {a.out}", flush=True)


if __name__ == "__main__":
    main()
