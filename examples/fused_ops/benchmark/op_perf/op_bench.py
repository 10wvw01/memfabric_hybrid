#!/usr/bin/env python3
"""Unified TP2 op-level benchmark: stock vs smem_mm_ar_310p3 vs smem_mm_iar_310p3.

What each path runs — byte-for-byte the same work vllm serve does:

  stock : F.linear(x, w_fp16_[N,K]_NZ) + HCCL all_reduce
          (AscendUnquantizedLinearMethod: weights maybe_trans_nz'ed at load,
          forward is torch.ops.vllm.unquantized_gemm -> F.linear; no smem
          pool exists when the fusion flag is off)
          Process-level allow_internal_format + jit_compile=disable replicates
          the serve internal-format fast route; route_rank{R}.txt captures the
          actual route (TD/WeightNz direct) as evidence.
  ar    : status.zero_() + npu_smem_mm_ar_310p3 wave (local reduction)
  iar   : status.zero_() + npu_smem_mm_iar_310p3 wave (hardware reduction)
          (the fused wrapper's exact per-op work, pool resident, native M)

Timing (PRIMARY): device-side task durations from torch_npu.profiler (msprof).
Host per-op wall-clock is NOT valid with the pool resident: per-op syncs
degrade SDMA completions to ~500ms watchdogs (wall-clock stays as reference
for stock only).

One process per rank (RANK=0/1 in env); both ranks run identical lockstep
loops. Per rank writes into --outdir:
  summary_rank{R}.json      machine-readable results
  trace_rank{R}_rep{i}.json chrome traces (one per repeat)
  taxonomy_rank{R}.txt      full (cat,name) histogram of rep 1 (for tuning)
"""
from __future__ import annotations

import argparse
import json
import os
import re
import sys
import time

import torch
import torch_npu
import torch.distributed as dist

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from analyze_trace import summarize, taxonomy, per_iter_stats  # noqa: E402

# Anchor device task (exactly 1/iteration) that opens each iteration window:
# adaptive route_evidence() overrides these with the SmemMm*310p3 kernel
# anchor whenever the fused kernels are detected (the common case); the
# regexes below are only the fallback. stock iter starts with the input
# transdata. First 2 windows are dropped: after profiler start the ranks are
# skewed and the leading kernels spin-wait for the lagging peer (durations
# inflated).
ANCHOR = {"stock": r"^trans_TransData",
          "ar": r"^ZerosLike",   # 兜底锚（常态被 SmemMmAr310p3 覆盖）
          "iar": r"^ZerosLike"}  # 兜底锚（常态被 SmemMmIar310p3 覆盖）
DROP_FIRST = 2

# ---- stock 路由取证与锚点自适应 ---------------------------------------------
# serve 快速路由因构建而异：常见 = MatMulV2 + TD3(x→NZ) + TD6(out→ND)。
# stock 锚点必须跟路由走：TD 路由 → 锚首个 transdata（stride=每迭代 TD 数）；
# 无 TD 路由 → 锚 MM 核本身。融合臂锚融合核（每迭代 AIC+AIV 共 2 个，
# stride=2）。锚事件数必须被 iters 整除，否则分段无效直接报错。
_ROUTE_MM = re.compile(r"^(aclnnMatmul|MatMul)")
_ROUTE_TD = re.compile(r"TransData", re.I)
_ROUTE_HCCL = re.compile(r"^hcom_allReduce")
_ROUTE_SMEM = re.compile(r"^SmemMm(Ar|Iar)310[pP]3")
_FUSED_KERNEL = {"ar": "SmemMmAr310p3", "iar": "SmemMmIar310p3"}


