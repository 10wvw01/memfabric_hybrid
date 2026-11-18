#!/usr/bin/env python3
"""smem_mm_310p3 fused operators functional verification against a running vllm server.

1. Waits for /health.
2. Long prompt (8k tokens) -> chunked prefill with 4096-token chunks ->
   M>=1280 -> fused MM+AR path (ar or iar operator).
3. Short prompt -> decode / small batch -> native fallback path.
4. Prints generations + timings for eyeball sanity (answer coherence).
"""
import json
import time
import urllib.request

BASE = "http://127.0.0.1:8000"
MODEL = "qwen3.6-35b-a3b-w8a8"


def wait_health(timeout_s=900):
    t0 = time.time()
    while time.time() - t0 < timeout_s:
        try:
            with urllib.request.urlopen(BASE + "/health", timeout=3) as r:
                if r.status == 200:
                    print(f"[health] OK after {time.time()-t0:.0f}s")
                    return
        except Exception:
            pass
        time.sleep(5)
    raise SystemExit("server not healthy in time")


def chat(prompt, max_tokens, tag):
    body = json.dumps({
        "model": MODEL,
        "messages": [{"role": "user", "content": prompt}],
        "max_tokens": max_tokens,
        "temperature": 0.0,
    }).encode()
    req = urllib.request.Request(BASE + "/v1/chat/completions", data=body,
                                 headers={"Content-Type": "application/json"})
    t0 = time.time()
    with urllib.request.urlopen(req, timeout=600) as r:
        out = json.loads(r.read())
    dt = time.time() - t0
    text = out["choices"][0]["message"]["content"]
    usage = out.get("usage", {})
    print(f"\n===== {tag} ({dt:.1f}s, prompt={usage.get('prompt_tokens')} tok, "
          f"completion={usage.get('completion_tokens')} tok) =====")
    print(text[:600])
    return text


# ~5.5k tokens of filler: chunked prefill splits it into 4096+rest, both
# chunks >= 2048 rows where possible, and stays under max_model_len 8192.
filler = ("The quick brown fox jumps over the lazy dog. " * 500)

if __name__ == "__main__":
    wait_health()
    long_out = chat(
        f"{filler}\n\nHere is a long passage above. Now answer concisely: "
        "what is the capital of France, and what is 17*23?",
        64, "LONG PREFILL (fused path, M>=2048)")
    short_out = chat(
        "Answer in one short sentence: what is the capital of Japan?",
        32, "SHORT (fallback path, M<2048)")
    ok = ("paris" in long_out.lower()) and ("391" in long_out.replace(",", "")) \
         and ("tokyo" in short_out.lower())
    print("\n===== sanity:", "PASS" if ok else "CHECK OUTPUT", "=====")
