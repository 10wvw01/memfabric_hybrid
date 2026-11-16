"""Checks for the vLLM HBM-to-DRAM eviction scenario."""

import http.client
import json
import random
import time
from functools import lru_cache
from urllib.parse import urlencode


def meta_text(path):
    connection = http.client.HTTPConnection("127.0.0.1", 8000, timeout=30)
    try:
        connection.request("GET", path, headers={"Connection": "keep-alive"})
        response = connection.getresponse()
        payload = response.read().decode()
        assert response.status == 200, f"MetaService HTTP {response.status}: {payload}"
        return payload
    finally:
        connection.close()


def meta_summary():
    fields = dict(field.split("=", 1) for field in meta_text("/metrics/summary").split())
    result = {name: int(fields[name].split("/")[0]) for name in ("evict", "hbm_used", "dram_used")}
    result.update({name + "_total": int(fields[name + "_used"].split("/")[1]) for name in ("hbm", "dram")})
    return result


def all_keys():
    text = meta_text("/get_all_keys")
    assert not text.lstrip().startswith("{"), f"Failed to list cache keys: {text}"
    return set(text.splitlines())


def query_keys(keys):
    result = []
    keys = sorted(keys)
    for start in range(0, len(keys), 16):
        payload = json.loads(meta_text("/batch_query_keys?" + urlencode({"keys": ",".join(keys[start : start + 16])})))
        assert payload.get("success") is True, f"Failed to query cache keys: {payload}"
        result.extend(payload["data"])
    assert {item["key"] for item in result} == set(keys), f"Incomplete cache metadata: {result}"
    return result


def readable(items, ranks, medium=None):
    if not items:
        return False
    present_ranks = set()
    for item in items:
        blobs = item.get("blobs", [])
        if not item.get("valid") or not blobs:
            return False
        for blob in blobs:
            if blob["state"] != 1 or (medium is not None and blob["medium"] != medium):
                return False
            present_ranks.add(blob["rank"])
    return present_ranks == set(range(ranks))


def wait_saved_keys(before, ranks, timeout=30):
    deadline = time.monotonic() + timeout
    previous, stable = set(), 0
    while time.monotonic() < deadline:
        keys = all_keys() - before
        stable = stable + 1 if keys and keys == previous else 0
        if stable >= 2 and readable(query_keys(keys), ranks):
            return keys
        previous = keys
        time.sleep(0.5)
    raise AssertionError(f"Cache writes did not finish on all {ranks} ranks: keys={previous}, {meta_summary()}")


def dram_candidates(samples, ranks):
    return [sample for sample in samples if readable(query_keys(sample["keys"]), ranks, "DRAM")]


def wait_dram_candidates(samples, ranks, initial_evict, timeout=30):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        summary = meta_summary()
        candidates = dram_candidates(samples, ranks)
        if summary["evict"] > initial_evict and summary["dram_used"] > 0 and candidates:
            print(f"DRAM eviction verified: {summary}, complete prompts={len(candidates)}")
            return candidates
        time.sleep(0.5)
    raise AssertionError(f"No complete readable DRAM prompt after eviction: {summary}, samples={len(samples)}")


def read_token_stream(response, start):
    tokens, ttft, finished = [], None, False
    for line in response:
        if not line.startswith(b"data: "):
            continue
        data = line[6:].strip()
        if data == b"[DONE]":
            assert finished and tokens, "Completion ended without tokens and finish_reason"
            return ttft, tokens
        event = json.loads(data)
        if "error" in event:
            raise RuntimeError(f"completion stream error: {event['error']}")
        choices = event.get("choices", [])
        if choices:
            token_ids = choices[0].get("token_ids") or []
            if token_ids and ttft is None:
                ttft = (time.perf_counter() - start) * 1000
            tokens.extend(token_ids)
            finished = finished or choices[0].get("finish_reason") is not None
    raise AssertionError("Completion stream ended before [DONE]")


def completion_with_tokens(benchmark, prompt):
    body = json.dumps(
        {
            "model": benchmark._served_model_name,
            "prompt": prompt,
            "max_tokens": 8,
            "stream": True,
            "return_token_ids": True,
            "temperature": 0,
            "ignore_eos": True,
        }
    )
    connection = http.client.HTTPConnection("127.0.0.1", benchmark._port, timeout=120)
    try:
        start = time.perf_counter()
        connection.request("POST", "/v1/completions", body=body, headers={"Content-Type": "application/json"})
        response = connection.getresponse()
        assert response.status == 200, f"Completion HTTP {response.status}: {response.read()!r}"
        ttft, tokens = read_token_stream(response, start)
        assert len(tokens) == 8, f"Expected 8 generated tokens, received {tokens}"
        benchmark.reset_local_prefix_cache()
        return ttft, tokens
    finally:
        connection.close()


def cache_counts(benchmark):
    connection = http.client.HTTPConnection("127.0.0.1", benchmark._port, timeout=30)
    try:
        connection.request("GET", "/metrics")
        response = connection.getresponse()
        payload = response.read().decode()
        assert response.status == 200, f"Metrics HTTP {response.status}: {payload}"
    finally:
        connection.close()
    names = ("prefix_cache_hits_total", "external_prefix_cache_hits_total", "prefix_cache_queries_total")
    result = []
    for suffix in names:
        name = "vllm:" + suffix
        values = [
            float(line.split()[-1])
            for line in payload.splitlines()
            if line.startswith(name + "{") or line.startswith(name + " ")
        ]
        assert values, f"Missing required vLLM metric: {name}"
        result.append(sum(values))
    return result


def checked_completion(benchmark, prompt, external):
    before = cache_counts(benchmark)
    ttft, tokens = completion_with_tokens(benchmark, prompt)
    deadline = time.monotonic() + 20
    while time.monotonic() < deadline:
        delta = [end - start for start, end in zip(before, cache_counts(benchmark))]
        if delta[2] > 0:
            assert delta[0] == 0, f"Local cache unexpectedly supplied tokens: {delta}"
            assert (delta[1] > 0) == external, f"Expected external={external}, metric deltas={delta}"
            print(f"Cache source verified: local_hits={delta[0]}, external_hits={delta[1]}, queries={delta[2]}")
            return ttft, tokens
        time.sleep(0.5)
    raise AssertionError(f"Request metrics did not update: {delta}")


@lru_cache(maxsize=1)
def _tokenizer_for_model(model_path):
    from transformers import AutoTokenizer

    return AutoTokenizer.from_pretrained(model_path, trust_remote_code=True)


def eviction_prompt(model_path, seed, token_count=4096):
    tokenizer = _tokenizer_for_model(model_path)
    words = (
        "apple",
        "river",
        "stone",
        "cloud",
        "green",
        "paper",
        "silver",
        "forest",
        "window",
        "garden",
        "yellow",
        "ocean",
        "orange",
        "bridge",
        "winter",
        "summer",
    )
    phrase = " ".join(random.Random(seed).sample(words, 8))
    header = tokenizer.encode(
        f"Verification case {seed}. The verification phrase is: {phrase}.\n", add_special_tokens=False
    )
    filler = tokenizer.encode(
        "This document contains supporting notes about ordinary objects and daily activities. "
        "The verification phrase is recorded at the beginning of this document.\n",
        add_special_tokens=False,
    )
    suffix = tokenizer.encode(
        "\nCopy the verification phrase from the beginning, without explanation.\nVerification phrase:",
        add_special_tokens=False,
    )
    if token_count <= len(header) + len(suffix):
        raise ValueError("Eviction prompt budget must include the verification phrase and question")
    return (header + filler * (token_count // len(filler) + 1))[: token_count - len(suffix)] + suffix
