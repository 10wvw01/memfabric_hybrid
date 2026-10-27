#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""U1小M单Die MM成本隔离：原生F.linear vs 诊断 NZ N-split MM+CleanLines。
非完整AR，非纯计算时钟；不要用二者中位数相减来推断MemFabric协议耗时。
"""
from __future__ import annotations
import argparse
import ctypes
import hashlib
import json
import math
from pathlib import Path
import statistics
import time

import torch
import torch.nn.functional as F
import torch_npu  # noqa: F401

from u1_spec import D4_DEVICE_SHA, K, N

TEMPLATE = (16, 32, 64, 128, 256)


def templ(m):
    return next(t for t in TEMPLATE if m <= t)


def bind_diag(path):
    if hashlib.sha256(Path(path).read_bytes()).hexdigest() != D4_DEVICE_SHA:
        raise RuntimeError("wrong D4 diagnostics ON device SO; do not bypass")
    lib = ctypes.CDLL(path)
    fun = lib.mf310p_v2_diag_nzdirect_mm_async
    fun.argtypes = [ctypes.c_uint64, ctypes.c_uint64, ctypes.c_uint64,
                    ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint32,
                    ctypes.c_uint32, ctypes.c_void_p]
    fun.restype = ctypes.c_int
    return fun


def sample(op, stream, ev0, ev1):
    t0 = time.perf_counter_ns()
    ev0.record(stream)
    y = op()
    ev1.record(stream)
    stream.synchronize()
    wall = (time.perf_counter_ns() - t0) / 1000.0
    event = ev0.elapsed_time(ev1) * 1000.0
    if not all(math.isfinite(x) and x > 0 for x in (wall, event)):
        raise RuntimeError("invalid timing")
    return y, round(wall, 3), round(event, 3)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--ms", type=int, nargs="+", default=[10, 16, 64, 256])
    p.add_argument("--mm-cores", type=int, nargs="+", default=[4, 8])
    p.add_argument("--device-so", type=Path, required=True)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--warmup", type=int, default=5)
    p.add_argument("--iters", type=int, default=30)
    a = p.parse_args()
    if a.out.exists() or any(m not in (1, 8, 10, 16, 17, 64, 255, 256) for m in a.ms):
        p.error("new output path and approved small-M list required")
    if len(set(a.ms)) != len(a.ms) or any(x not in (4, 8) for x in a.mm_cores):
        p.error("duplicate M or unsupported MM core count")
    if a.warmup < 3 or a.iters < 30:
        p.error("warmup>=3, iters>=30")
    torch.npu.set_device(0)
    device = torch.device("npu:0")
    stream = torch.npu.current_stream()
    raw_stream = ctypes.c_void_p(stream.npu_stream)
    fn = bind_diag(a.device_so)
    torch.manual_seed(4242)
    w = torch.randn(N, K, dtype=torch.float16, device=device)
    w_nz = torch_npu.npu_format_cast(w, 29)
    records = []
    for m in a.ms:
        torch.manual_seed(1200 + m)
        x = torch.randn(m, K, dtype=torch.float16, device=device)
        ref = F.linear(x, w)
        out = torch.zeros_like(ref)
        stream.synchronize()
        for mode, cores in [("native_mm", 0)] + [("t1_mm_plus_clean", k) for k in a.mm_cores]:
            e0 = torch.npu.Event(enable_timing=True)
            e1 = torch.npu.Event(enable_timing=True)
            if mode == "native_mm":
                def op():
                    return F.linear(x, w)
            else:
                def op():
                    rc = fn(x.data_ptr(), w_nz.data_ptr(), out.data_ptr(),
                            templ(m), m, cores, 1, raw_stream)
                    if rc:
                        raise RuntimeError(f"diag NZ MM launch rc={rc}")
                    return out
                # 不跳过必要首写守护：新地址先同步、符号空执行与真实预热。
                for _ in range(2):
                    rc = fn(x.data_ptr(), w_nz.data_ptr(), out.data_ptr(),
                            templ(m), m, cores, 0, raw_stream)
                    if rc:
                        raise RuntimeError("diagnostic symbol warmup failed")
                stream.synchronize()
            for _ in range(a.warmup):
                y, _, _ = sample(op, stream, e0, e1)
                if not torch.equal(y, ref):
                    raise AssertionError(f"M{m} {mode} cores{cores} warmup bit mismatch")
            vals = []
            for i in range(a.iters):
                y, wall, event = sample(op, stream, e0, e1)
                if not torch.equal(y, ref):
                    raise AssertionError(f"M{m} {mode} cores{cores} sample{i} wrong")
                vals.append({"i": i, "event_us": event, "wall_us": wall})
            records.append({
                "M": m, "mode": mode, "cores": cores, "iters": len(vals),
                "bit_exact": True, "p50_event_us":
                    round(statistics.median(x["event_us"] for x in vals), 3),
                "samples": vals,
            })
            print(f"U1-MM M{m} {mode} cores={cores} samples={len(vals)} PASS", flush=True)
    a.out.parent.mkdir(parents=True, exist_ok=True)
    with a.out.open("x") as fp:
        json.dump({
            "experiment": "U1_SINGLE_DIE_MM_WITH_CLEAN",
            "device_so_sha256": D4_DEVICE_SHA, "device": "logical npu:0",
            "note": "T1 diagnostic includes CleanLines; native F.linear has output allocation. "
                    "Neither is directly the critical MM fraction of fused TP2.",
            "records": records,
        }, fp, ensure_ascii=False, indent=2)
        fp.write("\n")


if __name__ == "__main__":
    main()
