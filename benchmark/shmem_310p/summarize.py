"""Aggregate per-iteration slowest-rank timings, after successful validation."""
import argparse
import json
import math
import re
import statistics
from pathlib import Path


def summarize(directory):
    ranks = []
    for rank in (0, 1):
        lines = (directory / f"rank{rank}.log").read_text().splitlines()
        if sum(line.startswith("PASS") and "full_payload_checked=1" in line for line in lines) != 5:
            raise ValueError(f"Missing correctness checks: {directory}, rank {rank}")
        if not any("cleanup_complete=1" in line for line in lines):
            raise ValueError(f"Missing successful cleanup: {directory}, rank {rank}")
        samples = {}
        for line in lines:
            if not line.startswith("SAMPLE "):
                continue
            row = dict(re.findall(r"(\w+)=([^ ]+)", line))
            if int(row["rank"]) != rank:
                raise ValueError(f"Wrong rank in {directory}")
            key = int(row["dim"]), int(row["iteration"])
            if key in samples:
                raise ValueError(f"Duplicate sample: {key}")
            samples[key] = {name: float(row[name]) for name in ("host_us", "event_us", "enqueue_us")}
        if any(not math.isfinite(value) or value < 0 for row in samples.values() for value in row.values()):
            raise ValueError(f"Invalid timing: {directory}")
        ranks.append(samples)
    if not ranks[0] or ranks[0].keys() != ranks[1].keys():
        raise ValueError(f"Rank samples differ: {directory}")
    result = {}
    for dim in sorted({key[0] for key in ranks[0]}):
        keys = [key for key in ranks[0] if key[0] == dim]
        result[dim] = {"samples": len(keys)}
        for name in ("host_us", "event_us", "enqueue_us"):
            values = sorted(max(rank[key][name] for rank in ranks) for key in keys)
            result[dim][name] = {
                "mean": statistics.mean(values), "p50": statistics.median(values),
                "p95": values[math.ceil(len(values) * .95) - 1],
                "p99": values[math.ceil(len(values) * .99) - 1], "max": max(values),
            }
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    result = {p.name: summarize(p) for p in sorted(args.directory.iterdir())
              if p.is_dir() and (p / "rank0.log").exists()}
    if not result:
        parser.error("No benchmark logs found")
    text = json.dumps(result, indent=2)
    (args.directory / "summary.json").write_text(text + "\n")
    print(text)


if __name__ == "__main__":
    main()
