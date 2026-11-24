"""Summarize GM communication and consumption after checking both ranks."""
import json
import math
from pathlib import Path
import re
import statistics
import sys


def summarize(directory):
    ranks = []
    for rank in (0, 1):
        lines = (directory / f"rank{rank}.log").read_text().splitlines()
        if not any("full_payload_checked=1" in line for line in lines):
            raise ValueError(f"Missing payload validation: {directory}, rank {rank}")
        if not any("cleanup_complete=1" in line for line in lines):
            raise ValueError(f"Missing cleanup: {directory}, rank {rank}")
        rows = [dict(re.findall(r"(\w+)=([^ ]+)", line)) for line in lines if line.startswith("SAMPLE ")]
        samples = {int(row["iteration"]): {key: float(row[key]) for key in
                   ("event_us", "host_us", "enqueue_us")} for row in rows}
        if len(samples) != len(rows) or set(samples) != set(range(len(rows))):
            raise ValueError(f"Invalid sample coverage: {directory}, rank {rank}")
        ranks.append(samples)
    if not ranks[0] or ranks[0].keys() != ranks[1].keys():
        raise ValueError(f"Rank sample mismatch: {directory}")
    result = {"samples": len(ranks[0])}
    for metric in ("event_us", "host_us", "enqueue_us"):
        values = sorted(max(rank[i][metric] for rank in ranks) for i in ranks[0])
        if any(not math.isfinite(value) or value <= 0 for value in values):
            raise ValueError(f"Invalid timing: {directory}, {metric}")
        result[metric] = {"mean": statistics.mean(values), "p50": statistics.median(values),
                          "p95": values[math.ceil(len(values) * .95) - 1], "max": values[-1]}
    return result


if __name__ == "__main__":
    root = Path(sys.argv[1])
    results = {d.name: summarize(d) for d in sorted(root.iterdir()) if (d / "rank0.log").exists()}
    if not results:
        raise ValueError(f"No GM logs: {root}")
    (root / "summary.json").write_text(json.dumps(results, indent=2) + "\n")
    print(json.dumps(results, indent=2))
