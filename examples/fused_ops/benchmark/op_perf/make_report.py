#!/usr/bin/env python3
"""Comparison report for the op-level matrix: stock vs ar vs iar.

Primary metric: device-side span P50 per cell (msprof windows), slow-side
rank (max of the two ranks' P50 — the lockstep convention of the project:
the slow rank paces the pair). Reads summary_rank{0,1}.json produced by
op_bench.py and prints a Markdown table plus route-evidence checks.
"""
import argparse
import glob
import json
import os
import re
import statistics


def cell_stats(cell_dir: str):
    """-> {rank: span_p50_ms (median over reps)} or None if incomplete."""
    out = {}
    for r in (0, 1):
        p = os.path.join(cell_dir, f"summary_rank{r}.json")
        if not os.path.exists(p):
            return None
        s = json.load(open(p))
        p50s = [rep["span_dist"]["p50_ms"] for rep in s["reps"]
                if rep.get("span_dist", {}).get("p50_ms") is not None]
        if not p50s:
            return None
        out[r] = statistics.median(p50s)
        out[("route", r)] = s.get("route")
    return out


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--root", required=True,
                   help="matrix output root containing {path}_m{M}/ dirs")
    p.add_argument("--anchors", default="",
                   help="optional JSON file {m: {arm: slow_p50_ms}} of historical "
                        "anchor numbers; each cell prints the drift vs anchor")
    a = p.parse_args()

    anchors = {}
    if a.anchors and os.path.exists(a.anchors):
        anchors = json.load(open(a.anchors))

    cells = {}
    for d in sorted(glob.glob(os.path.join(a.root, "*_m*"))):
        m = re.match(r".*/(stock|ar|iar)_m(\d+)$", d)
        if not m:
            continue
        st = cell_stats(d)
        if st:
            cells[(m.group(1), int(m.group(2)))] = st

    arms = [x for x in ("stock", "ar", "iar") if any(k[0] == x for k in cells)]
    ms = sorted({k[1] for k in cells})
    if not cells:
        print("no complete cells found under", a.root)
        return 1

    print(f"# op-level matrix report ({a.root})\n")
    print("主判据：设备侧 span P50（慢侧 rank = 双 rank P50 的较大者），单位 ms\n")

    header = "| M | " + " | ".join(
        f"{arm} 慢侧P50 | Δ vs stock" for arm in arms) + " |"
    print(header)
    print("|---|" + "---|" * (2 * len(arms)))
    for m in ms:
        row = [f"| {m} "]
        base = cells.get(("stock", m), {}).get(0)
        slow_stock = max(cells[("stock", m)][r] for r in (0, 1)) \
            if ("stock", m) in cells else None
        for arm in arms:
            st = cells.get((arm, m))
            if st is None:
                row.append(" | — | — ")
                continue
            slow = max(st[0], st[1])
            delta = (slow / slow_stock - 1) * 100 if slow_stock else float("nan")
            anchor_txt = ""
            if anchors.get(str(m), {}).get(arm):
                drift = (slow / anchors[str(m)][arm] - 1) * 100
                anchor_txt = f"（锚点 {anchors[str(m)][arm]:.3f}，漂移 {drift:+.1f}%）"
            row.append(f"| {slow:.3f} {anchor_txt} | {delta:+.1f}% ")
        print("".join(row) + "|")

    print("\n## 路由证据（每格 rank0 rep1）\n")
    for (arm, m), st in sorted(cells.items()):
        rt = st.get(("route", 0)) or {}
        print(f"- {arm}_m{m}: mm={rt.get('mm')} n={rt.get('mm_n')} "
              f"td={rt.get('td_n')} hccl={rt.get('hccl_n')} "
              f"smem={rt.get('smem_n')} anchor={rt.get('anchor')!r} "
              f"stride={rt.get('stride')}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
