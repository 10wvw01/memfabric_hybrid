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

"""TTFT (Time To First Token) latency report data structure."""

from dataclasses import dataclass, field
from typing import Dict, List, Optional

from .percentile_utils import percentile_report

MICROSECONDS_PER_SECOND = 1_000_000


@dataclass
class TtftReport:
    """Structured TTFT latency report containing percentile distribution and pass/fail."""

    test_name: str
    iterations: int
    p50_us: float
    p90_us: float
    p99_us: float
    avg_us: float
    throughput_ops_per_sec: float
    passed: bool
    extra: Dict[str, float] = field(default_factory=dict)

    @classmethod
    def from_latencies(
        cls,
        test_name: str,
        latencies_us: List[float],
        pass_condition: Optional[bool] = None,
    ) -> "TtftReport":
        """Build a TtftReport from a list of per-iteration latencies (microseconds).

        When *pass_condition* is None, the report is considered passed if there
        are valid latencies.
        """
        report = percentile_report(latencies_us)
        iterations = len(latencies_us)
        total_us = sum(latencies_us)
        avg_us = report["avg"]
        if total_us > 0:
            throughput_ops_per_sec = MICROSECONDS_PER_SECOND * iterations / total_us
        else:
            throughput_ops_per_sec = 0.0
        passed = pass_condition if pass_condition is not None else iterations > 0
        return cls(
            test_name=test_name,
            iterations=iterations,
            p50_us=report["p50"],
            p90_us=report["p90"],
            p99_us=report["p99"],
            avg_us=avg_us,
            throughput_ops_per_sec=throughput_ops_per_sec,
            passed=passed,
        )

    def to_dict(self) -> Dict:
        """Return a dictionary representation suitable for JSON serialisation."""
        result = {
            "test_name": self.test_name,
            "iterations": self.iterations,
            "p50_us": round(self.p50_us, 2),
            "p90_us": round(self.p90_us, 2),
            "p99_us": round(self.p99_us, 2),
            "avg_us": round(self.avg_us, 2),
            "throughput_ops_per_sec": round(self.throughput_ops_per_sec, 2),
            "pass": self.passed,
        }
        for key, value in self.extra.items():
            result[key] = round(value, 2) if isinstance(value, float) else value
        return result

    def print(self) -> str:
        """Print the report to stdout and return the formatted string."""
        status = "PASS" if self.passed else "FAIL"
        lines = [
            f"===== TTFT Report: {self.test_name} =====",
            f"  iterations:           {self.iterations}",
            f"  p50_us:               {self.p50_us:.2f}",
            f"  p90_us:               {self.p90_us:.2f}",
            f"  p99_us:               {self.p99_us:.2f}",
            f"  avg_us:               {self.avg_us:.2f}",
            f"  throughput_ops_per_sec: {self.throughput_ops_per_sec:.2f}",
            f"  status:               {status}",
        ]
        for key, value in self.extra.items():
            lines.append(f"  {key}: {value:.2f}" if isinstance(value, float) else f"  {key}: {value}")
        text = "\n".join(lines)
        print(text)
        return text
