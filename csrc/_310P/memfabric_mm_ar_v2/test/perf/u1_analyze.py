#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""U1纯CPU分析：多M双Rank配对，screen与confirm分离，缺原始日志/证据fail-closed。"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import statistics
from pathlib import Path

from u1_spec import (
    ANCHOR_PCT_MAX, D4_DEVICE_SHA, FUSED_ARM, ITERS, K, KVER_SHA,
    LARGE_MS, MAX_TAIL_DEGRADE, MEMFABRIC_HOST_SHA, MIN_SPEEDUP_PCT,
    N, PLUGIN_SHA, SHAPES, SMALL_MS, WARMUP, arm_for, order_for, plan,
    session_id,
)


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def pctl(values: list[float], pc: float) -> float:
    if not values:
        raise ValueError("empty timing vector")
    a = sorted(values)
    return a[math.ceil(len(a) * pc / 100) - 1]


def _finite(v) -> bool:
    return isinstance(v, (int, float)) and not isinstance(v, bool) and math.isfinite(v) and v > 0


def paired(ranks: list[dict], spec: dict) -> dict:
    scope, phase = spec["scope"], spec["phase"]
    expected_arm = spec["arm"]
    for rank, record in enumerate(ranks):
        expected = {
            "experiment": "U1_FULLCHAIN", "scope": scope, "phase": phase,
            "session": spec["session"], "stage": spec["stage"],
            "arm": expected_arm, "rank": rank, "TP": 2, "K": K, "N": N,
            "dtype": "fp16", "reference": "CPU_GLOO_FP32_SUM_TO_FP16",
            "stock_collective": "FP16_HCCL_no_FP32_conversion",
            "comm_isolation": "separate process; never HCCL with MemFabric pool",
            "device_so_sha256": D4_DEVICE_SHA, "plugin_sha256": PLUGIN_SHA,
            "host_so_sha256": MEMFABRIC_HOST_SHA, "kver11_sha256": KVER_SHA,
            "profile": False,
        }
        for k, v in expected.items():
            if record.get(k) != v:
                raise ValueError(f"{spec['session']} rank{rank} invalid {k}")
        if tuple(record.get("M_order", [])) != tuple(spec["M_order"]):
            raise ValueError("M order rotated or skipped incorrectly")
        rs = record.get("results", {})
        if not isinstance(rs, dict):
            raise ValueError("results must be object")
        if set(k for k in rs if not k.startswith("_")) != {str(m) for m in SHAPES[scope]}:
            raise ValueError("missing/extra M in session")
    out = {}
    for m in SHAPES[scope]:
        dat = [rec["results"][str(m)] for rec in ranks]
        for d in dat:
            expected = {"M": m, "arm": expected_arm,
                        "warmup": WARMUP[phase], "iters": ITERS[phase],
                        "bit_exact": True,
                        "status": None if expected_arm == "stock_fp16" else 0}
            for k, v in expected.items():
                if d.get(k) != v:
                    raise ValueError(f"M{m} broken contract {k}")
            if len(d.get("samples", [])) != ITERS[phase]:
                raise ValueError("missing complete samples")
        events, walls, gaps = [], [], []
        for i, (r0, r1) in enumerate(zip(dat[0]["samples"], dat[1]["samples"])):
            for item in (r0, r1):
                if item.get("i") != i or item.get("variant") != (i + WARMUP[phase]) % 2:
                    raise ValueError(f"M{m} misaligned Rank samples")
                if not _finite(item.get("event_us")) or not _finite(item.get("wall_us")):
                    raise ValueError(f"M{m} invalid timing sample")
            events.append(max(r0["event_us"], r1["event_us"]))
            walls.append(max(r0["wall_us"], r1["wall_us"]))
            gaps.append(max(r0["wall_us"] - r0["event_us"],
                            r1["wall_us"] - r1["event_us"]))
        out[str(m)] = {
            "M": m, "arm": expected_arm, "count": len(events),
            "event_p50_us": round(statistics.median(events), 3),
            "event_p90_us": round(pctl(events, 90), 3),
            "event_p99_us": round(pctl(events, 99), 3),
            "wall_p50_us": round(statistics.median(walls), 3),
            "host_gap_p50_us": round(statistics.median(gaps), 3),
            "paired_event_us": events, "paired_wall_us": walls,
        }
        if expected_arm == "stock_fp16" and phase == "screen":
            mm = [data.get("native_mm_only") for data in dat]
            if any(not isinstance(x, dict) or x.get("bit_exact") is not True or
                   x.get("iters") != ITERS[phase] or
                   len(x.get("samples", [])) != ITERS[phase] for x in mm):
                raise ValueError(f"M{m} Stock native MM-only samples missing")
            paired_mm = []
            for i, (r0, r1) in enumerate(zip(mm[0]["samples"], mm[1]["samples"])):
                for row in (r0, r1):
                    if row.get("i") != i or row.get("variant") != (i + WARMUP[phase]) % 2:
                        raise ValueError("Stock MM-only samples not paired")
                    if not _finite(row.get("event_us")) or not _finite(row.get("wall_us")):
                        raise ValueError("Stock MM-only event invalid")
                paired_mm.append(max(r0["event_us"], r1["event_us"]))
            out[str(m)]["native_mm_only_p50_us"] = round(statistics.median(paired_mm), 3)
            out[str(m)]["native_mm_only_p90_us"] = round(pctl(paired_mm, 90), 3)
    anchor_m = spec["M_order"][0]
    late = [rec["results"].get("_anchor_after") for rec in ranks]
    if any(not isinstance(x, dict) or x.get("M") != anchor_m or
           len(x.get("samples", [])) != 8 for x in late):
        raise ValueError("late same-worker anchor missing/invalid")
    late_events = []
    for i, (r0, r1) in enumerate(zip(late[0]["samples"], late[1]["samples"])):
        for sample in (r0, r1):
            if sample.get("i") != i or sample.get("variant") != i % 2:
                raise ValueError("late anchor rank index/variant mismatch")
            if not _finite(sample.get("event_us")) or not _finite(sample.get("wall_us")):
                raise ValueError("late anchor invalid timing")
        late_events.append(max(r0["event_us"], r1["event_us"]))
    early = statistics.median(out[str(anchor_m)]["paired_event_us"][:8])
    after = statistics.median(late_events)
    drift = 100 * (after / early - 1)
    out["_anchor"] = {"M": anchor_m, "early_pair_event_p50_us": round(early, 3),
                      "late_pair_event_p50_us": round(after, 3),
                      "late_over_early_pct": round(drift, 3),
                      "abs_drift_pct": round(abs(drift), 3)}
    return out