def route_evidence(trace_path: str, path: str, iters: int) -> dict:
    """从 rep trace 提取路由证据并解析锚点/锚步长（路由门）。

    返回 {anchor, stride, mm, mm_n, td_n, hccl_n, smem_n}。
    stock：td_n>0 → '^trans_TransData'（stride=td_n/iters）；td_n==0 → MM 核（stride=1）。
    融合臂：锚 '^SmemMm{Ar,Iar}310p3'（stride=smem_n/iters）。
    """
    from collections import Counter
    t = json.load(open(trace_path))
    evs = t.get("traceEvents", t) if isinstance(t, dict) else t
    mm_c, td_n, hccl_n, smem_n = Counter(), 0, 0, 0
    for e in evs:
        if not isinstance(e, dict) or e.get("cat") == "cpu_op":
            continue
        n = e.get("name", "")
        if n.startswith(("Enqueue@", "Dequeue@")):
            continue
        if _ROUTE_TD.search(n):
            td_n += 1
        elif _ROUTE_MM.match(n):
            mm_c[n[:64]] += 1
        elif _ROUTE_HCCL.match(n):
            hccl_n += 1
        elif _ROUTE_SMEM.match(n):
            smem_n += 1
    mm, mm_n = (mm_c.most_common(1)[0] if mm_c else ("?", 0))

    anchor, stride = ANCHOR[path], 1
    if path == "stock":
        if td_n > 0:
            if td_n % iters:
                raise RuntimeError(
                    f"stock TD-anchor invalid: transdata n={td_n} not divisible "
                    f"by iters={iters} (windowing would be wrong)")
            anchor, stride = r"^trans_TransData", td_n // iters
        elif mm_n > 0:
            if mm_n % iters:
                raise RuntimeError(
                    f"stock MM-anchor invalid: {mm!r} n={mm_n} not divisible "
                    f"by iters={iters}")
            anchor, stride = "^" + re.escape(mm), 1
    elif path in ("ar", "iar") and smem_n:
        if smem_n % iters:
            raise RuntimeError(
                f"{path} smem-anchor invalid: n={smem_n} not divisible by "
                f"iters={iters}")
        anchor, stride = "^" + _FUSED_KERNEL[path], smem_n // iters
    return {"anchor": anchor, "stride": stride, "mm": mm, "mm_n": mm_n,
            "td_n": td_n, "hccl_n": hccl_n, "smem_n": smem_n}

WIDTH = 2048            # K == N per rank at TP2
MAX_ROWS = 4096         # kernel work-area capacity (session facade re-exports)

p = argparse.ArgumentParser()
p.add_argument("--path", choices=["stock", "ar", "iar"], required=True,
               help="stock=MM+HCCL; ar=smem_mm_ar_310p3(本地归约); "
                    "iar=smem_mm_iar_310p3(硬件归约)")
p.add_argument("--devices", default="4,5")
p.add_argument("--m", type=int, required=True,
               help="rows (%256==0 且 ∈[1024,4096]; stock 任意 ≤4096)")
p.add_argument("--iters", type=int, default=30)
p.add_argument("--warmup", type=int, default=5)
p.add_argument("--reps", type=int, default=3)
p.add_argument("--store-url", required=True)
p.add_argument("--dist-port", type=int, required=True)
p.add_argument("--outdir", required=True)
a = p.parse_args()

RANK = int(os.environ["RANK"])
M = a.m
assert 1 <= M <= MAX_ROWS, f"M={M} outside range [1, {MAX_ROWS}]"
dev = [int(x) for x in a.devices.split(",")][RANK]

torch.npu.set_device(dev)
# serve 同构配方：裸进程 F.linear(ND-x, NZ-w) 落 row-major 慢路径；serve 进程
# 因内部格式机制走快速 NZ 路由（MatMulV2 + TD3(x→NZ)/TD6(out→ND)）。触发条件 =
# allow_internal_format + jitCompile=disable（+下方 dist hccl）。对 stock 与融合
# 臂同等生效——serve 全进程都在此状态下跑。
torch.npu.config.allow_internal_format = True
torch_npu.npu.set_compile_mode(jit_compile=False)
device = torch.device(f"npu:{dev}")
torch.manual_seed(42)
os.makedirs(a.outdir, exist_ok=True)

dist.init_process_group(backend="hccl",
                        init_method=f"tcp://127.0.0.1:{a.dist_port}",
                        world_size=2, rank=RANK)

