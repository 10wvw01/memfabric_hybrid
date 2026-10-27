# SPDX-License-Identifier: Apache-2.0
"""G3 V2 与 Stock 的严格性能验收：不能仅凭单算子微基准宣称通过。

评估吞吐、TTFT 和 TPOT 之前，必须采集至少三次独立
Stock/V2 Graph Serving 会话，每次放在独立结果目录。
"""
from __future__ import annotations

import argparse
import json
import statistics
from pathlib import Path


METRICS = (
    ("output_throughput", "Throughput (tok/s)", True),
    ("median_ttft_ms", "TTFT median (ms)", False),
    ("median_tpot_ms", "TPOT median (ms)", False),
)


def read(path: Path) -> dict:
    with path.open() as f:
        return json.load(f)


def improvement(stock: float, v2: float, higher: bool) -> float:
    if stock <= 0:
        raise ValueError(f"nonpositive Stock baseline {stock}")
    return (v2 - stock) / stock * 100 if higher else (stock - v2) / stock * 100


def main():
    p = argparse.ArgumentParser()
    p.add_argument("result_dirs", nargs="+", type=Path)
    p.add_argument("--mode", choices=("graph", "eager"), default="graph")
    p.add_argument("--min-pairs", type=int, default=3)
    p.add_argument("--min-sessions", type=int, default=3)
    p.add_argument("--min-gain-pct", type=float, default=2.0)
    p.add_argument("--expected-completed", type=int, default=50)
    p.add_argument("--max-tail-regression-pct", type=float, default=5.0)
    p.add_argument("--allow-missing-v2-log", action="store_true",
                   help="Diagnostic only; do not call a result production-verified")
    a = p.parse_args()
    pairs = []
    for root in a.result_dirs:
        for stock_file in sorted(root.glob(f"stock_{a.mode}_rep*_run.json")):
            stem = stock_file.name.replace(f"stock_{a.mode}_", "")
            v2_file = root / f"fused_{a.mode}_{stem}"
            if not v2_file.is_file():
                raise SystemExit(f"FAIL missing paired V2 result {v2_file}")
            stock, fused = read(stock_file), read(v2_file)
            if (stock.get("completed") != a.expected_completed or
                    fused.get("completed") != a.expected_completed):
                raise SystemExit(
                    f"FAIL incomplete requests for {root}: "
                    f"Stock={stock.get('completed')} V2={fused.get('completed')}"
                )
            if not a.allow_missing_v2_log:
                log_file = root / f"fused_{a.mode}.log"
                if not log_file.is_file():
                    raise SystemExit(f"FAIL V2 serve log absent: {log_file}")
                log = log_file.read_text(errors="replace")
                if "[mf310p-v2] M=" not in log or "V2 MM+MemFabric" not in log:
                    raise SystemExit(
                        f"FAIL no evidence M>=2048 actually used V2 in {log_file}"
                    )
            pairs.append((stock_file, stock, fused))
    if len(pairs) < a.min_pairs:
        raise SystemExit(f"FAIL need >= {a.min_pairs} Stock/V2 paired runs, got {len(pairs)}")
    unique_sessions = {str(path.parent.resolve()) for path, _, _ in pairs}
    if len(unique_sessions) < a.min_sessions:
        raise SystemExit(
            f"FAIL need >= {a.min_sessions} independent serve directories, "
            f"got {len(unique_sessions)}; repeating on one server is not independent."
        )

    ok = True
    print(f"G3 {a.mode} evaluation: {len(pairs)} paired Stock vs V2 runs")
    for field, label, higher in METRICS:
        improvements = []
        for path, stock, fused in pairs:
            if field not in stock or field not in fused:
                raise SystemExit(f"FAIL missing {field} in {path} or V2 counterpart")
            gain = improvement(float(stock[field]), float(fused[field]), higher)
            improvements.append(gain)
            print(f"{label}: {path.parent.name}/{path.name} {gain:+.2f}%")
        median = statistics.median(improvements)
        positive = sum(gain > 0 for gain in improvements)
        passed = median >= a.min_gain_pct and positive >= (2 * len(pairs) + 2) // 3
        print(f"{label}: median improvement={median:+.2f}%, "
              f"positive={positive}/{len(pairs)}, "
              f"gate={'PASS' if passed else 'FAIL'}")
        ok &= passed

    for tail in ("p99_ttft_ms", "p99_tpot_ms"):
        if not all(tail in s and tail in f for _, s, f in pairs):
            print(f"NOTE {tail} unavailable in some runs; preserve raw JSON")
            continue
        regressions = [
            -improvement(float(s[tail]), float(f[tail]), higher=False)
            for _, s, f in pairs
        ]
        worst = max(regressions)
        passed = worst <= a.max_tail_regression_pct
        print(f"{tail}: worst tail regression={worst:+.2f}% "
              f"gate={'PASS' if passed else 'FAIL'}")
        ok &= passed

    if not ok:
        raise SystemExit("FAIL V2 has no stable end-to-end benefit over Stock")
    print("PASS stable G3 Stock-beating throughput, TTFT and TPOT. "
          "Safety/long-soak gates must still pass independently.")


if __name__ == "__main__":
    main()
