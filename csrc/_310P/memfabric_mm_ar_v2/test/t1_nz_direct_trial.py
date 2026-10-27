# SPDX-License-Identifier: Apache-2.0
# 310P3 TP=2 V2 T1 测试驱动：正确性矩阵、压力、y 首写保护探针、
# 以及 mf310p_v2_trial_run_nz_direct_small 的 Stock 性能基线。
#
# 测试模式（torchrun --nproc-per-node=2）：
# quick：M={1,16,17,255,256}，MM 核数 1/2/4/8，Add 核数 1/8，重复 3 次。
# full：M={1,2,4,8,16,32,64,128,255,256}，MM/Add 组合各为 1/2/4/8。
# stress：随机 M、4 组权重种子、轮换输出地址、1000 次连续调用。
# yguard：首次 MTE3 丢写概率探针，需设置 MF310P_V2_NOWARMUP=1。
# stock：只测原生 F.linear+HCCL AR，不创建 MemFabric 池。
#
# 环境要求见 NZ_DIRECT_TRIAL.md：先 source MemFabric set_env.sh，
# 设置 ASCEND_RT_VISIBLE_DEVICES=0,1；早期设备 KFC 失效时
# 曾使用 MF_KFC_BYPASS=1；目前须优先验证正常部署。
import argparse
import contextlib
import ctypes
import os
import random
import statistics
import time

import torch
import torch.distributed as dist
import torch.nn.functional as F
import torch_npu

ACL_FORMAT_FRACTAL_NZ = 29
K_LOCAL = 2048
N_OUT = 2048
try:
    import vllm_ascend.vllm_ascend_C as _ascend_ext

    _DEFAULT_LIB = _ascend_ext.__file__
except Exception:  # 尝试加载 tools/build_host.sh 构建的独立 Host 库
    _DEFAULT_LIB = "/tmp/opencode/v2/libmf310p_v2_trial_host.so"
HOST_LIB = os.environ.get("V2_HOST_LIB", _DEFAULT_LIB)
STORE_URL = os.environ.get("VLLM_ASCEND_310P_MEMFABRIC_STORE_URL", "tcp://127.0.0.1:8581")

# 压力模式使用的建池前参考数据。
_STRESS_REFS = {}
_STRESS_WNZ = []


class Trial:
    def __init__(self, rank: int, chunk: int):
        self.lib = ctypes.CDLL(HOST_LIB)
        self.lib.mf310p_v2_trial_create.argtypes = [
            ctypes.c_int,
            ctypes.c_char_p,
            ctypes.c_uint,
            ctypes.POINTER(ctypes.c_void_p),
        ]
        self.lib.mf310p_v2_trial_create.restype = ctypes.c_int
        self.lib.mf310p_v2_trial_ib_probe.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_void_p]
        self.lib.mf310p_v2_trial_ib_probe.restype = ctypes.c_int
        self.lib.mf310p_v2_trial_run_nz_direct_small.argtypes = [
            ctypes.c_void_p,
            ctypes.c_uint64,
            ctypes.c_uint64,
            ctypes.c_uint64,
            ctypes.c_uint32,
            ctypes.c_uint32,
            ctypes.c_uint32,
            ctypes.c_void_p,
        ]
        self.lib.mf310p_v2_trial_run_nz_direct_small.restype = ctypes.c_int
        self.lib.mf310p_v2_trial_run.argtypes = [
            ctypes.c_void_p,
            ctypes.c_uint64,
            ctypes.c_uint64,
            ctypes.c_uint64,
            ctypes.c_uint32,
            ctypes.c_uint32,
            ctypes.c_void_p,
        ]
        self.lib.mf310p_v2_trial_run.restype = ctypes.c_int
        self.lib.mf310p_v2_trial_check_status.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint64)]
        self.lib.mf310p_v2_trial_check_status.restype = ctypes.c_int
        self.lib.mf310p_v2_trial_destroy.argtypes = [ctypes.c_void_p]
        self.lib.mf310p_v2_trial_destroy.restype = ctypes.c_int
        self.handle = ctypes.c_void_p()
        rc = self.lib.mf310p_v2_trial_create(rank, STORE_URL.encode(), chunk, ctypes.byref(self.handle))
        assert rc == 0, f"trial_create rc={rc}"
        self.rank = rank
        self.status = ctypes.c_uint64()

    def ib_probe(self, reps: int, stream) -> None:
        rc = self.lib.mf310p_v2_trial_ib_probe(self.handle, reps, stream)
        assert rc == 0, f"ib_probe rc={rc}"

    def run_small(self, x_ptr, w_ptr, y_ptr, m, mm, add, stream) -> None:
        rc = self.lib.mf310p_v2_trial_run_nz_direct_small(self.handle, x_ptr, w_ptr, y_ptr, m, mm, add, stream)
        assert rc == 0, f"run_nz_direct_small rc={rc} m={m} mm={mm} add={add}"

    def run(self, x_ptr, w_ptr, y_ptr, m, overlap, stream) -> None:
        rc = self.lib.mf310p_v2_trial_run(self.handle, x_ptr, w_ptr, y_ptr, m, overlap, stream)
        assert rc == 0, f"run rc={rc}"

    def check_status(self) -> int:
        rc = self.lib.mf310p_v2_trial_check_status(self.handle, ctypes.byref(self.status))
        assert rc == 0, f"check_status rc={rc}"
        return self.status.value

    def destroy(self) -> None:
        rc = self.lib.mf310p_v2_trial_destroy(self.handle)
        assert rc == 0, f"destroy rc={rc}"