if a.path != "stock":
    # smem 会话：xops 会话封装层（建池/几何校验/通道/epoch 内管全在层内，
    # 与 vllm serve 的会话同序列——含 status.zero_() 每调用清零）。
    if a.path == "ar":
        import xops_ar_310p3 as xops
    else:
        import xops_iar_310p3 as xops
    sess = xops.SmemSession(device=dev, rank=RANK, store_url=a.store_url)
    gva = sess.gva
    symm = sess.symm
else:
    sess = None
    gva = symm = 0

# ---- tensors --------------------------------------------------------------
x = (torch.randn(M, WIDTH, device=device) * 0.1).to(torch.float16)
w_nz = torch_npu.npu_format_cast(
    (torch.randn(WIDTH, WIDTH, device=device) * 0.1).to(torch.float16)
    .contiguous(), 29)                      # [N,K] fp16 NZ — stock layout
cold_ms = None

if a.path != "stock":
    # 去pad契约（部分尾组/末子组支持）：原生 M，无 pad 缓冲、无 staging 拷贝。
    assert 1024 <= M <= 4096 and M % 256 == 0, \
        f"fused path M={M} outside [1024,4096] or not 256-aligned"
    rows = M
    y_p = torch.zeros(M, WIDTH, dtype=torch.float16, device=device)


def fused_call():
    # 封装层内：status 清零 + epoch 递增 + 算子发射；无 staging 拷贝（原生 M）
    sess.launch(x, w_nz, rows, out=y_p)


def stock_call():
    out = torch.nn.functional.linear(x, w_nz)
    dist.all_reduce(out)


call = {"ar": fused_call, "iar": fused_call, "stock": stock_call}[a.path]


def log(msg: str) -> None:
    print(f"[rank{RANK}] {msg}", flush=True)


log(f"path={a.path} M={M} iters={a.iters} reps={a.reps}"
    + (f" pool gva=0x{gva:x}" if a.path != "stock" else " no pool"))

# ---- cold first wave (fused only; watchdog-contaminated, reference) -------
if a.path != "stock":
    torch.npu.synchronize()
    sess.barrier()
    t0 = time.perf_counter()
    call()
    torch.npu.synchronize()
    cold_ms = (time.perf_counter() - t0) * 1e3
    sess.barrier()
    log(f"cold-1st {cold_ms:.1f} ms (watchdog-contaminated, reference only)")

# ---- warmup ---------------------------------------------------------------
for _ in range(a.warmup):
    call()
torch.npu.synchronize()
dist.barrier()
log("warmup done")

# ---- repeats: pipelined wall-clock (reference) + profiler (primary) -------
profiler_cfg = dict(
    activities=[torch_npu.profiler.ProfilerActivity.CPU,
                torch_npu.profiler.ProfilerActivity.NPU],
    with_stack=False,
    experimental_config=torch_npu.profiler._ExperimentalConfig(
        export_type=torch_npu.profiler.ExportType.Text,
        profiler_level=torch_npu.profiler.ProfilerLevel.Level1,
        msprof_tx=False,
        aic_metrics=torch_npu.profiler.AiCMetrics.PipeUtilization,
        l2_cache=False,
        op_attr=False,
        data_simplification=True,
        record_op_args=False,
        gc_detect_threshold=None,
    ),
)

