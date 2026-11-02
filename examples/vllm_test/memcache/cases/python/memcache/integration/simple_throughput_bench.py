#!/usr/bin/env python3
"""Simple throughput benchmark using OpenAI API (no vllm bench dependency)."""

import argparse
import json
import random
import time
import threading
from collections import defaultdict
from concurrent.futures import ThreadPoolExecutor, as_completed
import urllib.request

VOCAB_SIZE = 151936


def generate_prompt(num_tokens, seed):
    rng = random.Random(seed)
    return [rng.randrange(VOCAB_SIZE) for _ in range(num_tokens)]


def send_request(url, model, prompt, max_tokens, timeout=120):
    payload = json.dumps({
        "model": model,
        "prompt": prompt,
        "max_tokens": max_tokens,
        "temperature": 0,
        "ignore_eos": True,
    }).encode()
    req = urllib.request.Request(url, data=payload, headers={"Content-Type": "application/json"})
    t0 = time.perf_counter()
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        data = json.loads(resp.read())
    t1 = time.perf_counter()
    usage = data.get("usage", {})
    completion_tokens = usage.get("completion_tokens", max_tokens)
    prompt_tokens = usage.get("prompt_tokens", len(prompt))
    cached_tokens = 0
    ptd = usage.get("prompt_tokens_details")
    if ptd:
        cached_tokens = ptd.get("cached_tokens", 0) or 0
    return {
        "latency_ms": (t1 - t0) * 1000,
        "ttft_ms": (t1 - t0) * 1000,
        "prompt_tokens": prompt_tokens,
        "completion_tokens": completion_tokens,
        "total_tokens": prompt_tokens + completion_tokens,
        "cached_tokens": cached_tokens,
        "cache_hit": cached_tokens > 0,
    }


def percentile(sorted_list, p):
    if not sorted_list:
        return 0
    idx = int(len(sorted_list) * p / 100)
    idx = min(idx, len(sorted_list) - 1)
    return sorted_list[idx]


def reset_prefix_cache(host, port):
    try:
        reset_url = f"http://{host}:{port}/reset_prefix_cache"
        req = urllib.request.Request(reset_url, method="POST")
        urllib.request.urlopen(req, timeout=10)
        print("[INFO] vLLM prefix cache reset")
    except Exception as e:
        print(f"[INFO] prefix cache reset not available: {e}")