def kfc_bypass_files():
    if os.environ.get("MF_KFC_BYPASS") != "1":
        return
    for i in range(16):
        p = f"/tmp/mf_kfc_{os.getpid()}_{i}.json"
        try:
            fd = os.open(p, os.O_CREAT | os.O_EXCL | os.O_RDWR, 0o600)
            os.close(fd)
        except FileExistsError:
            pass


def kfc_bypass_cleanup():
    if os.environ.get("MF_KFC_BYPASS") != "1":
        return
    for i in range(16):
        with contextlib.suppress(FileNotFoundError):
            os.unlink(f"/tmp/mf_kfc_{os.getpid()}_{i}.json")


def sync():
    torch.npu.current_stream().synchronize()


def barrier(cpu_group):
    dist.barrier(group=cpu_group)


def verify(y, ref, tag, rank):
    if torch.equal(y, ref):
        return True
    diff = (y.float() - ref.float()).abs()
    n_bad = int((diff > 0).sum().item())
    if rank == 0:
        print(
            f"VERIFY\t{tag}\tFAIL\tmax_abs={diff.max().item():.4f}\t"
            f"n_bad={n_bad}\ty_abs_max={y.float().abs().max().item():.4f}\t"
            f"ref_abs_max={ref.float().abs().max().item():.4f}",
            flush=True,
        )
    return False


