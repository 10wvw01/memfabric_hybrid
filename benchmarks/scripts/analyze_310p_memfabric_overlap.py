# SPDX-License-Identifier: Apache-2.0
"""Analyze ABI v7 MM/SDMA/reduce overlap from CANN task_time.csv.

ABI v7 launches one 8-core cooperative producer per communication batch.
Lookahead=1 enqueues P(n+1) before W/A(n), then drains the wave with one quiet
and one ack. A short waiter relative to the batch transfer lower bound is
evidence that SDMA progressed while later compute was running; final
acceptance must still inspect the CANN timeline.

Usage:
  python3 benchmarks/scripts/analyze_310p_memfabric_overlap.py task_time.csv \
      --rows 8192 --batch-basem-count 1 --bandwidth-gbps 20
"""

from __future__ import annotations

import argparse
import contextlib
import csv
import statistics

PRODUCER_MARK = "Mf310pDirectProducerKernel"
WAITER_MARK = "mf310pWaitBatchKernel"
ADD_MARK = "mf310pAddKernel"
GATE_MARK = "mf310pGateKernel"
QUIET_MARK = "mf310pQuietKernel"
ACK_MARK = "mf310pAckKernel"


def load(path: str) -> list[dict]:
    events = []
    with open(path) as f:
        for row in csv.DictReader(f):
            with contextlib.suppress(ValueError, KeyError):
                events.append(
                    {
                        "name": row["kernel_name"],
                        "stream": int(row["stream_id"]),
                        "dur": float(row["task_time(us)"]),
                        "start": float(row["task_start(us)"].strip()),
                        "stop": float(row["task_stop(us)"].strip()),
                    }
                )
    return events


def _median(events: list[dict]) -> float:
    return statistics.median(e["dur"] for e in events) if events else 0.0


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("csv")
    ap.add_argument("--rows", type=int, default=8192)
    ap.add_argument("--batch-basem-count", type=int, choices=(1, 2, 4), default=1)
    ap.add_argument("--arena-rows", type=int, default=8192)
    ap.add_argument("--bandwidth-gbps", type=float, default=20.0)
    args = ap.parse_args()

    if args.rows <= 0 or args.arena_rows <= 0:
        raise SystemExit("rows and arena-rows must be positive")

    batch_m = 256 * args.batch_basem_count
    batch_bytes = batch_m * 2048 * 2
    if args.arena_rows < batch_m or args.arena_rows % batch_m:
        raise SystemExit("arena-rows must be a positive multiple of batch_m")

    events = load(args.csv)
    by = {
        mark: sorted(
            (event for event in events if mark in event["name"]),
            key=lambda event: event["start"],
        )
        for mark in (
            PRODUCER_MARK,
            WAITER_MARK,
            ADD_MARK,
            GATE_MARK,
            QUIET_MARK,
            ACK_MARK,
        )
    }

    print(
        "events: "
        + " ".join(
            f"{mark}={len(by[mark])}"
            for mark in (
                PRODUCER_MARK,
                WAITER_MARK,
                ADD_MARK,
                GATE_MARK,
                QUIET_MARK,
                ACK_MARK,
            )
        )
    )
    if not by[PRODUCER_MARK] or not by[WAITER_MARK] or not by[ADD_MARK]:
        raise SystemExit("no ABI v7 fused batch events found")

    waves = (args.rows + args.arena_rows - 1) // args.arena_rows
    batches = 0
    remaining = args.rows
    per_wave = []
    while remaining:
        wave_rows = min(args.arena_rows, remaining)
        wave_batches = (wave_rows + batch_m - 1) // batch_m
        per_wave.append(wave_batches)
        batches += wave_batches
        remaining -= wave_rows

    print(
        f"shape: rows={args.rows} base_m=256 q={args.batch_basem_count} "
        f"batch_m={batch_m} batch_bytes={batch_bytes} "
        f"waves={waves} batches_per_wave={per_wave}"
    )
    print(
        "medians(us): "
        f"producer={_median(by[PRODUCER_MARK]):.1f} "
        f"wait={_median(by[WAITER_MARK]):.1f} "
        f"add={_median(by[ADD_MARK]):.1f} "
        f"gate={_median(by[GATE_MARK]):.1f} "
        f"quiet={_median(by[QUIET_MARK]):.1f} "
        f"ack={_median(by[ACK_MARK]):.1f}"
    )

    expected_ratio = len(by[PRODUCER_MARK]) / max(1, len(by[GATE_MARK]))
    print(f"observed producers/wave={expected_ratio:.2f}; shape expects about {batches / waves:.2f}")

    wait_us = _median(by[WAITER_MARK])
    if wait_us <= 0:
        raise SystemExit("invalid waiter median")
    implied_gbps = batch_bytes / (wait_us * 1e-6) / 1e9
    serial_sdma_us = batch_bytes / (args.bandwidth_gbps * 1e9) * 1e6
    print(
        f"wait-only implication={implied_gbps:.1f} GB/s; "
        f"serial batch SDMA lower bound at {args.bandwidth_gbps:.1f} GB/s="
        f"{serial_sdma_us:.1f} us"
    )

    if implied_gbps > 2 * args.bandwidth_gbps:
        print(
            "OVERLAP EVIDENCE: peer batch transfer substantially progressed "
            "before its waiter ran. Confirm P(n+1)/SDMA(n)/A(n-1) ordering "
            "in the CANN timeline."
        )
    else:
        print(
            "INCONCLUSIVE: waiter exposure alone does not prove enough overlap; "
            "inspect the CANN timeline and SDMA events."
        )


if __name__ == "__main__":
    main()
