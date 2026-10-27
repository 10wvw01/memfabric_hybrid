# SPDX-License-Identifier: Apache-2.0
"""310P3 TP=2 下 T2 非融合传输路径性能基准。

A：torch_npu 原生 F.linear + FP16 HCCL all_reduce。
B：同一个 F.linear + D2D 暂存 + MemFabric signal/wait + Vector Add。
C：V2 融合 MM + MemFabric AR（仅适用形状，可选）。

所有 HCCL 工作（包括 FP32 正确性参考）都在 MemFabric 建池之前完成。
本测试不能证明 HCCL 与 MemFabric 在真实 Serving 中并行共存安全；
共存需要独立的 G3 验收。

构建时须同时启用 V1/V2 开关（见 T2_AR_ONLY_TRIAL.md）。
示例：
  ASCEND_RT_VISIBLE_DEVICES=0,1 torchrun --standalone --nproc-per-node=2 \
    csrc/_310P/memfabric_mm_ar_v2/test/bench_ar_only.py \
    --rows 1 16 64 128 255 256 257 512 1024 2048 4096 8192 \
    --chunk-rows 1024 --add-cores 1 2 4 8 --repeat 30 --fused
"""

from __future__ import annotations

import argparse
import atexit
import contextlib
import ctypes
import os
import statistics
import time

import torch
import torch.distributed as dist
import torch.nn.functional as F
import torch_npu  # noqa: F401
import vllm_ascend.vllm_ascend_C as extension

from vllm_ascend.utils import maybe_trans_nz


def bind_library():
    lib = ctypes.CDLL(extension.__file__)
    P, U32, U64 = ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint64
    signatures = {
        "mf310p_v2_trial_create": ([ctypes.c_int, ctypes.c_char_p, U32, ctypes.POINTER(P)], ctypes.c_int),
        "mf310p_v2_trial_ib_probe": ([P, U32, P], ctypes.c_int),
        "mf310p_v2_trial_run_ar_only": ([P, U64, U64, U32, U32, P], ctypes.c_int),
        "mf310p_v2_trial_run_nz_direct_small": ([P, U64, U64, U64, U32, U32, U32, P], ctypes.c_int),
        "mf310p_v2_trial_run": ([P, U64, U64, U64, U32, U32, P], ctypes.c_int),
        "mf310p_v2_trial_check_status": ([P, ctypes.POINTER(U64)], ctypes.c_int),
        "mf310p_v2_trial_destroy": ([P], ctypes.c_int),
    }
    for name, (args, result) in signatures.items():
        fn = getattr(lib, name)
        fn.argtypes, fn.restype = args, result
    return lib


def optional_test_kfc_bypass():
    """仅用于 dev0/dev1 已知损坏的试验 KFC 控制面。

    该方法沿用 T1 显式启用的临时绕过，不得在生产启用，
    也不得将绕过后的初始化当作正常部署验收通过。
    """
    if os.environ.get("MF_KFC_BYPASS") != "1":
        return
    paths = []
    for index in range(16):
        path = f"/tmp/mf_kfc_{os.getpid()}_{index}.json"
        try:
            fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_RDWR, 0o600)
            os.close(fd)
            paths.append(path)
        except FileExistsError:
            continue

    def cleanup():
        for path in paths:
            with contextlib.suppress(FileNotFoundError):
                os.unlink(path)

    atexit.register(cleanup)


def chk(rc, desc):
    if rc:
        raise RuntimeError(f"{desc}: rc={rc}")