def nz_weight(seed, device):
    torch.manual_seed(seed)
    w = torch.randn(N_OUT, K_LOCAL, dtype=torch.float16, device=device)
    return w, torch_npu.npu_format_cast(w, ACL_FORMAT_FRACTAL_NZ)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--mode", required=True, choices=["quick", "full", "stress", "stock", "yguard"])
    p.add_argument("--rows", type=int, nargs="+", default=None)
    p.add_argument("--mm-cores", type=int, nargs="+", default=None)
    p.add_argument("--add-cores", type=int, nargs="+", default=None)
    p.add_argument("--repeat", type=int, default=None)
    p.add_argument("--chunk-rows", type=int, default=256)
    args = p.parse_args()

    rank = int(os.environ["RANK"])
    local_rank = int(os.environ["LOCAL_RANK"])
    assert int(os.environ["WORLD_SIZE"]) == 2
    torch.npu.set_device(local_rank)
    device = torch.device(f"npu:{local_rank}")
    dist.init_process_group(backend="hccl")
    cpu_group = dist.new_group(backend="gloo")
    stream = torch.npu.current_stream().npu_stream

    # 在 MemFabric 初始化前计算权重和 Stock/HCCL 正确性参考。
    w_plain, w_nz = nz_weight(4242, device)

    if args.mode == "stock":
        rows_list = args.rows or [1, 2, 4, 8, 16, 32, 64, 128, 255, 256]
        for rows in rows_list:
            torch.manual_seed(1000 * (rank + 1) + 7 * rows)
            x = torch.randn(rows, K_LOCAL, dtype=torch.float16, device=device)
            for _ in range(5):
                y = F.linear(x, w_plain)
                dist.all_reduce(y)
                sync()

            def stock(x=x):
                y = F.linear(x, w_plain)
                dist.all_reduce(y)

            times = []
            for _ in range(50):
                t0 = time.perf_counter()
                stock()
                sync()
                times.append((time.perf_counter() - t0) * 1e3)
            times.sort()
            if rank == 0:
                print(f"STOCK\trows={rows}\tmed={times[25]:.3f}\tmin={times[0]:.3f}\tp95={times[47]:.3f}", flush=True)
        barrier(cpu_group)
        dist.destroy_process_group()
        return

    if args.mode == "quick":
        rows_list = args.rows or [1, 16, 17, 255, 256]
        mm_list = args.mm_cores or [1, 2, 4, 8]
        add_list = args.add_cores or [1, 8]
        repeat = args.repeat or 3
    elif args.mode == "full":
        rows_list = args.rows or [1, 2, 4, 8, 16, 32, 64, 128, 255, 256]
        mm_list = args.mm_cores or [1, 2, 4, 8]
        add_list = args.add_cores or [1, 2, 4, 8]
        repeat = args.repeat or 30
    elif args.mode == "yguard":
        rows_list = args.rows or [128]
        mm_list = args.mm_cores or [1, 2, 4, 8]
        add_list = args.add_cores or [1, 8]
        repeat = args.repeat or 5
    else:  # 压力测试
        # 必须用同一个确定性随机数生成器抽取不同的内部 M 值。
        # 显式加入 1/255/256，最终应覆盖 30 个唯一 M。
        rows_list = args.rows or sorted(set(random.Random(20261017).sample(range(2, 255), 27)) | {1, 255, 256})
        mm_list = args.mm_cores or [1, 2, 4, 8]
        add_list = args.add_cores or [1, 8]
        repeat = args.repeat or 5

    if args.mode == "stress" and rank == 0:
        print(
            f"STRESS\tphase=coverage_plan\tunique_m={len(set(rows_list))}\t"
            f"mm={len(mm_list)}\tadd={len(add_list)}\tweight_seeds=4\t"
            f"phase1_calls={len(rows_list) * len(mm_list) * len(add_list) * repeat}",
            flush=True,
        )

    # 输入与 HCCL 参考均在建池之前准备。
    xs, refs = {}, {}
    for m in sorted(set(rows_list)):
        torch.manual_seed(1200 + rank * 17 + m)
        x = torch.randn(m, K_LOCAL, device=device, dtype=torch.float16)
        local = F.linear(x, w_plain).float()
        dist.all_reduce(local)
        xs[m] = x
        refs[m] = local.to(torch.float16)
        sync()
    torch.npu.synchronize()
    barrier(cpu_group)

    # Stock 性能基线在建池前测量。
    stock_ms = {}
    for m in sorted(set(rows_list)):
        x = xs[m]
        times = []
        for _ in range(50):
            t0 = time.perf_counter()
            y = F.linear(x, w_plain).float()
            dist.all_reduce(y)
            y = y.to(torch.float16)
            sync()
            times.append((time.perf_counter() - t0) * 1e3)
        stock_ms[m] = statistics.median(times)
    sync()
    barrier(cpu_group)

    global _STRESS_REFS, _STRESS_WNZ
    _STRESS_REFS, _STRESS_WNZ = {}, []
    if args.mode == "stress":
        # 4 组权重种子的 HCCL 参考全部在建池之前生成。
        for s in range(4):
            wp, wn = nz_weight(9000 + 17 * s, device)
            _STRESS_WNZ.append(wn)
            for m in sorted(set(rows_list)):
                local = F.linear(xs[m], wp).float()
                dist.all_reduce(local)
                _STRESS_REFS[(m, s)] = local.to(torch.float16)
            del wp
        sync()
        barrier(cpu_group)
        if rank == 0:
            print(f"STRESS\tphase=pre_refs\tcombos={len(_STRESS_REFS)}", flush=True)

    kfc_bypass_files()
    trial = None
    try:
        trial = Trial(rank, args.chunk_rows)
        barrier(cpu_group)
        trial.ib_probe(1000, stream)
        sync()
        assert trial.check_status() == 0
        if rank == 0:
            print("G0\tib_probe\tPASS", flush=True)

        if args.mode == "stress":
            run_stress(trial, rank, device, cpu_group, stream, xs, refs, rows_list, mm_list, add_list, repeat, w_plain)
        elif args.mode == "yguard":
            run_yguard(trial, rank, cpu_group, stream, xs, refs, rows_list, mm_list, add_list, w_nz, w_plain)
        else:
            run_matrix(trial, rank, cpu_group, stream, xs, refs, rows_list, mm_list, add_list, repeat, stock_ms, w_nz)
        barrier(cpu_group)
    finally:
        if trial is not None:
            sync()
            trial.destroy()
        kfc_bypass_cleanup()
        barrier(cpu_group)
    dist.destroy_process_group()


