"""Summarize one active AICore after validating both endpoints of every case."""

import argparse
import json
import math
import re
import statistics
from pathlib import Path

NAMES = {
    1: "put",
    2: "put_nbi",
    3: "get",
    4: "get_nbi",
    5: "put_signal",
    6: "put_signal_nbi",
    7: "signal_set",
    8: "signal_set_nbi",
    9: "quiet_after_nbi",
    10: "quiet_completed",
    11: "signal_test_ready",
    12: "signal_wait_ready",
}
SIZES = (262144, 1048576, 4194304, 16777216, 67108864)
METRICS = ("event_us", "completed_us", "host_us")


def expected_cases():
    return {
        (mode, variant, size)
        for mode in NAMES
        for variant in range(1 if mode >= 11 else 2)
        for size in (SIZES if mode <= 6 or mode == 9 else (4,))
    }


def fields(line):
    return dict(re.findall(r"(\w+)=([^ ]+)", line))


def read_rank(directory, rank):
    samples, passed = {}, {}
    lines = (directory / f"rank{rank}.log").read_text().splitlines()
    if not any(line == f"PASS rank={rank} cleanup_complete=1" for line in lines):
        raise ValueError(f"Missing completed run: rank {rank}")
    for line in lines:
        if not line.startswith(("SAMPLE ", "PASS ")):
            continue
        row = fields(line)
        if int(row["rank"]) != rank:
            raise ValueError(f"Wrong rank in rank{rank}.log")
        if "mode" not in row:
            continue
        case = tuple(int(row[key]) for key in ("mode", "context", "bytes"))
        if case not in expected_cases():
            raise ValueError(f"Unexpected case: {case}")
        if line.startswith("PASS "):
            if row.get("full_payload_checked") != "1" or case in passed:
                raise ValueError(f"Missing validation or duplicate result: {case}")
            passed[case] = int(row["rounds"])
            continue
        if rank != 0:
            raise ValueError("Passive rank emitted timed samples")
        key = (*case, int(row["iteration"]))
        if key in samples:
            raise ValueError(f"Duplicate sample: {key}")
        values = {name: float(row[name]) for name in METRICS}
        if any(not math.isfinite(value) or value <= 0 for value in values.values()):
            raise ValueError(f"Invalid timing: {key}")
        if values["completed_us"] < values["event_us"]:
            raise ValueError(f"Completion precedes API completion: {key}")
        samples[key] = values
    if set(passed) != expected_cases():
        raise ValueError(f"Incomplete API coverage: rank {rank}")
    for case, rounds in passed.items():
        actual = {key[3] for key in samples if key[:3] == case}
        if rounds <= 0 or (rank == 0 and actual != set(range(rounds))):
            raise ValueError(f"Incomplete iterations: {case}")
    return samples, passed


def statistics_for(values):
    values = sorted(values)
    return {
        "mean": statistics.mean(values),
        "p50": statistics.median(values),
        "p95": values[math.ceil(len(values) * 0.95) - 1],
        "p99": values[math.ceil(len(values) * 0.99) - 1],
        "max": values[-1],
    }


def summarize(directory):
    samples, active = read_rank(directory, 0)
    _, passive = read_rank(directory, 1)
    if active != passive:
        raise ValueError("Endpoint validation rounds differ")
    result = []
    for case in sorted(expected_cases()):
        keys = sorted(key for key in samples if key[:3] == case)
        mode, context, size = case
        row = {
            "mode": mode,
            "operation": NAMES[mode],
            "variant": "at" if context else ("local" if mode >= 11 else "gva"),
            "bytes": size,
            "samples": len(keys),
        }
        row.update({name: statistics_for([samples[key][name] for key in keys]) for name in METRICS})
        result.append(row)
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    result = summarize(args.directory)
    text = json.dumps(result, indent=2)
    (args.directory / "summary.json").write_text(text + "\n")
    print(text)


if __name__ == "__main__":
    main()
