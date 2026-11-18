#!/usr/bin/env python3
"""Chrome-trace analyzer shared by the op-level and E2E benchmarks.

torch_npu/msprof chrome traces mark device tasks with cat=None plus
task-ish args ('Task Type' for AI-core/memcpy/event tasks, 'rank_size' for
HCCL kernels); host ops have cat='cpu_op'/'dequeue'/..., host AscendCL API
calls are 'AscendCL@*' with cat=None, and msprof aggregates ('Computing',
'Communication', ...) have empty args. Device filtering follows those rules
and additionally excludes pure waits (EVENT_WAIT) and the SDMA orchestrator
container task (HybmSdmaOrchEpoch), whose duration is a watchdog-polling
loop rather than work.

Per-iteration segmentation uses anchor time windows: the first device task
of each iteration (e.g. the input transdata for stock, the pad copy for
fused) anchors iteration boundaries, and every device task is assigned to
the window containing its timestamp. This survives per-iteration kernel
name changes (HCCL names carry stream/channel ids).
"""
from __future__ import annotations

import argparse
import json
import math
import re
from bisect import bisect_right
from collections import defaultdict

# Names that are waits/containers, not device work.
EXCLUDE_NAMES = {"HybmSdmaOrchEpoch", "EVENT_WAIT"}

# Cluster rules, first match wins (tune with --taxonomy).
CLUSTER_RULES = [
    # 注意：融合核在 trace 里的设备任务名是 CamelCase ABI 名（SmemMmAr310p3 / SmemMmIar310p3 (prd_code) / SmemMmArReduce (研发期) /
    # MixV3Ar），不是 kernel 源文件名；此规则必须在 hccl_ar 之前，否则 "Reduce"
    # 子串会被 hccl_ar 误吞（2026-11-01 实测坑）。
    ("smem",    re.compile(r"mix[_ ]?v3|smem.?mm.?ar", re.I)),
    ("hccl_ar", re.compile(r"hccl|all_?reduce", re.I)),
    ("matmul",  re.compile(r"matmul|gemm|grouped.*mm|quant_matmul", re.I)),
    ("copy",    re.compile(r"copy|memcpy|transdata", re.I)),
    ("zero",    re.compile(r"zero|fill_|memset|zeroslike", re.I)),
    ("sdma",    re.compile(r"sdma", re.I)),
    ("aiv",     re.compile(r"vector|elementwise|act[io]n|norm|silu|gelu|add|mul|cast|clamp", re.I)),
]


def load_events(path: str) -> list[dict]:
    with open(path) as f:
        data = json.load(f)
    if isinstance(data, dict):
        return data.get("traceEvents", [])
    return data


def is_device_task(e: dict) -> bool:
    if e.get("cat") is not None or "dur" not in e:
        return False
    name = e.get("name") or ""
    if name in EXCLUDE_NAMES or name.startswith("AscendCL@"):
        return False
    args = e.get("args")
    if not isinstance(args, dict):
        return False
    return "Task Type" in args or "rank_size" in args


def device_events(events: list[dict]) -> list[dict]:
    return [e for e in events if is_device_task(e)]


def cluster_of(name: str) -> str:
    for tag, rx in CLUSTER_RULES:
        if rx.search(name):
            return tag
    return "other"


# ---- P50/P90/P95 ----------------------------------------------------------

def pct(vals: list[float], p: float) -> float:
    """Linear-interpolation percentile (p in [0,1])."""
    s = sorted(vals)
    if not s:
        return 0.0
    k = (len(s) - 1) * p
    f, c = math.floor(k), math.ceil(k)
    if f == c:
        return s[int(k)]
    return s[f] * (c - k) + s[c] * (k - f)


def dist_stats(vals: list[float]) -> dict:
    """P50/P90/P95 (+mean, min, max) of a duration list, in ms."""
    if not vals:
        return {"p50_ms": 0.0, "p90_ms": 0.0, "p95_ms": 0.0,
                "mean_ms": 0.0, "min_ms": 0.0, "max_ms": 0.0, "n": 0}
    return {
        "p50_ms": round(pct(vals, 0.50), 3),
        "p90_ms": round(pct(vals, 0.90), 3),
        "p95_ms": round(pct(vals, 0.95), 3),
        "mean_ms": round(sum(vals) / len(vals), 3),
        "min_ms": round(min(vals), 3),
        "max_ms": round(max(vals), 3),
        "n": len(vals),
    }


# ---- summary (whole-window totals; E2E profile uses this) ------------------

def summarize(path: str, iters: int = 1) -> dict:
    """Whole-window per-cluster {count, total_ms} (+ top kernel names)."""
    evs = device_events(load_events(path))
    per_cluster: dict[str, dict] = defaultdict(
        lambda: {"count": 0, "total_us": 0.0, "names": defaultdict(float)})
    for e in evs:
        c = cluster_of(e["name"])
        per_cluster[c]["count"] += 1
        per_cluster[c]["total_us"] += e["dur"]
        per_cluster[c]["names"][e["name"]] += e["dur"]
    out = {}
    for c, v in sorted(per_cluster.items(), key=lambda kv: -kv[1]["total_us"]):
        out[c] = {
            "count": v["count"],
            "count_per_iter": v["count"] / max(iters, 1),
            "total_ms": round(v["total_us"] / 1e3, 3),
            "per_iter_ms": round(v["total_us"] / 1e3 / max(iters, 1), 3),
            "top_names": sorted(
                ((round(us / 1e3, 3), n) for n, us in v["names"].items()),
                reverse=True)[:8],
        }
    return out


