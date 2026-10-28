#!/usr/bin/env python
# coding=utf-8
# Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
# MemCache_Hybrid is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#          http://license.coscl.org.cn/MulanPSL2
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
# See the Mulan PSL v2 for more details.

"""Percentile utility functions for TTFT latency analysis."""

from typing import Dict, List

PERCENTILE_P50 = 0.50
PERCENTILE_P90 = 0.90
PERCENTILE_P99 = 0.99
EMPTY_LIST_DEFAULT = 0


def percentile(values: List[float], pct: float) -> float:
    """Return the pct-th percentile of *values* using nearest-rank method.

    pct is a fraction in [0, 1].  An empty list returns 0.
    """
    if not values:
        return float(EMPTY_LIST_DEFAULT)
    sorted_values = sorted(values)
    n = len(sorted_values)
    rank = pct * (n - 1)
    lower = int(rank)
    upper = min(lower + 1, n - 1)
    fraction = rank - lower
    return sorted_values[lower] * (1 - fraction) + sorted_values[upper] * fraction


def percentile_report(values: List[float]) -> Dict[str, float]:
    """Return a dict with p50/p90/p99/avg from *values*.

    An empty list returns all zeros.
    """
    if not values:
        return {
            "p50": float(EMPTY_LIST_DEFAULT),
            "p90": float(EMPTY_LIST_DEFAULT),
            "p99": float(EMPTY_LIST_DEFAULT),
            "avg": float(EMPTY_LIST_DEFAULT),
        }
    avg = sum(values) / len(values)
    return {
        "p50": percentile(values, PERCENTILE_P50),
        "p90": percentile(values, PERCENTILE_P90),
        "p99": percentile(values, PERCENTILE_P99),
        "avg": avg,
    }
