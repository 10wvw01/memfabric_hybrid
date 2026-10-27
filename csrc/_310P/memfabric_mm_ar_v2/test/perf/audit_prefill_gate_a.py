#!/usr/bin/env python3
"""Offline Gate-A audit of captured per-site CSVs, without NPU access.

The script NEVER treats a sum of overlapping device activities as a Forward wall time.
Use only with preserved source CSVs from v2_prefill_attribution_20261021.
"""
import argparse
import csv
import json
from pathlib import Path
from statistics import mean, median

ARMS = ("stock_r0", "stock_r1", "v2_r0", "v2_r1")


def load_sites(path: Path):
    with path.open(newline="", encoding="utf-8") as f:
        rows = list(csv.DictReader(f))
    if not rows:
        raise ValueError(f"no rows: {path}")
    result = []
    for row in rows:
        converted = {k: float(v) for k, v in row.items() if k != "" and v not in (None, "")}
        converted["step"] = int(converted["step"])
        converted["site_idx"] = int(converted["site_idx"])
        result.append(converted)
    if [r["site_idx"] for r in result] != list(range(len(result))):
        raise ValueError(f"site index not consecutive: {path}")
    return result


def audit_rows(rows, arm):
    is_stock = arm.startswith("stock")
    steps = sorted(set(r["step"] for r in rows))
    if steps != list(range(len(steps))):
        raise ValueError(f"steps not consecutive: {arm}")
    groups = []
    for s in steps:
        g = [r for r in rows if r["step"] == s]
        if len(g) != 40:
            raise ValueError(f"expected exactly 40 sites in step {s} of {arm}, got {len(g)}")
        times = [r["mm_start_us_rel" if is_stock else "kernel_start_us_rel"] for r in g]
        if times != sorted(times):
            raise ValueError(f"unordered anchor times: {arm} step {s}")
        durs = [r["mm_dur_us"] + r["ar_dur_us"] if is_stock else r["kernel_dur_us"] for r in g]
        groups.append({
            "step": s,
            "sites": len(g),
            "anchor_first_ms": times[0] / 1000,
            "kernel_sum_ms": sum(durs) / 1000,
            "p50_site_us": median(durs),
            "max_site_us": max(durs),
        })
    periods = [groups[i + 1]["anchor_first_ms"] - groups[i]["anchor_first_ms"]
               for i in range(len(groups) - 1)]
    result = {"arm": arm, "n_sites": len(rows), "n_steps": len(groups),
              "step_site_sums_ms": [round(g["kernel_sum_ms"], 4) for g in groups],
              "step_periods_ms": [round(x, 4) for x in periods],
              "mean_period_ms": round(mean(periods), 4), "steps": groups}
    if not is_stock:
        outliers = [(r["site_idx"], r["step"], r["kernel_dur_us"])
                    for r in rows if r["kernel_dur_us"] >= 10000]
        result["outliers_us"] = outliers
        # Report only complete clean steps; do not divide 199 kernels by five steps.
        stable = [g for g in groups if not any(step == g["step"] for _, step, _ in outliers)]
        result["stable_complete_steps"] = [g["step"] for g in stable]
        result["stable_kernel_sum_ms_per_step"] = round(mean(g["kernel_sum_ms"] for g in stable), 4) if stable else None
        stable_periods = [periods[i] for i in range(len(periods))
                          if not any(step == i for _, step, _ in outliers)]
        result["stable_period_ms"] = round(mean(stable_periods), 4) if stable_periods else None
    return result


def corrected_clock(rank0_time_us, rank1_time_us, offset_rank0_minus_rank1_us):
    """offset=r0-r1; convert rank1 timestamp to rank0 clock by ADDING offset."""
    return rank0_time_us - (rank1_time_us + offset_rank0_minus_rank1_us)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--evidence-dir", type=Path, required=True,
                    help="the v2_prefill_attribution_20261021 evidence folder")
    ap.add_argument("--output", type=Path, required=True)
    args = ap.parse_args()
    root = args.evidence_dir
    audit = {arm: audit_rows(load_sites(root / "sites" / (arm + "_sites.csv")), arm)
             for arm in ARMS}
    orig = json.loads((root / "analysis/final_attribution.json").read_text(encoding="utf-8"))
    for arm in ARMS:
        if audit[arm]["n_sites"] != orig[arm]["n_sites"]:
            raise ValueError(f"site count mismatch with original JSON: {arm}")
    provenance = json.loads((root / "provenance.json").read_text(encoding="utf-8"))
    out = {
        "source": "offline CSV audit; not a calibrated cross-rank critical path",
        "provenance_host_sha256": provenance.get("host_core_so_sha256"),
        "arm_stats": audit,
        "delta_kernel_sum_stable_ms": {
            "r0": round(audit["v2_r0"]["stable_kernel_sum_ms_per_step"] - mean(audit["stock_r0"]["step_site_sums_ms"]), 4),
            "r1": round(audit["v2_r1"]["stable_kernel_sum_ms_per_step"] - mean(audit["stock_r1"]["step_site_sums_ms"]), 4),
        },
        "clock_alignment": "offset = median(r0_time-r1_time); corrected_r1=r1+offset; sign needs correction in historical compare2.py",
        "cautions": [
            "Kernel durations can overlap; do not call sums end-to-end Forward latency.",
            "Outliers: use complete clean steps, not 199 kernels divided by five steps.",
            "NPU pipe counters are activity metrics, not direct SDMA/IB phase timers.",
            "Input contiguous attribution needs original kernel and CPU API correlation.",
        ],
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(out, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(json.dumps({"stable_kernel_delta_ms": out["delta_kernel_sum_stable_ms"],
                      "host_sha": provenance.get("host_core_so_sha256")}, ensure_ascii=False))


if __name__ == "__main__":
    main()
