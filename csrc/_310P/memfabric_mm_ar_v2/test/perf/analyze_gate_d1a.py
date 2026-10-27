#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""汇总 D1-A Profiler 扰动会话：三模式（external/profile/aic）×（formal/loop4）。

对每会话每 Rank 计算 wall_us / event_us /（profiler 会话）chrome trace Kernel
时长的 p50/p90/p99/min/max；停顿普查（>=6ms，Gate C 同阈值）；双 Rank 同样本
配对检验；aic 会话另从 msprof op_summary 取逐样本管道计数器（fail-closed）。

口径声明：wall/event 是外部测量（见 driver JSON 内 timing_semantics），
kernel_us 是 profiler 设备侧时长；三者在报告中共存但禁止互相混同。
"""

from __future__ import annotations

import argparse
import csv
import json
import statistics
from pathlib import Path

KERNEL_NAMES = {"formal": "V2TrialKernel", "loop4": "V2DiagLoopKernel"}
STALL_US = 6000.0
PIPE_METRICS = ("Task Duration(us)", "mte2_time(us)", "scalar_time(us)", "mac_time(us)", "vec_time(us)")


def quantile(sorted_vals, fract):
    return sorted_vals[min(len(sorted_vals) - 1, int((len(sorted_vals) - 1) * fract))]


def stats(vals):
    s = sorted(vals)
    return {
        "n": len(s),
        "p50": round(statistics.median(s), 1),
        "p90": round(quantile(s, 0.90), 1),
        "p99": round(quantile(s, 0.99), 1),
        "max": round(max(s), 1),
        "min": round(min(s), 1),
    }


def collect_kernel(trace: Path, needle: str):
    content = json.loads(trace.read_text(encoding="utf-8"))
    events = content.get("traceEvents", []) if isinstance(content, dict) else content
    if not isinstance(events, list):
        raise ValueError(f"traceEvents must be array: {trace}")
    vals = [
        float(e["dur"])
        for e in events
        if isinstance(e, dict)
        and e.get("ph") == "X"
        and isinstance(e.get("dur"), (int, float))
        and needle in str(e.get("name", ""))
    ]
    if not vals:
        raise ValueError(f"no kernel '{needle}' in {trace}")
    return vals  # us, time order = list order


def collect_pipe_rows(prof_dir: Path, needle: str):
    files = sorted(prof_dir.glob("profiling_data/*_ascend_pt/PROF_*/mindstudio_profiler_output/op_summary_*.csv"))
    if not files:
        raise FileNotFoundError(f"no op_summary CSV under {prof_dir}")
    rows = []
    for f in files:
        with open(f, newline="", encoding="utf-8") as fh:
            rows.extend(r for r in csv.DictReader(fh) if needle in r.get("Op Name", ""))
    rows.sort(key=lambda r: float(r["Task Start Time(us)"]))
    return rows


def analyze_session(session_dir: Path):
    results = sorted(session_dir.glob("*_rank*_d1a.json"))
    if len(results) != 2:
        raise ValueError(f"{session_dir}: need exactly two rank results, found {len(results)}")
    metas = {}
    for rp in results:
        meta = json.loads(rp.read_text(encoding="utf-8"))
        if not (meta["bit_exact"] and meta["status"] == 0):
            raise ValueError(f"{rp}: correctness gate failed")
        metas[int(meta["rank"])] = meta
    if set(metas) != {0, 1}:
        raise ValueError(f"{session_dir}: expected ranks 0,1")
    m0, m1 = metas[0], metas[1]
    for key in ("case", "mode", "iters"):
        if m0[key] != m1[key]:
            raise ValueError(f"{session_dir}: rank metadata mismatch on {key}")

    case, mode, iters = m0["case"], m0["mode"], int(m0["iters"])
    needle = KERNEL_NAMES[case]
    out = {"session": session_dir.name, "case": case, "mode": mode, "iters": iters, "ranks": {}}
    for rank, meta in metas.items():
        samples = meta["samples"]
        if len(samples) != iters:
            raise ValueError(f"{session_dir} rank{rank}: {len(samples)} samples != iters {iters}")
        walls = [s["wall_us"] for s in samples]
        events = [s["event_us"] for s in samples]
        entry = {
            "wall": stats(walls),
            "event": stats(events),
            "wall_clean": stats([v for v in walls if v < STALL_US]),
            "event_clean": stats([v for v in events if v < STALL_US]),
            "samples": samples,
            "stall_samples_wall": [i for i, v in enumerate(walls) if v >= STALL_US],
            "stall_samples_event": [i for i, v in enumerate(events) if v >= STALL_US],
        }
        if mode != "external":
            kernels = collect_kernel(Path(meta["trace"]), needle)
            if len(kernels) != iters:
                raise ValueError(f"{session_dir} rank{rank}: trace kernels {len(kernels)} != iters {iters}")
            entry["kernel"] = stats(kernels)
            entry["kernel_clean"] = stats([v for v in kernels if v < STALL_US])
            entry["stall_samples_kernel"] = [i for i, v in enumerate(kernels) if v >= STALL_US]
            entry["kernel_us_by_sample"] = [round(v, 1) for v in kernels]
        if mode == "aic":
            rows = collect_pipe_rows(Path(meta["prof_dir"]), needle)
            if len(rows) != iters:
                raise ValueError(f"{session_dir} rank{rank}: op_summary rows {len(rows)} != iters {iters}")
            pipes = {}
            for metric in PIPE_METRICS:
                pipes[metric] = [round(float(r[metric]), 1) for r in rows]
            entry["pipe_by_sample"] = pipes
        out["ranks"][str(rank)] = entry

    # 双 Rank 同样本配对（external 口径优先；profiler 会话补 kernel 口径）
    r0, r1 = out["ranks"]["0"], out["ranks"]["1"]
    pairing = {}
    for metric in ("wall", "event"):
        s0 = [s[metric + "_us"] for s in m0["samples"]]
        s1 = [s[metric + "_us"] for s in m1["samples"]]
        both = [i for i in range(iters) if s0[i] >= STALL_US and s1[i] >= STALL_US]
        only0 = [i for i in range(iters) if s0[i] >= STALL_US and s1[i] < STALL_US]
        only1 = [i for i in range(iters) if s1[i] >= STALL_US and s0[i] < STALL_US]
        ratios = [
            {"i": i, "r0": s0[i], "r1": s1[i], "dur_ratio": round(max(s0[i], s1[i]) / max(1e-9, min(s0[i], s1[i])), 3)}
            for i in both
        ]
        pairing[metric] = {"both_stall": both, "only_rank0": only0, "only_rank1": only1, "paired_detail": ratios}
    if mode != "external":
        k0 = r0["kernel_us_by_sample"]
        k1 = r1["kernel_us_by_sample"]
        both = [i for i in range(iters) if k0[i] >= STALL_US and k1[i] >= STALL_US]
        pairing["kernel"] = {
            "both_stall": both,
            "only_rank0": [i for i in range(iters) if k0[i] >= STALL_US and k1[i] < STALL_US],
            "only_rank1": [i for i in range(iters) if k1[i] >= STALL_US and k0[i] < STALL_US],
        }
    out["pairing"] = pairing
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", required=True, type=Path, help="dir containing session subdirs")
    ap.add_argument("--out", required=True, type=Path)
    a = ap.parse_args()
    sessions = []
    for session_dir in sorted(p for p in a.root.iterdir() if p.is_dir()):
        # 下划线前缀 = 隔离/保全目录（被杀或被污染的会话），不进入统计。
        if session_dir.name.startswith(("c0", "_")):
            continue
        sessions.append(analyze_session(session_dir))

    group_summary = {}
    for case in KERNEL_NAMES:
        group_summary[case] = {}
        for mode in ("external", "profile", "aic"):
            sess = [s for s in sessions if s["case"] == case and s["mode"] == mode]
            agg = {"sessions": [s["session"] for s in sess]}
            for rank in ("0", "1"):
                ev_clean = [s["ranks"][rank]["event_clean"]["p50"] for s in sess]
                agg[f"rank{rank}_event_clean_p50_us"] = ev_clean
                stalls = sum(len(s["ranks"][rank]["stall_samples_event"]) for s in sess)
                total = sum(s["iters"] for s in sess)
                agg[f"rank{rank}_event_stalls"] = f"{stalls}/{total}"
            group_summary[case][mode] = agg

    report = {
        "note": (
            "D1-A perturbation census; wall/event are external measurements, "
            "kernel_us is profiler device duration; stall threshold 6ms (Gate C)."
        ),
        "sessions": sessions,
        "group_summary": group_summary,
    }
    a.out.parent.mkdir(parents=True, exist_ok=True)
    a.out.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(json.dumps(group_summary, indent=2))


if __name__ == "__main__":
    main()