def run_yguard(trial, rank, cpu_group, stream, xs, refs, rows_list, mm_list, add_list, w_nz, w_plain):
    """在关闭适配层设备同步首写保护时测量新输出缓冲的 MTE3 丢写概率。

    必须设置 MF310P_V2_NOWARMUP=1。
    测试方式：none、stream、fill、fillsync。
    """
    if os.environ.get("MF310P_V2_NOWARMUP") != "1":
        raise RuntimeError("yguard mode requires MF310P_V2_NOWARMUP=1")
    m = 128
    mm, add = 4, 1
    x = xs.get(m)
    ref = refs.get(m)
    if x is None:
        torch.manual_seed(1200 + rank * 17 + m)
        x = torch.randn(m, K_LOCAL, device=w_plain.device, dtype=torch.float16)
        local = F.linear(x, w_plain).float()
        dist.all_reduce(local)
        ref = local.to(torch.float16)
    variants = ("none", "stream", "fill", "fillsync", "delay")
    n_iter = 30
    KEEP = []
    if rank == 0:
        print("GUARD\tvariant\tdrops\twall_ms\tper_call_ms\tdistinct_ptr", flush=True)
    for variant in variants:
        drops = 0
        ptrs = set()
        t_start = time.perf_counter()
        for i in range(n_iter):
            # 必须保持全部输出缓冲存活，确保每次分配的地址真正全新，
            # 不会被缓存分配器回收后复用。
            y = torch.empty(m, N_OUT, dtype=torch.float16, device=x.device)
            ptrs.add(y.data_ptr())
            if variant in ("fill", "fillsync"):
                y.zero_()
            if variant in ("stream", "fillsync"):
                torch.npu.current_stream().synchronize()
            if variant == "delay":
                time.sleep(0.05)
            trial.run_small(x.data_ptr(), w_nz.data_ptr(), y.data_ptr(), m, mm, add, stream)
            sync()
            if trial.check_status() != 0:
                print(f"GUARD\t{variant}\tSTATUS={trial.check_status():#x}", flush=True)
                drops += 1
                continue
            if not torch.equal(y, ref):
                drops += 1
            KEEP.append(y)
        wall = (time.perf_counter() - t_start) * 1e3
        if rank == 0:
            print(f"GUARD\t{variant}\t{drops}/{n_iter}\t{wall:.1f}\t{wall / n_iter:.2f}\t{len(ptrs)}", flush=True)
        barrier(cpu_group)


def run_matrix(trial, rank, cpu_group, stream, xs, refs, rows_list, mm_list, add_list, repeat, stock_ms, w_nz):
    if rank == 0:
        print("M\tMM\tADD\tstock_ms\tv2_med_ms\tv2_first_ms\tv2_p95_ms\tstatus\tbitexact", flush=True)
    n_fail = 0
    for m in sorted(set(rows_list)):
        x = xs[m]
        ref = refs[m]
        for mm in mm_list:
            for add in add_list:
                tag = f"m={m} mm={mm} add={add}"
                out = torch.empty_like(ref)
                times, first = [], 0.0
                ok_all = True
                for k in range(max(repeat, 3)):
                    t0 = time.perf_counter()
                    trial.run_small(x.data_ptr(), w_nz.data_ptr(), out.data_ptr(), m, mm, add, stream)
                    sync()
                    ms = (time.perf_counter() - t0) * 1e3
                    if k == 0:
                        first = ms
                    else:
                        times.append(ms)
                    if trial.check_status() != 0:
                        print(f"STATUS-FAIL\t{tag}\t{trial.check_status():#x}", flush=True)
                        n_fail += 1
                        ok_all = False
                        break
                    if not verify(out, ref, tag, rank):
                        n_fail += 1
                        ok_all = False
                        break
                med = statistics.median(times) if times else float("nan")
                p95 = sorted(times)[int(len(times) * 0.95)] if times else float("nan")
                if rank == 0:
                    print(
                        f"{m}\t{mm}\t{add}\t{stock_ms[m]:.3f}\t{med:.3f}\t"
                        f"{first:.3f}\t{p95:.3f}\t0\t"
                        f"{'PASS' if ok_all else 'FAIL'}",
                        flush=True,
                    )
                if not ok_all:
                    raise AssertionError(f"correctness failed: {tag}")
    if rank == 0:
        print(f"MATRIX {'ALL PASS' if n_fail == 0 else 'FAIL'}", flush=True)


