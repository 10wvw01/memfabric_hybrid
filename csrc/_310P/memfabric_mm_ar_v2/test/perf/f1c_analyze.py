#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""F1-C 独立进程 Stock-FP16-HCCL/T1-MemFabric 双Rank配对 Event 交叉ABBA评审。
只处理完整8场；既不删失败/慢样本，也不拿历史host计时推导新收益。
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import re
import statistics
from pathlib import Path

ORDER = ("stock_fp16", "memfabric_t1", "memfabric_t1", "stock_fp16") * 2
D4_DEVICE = "2a19390c83c3fe6b0bdb8946d88990cca66b331f765ca445dbda2e787c2d2a19"
GATE_C_PLUGIN = "8eb65de98c3d8664ff10b421c1d7ad31318dc720c3f5679cd4d34e187203bea1"
HOST_PATCH = "1dc46d7dcbe583a5dbb78b4c65a0b5e22f9f352976c53ed31b761fa744f91a30"
KVER11 = "25e16b0af2f16989231ff0443698261257bd8142834783a0c1e4ad57cd9ebc78"
MS = (1, 8, 10, 16, 32)
N_ITERS = 50


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def percentile(x: list[float], pct: float) -> float:
    t = sorted(x)
    return t[max(0, math.ceil(len(t) * pct / 100) - 1)]


def validate_pair(rank0: dict, rank1: dict, m: int, arm: str) -> dict:
    for rank, d in enumerate((rank0, rank1)):
        pairs = {
            "experiment": "F1C_SEPARATE_PROCESS_CROSS_ABBA",
            "rank": rank, "arm": arm, "M": m, "K": 2048,
            "N": 2048, "TP": 2, "dtype": "fp16",
            "stock_collective_dtype": "fp16",
            "reference": "CPU_GLOO_FP32_SUM_TO_FP16",
            "communication_isolation": "HCCL group only Stock; MemFabric pool only T1",
            "host_f32_conversion_in_timed_stock": False,
            "iters": N_ITERS, "bit_exact": True,
            "device_so_sha256": D4_DEVICE, "plugin_sha256": GATE_C_PLUGIN,
            "host_so_sha256": HOST_PATCH, "kver11_sha256": KVER11,
        }
        for k, expect in pairs.items():
            if d.get(k) != expect:
                raise ValueError(f"rank{rank} {k} mismatch: {d.get(k)!r}, expected {expect!r}")
        if d.get("warmup") != 5:
            raise ValueError("F1-C requires exactly 5 warmup iterations")
        if arm == "memfabric_t1":
            if d.get("mm_cores") != 8 or d.get("add_cores") != 4 or d.get("protocol_status") != 0:
                raise ValueError("fused MM8/Add4/status0 required")
        else:
            if d.get("mm_cores") is not None or d.get("add_cores") is not None or d.get("protocol_status") is not None:
                raise ValueError("stock unexpectedly included a fused-operator claim")
        if len(d.get("samples", [])) != N_ITERS:
            raise ValueError("missing raw 50 NPU samples")
    if rank0.get("session") != rank1.get("session"):
        raise ValueError("unmatched session name")
    events, walls, gaps = [], [], []
    for i, (a, b) in enumerate(zip(rank0["samples"], rank1["samples"])):
        for z in (a, b):
            if z.get("i") != i or z.get("input_variant") != (i + rank0["warmup"]) % 2:
                raise ValueError("unaligned sample index/input rotation")
            for field in ("event_us", "wall_us"):
                val = z.get(field)
                if not isinstance(val, (float, int)) or isinstance(val, bool) or not math.isfinite(val) or val <= 0:
                    raise ValueError(f"invalid {field} at {i}")
        events.append(max(a["event_us"], b["event_us"]))
        walls.append(max(a["wall_us"], b["wall_us"]))
        gaps.append(max(a["wall_us"] - a["event_us"], b["wall_us"] - b["event_us"]))
    return {
        "sample_count": N_ITERS, "pair_event_p50_us": round(statistics.median(events), 3),
        "pair_event_p90_us": round(percentile(events, 90), 3),
        "pair_event_p99_us": round(percentile(events, 99), 3),
        "pair_wall_p50_us": round(statistics.median(walls), 3),
        "host_minus_event_p50_us": round(statistics.median(gaps), 3),
        "samples_ge_2ms": sum(x >= 2000 for x in events),
        "pair_event_us": events, "pair_wall_us": walls,
    }