def evaluate(root: Path, scope: str, phase: str, audit: Path | None = None) -> dict:
    if phase == "smoke":
        raise ValueError("smoke proves correctness only; never analyze as performance")
    specs = plan(scope, phase)
    sessions = sorted(p for p in (root / "sessions").iterdir() if p.is_dir())
    if [p.name for p in sessions] != [z["session"] for z in specs]:
        raise ValueError("missing/mismatched ABBA sessions")
    manifest = root / "manifest.json"
    if not manifest.is_file():
        raise ValueError("missing immutable G0 and session identity manifest")
    meta = json.loads(manifest.read_text())
    for key, value in {
        "experiment": "U1_FULLCHAIN", "scope": scope, "phase": phase,
        "device_so_sha256": D4_DEVICE_SHA, "plugin_sha256": PLUGIN_SHA,
        "host_so_sha256": MEMFABRIC_HOST_SHA, "kver11_sha256": KVER_SHA,
        "session_ids": [z["session"] for z in specs],
    }.items():
        if meta.get(key) != value:
            raise ValueError(f"manifest mismatch: {key}")
    rows = []
    for directory, spec in zip(sessions, specs):
        sid = spec["session"]
        log = directory / "session.log"
        files = [directory / f"{sid}_rank{r}.json" for r in (0, 1)]
        if not log.is_file() or not log.stat().st_size or any(
                not x.is_file() for x in files):
            raise ValueError(f"{sid}: raw log or two rank JSONs missing")
        if sorted(p.name for p in directory.glob("*rank*.json")) != sorted(x.name for x in files):
            raise ValueError(f"{sid}: unrecognized rank evidence")
        pair = paired([json.loads(p.read_text()) for p in files], spec)
        rows.append({
            "session": sid, "arm": spec["arm"], "M_order": spec["M_order"],
            "log_sha256": digest(log),
            "rank_sha256": [digest(x) for x in files],
            "measurements": pair,
        })
    anchor_drift_fail = any(
        row["measurements"]["_anchor"]["abs_drift_pct"] > ANCHOR_PCT_MAX
        for row in rows
    )
    table = []
    for m in SHAPES[scope]:
        arms = {a: [s["measurements"][str(m)] for s in rows if s["arm"] == a]
                for a in ("stock_fp16", FUSED_ARM[scope])}
        xs = arms["stock_fp16"]
        ys = arms[FUSED_ARM[scope]]
        if not xs or not ys:
            raise ValueError("missing stock or fused session")
        s_med = statistics.median(x["event_p50_us"] for x in xs)
        f_med = statistics.median(y["event_p50_us"] for y in ys)
        stock_anchor_drift_pct = 100 * (max(x["event_p50_us"] for x in xs) /
                                        min(x["event_p50_us"] for x in xs) - 1)
        summary = {
            "M": m, "stock_p50_us": round(s_med, 3),
            "fused_p50_us": round(f_med, 3),
            "speedup_pct": round(100 * (s_med / f_med - 1), 3),
            "stock_session_drift_pct": round(stock_anchor_drift_pct, 3),
            "route": FUSED_ARM[scope],
            "stock_native_mm_only_p50_us": round(statistics.median(
                x["native_mm_only_p50_us"] for x in xs), 3) if phase == "screen" else None,
            "stock_sessions": len(xs), "fused_sessions": len(ys),
        }
        if phase == "confirm":
            windows = []
            for j in range(2):
                window = rows[4*j:4*j+4]
                stock = statistics.median(x["measurements"][str(m)]["event_p50_us"]
                                          for x in window if x["arm"] == "stock_fp16")
                fused = statistics.median(x["measurements"][str(m)]["event_p50_us"]
                                          for x in window if x["arm"] != "stock_fp16")
                windows.append(round(100 * (stock / fused - 1), 3))
            sa = [t for x in xs for t in x["paired_event_us"]]
            fa = [t for x in ys for t in x["paired_event_us"]]
            summary.update({
                "abba_speedup_pct": windows,
                "stock_p90_us": round(pctl(sa, 90), 3),
                "fused_p90_us": round(pctl(fa, 90), 3),
                "stock_p99_us": round(pctl(sa, 99), 3),
                "fused_p99_us": round(pctl(fa, 99), 3),
            })
            winner = (all(v >= MIN_SPEEDUP_PCT for v in windows)
                      and summary["fused_p90_us"] <= summary["stock_p90_us"] * MAX_TAIL_DEGRADE
                      and summary["fused_p99_us"] <= summary["stock_p99_us"] * 1.10
                      and stock_anchor_drift_pct <= ANCHOR_PCT_MAX)
            summary["decision"] = ("INCONCLUSIVE_DRIFT" if anchor_drift_fail
                                   else "CANDIDATE_REQUIRES_ROUTE_AUDIT" if winner
                                   else "INCONCLUSIVE_STOCK_VARIATION"
                                   if stock_anchor_drift_pct > ANCHOR_PCT_MAX else "NO_GO")
        else:
            summary["decision"] = "SCREEN_ONLY_NO_GO_POSSIBLE"
        table.append(summary)

    audit_sha = None
    if audit is not None:
        if phase != "confirm":
            raise ValueError("route audit does not authorize screening")
        info = json.loads(audit.read_text())
        fields = ("stock_hccl_fp16_verified", "memfabric_route_verified",
                  "hccl_stream_end_to_end_verified", "no_backend_coexistence")
        if any(info.get(k) is not True for k in fields):
            raise ValueError("audit cannot assert HCCL/stream semantics")
        raw_files = info.get("evidence_files")
        if not isinstance(raw_files, list) or len(raw_files) < 2 or any(
                not isinstance(p, str) or not (root / p).is_file()
                for p in raw_files):
            raise ValueError("missing independent trace evidence")
        audit_sha = digest(audit)
        for line in table:
            if line["decision"] == "CANDIDATE_REQUIRES_ROUTE_AUDIT":
                line["decision"] = "OPERATOR_CANDIDATE_ONLY_MODEL_GATE_REMAINS"
    return {
        "experiment": "U1_FULLCHAIN", "scope": scope, "phase": phase,
        "manifest_sha256": digest(manifest), "audit_sha256": audit_sha,
        "sessions": rows, "performance_map": table,
        "within_worker_anchor_drift_exceeds_5pct": anchor_drift_fail,
        "important": [
            "Screening never grants GO; performance is Stock FP16-HCCL vs complete fused op.",
            "Two ranks paired per sample; different sessions compared only through crossover.",
            "NPU Event stream intervals are not pure device kernel cycles.",
            "Diagnostics measure non-equivalent parts and must NEVER be algebraically added/subtracted.",
            "No claim of Qwen E2E gain; model traffic M distribution and route hits remain separate.",
        ],
    }


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--root", type=Path, required=True)
    p.add_argument("--scope", choices=SHAPES, required=True)
    p.add_argument("--phase", choices=ITERS, required=True)
    p.add_argument("--audit", type=Path)
    p.add_argument("--out", type=Path, required=True)
    a = p.parse_args()
    if a.out.exists():
        p.error("refuse to overwrite any existing evidence")
    result = evaluate(a.root, a.scope, a.phase, a.audit)
    a.out.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n")
    print(f"U1 {a.scope}/{a.phase}: {len(result['performance_map'])} shapes; "
          f"no E2E GO inferred")


if __name__ == "__main__":
    main()