def run_benchmark(url, model, input_len, output_len, num_prompts, request_rate, num_warmups,
                  cache_test=False, warmup_delay=3.0, host=None, port=None):
    mode = "cache-hit (repeated prompts)" if cache_test else "all-miss (unique prompts)"
    print(f"\n[INFO] Mode: {mode}")
    print(f"[INFO] Warmup: {num_warmups} requests...")
    for i in range(num_warmups):
        warmup_seed = 1000 if cache_test else -(i + 1)
        prompt = generate_prompt(input_len, seed=warmup_seed)
        send_request(url, model, prompt, output_len)
        if warmup_delay > 0:
            time.sleep(warmup_delay)
    print(f"[INFO] Warmup done")

    print(f"[INFO] Benchmark: {num_prompts} requests at {request_rate} req/s...")
    interval = 1.0 / request_rate
    results = []
    start_time = time.perf_counter()

    with ThreadPoolExecutor(max_workers=num_prompts) as pool:
        futures = []
        for i in range(num_prompts):
            if cache_test:
                prompt = generate_prompt(input_len, seed=1000)
            else:
                prompt = generate_prompt(input_len, seed=i)
            future = pool.submit(send_request, url, model, prompt, output_len)
            futures.append(future)
            if i < num_prompts - 1:
                time.sleep(interval)

        for i, future in enumerate(as_completed(futures)):
            try:
                result = future.result()
                results.append(result)
                if (i + 1) % 10 == 0:
                    cached = result.get("cached_tokens", 0)
                    print(f"  [{i+1}/{num_prompts}] latency={result['latency_ms']:.0f}ms cached={cached}")
            except Exception as e:
                print(f"  [ERROR] request {i}: {e}")
                results.append({"latency_ms": 0, "ttft_ms": 0, "prompt_tokens": 0,
                                "completion_tokens": 0, "total_tokens": 0,
                                "cached_tokens": 0, "cache_hit": False})

    total_time = time.perf_counter() - start_time

    latencies = sorted(r["latency_ms"] for r in results)
    total_input = sum(r["prompt_tokens"] for r in results)
    total_output = sum(r["completion_tokens"] for r in results)
    total_tokens = sum(r["total_tokens"] for r in results)
    total_cached = sum(r["cached_tokens"] for r in results)
    cache_hits = sum(1 for r in results if r.get("cache_hit", False))

    summary = {
        "total_time_s": total_time,
        "num_requests": len(results),
        "request_throughput": len(results) / total_time,
        "input_throughput": total_input / total_time,
        "output_throughput": total_output / total_time,
        "total_token_throughput": total_tokens / total_time,
        "mean_latency_ms": sum(latencies) / len(latencies) if latencies else 0,
        "p50_latency_ms": percentile(latencies, 50),
        "p90_latency_ms": percentile(latencies, 90),
        "p99_latency_ms": percentile(latencies, 99),
        "min_latency_ms": latencies[0] if latencies else 0,
        "max_latency_ms": latencies[-1] if latencies else 0,
        "total_input_tokens": total_input,
        "total_output_tokens": total_output,
        "total_cached_tokens": total_cached,
        "cache_hit_count": cache_hits,
        "cache_hit_rate": cache_hits / len(results) if results else 0,
        "cache_token_rate": total_cached / total_input if total_input > 0 else 0,
    }

    print(f"\n[INFO] Results:")
    print(f"  Total time:           {total_time:.1f}s")
    print(f"  Request throughput:   {summary['request_throughput']:.2f} req/s")
    print(f"  Input throughput:     {summary['input_throughput']:.2f} tok/s")
    print(f"  Output throughput:    {summary['output_throughput']:.2f} tok/s")
    print(f"  Total token throughput: {summary['total_token_throughput']:.2f} tok/s")
    print(f"  Mean latency:         {summary['mean_latency_ms']:.0f}ms")
    print(f"  P50 latency:          {summary['p50_latency_ms']:.0f}ms")
    print(f"  P90 latency:          {summary['p90_latency_ms']:.0f}ms")
    print(f"  P99 latency:          {summary['p99_latency_ms']:.0f}ms")
    print(f"  Cache hit count:      {cache_hits}/{len(results)} ({summary['cache_hit_rate']*100:.1f}%)")
    print(f"  Cached tokens:        {total_cached}/{total_input} ({summary['cache_token_rate']*100:.1f}%)")

    return summary


def main():
    parser = argparse.ArgumentParser(description="Simple throughput benchmark")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=4077)
    parser.add_argument("--model", default="Qwen3.6-35B-A3B")
    parser.add_argument("--input-len", type=int, default=1024)
    parser.add_argument("--output-len", type=int, default=128)
    parser.add_argument("--num-prompts", type=int, default=100)
    parser.add_argument("--request-rate", type=float, default=10)
    parser.add_argument("--num-warmups", type=int, default=5)
    parser.add_argument("--warmup-delay", type=float, default=3.0,
                        help="Seconds to wait after each warmup request for async KV save to complete")
    parser.add_argument("--label", default="bench")
    parser.add_argument("--output-file", default="")
    parser.add_argument("--cache-test", action="store_true",
                        help="Send repeated prompts to test cache hit rate")
    args = parser.parse_args()

    url = f"http://{args.host}:{args.port}/v1/completions"
    print(f"[INFO] Server: {url}")
    print(f"[INFO] Model: {args.model}")
    print(f"[INFO] Input len: {args.input_len}, Output len: {args.output_len}")
    print(f"[INFO] Prompts: {args.num_prompts}, Rate: {args.request_rate} req/s")

    result = run_benchmark(
        url, args.model,
        args.input_len, args.output_len,
        args.num_prompts, args.request_rate, args.num_warmups,
        cache_test=args.cache_test,
        warmup_delay=args.warmup_delay,
        host=args.host, port=args.port,
    )

    result["label"] = args.label
    if args.output_file:
        with open(args.output_file, "w") as f:
            json.dump(result, f, indent=2)
        print(f"[INFO] Saved to {args.output_file}")


if __name__ == "__main__":
    main()