reps_out = []
route_rep1 = None
for rep in range(1, a.reps + 1):
    # pipelined wall-clock (reference only — pool-resident host syncs are
    # watchdog-suspect; the PRIMARY metric is the profiler window below).
    torch.npu.synchronize()
    dist.barrier()
    t0 = time.perf_counter()
    for _ in range(a.iters):
        call()
    torch.npu.synchronize()
    wall_ms = (time.perf_counter() - t0) / a.iters * 1e3
    dist.barrier()

    # device-side durations via profiler; NO sync inside the window
    prof = torch_npu.profiler.profile(**profiler_cfg)
    prof.start()
    for _ in range(a.iters):
        call()
    prof.stop()
    # stop 后设备侧 flip 落盘裕量——间歇性导出竞争会导致该 rep trace 无设备
    # 事件（分段 "no anchor events" 干净失败）。3s 不影响已记录的设备事件本身。
    time.sleep(3)
    trace_path = os.path.join(a.outdir, f"trace_rank{RANK}_rep{rep}.json")
    # 双 rank 并发 export 存在竞争，rank1 错峰 8s 串行化导出。
    if RANK == 1:
        time.sleep(8)
    prof.export_chrome_trace(trace_path)
    clusters = summarize(trace_path, iters=a.iters)
    route = route_evidence(trace_path, a.path, a.iters)
    if a.path == "stock" and route["mm_n"] == 0:
        # 路由门：stock 没识别到 MatMul 核 = 环境不对，数字无效
        raise RuntimeError(
            f"stock route unidentified: no MatMul kernel in {trace_path} "
            f"(serve 同构配方失效？)")
    dst = per_iter_stats(trace_path, iters=a.iters,
                         anchor_regex=route["anchor"],
                         drop_first=DROP_FIRST,
                         anchor_stride=route["stride"])
    if "error" in dst:
        # 分段失败 = 窗口/锚点口径不对，数字无效——干净地失败而不是 KeyError
        raise RuntimeError(f"per-iteration segmentation failed: {dst['error']}")
    if rep == 1:
        route_rep1 = route
        with open(os.path.join(a.outdir, f"taxonomy_rank{RANK}.txt"), "w") as f:
            import contextlib
            with contextlib.redirect_stdout(f):
                taxonomy(trace_path)
        with open(os.path.join(a.outdir, f"route_rank{RANK}.txt"), "w") as f:
            json.dump(route, f, indent=2, ensure_ascii=False)
        if a.path == "stock":
            log(f"ROUTE: mm={route['mm']} n={route['mm_n']} transdata={route['td_n']} "
                f"hccl={route['hccl_n']} anchor={route['anchor']!r} stride={route['stride']}")
        exp_win = a.iters - DROP_FIRST
        if dst.get("windows", 0) != exp_win:
            log(f"WARNING: window count {dst.get('windows')} != expected {exp_win} "
                f"(iters={a.iters}, drop={DROP_FIRST}) — 窗口口径异常，核查锚点")
    od = dst["op_dist"]
    sd = dst.get("span_dist", {})
    reps_out.append({
        "rep": rep,
        "wall_ms": round(wall_ms, 3),
        "wall_watchdog_suspect": None if wall_ms != wall_ms else wall_ms > 100,
        "op_dist": od,
        "span_dist": sd,
        "op_vals_ms": dst.get("op_vals_ms", []),
        "span_vals_ms": dst.get("span_vals_ms", []),
        "cluster_dist": dst.get("cluster_dist", {}),
        "cluster_vals_ms": dst.get("cluster_vals_ms", {}),
        "windows": dst.get("windows", 0),
        "unassigned_events": dst.get("unassigned_events", -1),
        "cluster_totals": clusters,
    })
    cd = dst["cluster_dist"]
    cl_str = ", ".join(
        f"{k}:p50={v['p50_ms']:.3f}" for k, v in
        sorted(cd.items(), key=lambda kv: -kv[1]["p50_ms"]))
    log(f"rep{rep}: wall={wall_ms:.3f} ms/iter "
        f"({'WATCHDOG-SUSPECT' if wall_ms > 100 else 'ok'}), "
        f"device span P50/P90/P95 = {sd.get('p50_ms', 0):.3f}/"
        f"{sd.get('p90_ms', 0):.3f}/{sd.get('p95_ms', 0):.3f} ms "
        f"(sum {od['p50_ms']:.3f}), clusters[{cl_str}]")

summary = {
    "path": a.path, "m": M, "iters": a.iters, "rank": RANK,
    "packet": 1024 * 1024, "gva": gva, "cold_ms": cold_ms, "route": route_rep1,
    "reps": reps_out,
}
out_json = os.path.join(a.outdir, f"summary_rank{RANK}.json")
with open(out_json, "w") as f:
    json.dump(summary, f, indent=2)
log(f"summary -> {out_json}")

# Clean pool teardown before exit — without this the smem library's static
# destructors abort at process exit ("terminate called without an active
# exception", core dumped).
if a.path != "stock":
    sess.teardown()
    log("pool destroyed cleanly")

dist.barrier()
dist.destroy_process_group()
log("DONE")
