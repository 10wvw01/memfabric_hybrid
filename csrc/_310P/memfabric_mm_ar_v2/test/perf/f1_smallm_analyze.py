#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""F1小M：离线核验T1 MM+MemFabric+AR与Stock（F.linear+HCCL）的初筛表。

t1_nz_direct_trial.py 的Stock在建池前测、V2在建池后测，且用host sync计时，
因此只可筛候选，不可宣称同场严格无偏的胜负；另做Event配对ABBA。
"""
from __future__ import annotations
import argparse
import json
import math
import re
from pathlib import Path

REQUIRED_M = (1, 8, 10, 16, 32, 64)
MM = (4, 8)
ADD = (1, 4, 8)
HEADER = "M\tMM\tADD\tstock_ms\tv2_med_ms\tv2_first_ms\tv2_p95_ms\tstatus\tbitexact"


def parse(log: str) -> dict:
    if HEADER not in log:
        raise ValueError("not a T1 full-mode table")
    if "G0\tib_probe\tPASS" not in log:
        raise ValueError("IB precondition missing")
    if "MATRIX ALL PASS" not in log:
        raise ValueError("missing final no-failure marker")
    records = {}
    noise_prefix_rows = 0
    started = False
    for line in log.splitlines():
        if line.strip() == HEADER:
            started = True
            continue
        if not started:
            continue
        row = line.strip()
        # 实机噪声容错：并发日志写入器可能向 stdout 写入无换行前缀（2026-10-24
        # 实测两例 ".." 被拼接到下一行行首，其一命中矩阵行）。仅剥离首个数字段
        # 之前的非数字前缀；剥离后仍须通过完整 9 字段、组合合法性、status=0、
        # 位精确 PASS 的全部校验，且异常行计数留痕，不掩盖任何真实失败。
        cleaned = re.sub(r"^[^\d]+(?=\d)", "", row, count=1)
        if cleaned != row:
            noise_prefix_rows += 1
        fields = cleaned.split("\t")
        if not fields or not re.fullmatch(r"\d+", fields[0]):
            continue
        if len(fields) != 9:
            raise ValueError(f"malformed T1 row: {line[:100]}")
        m, mm, add = (int(x) for x in fields[:3])
        if (m, mm, add) not in {
            (x, y, z) for x in REQUIRED_M for y in MM for z in ADD
        }:
            raise ValueError(f"unknown M/MM/Add {m}/{mm}/{add}")
        floats = [float(x) for x in fields[3:7]]
        if any(not math.isfinite(x) or x <= 0 for x in floats):
            raise ValueError("nonpositive / nonfinite times")
        if fields[7:] != ["0", "PASS"]:
            # T1正式驱动的status字段必须是数值0，位精确必须PASS。
            raise ValueError(f"incorrect/protocol failed {m}/{mm}/{add}: {fields[7:]}")
        key = (m, mm, add)
        if key in records:
            raise ValueError(f"duplicate trial row {key}")
        records[key] = {
            "m": m, "mm_cores": mm, "add_cores": add,
            "stock_ms": floats[0], "v2_ms": floats[1],
            "first_ms": floats[2], "v2_p95_ms": floats[3],
            "delta_vs_stock_pct": round(100 * (floats[0] / floats[1] - 1), 3),
        }
    expected = len(REQUIRED_M) * len(MM) * len(ADD)
    if len(records) != expected:
        raise ValueError(f"incomplete 6x2x3 matrix: {len(records)}/{expected}")
    rows = []
    for m in REQUIRED_M:
        choices = [r for k, r in records.items() if k[0] == m]
        stock_values = {r["stock_ms"] for r in choices}
        if len(stock_values) != 1:
            raise ValueError(f"Stock time drift within same M={m} run")
        winner = min(choices, key=lambda r: r["v2_ms"])
        rows.append({
            "M": m, "stock_ms": winner["stock_ms"],
            "best_v2_ms": winner["v2_ms"], "best_mm_cores": winner["mm_cores"],
            "best_add_cores": winner["add_cores"],
            "best_delta_pct": winner["delta_vs_stock_pct"],
            "tested_variants": len(choices),
        })
    return {
        "matrix": rows,
        "all_measurements": [records[k] for k in sorted(records)],
        "noise_prefix_rows": noise_prefix_rows,
        "verdict": "SCREEN_ONLY_HOST_SYNC_PRE_POOL_VS_POST_POOL_NOT_FINAL_GO",
        "safety": "No V1 serving, no KVER11 protocol/IB modification",
        "next": "Verify M10 topology first; use independent per-stage diagnostics and proper "
                "same-hardware paired event ABBA before any F1 protocol source change",
    }


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--log", type=Path, required=True)
    p.add_argument("--out", type=Path, required=True)
    a = p.parse_args()
    if a.out.exists():
        p.error("refuse to overwrite experiment evidence")
    report = parse(a.log.read_text(errors="replace"))
    a.out.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
    print("F1 36-way CPU audit PASS; screening only, no winner yet")


if __name__ == "__main__":
    main()
