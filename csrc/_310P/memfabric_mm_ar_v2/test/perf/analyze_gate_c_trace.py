#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""汇总 Gate C 两 Rank 设备 Kernel Trace；只比较整体 Kernel 耗时。

不把诊断变体时间差解释成正式 V2 的阶段时间，或当作 Qwen Forward。
可选管道聚合：仅当 result JSON 带 aic_metrics=true 时，从
prof_dir/profiling_data/*/mindstudio_profiler_output/op_summary_*.csv
读取同名 Kernel 的 mte2/scalar/mac/vec 管道计数器（硬件 PMU，非埋点）。
"""

from __future__ import annotations

import argparse
import csv
import json
import statistics
from pathlib import Path

NAMES = {
    "formal": "V2TrialKernel",
    "loop4": "V2DiagLoopKernel",
    "diag0": "V2DiagMmKernel",
    "diag1": "V2DiagNoIbKernel",
    "diag3": "V2DiagIbKernel",
    "diag4": "V2DiagIbDoneKernel",
    "diag5": "V2TrialKernel",
}

# 旧 mf310p_v2_trial_diag 每次调用固定 2 次 enable=0 牺牲发射 + 1 次
# enable=1 真实发射（时间序每三元组第 3 个）；formal/loop4 稳态每次 1 发射。
LAUNCHES_PER_CALL = {
    "formal": 1,
    "loop4": 1,
    "diag0": 3,
    "diag1": 3,
    "diag3": 3,
    "diag4": 3,
    "diag5": 3,
}

PIPE_METRICS = (
    "Task Duration(us)",
    "aicore_time(us)",
    "mte2_time(us)",
    "scalar_time(us)",
    "mac_time(us)",
    "vec_time(us)",
    "mte1_time(us)",
    "mte3_time(us)",
    "total_cycles",
)


def collect(path: Path, needle: str, launches_per_call: int = 1):
    content = json.loads(path.read_text(encoding="utf-8"))
    events = content.get("traceEvents", []) if isinstance(content, dict) else content
    if not isinstance(events, list):
        raise ValueError(f"traceEvents must be array: {path}")
    vals = [
        float(e["dur"])
        for e in events
        if isinstance(e, dict)
        and e.get("ph") == "X"
        and isinstance(e.get("dur"), (int, float))
        and needle in str(e.get("name", ""))
    ]
    if not vals:
        raise ValueError(f"no kernel '{needle}' in {path}; cannot report performance")
    # 时间序等价于列表序（导出按 ts 排序）；真实发射为每 launches_per_call 个的最后一个。
    if launches_per_call > 1:
        if len(vals) % launches_per_call:
            raise ValueError(f"{path}: kernel count {len(vals)} not divisible by {launches_per_call} launches/call")
        vals = vals[launches_per_call - 1 :: launches_per_call]
    return sorted(vals)


def quantile(vals, fract):
    return vals[min(len(vals) - 1, int((len(vals) - 1) * fract))]


def collect_pipe(prof_dir: Path, needle: str, iters: int, launches_per_call: int = 1):
    """读取 msprof op_summary 中同名 Kernel 的管道计数器；数量必须恰为 iters。"""
    files = sorted(prof_dir.glob("profiling_data/*_ascend_pt/PROF_*/mindstudio_profiler_output/op_summary_*.csv"))
    if not files:
        raise FileNotFoundError(f"no op_summary CSV under {prof_dir}")
    rows = []
    for f in files:
        with open(f, newline="", encoding="utf-8") as fh:
            rows.extend(r for r in csv.DictReader(fh) if needle in r.get("Op Name", ""))
    rows.sort(key=lambda r: float(r["Task Start Time(us)"]))
    if launches_per_call > 1:
        if len(rows) % launches_per_call:
            raise ValueError(f"{prof_dir}: kernel rows {len(rows)} not divisible by {launches_per_call} launches/call")
        rows = rows[launches_per_call - 1 :: launches_per_call]
    if len(rows) != iters:
        raise ValueError(f"{prof_dir}: expected {iters} '{needle}' rows, got {len(rows)}")
    out = {}
    for metric in PIPE_METRICS:
        try:
            vals = sorted(float(r[metric]) for r in rows)
        except (KeyError, ValueError) as exc:
            raise ValueError(f"{prof_dir}: metric {metric} missing/unparsable") from exc
        out[metric] = {
            "p50": round(statistics.median(vals), 3),
            "p90": round(quantile(vals, 0.90), 3),
            "max": round(max(vals), 3),
        }
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", required=True, type=Path)
    ap.add_argument("--out", required=True, type=Path)
    a = ap.parse_args()
    report = {
        "note": (
            "Each case captured in a separate TP2 torchrun session; numbers are device "
            "Kernel durations, NOT full Qwen Forward, host sync wall or SDMA/IB phase times."
        ),
        "cases": {},
    }
    for case, needle in NAMES.items():
        paths = sorted(a.root.glob(f"**/{case}_rank*_result.json"))
        if not paths:
            continue
        if len(paths) != 2:
            raise ValueError(f"{case}: need exactly two rank results, found {len(paths)}")
        pairs = {}
        for result_path in paths:
            meta = json.loads(result_path.read_text(encoding="utf-8"))
            rank = int(meta["rank"])
            if rank in pairs:
                raise ValueError(f"{case}: duplicate rank{rank}")
            if not (meta["bit_exact"] and meta["status"] == 0 and meta["profile"]):
                raise ValueError(f"{case} rank{rank}: invalid correctness/profile gate")
            trace = Path(meta["trace"])
            if not trace.is_file():
                raise FileNotFoundError(f"{case} rank{rank} trace missing: {trace}")
            vals = collect(trace, needle, LAUNCHES_PER_CALL[case])
            if len(vals) != int(meta["iters"]):
                raise ValueError(f"{case} rank{rank}: expected {meta['iters']} kernels, got {len(vals)}")
            pairs[rank] = {
                "n": len(vals),
                "p50_us": round(statistics.median(vals), 2),
                "p90_us": round(quantile(vals, 0.90), 2),
                "max_us": round(max(vals), 2),
                "min_us": round(min(vals), 2),
                "trace": str(trace),
                "M": int(meta["M"]),
                "chunks": int(meta["chunks"]),
            }
            if meta.get("aic_metrics"):
                prof_dir = Path(meta["prof_dir"])
                if not prof_dir.is_dir():
                    raise FileNotFoundError(f"{case} rank{rank} prof_dir missing: {prof_dir}")
                pairs[rank]["pipe"] = collect_pipe(prof_dir, needle, int(meta["iters"]), LAUNCHES_PER_CALL[case])
        if set(pairs) != {0, 1}:
            raise ValueError(f"{case}: expected ranks 0,1 got {sorted(pairs)}")
        report["cases"][case] = pairs
    if not report["cases"]:
        raise ValueError("no completed profiled case results")
    a.out.parent.mkdir(parents=True, exist_ok=True)
    a.out.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(json.dumps({case: {r: p["p50_us"] for r, p in rs.items()} for case, rs in report["cases"].items()}, indent=2))


if __name__ == "__main__":
    main()
