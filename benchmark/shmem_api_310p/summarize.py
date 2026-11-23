"""Require complete two-rank correctness records before aggregating API timings."""

import argparse
import json
import math
import re
import statistics
from pathlib import Path

PAYLOAD = {
    "put",
    "get",
    "put-nbi",
    "get-nbi",
    "put-signal",
    "put-signal-nbi",
    "quiet-after-put",
    "put-add-fp32",
    "put-add-fp16",
}
CONTROL = {"signal-set", "signal-set-nbi", "quiet-idle", "signal-test", "signal-wait-ready", "ctx"}
SIZES = (64, 4096, 262144, 1048576, 4194304, 16777216, 67108864)
METRICS = ("event_us", "host_us")
EXPECTED = {(api, size) for api in PAYLOAD for size in SIZES} | {(api, 0) for api in CONTROL}


def fields(line):
    return dict(re.findall(r"(\w+)=([^ ]+)", line))


def read_rank(directory, rank):
    lines = (directory / f"rank{rank}.log").read_text().splitlines()
    if any(line.startswith("ERROR") for line in lines) or not any("cleanup_complete=1" in line for line in lines):
        raise ValueError(f"Failed or incomplete rank {rank}")
    configurations = [fields(line) for line in lines if line.startswith("CONFIG api_style=")]
    if len(configurations) != 1:
        raise ValueError(f"Missing or duplicate run configuration for rank {rank}")
    config = configurations[0]
    api_filter = config["api_filter"]
    expected = {key for key in EXPECTED if api_filter == "all" or key[0] == api_filter}
    if not expected or config["api_style"] not in ("at", "gva") or int(config["rounds"]) <= 0:
        raise ValueError(f"Invalid run configuration for rank {rank}")
    cases, samples = {}, {}
    for line in lines:
        row = fields(line)
        if line.startswith("PASS ") and "api" in row:
            key = row["api"], int(row["bytes"])
            if key in cases or int(row["rank"]) != rank or row.get("full_payload_checked") != "1":
                raise ValueError(f"Invalid correctness record: {line}")
            cases[key] = int(row["rounds"])
        if line.startswith("SAMPLE "):
            key = row["api"], int(row["bytes"]), int(row["iteration"])
            if rank or key in samples:
                raise ValueError(f"Unexpected or duplicate sample: {line}")
            samples[key] = {metric: float(row[metric]) for metric in METRICS}
    if cases.keys() != expected or any(count != int(config["rounds"]) for count in cases.values()):
        raise ValueError(f"Missing or unexpected cases for rank {rank}")
    return cases, config, samples


def aggregate(directory):
    cases, config, samples = read_rank(directory, 0)
    peer_cases, peer_config, _ = read_rank(directory, 1)
    if config != peer_config or cases != peer_cases:
        raise ValueError("Rank correctness records differ")
    expected_samples = {(api, size, iteration) for (api, size), count in cases.items() for iteration in range(count)}
    if samples.keys() != expected_samples or not samples:
        raise ValueError("Missing or unexpected samples")
    if any(not math.isfinite(v) or v < 0 for row in samples.values() for v in row.values()):
        raise ValueError("Invalid timing")
    result = []
    for api, size in sorted(cases):
        count = cases[api, size]
        entry = {"api": api, "api_style": config["api_style"], "bytes": size, "samples": count}
        for metric in METRICS:
            values = sorted(samples[api, size, i][metric] for i in range(count))
            entry[metric] = {
                "mean": statistics.mean(values),
                "p50": statistics.median(values),
                "p95": values[math.ceil(count * 0.95) - 1],
                "p99": values[math.ceil(count * 0.99) - 1],
                "max": max(values),
            }
        result.append(entry)
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    result = aggregate(args.directory)
    content = json.dumps(result, indent=2) + "\n"
    (args.directory / "summary.json").write_text(content)
    print(content, end="")


if __name__ == "__main__":
    main()