def run_stress(trial, rank, device, cpu_group, stream, xs, refs, rows_list, mm_list, add_list, repeat, w_plain):
    """使用随机 M、4 组权重种子和动态 y 地址，连续验证 1000 次。

    全部正确性参考均在 MemFabric 建池前通过 HCCL 收集；
    refs2 在 main() 的建池前阶段已经生成。
    """
    rng = random.Random(777)  # 两个 Rank 使用一致的随机数序列，确保 TP=2 权重匹配
    # 每个 M 使用 4 个轮换 y 缓冲，保证输出地址各不相同。
    ypool = {m: [torch.empty(m, N_OUT, dtype=torch.float16, device=device) for _ in range(4)] for m in rows_list}

    # 第一阶段：遍历随机 M、全部 MM/Add 核数组合及轮换输出。
    n_calls = 0
    for m in rows_list:
        x = xs[m]
        for mm in mm_list:
            for add in add_list:
                s = rng.randrange(4)
                ref = _STRESS_REFS[(m, s)]
                w_nz = _STRESS_WNZ[s]
                for k in range(repeat):
                    y2 = ypool[m][(n_calls + k) % 4]
                    trial.run_small(x.data_ptr(), w_nz.data_ptr(), y2.data_ptr(), m, mm, add, stream)
                    sync()
                    assert trial.check_status() == 0, f"stress status m={m} mm={mm} add={add}"
                    if not verify(y2, ref, f"stress m={m} mm={mm} add={add}", rank):
                        raise AssertionError("stress bitexact fail")
                    n_calls += 1
    if rank == 0:
        print(f"STRESS\tphase=random_m\tcalls={n_calls}\tPASS", flush=True)

    # 第二阶段：独立抽样 1000 次，两个 Rank 的随机序列一致。
    # 旧版取模索引会使 MM/Add/权重种子仅形成四个相关组合。
    # 采用新的固定种子，使第二阶段结果不依赖第一阶段耗用
    # 的随机数数量。
    seq_rng = random.Random(20261018)
    n_seq = 1000
    observed_m = set()
    observed_mm_add_seed = set()
    for i in range(n_seq):
        m = seq_rng.choice(rows_list)
        mm = seq_rng.choice(mm_list)
        add = seq_rng.choice(add_list)
        s = seq_rng.randrange(4)
        observed_m.add(m)
        observed_mm_add_seed.add((mm, add, s))
        x = xs[m]
        y = ypool[m][i % len(ypool[m])]
        trial.run_small(x.data_ptr(), _STRESS_WNZ[s].data_ptr(), y.data_ptr(), m, mm, add, stream)
        sync()
        assert trial.check_status() == 0, f"seq status i={i}"
        if not verify(y, _STRESS_REFS[(m, s)], f"seq i={i} m={m} mm={mm} add={add}", rank):
            raise AssertionError("seq bitexact fail")

    expected_combo_count = len(mm_list) * len(add_list) * 4
    if len(observed_m) != len(rows_list) or len(observed_mm_add_seed) != expected_combo_count:
        raise AssertionError(
            f"incomplete stress coverage M={len(observed_m)}/{len(rows_list)} "
            f"MM/Add/seed={len(observed_mm_add_seed)}/{expected_combo_count}"
        )
    if rank == 0:
        print(
            f"STRESS\tphase=seq_1000\tcalls={n_seq}\t"
            f"unique_m={len(observed_m)}\tmm_add_seed={len(observed_mm_add_seed)}\tPASS",
            flush=True,
        )


if __name__ == "__main__":
    main()