# ---- per-iteration distribution via anchor windows -------------------------

def per_iter_stats(path: str, iters: int, anchor_regex: str,
                   drop_first: int = 0, anchor_stride: int = 1) -> dict:
    """Per-op device-time distribution (P50/P90/P95, in ms).

    Anchor events (regex) define iteration boundaries; each device task is
    assigned to the window containing its ts. The last window extends by the
    median anchor gap.

    anchor_stride > 1: 每迭代出现 k 个锚事件时（快速路由下 stock 每迭代 2 个
    TransData——x→NZ 与 out→ND；融合臂每迭代 2 个 SmemMm*310p3 核——AIC+AIV），
    取每第 k 个锚为迭代边界，窗口仍对齐完整迭代。

    drop_first skips the first N windows — after profiler start the two
    ranks are skewed, and kernel durations of the first wave(s) include
    in-kernel spins waiting for the lagging peer (observed: first-wave
    fused-kernel/hcom durations inflated 100-1000x). Those windows are not
    steady state and are dropped.
    """
    evs = device_events(load_events(path))
    rx = re.compile(anchor_regex)
    anchors = sorted(float(e["ts"]) for e in evs if rx.search(e["name"]))
    if not anchors:
        return {"error": f"no anchor events matched {anchor_regex!r}"}
    if anchor_stride > 1:
        anchors = anchors[::anchor_stride]
    if drop_first:
        anchors = anchors[drop_first:]
    gaps = [b - a for a, b in zip(anchors, anchors[1:])]
    med_gap = sorted(gaps)[len(gaps) // 2] if gaps else 0.0
    bounds = anchors + [anchors[-1] + med_gap]

    n_win = len(anchors)
    iter_tot = [0.0] * n_win
    iter_span = [0.0] * n_win
    iter_cluster: dict[str, list[float]] = defaultdict(
        lambda: [0.0] * n_win)
    unassigned = 0
    for e in evs:
        ts = float(e["ts"])
        i = bisect_right(bounds, ts) - 1
        if i < 0 or i >= n_win or ts < anchors[0]:
            unassigned += 1
            continue
        iter_tot[i] += e["dur"] / 1e3          # us -> ms
        # span：窗口内任务时间轴覆盖长度（自 anchor 起的最大任务末端）。并发
        # MIX 核（AIC MM 与 AIV 调度/ADD 同时跑）用求和会重复计费交叠部分，
        # span 才是每迭代真实设备侧时延；串行路径（stock）span==sum。
        end_ms = (ts + e["dur"]) / 1e3
        iter_span[i] = max(iter_span[i], end_ms - anchors[i] / 1e3)
        iter_cluster[cluster_of(e["name"])][i] += e["dur"] / 1e3

    return {
        "anchor_regex": anchor_regex,
        "windows": n_win,
        "dropped_first": drop_first,
        "unassigned_events": unassigned,
        "op_dist": dist_stats(iter_tot),
        "op_vals_ms": [round(v, 3) for v in iter_tot],
        "span_dist": dist_stats(iter_span),
        "span_vals_ms": [round(v, 3) for v in iter_span],
        "cluster_dist": {c: dist_stats(v) for c, v in iter_cluster.items()},
        "cluster_vals_ms": {c: [round(v, 3) for v in vls]
                            for c, vls in iter_cluster.items()},
    }


def taxonomy(path: str) -> None:
    """Dump the full (cat, name) histogram — for tuning the rules above."""
    hist: dict[tuple, list] = defaultdict(lambda: [0, 0.0])
    for e in load_events(path):
        if "dur" not in e:
            continue
        k = (e.get("cat") if e.get("cat") is not None else "?",
             (e.get("name") or "?")[:120])
        hist[k][0] += 1
        hist[k][1] += e["dur"]
    for (cat, name), (n, us) in sorted(hist.items(), key=lambda kv: -kv[1][1]):
        print(f"{cat:12s} n={n:6d} total={us/1e3:12.3f}ms {name}")


def watchdog_events(path: str, threshold_ms: float = 300.0) -> list[dict]:
    """Device tasks at/above threshold_ms — the ~500ms watchdog signature."""
    return [{"name": e["name"][:120], "dur_ms": e["dur"] / 1e3,
             "ts": e.get("ts", 0)}
            for e in device_events(load_events(path))
            if e["dur"] >= threshold_ms * 1e3]


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("trace")
    p.add_argument("--iters", type=int, default=1)
    p.add_argument("--anchor", default=None,
                   help="anchor regex for --dist segmentation")
    p.add_argument("--taxonomy", action="store_true")
    p.add_argument("--watchdog", action="store_true")
    p.add_argument("--dist", action="store_true",
                   help="per-iteration P50/P90/P95 distribution")
    a = p.parse_args()
    if a.taxonomy:
        taxonomy(a.trace)
    elif a.watchdog:
        for w in watchdog_events(a.trace):
            print(w)
    elif a.dist:
        if not a.anchor:
            p.error("--dist requires --anchor")
        print(json.dumps(per_iter_stats(a.trace, a.iters, a.anchor), indent=2))
    else:
        print(json.dumps(summarize(a.trace, a.iters), indent=2))