def analyze(directory: Path, m: int, audit: Path | None = None) -> dict:
    if m not in MS:
        raise ValueError(f"unapproved M={m}")
    sessions = sorted(d for d in directory.iterdir() if d.is_dir())
    if len(sessions) != 8:
        raise ValueError(f"need 8 sessions for one M, got {len(sessions)}")
    rows = []
    for index, (dirpath, arm) in enumerate(zip(sessions, ORDER)):
        expected_dir = f"{index + 1:02d}_m{m}_{arm}"
        if dirpath.name != expected_dir:
            raise ValueError(f"wrong sequence: {dirpath.name} != {expected_dir}")
        js = sorted(dirpath.glob("*_rank*.json"))
        if len(js) != 2 or not (dirpath / "session.log").is_file() or not (dirpath / "session.log").stat().st_size:
            raise ValueError(f"{dirpath}: require 2 JSONs and raw session.log")
        content = [json.loads(x.read_text()) for x in js]
        ranks = {item.get("rank"): item for item in content}
        if set(ranks) != {0, 1}:
            raise ValueError("duplicate or missing rank")
        if ranks[0]["session"] != expected_dir or ranks[1]["session"] != expected_dir:
            raise ValueError("JSON session mismatch")
        pair = validate_pair(ranks[0], ranks[1], m, arm)
        expected_files = {dirpath / f"{expected_dir}_rank{r}.json" for r in (0, 1)}
        if set(js) != expected_files:
            raise ValueError("unexpected or mislabeled rank JSON files")
        pair.update({"session": expected_dir, "arm": arm,
                     "rank_json_sha256": {str(r): sha(dirpath / f"{expected_dir}_rank{r}.json")
                                          for r in (0, 1)},
                     "log_sha256": sha(dirpath / "session.log")})
        rows.append(pair)

    manifest = directory.parent / "manifest.txt"
    if not manifest.is_file() or not manifest.stat().st_size:
        raise ValueError("missing session environment/identity manifest")
    lines = dict(row.split("=", 1) for row in manifest.read_text().splitlines() if "=" in row)
    if (lines.get("experiment") != "F1C_SEPARATE_PROCESS_CROSS_ABBA"
            or lines.get("M") != str(m) or lines.get("TP") != "2"
            or lines.get("order") != "A_B_B_A__A_B_B_A"
            or lines.get("device_so_sha256") != D4_DEVICE
            or lines.get("plugin_sha256") != GATE_C_PLUGIN
            or lines.get("memfabric_host_sha256") != HOST_PATCH
            or lines.get("kver11_sha256") != KVER11):
        raise ValueError("manifest topology/stock/fused binary identity mismatch")
    windows = []
    for n in range(2):
        batch = rows[n * 4:n * 4 + 4]
        stock = statistics.median(z["pair_event_p50_us"] for z in batch if z["arm"] == "stock_fp16")
        fused = statistics.median(z["pair_event_p50_us"] for z in batch if z["arm"] == "memfabric_t1")
        windows.append({"repeat": n + 1, "stock_p50_us": round(stock, 3),
                        "fused_p50_us": round(fused, 3),
                        "speedup_pct": round(100 * (stock / fused - 1), 3)})
    med_s = statistics.median(x["pair_event_p50_us"] for x in rows if x["arm"] == "stock_fp16")
    med_f = statistics.median(x["pair_event_p50_us"] for x in rows if x["arm"] == "memfabric_t1")
    all_s = [x for z in rows if z["arm"] == "stock_fp16" for x in z["pair_event_us"]]
    all_f = [x for z in rows if z["arm"] == "memfabric_t1" for x in z["pair_event_us"]]
    conclusion = "PROVISIONAL_ONLY_ROUTE_AND_HCCL_EVENT_AUDIT_REQUIRED"
    if audit:
        info = json.loads(audit.read_text())
        requirements = ("stock_fp16_hccl_verified", "memfabric_t1_only_verified",
                        "no_simultaneous_hccl_memfabric", "hccl_event_stream_bracketing_verified")
        if any(info.get(q) is not True for q in requirements):
            raise ValueError("independent route/timing audit is incomplete or negative")
        paths = info.get("evidence_files")
        if not isinstance(paths, list) or len(paths) < 2 or any(
                not isinstance(x, str) or not (directory.parent / x).is_file()
                for x in paths):
            raise ValueError("actual evidence_files must be present under trial parent")
        if (all(x["speedup_pct"] >= 5 for x in windows)
                and percentile(all_f, 90) <= percentile(all_s, 90) * 1.05
                and percentile(all_f, 99) <= percentile(all_s, 99) * 1.10
                and not any(row["samples_ge_2ms"] for row in rows)):
            conclusion = "F1C_OPERATOR_GO_FURTHER_CORRECTNESS_AND_QWEN_REQUIRED"
        else:
            conclusion = "F1C_NO_GO_OR_MORE_EVIDENCE"
    return {
        "experiment": "F1C", "m": m, "sessions": rows, "abba_windows": windows,
        "primary_stock_fp16_event_median_of_sessions_us": round(med_s, 3),
        "primary_t1_event_median_of_sessions_us": round(med_f, 3),
        "event_speedup_pct": round(100 * (med_s / med_f - 1), 3),
        "all_stock_event_p90_us": round(percentile(all_s, 90), 3),
        "all_fused_event_p90_us": round(percentile(all_f, 90), 3),
        "all_stock_event_p99_us": round(percentile(all_s, 99), 3),
        "all_fused_event_p99_us": round(percentile(all_f, 99), 3),
        "manifest_sha256": sha(manifest),
        "audit_sha256": sha(audit) if audit else None,
        "verdict": conclusion,
        "caveat": "per-sample paired max within each session; separate-process time-window ABBA across arms, not exact simultaneous matching; stream events require independent HCCL audit",
    }


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--sessions", type=Path, required=True)
    p.add_argument("--m", type=int, required=True)
    p.add_argument("--audit", type=Path)
    p.add_argument("--out", type=Path, required=True)
    a = p.parse_args()
    if a.out.exists():
        p.error("refuse overwrite")
    try:
        result = analyze(a.sessions, a.m, a.audit)
    except (ValueError, KeyError) as ex:
        p.error(str(ex))
    a.out.parent.mkdir(parents=True, exist_ok=True)
    a.out.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n")
    print(result["verdict"], "net_event_speedup", result["event_speedup_pct"])


if __name__ == "__main__":
    main()
