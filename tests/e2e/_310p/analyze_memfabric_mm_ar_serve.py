#!/usr/bin/env python3
# SPDX-License-Identifier-Identifier: Apache-2.0
# Copyright (c) 2026 Huawei Technologies Co., Ltd. All Rights Reserved.
"""Summarize mm_ar serve-bench result JSONs and quantify fused-op impact scope.

Usage:
    python3 tests/e2e/_310p/analyze_memfabric_mm_ar_serve.py <result_dir> [result_dir ...]
"""

from __future__ import annotations

import glob
import json
import os
import sys

FIELDS = (
    ("duration", "dur_s"),
    ("completed", "done"),
    ("output_throughput", "out_tokps"),
    ("request_output_throughput", "req_tokps"),
    ("mean_ttft_ms", "ttft_mean"),
    ("median_ttft_ms", "ttft_med"),
    ("p99_ttft_ms", "ttft_p99"),
    ("mean_itl_ms", "itl_mean"),
    ("median_itl_ms", "itl_med"),
    ("p99_itl_ms", "itl_p99"),
    ("mean_e2el_ms", "e2e_mean"),
    ("median_e2el_ms", "e2e_med"),
)


def load_all(root: str) -> dict[str, dict]:
    out: dict[str, dict] = {}
    for path in sorted(glob.glob(os.path.join(root, "*.json"))):
        name = os.path.basename(path).replace(".json", "")
        with open(path) as f:
            out[name] = json.load(f)
    return out


def fmt(v) -> str:
    if isinstance(v, float):
        return f"{v:.2f}"
    return str(v)


def main() -> None:
    rows: dict[str, dict] = {}
    for root in sys.argv[1:]:
        rows.update(load_all(root))

    header = ["combo"] + [label for _, label in FIELDS]
    print("\t".join(header))
    for name, d in sorted(rows.items()):
        cells = [name]
        for key, label in FIELDS:
            cells.append(fmt(d[key]) if key in d else "-")
        print("\t".join(cells))

    # Pairwise fused-vs-stock comparison per graph mode.
    print("\n== fused vs stock (same graph mode) ==")
    for graph_mode in ("eager", "graph"):
        stock_key = f"stock_{graph_mode}_rep1"
        if stock_key not in rows:
            continue
        stock = rows[stock_key]
        for name, d in sorted(rows.items()):
            if not name.startswith(f"fused_{graph_mode}"):
                continue
            base = stock.get("output_throughput") or 0
            fused = d.get("output_throughput") or 0
            if base > 0:
                delta = (fused - base) / base * 100
                print(
                    f"{name}: out_tokps {fused:.2f} vs {base:.2f} "
                    f"({delta:+.1f}%), ttft_med {fmt(d.get('median_ttft_ms'))} vs "
                    f"{fmt(stock.get('median_ttft_ms'))} ms, "
                    f"itl_med {fmt(d.get('median_itl_ms'))} vs "
                    f"{fmt(stock.get('median_itl_ms'))} ms"
                )


if __name__ == "__main__":
    main()
