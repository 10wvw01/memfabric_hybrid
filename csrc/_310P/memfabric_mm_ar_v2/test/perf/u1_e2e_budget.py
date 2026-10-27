#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""U1 Eager全模型收益空间预算：必须输入实际调用次数与总耗时。
算出的只是“所有节省都位于关键路径时”的乐观边界，不是Serving实测吞吐改善。
"""
from __future__ import annotations
import argparse
import json
import math
from pathlib import Path


def build_budget(perf_maps: list[dict], observed: dict) -> dict:
    workload = {
        "input_tokens": 4096, "output_tokens": 2048, "tp": 2,
        "batch_max": 10, "eager": True,
    }
    for k, v in workload.items():
        if observed.get(k) != v:
            raise ValueError(f"workload mismatch {k}: expected {v!r}")
    total = observed.get("elapsed_seconds")
    if isinstance(total, bool) or not isinstance(total, (int, float)) or not math.isfinite(total) or total <= 0:
        raise ValueError("elapsed_seconds must be real positive user-observed value")
    if not isinstance(observed.get("measurement_id"), str) or not observed["measurement_id"].strip():
        raise ValueError("actual Eager measurement ID required")
    calls = observed.get("calls")
    if not isinstance(calls, list) or not calls:
        raise ValueError("no actual routed operator calls; refuse to assume call frequency")
    known = {}
    for report in perf_maps:
        if report.get("experiment") != "U1_FULLCHAIN":
            raise ValueError("not U1 evidence")
        for r in report.get("performance_map", []):
            if r["M"] in known:
                raise ValueError(f"duplicate measured M={r['M']}")
            if r.get("decision") == "INCONCLUSIVE_DRIFT":
                continue
            known[r["M"]] = r
    total_potential_us = 0
    details = []
    seen = set()
    for row in calls:
        m, n = row.get("M"), row.get("count")
        if (type(m) is not int or m < 1 or type(n) is not int or n < 0 or m in seen):
            raise ValueError("duplicate/noninteger/negative M or count")
        seen.add(m)
        ref = known.get(m)
        if ref is None:
            details.append({"M": m, "count": n, "status": "UNKNOWN_MEASURED_SHAPE_OR_DRIFT"})
            continue
        delta = ref["stock_p50_us"] - ref["fused_p50_us"]
        optimistic = max(0.0, delta * n)
        total_potential_us += optimistic
        details.append({
            "M": m, "count": n, "stock_p50_us": ref["stock_p50_us"],
            "fused_p50_us": ref["fused_p50_us"],
            "potential_saved_us_if_all_critical": round(optimistic, 3),
            "candidate_only": ref["decision"],
        })
    return {
        "measurement_id": observed["measurement_id"], "workload": workload,
        "elapsed_seconds": total, "calls": details,
        "optimistic_time_saving_seconds": round(total_potential_us / 1e6, 6),
        "optimistic_wall_fraction_pct": round(total_potential_us / (total * 1e6) * 100, 6),
        "observed_model_speedup_pct": None,
        "note": "Upper bound under independent additive critical-path assumption; "
                "actual stream/compute overlap and dispatch can reduce gains to zero. "
                "No claims from missing M or estimated call counts.",
    }


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--maps", nargs="+", required=True, type=Path)
    p.add_argument("--observed", required=True, type=Path)
    p.add_argument("--out", required=True, type=Path)
    a = p.parse_args()
    if a.out.exists():
        p.error("refuse overwrite")
    o = build_budget([json.loads(x.read_text()) for x in a.maps],
                     json.loads(a.observed.read_text()))
    a.out.write_text(json.dumps(o, ensure_ascii=False, indent=2) + "\n")
    print("Budget ONLY (not new Qwen E2E validation):", o["optimistic_wall_fraction_pct"], "%")


if __name__ == "__main__":
    main()