def measure(fn, warmup, repeat):
    for _ in range(warmup):
        keepalive = fn()
        torch.npu.current_stream().synchronize()
        del keepalive
    samples = []
    for _ in range(repeat):
        begin = time.perf_counter()
        keepalive = fn()
        torch.npu.current_stream().synchronize()
        samples.append((time.perf_counter() - begin) * 1e3)
        del keepalive
    samples.sort()
    return {
        "median": round(statistics.median(samples), 5),
        "p95": round(samples[min(len(samples) - 1, int(0.95 * (len(samples) - 1)))], 5),
        "min": round(samples[0], 5),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rows", nargs="+", type=int, default=[1, 16, 64, 128, 255, 256, 257, 512, 1024, 2048, 4096, 8192])
    ap.add_argument("--add-cores", nargs="+", type=int, choices=(1, 2, 4, 8), default=[1, 2, 4, 8])
    ap.add_argument("--chunk-rows", type=int, choices=(256, 512, 1024), default=1024)
    ap.add_argument("--repeat", type=int, default=30)
    ap.add_argument("--warmup", type=int, default=3)
    ap.add_argument(
        "--fused",
        action="store_true",
        help="Also measure the existing V2 small or T0 fused path when shape is eligible",
    )
    ap.add_argument(
        "--store-url", default=os.environ.get("VLLM_ASCEND_310P_MEMFABRIC_STORE_URL", "tcp://127.0.0.1:8581")
    )
    a = ap.parse_args()
    if any(m < 1 or m > 8192 for m in a.rows):
        ap.error("M must be 1..8192; multi-wave not yet supported")
    if a.repeat < 5 or a.warmup < 1:
        ap.error("require repeat >= 5 and warmup >= 1")
    if len(set(a.rows)) != len(a.rows):
        ap.error("duplicate M values are not supported in this benchmark")

    rank = int(os.environ["RANK"])
    local_rank = int(os.environ["LOCAL_RANK"])
    if int(os.environ["WORLD_SIZE"]) != 2:
        raise RuntimeError("TP must be exactly 2")
    torch.npu.set_device(local_rank)
    device = torch.device(f"npu:{local_rank}")
    dist.init_process_group(backend="hccl")
    cpu_group = dist.new_group(backend="gloo")
    stream = torch.npu.current_stream()
    raw_stream = ctypes.c_void_p(stream.npu_stream)

    torch.manual_seed(7865 + rank)
    weight = maybe_trans_nz(torch.randn(2048, 2048, dtype=torch.float16, device=device))
    x_map, reference, stock, mm_only = {}, {}, {}, {}

    # Stock 的性能口径为实际部署的 FP16 HCCL AllReduce，而不是 FP32。
    # 正确性参考独立使用 FP32 跨 TP 求和，再转回 FP16。
    # 所有 HCCL 测量均在 MemFabric 建池之前完成。
    for m in a.rows:
        torch.manual_seed(2500 + rank * 103 + m)
        x = torch.randn(m, 2048, device=device, dtype=torch.float16)
        x_map[m] = x
        partial = F.linear(x, weight)
        ref = partial.float()
        dist.all_reduce(ref)
        reference[m] = ref.half()
        stream.synchronize()

        def native_hccl(x=x):
            local = F.linear(x, weight)
            dist.all_reduce(local)
            return local  # 保持分配对象存活，直到 Stream 同步完成

        stock[m] = measure(native_hccl, a.warmup, a.repeat)
        mm_only[m] = measure(lambda x=x: F.linear(x, weight), a.warmup, a.repeat)
    torch.npu.synchronize()
    dist.barrier(group=cpu_group)

    optional_test_kfc_bypass()
    lib = bind_library()
    handle = ctypes.c_void_p()
    chk(
        lib.mf310p_v2_trial_create(rank, a.store_url.encode("utf-8"), a.chunk_rows, ctypes.byref(handle)),
        "trial_create",
    )
    if not handle.value:
        raise RuntimeError("null MemFabric trial context")

    rows = []
    try:
        chk(lib.mf310p_v2_trial_ib_probe(handle, 1000, raw_stream), "IB probe")
        for m in a.rows:
            x = x_map[m]
            ref = reference[m]
            # 每个 M 复用同一个 y，并在不同 Add 核数方案间轮换；
            # 这样可以减少首次写保护引发的设备级同步次数，
            # 因为其耗时随机为 0～15 秒，并可能造成两 Rank 发射偏斜，
            # 使对端通信核超过约 28 秒的 AICore 邮件等待看门狗时限，
            # 曾在 chunk=256、M=8192 时观察到超时。
            y = torch.empty_like(ref)
            for add in a.add_cores:

                def native_memfabric(x=x, y=y, m=m, add=add):
                    # 这里使用框架原生 MM，不是 V1/V2 定制 MM。
                    partial = F.linear(x, weight)
                    # 2026-10-18 T2 实测平台缓存一致性约束：
                    # AR-only 暂存核通过 MTE1 读取 MM 输出，
                    # 可能漏读 MatMul 尚驻留缓存的写入，
                    # 导致小 M 数据滞后一轮，
                    # 例如混合读到本端第 1 轮、对端第 0 轮。
                    # MM 与暂存之间同步 Stream 是实测有效的处理，
                    # M=1 已通过八组千次验证；该同步开销必须
                    # 真实计入非融合 B 路径的总时间，
                    # 不能从基线中剔除；融合 V2 则无需此暂存成本，
                    # 因为 MM 在单 Kernel 内直接读取已准备的输入。
                    stream.synchronize()
                    chk(
                        lib.mf310p_v2_trial_run_ar_only(handle, partial.data_ptr(), y.data_ptr(), m, add, raw_stream),
                        f"AR-only m={m} add={add}",
                    )
                    # 生产者 Tensor 必须保留至调用方同步完成，
                    # D2D 尚未结束时禁止将其内存返回缓存分配器，
                    # 以免异步搬运读取被提前复用的内存。
                    return partial

                def run_and_verify(m=m, add=add, ref=ref, y=y):
                    partial = native_memfabric()
                    stream.synchronize()
                    status = ctypes.c_uint64(0)
                    chk(lib.mf310p_v2_trial_check_status(handle, ctypes.byref(status)), "AR-only protocol status")
                    if status.value != 0:
                        raise RuntimeError(f"AR-only status={status.value} M={m} add={add}")
                    if not torch.equal(y, ref):
                        bad = int((y != ref).sum().item())
                        diff = float((y.float() - ref.float()).abs().max().item())
                        raise AssertionError(f"AR-only bit-exact mismatch M={m} add={add} bad={bad} max_abs={diff}")
                    return partial

                # 正式计时前先执行一次正确性检查，该调用
                # 也是该 (M,Add) 组合的首次 AR-only 发射，
                # 其耗时包含首次写保护同步，应单独记录为
                # first_call_ms，不得混入稳定态计时。
                t_first_begin = time.perf_counter()
                run_and_verify()
                first_call_ms = round((time.perf_counter() - t_first_begin) * 1e3, 5)
                # 每次调用保持 Tensor 的有效生命周期直至同步完成；
                # measure() 会调用闭包并等待 Stream 同步。
                t = measure(native_memfabric, a.warmup, a.repeat)
                run_and_verify()
                rows.append(
                    {
                        "m": m,
                        "method": "native_mm+memfabric_stage_ar",
                        "add": add,
                        **t,
                        "first_call_ms": first_call_ms,
                        "correct": True,
                    }
                )

            if a.fused:
                fused_kind = (
                    "V2_T1_nz_direct"
                    if m <= 256
                    else (
                        "V2_T0_overlap"
                        if m % a.chunk_rows == 0 and m >= 2 * a.chunk_rows
                        else "V2_T0_serial"
                        if m % a.chunk_rows == 0
                        else None
                    )
                )
                if fused_kind:
                    yy = torch.empty_like(ref)

                    def fused(m=m, x=x, weight=weight, yy=yy, fused_kind=fused_kind):
                        if m <= 256:
                            add = 1 if m < 255 else 8
                            chk(
                                lib.mf310p_v2_trial_run_nz_direct_small(
                                    handle, x.data_ptr(), weight.data_ptr(), yy.data_ptr(), m, 8, add, raw_stream
                                ),
                                "V2 T1",
                            )
                        else:
                            chk(
                                lib.mf310p_v2_trial_run(
                                    handle,
                                    x.data_ptr(),
                                    weight.data_ptr(),
                                    yy.data_ptr(),
                                    m,
                                    1 if fused_kind.endswith("overlap") else 0,
                                    raw_stream,
                                ),
                                "V2 T0",
                            )

                    # 完成预热不代表满足 Graph 或生产安全条件；
                    # T0 首次输出仍可能需要设备全局同步。
                    fused()
                    stream.synchronize()
                    status = ctypes.c_uint64(0)
                    chk(lib.mf310p_v2_trial_check_status(handle, ctypes.byref(status)), "fused protocol")
                    if status.value or not torch.equal(yy, ref):
                        raise AssertionError(f"fused mismatch M={m}, status={status.value}")
                    ft = measure(fused, a.warmup, a.repeat)
                    rows.append({"m": m, "method": fused_kind, "add": 1 if m < 255 else 8, **ft, "correct": True})

        dist.barrier(group=cpu_group)
    finally:
        stream.synchronize()
        chk(lib.mf310p_v2_trial_destroy(handle), "trial_destroy")

    # 收集两个 Rank 的耗时，TP 完成时间应取较慢一侧，
    # 不能只使用打印输出的那个 Rank。
    gathered = [None, None]
    dist.all_gather_object(gathered, {"rows": rows, "stock": stock, "mm_only": mm_only}, group=cpu_group)
    if rank == 0:
        mapping = {(r["m"], r["method"], r["add"]): r for r in gathered[1]["rows"]}
        print(
            "M\tmethod\tadd\tstock_max_ms\tnative_mm_max_ms\tcandidate_max_ms\tcandidate_max_p95\t"
            "speedup_percent\tcorrect",
            flush=True,
        )
        for rr in rows:
            key = (rr["m"], rr["method"], rr["add"])
            other = mapping[key]
            two = max(rr["median"], other["median"])
            p95 = max(rr["p95"], other["p95"])
            first = max(rr.get("first_call_ms", 0.0), other.get("first_call_ms", 0.0))
            # TP 的单次计算仅在两个 Rank 都完成时才算结束。
            stock_time = max(gathered[0]["stock"][rr["m"]]["median"], gathered[1]["stock"][rr["m"]]["median"])
            mm_time = max(gathered[0]["mm_only"][rr["m"]]["median"], gathered[1]["mm_only"][rr["m"]]["median"])
            gain = (stock_time - two) / stock_time * 100.0
            print(
                f"{rr['m']}\t{rr['method']}\t{rr['add']}\t"
                f"{stock_time:.4f}\t{mm_time:.4f}\t{two:.4f}\t{p95:.4f}\t"
                f"{gain:.1f}%\t{rr['correct'] and other['correct']}",
                flush=True,
            )
            if "first_call_ms" in rr:
                print(
                    f"# first_call_max_ms {rr['m']}\t{rr['add']}\t{first:.4f} (includes first-write guard sync)",
                    flush=True,
                )
        dump_path = os.environ.get("T2_RESULT_JSON")
        if dump_path:
            import json

            with open(dump_path, "w") as f:
                json.dump(
                    {
                        "rows": gathered[0]["rows"],
                        "peer_rows": gathered[1]["rows"],
                        "stock": gathered[0]["stock"],
                        "peer_stock": gathered[1]["stock"],
                        "mm_only": gathered[0]["mm_only"],
                        "peer_mm_only": gathered[1]["mm_only"],
                    },
                    f,
                    indent=1,
                )
    dist.barrier(group=cpu_group)
    dist.destroy_process_group()


if __name__ == "__main__":
    main()
