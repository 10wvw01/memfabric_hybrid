# SPDX-License-Identifier: Apache-2.0
"""验证完整 B 路径（F.linear 输出→暂存→AR Kernel）在输入
每轮变化时是否错误读取过时的暂存数据。

建池前：为固定 M 生成 N 组输入及 HCCL 参考。
建池后：轮换输入，复用已预热的 y 与局部结果内存地址，
逐次进行位精确校验。由于每轮输入不同，只要暂存误读
上轮局部结果便会立刻产生校验失败。
"""

from __future__ import annotations

import argparse
import atexit
import contextlib
import ctypes
import os
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
    sigs = {
        "mf310p_v2_trial_create": ([ctypes.c_int, ctypes.c_char_p, U32, ctypes.POINTER(P)], ctypes.c_int),
        "mf310p_v2_trial_ib_probe": ([P, U32, P], ctypes.c_int),
        "mf310p_v2_trial_run_ar_only": ([P, U64, U64, U32, U32, P], ctypes.c_int),
        "mf310p_v2_trial_check_status": ([P, ctypes.POINTER(U64)], ctypes.c_int),
        "mf310p_v2_trial_read_send_arena": ([P, ctypes.c_void_p, ctypes.c_uint32], ctypes.c_int),
        "mf310p_v2_trial_destroy": ([P], ctypes.c_int),
    }
    for name, (args, result) in sigs.items():
        fn = getattr(lib, name)
        fn.argtypes, fn.restype = args, result
    return lib


def optional_test_kfc_bypass():
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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--m", type=int, default=64)
    ap.add_argument("--add", type=int, default=8)
    ap.add_argument("--variants", type=int, default=64)
    ap.add_argument("--cycles", type=int, default=5)
    a = ap.parse_args()

    rank = int(os.environ["RANK"])
    local_rank = int(os.environ["LOCAL_RANK"])
    torch.npu.set_device(local_rank)
    device = torch.device(f"npu:{local_rank}")
    dist.init_process_group(backend="hccl")
    cpu_group = dist.new_group(backend="gloo")
    stream = torch.npu.current_stream()
    raw_stream = ctypes.c_void_p(stream.npu_stream)

    torch.manual_seed(7865 + rank)
    weight = maybe_trans_nz(torch.randn(2048, 2048, dtype=torch.float16, device=device))

    xs, refs = [], []
    for v in range(a.variants):
        torch.manual_seed(9000 + rank * 13 + v)
        x = torch.randn(a.m, 2048, device=device, dtype=torch.float16)
        local = F.linear(x, weight).float()
        dist.all_reduce(local)
        xs.append(x)
        refs.append(local.to(torch.float16))
    y = torch.empty_like(refs[0])
    # 创建确定性本端局部结果，并收集对端结果以定位错误来源。
    owns = [F.linear(x, weight) for x in xs]
    peer_all = [None] * dist.get_world_size()
    dist.all_gather_object(peer_all, [o.cpu() for o in owns], group=cpu_group)
    peer_parts = peer_all[1 - rank]
    peer_parts = [p.to(device) for p in peer_parts]
    torch.npu.synchronize()
    dist.barrier(group=cpu_group)
    if rank == 0:
        print(f"pre-pool refs ready: {a.variants} variants, M={a.m}", flush=True)

    optional_test_kfc_bypass()
    lib = bind_library()
    handle = ctypes.c_void_p()
    assert lib.mf310p_v2_trial_create(rank, b"tcp://127.0.0.1:8581", 1024, ctypes.byref(handle)) == 0
    assert lib.mf310p_v2_trial_ib_probe(handle, 100, raw_stream) == 0

    status = ctypes.c_uint64(0)
    fails = 0
    total = 0
    t0 = time.perf_counter()
    for cycle in range(a.cycles):
        for v in range(a.variants):
            x, ref = xs[v], refs[v]
            partial = F.linear(x, weight)
            if os.environ.get("T2_SYNC_BEFORE_AR") == "1":
                stream.synchronize()  # 确保 MM 输出对 DDR/L2 读取方可见
            rc = lib.mf310p_v2_trial_run_ar_only(handle, partial.data_ptr(), y.data_ptr(), a.m, a.add, raw_stream)
            assert rc == 0
            stream.synchronize()
            del partial
            rc = lib.mf310p_v2_trial_check_status(handle, ctypes.byref(status))
            assert rc == 0 and status.value == 0
            total += 1
            if not torch.equal(y, ref):
                fails += 1
                prev_v = (v - 1) % a.variants
                stale_prev = torch.equal(y, refs[prev_v])
                bad = int((y != ref).sum().item())
                mask = (y != ref).flatten()
                idx = mask.nonzero().flatten()
                runs = []
                if idx.numel():
                    start = prev = int(idx[0])
                    for j in idx[1:].tolist():
                        if j != prev + 1:
                            runs.append((start, prev - start + 1))
                            start = j
                        prev = j
                    runs.append((start, prev - start + 1))
                runs = runs[:4]
                # 按不同调用代际对错误片段分类，判断
                # 哪一轮数据可以解释实际读到的错误字节。
                cls = []
                flat_y = y.flatten()
                for rs, rl in runs:
                    seg = slice(rs, rs + rl)
                    tags = []
                    for vv in range(a.variants):
                        if torch.equal(flat_y[seg], refs[vv].flatten()[seg]):
                            tags.append(f"ref=v{vv}(v-{(v - vv) % a.variants})")
                    for vv in range(a.variants):
                        for ww in range(a.variants):
                            if vv != v or ww != v:
                                mixed = (owns[vv].float() + peer_parts[ww].float()).half()
                                if torch.equal(flat_y[seg], mixed.flatten()[seg]):
                                    tags.append(f"own=v{vv}+peer=v{ww}")
                    if not tags:
                        o = owns[v].flatten()[rs : rs + 8]
                        pe = peer_parts[v].flatten()[rs : rs + 8]
                        rf = refs[v].flatten()[rs : rs + 8]
                        yy = flat_y[rs : rs + 8]
                        d_yp = yy.float() - pe.float()
                        d_yo = yy.float() - o.float()
                        tags.append(
                            f"novint y={yy.tolist()} own={o.tolist()} "
                            f"peer={pe.tolist()} ref={rf.tolist()} "
                            f"y-peer={d_yp.tolist()} y-own={d_yo.tolist()}"
                        )
                    cls.append((rs, rl, tags))
                print(f"[rank{rank}] FAIL cycle={cycle} v={v} bad={bad} stale_prev={stale_prev} cls={cls}", flush=True)
    dt = time.perf_counter() - t0
    print(
        f"[rank{rank}] RESULT: {fails}/{total} failed, elapsed={dt:.1f}s ({dt / total * 1e3:.2f} ms/call)", flush=True
    )

    dist.barrier(group=cpu_group)
    stream.synchronize()
    lib.mf310p_v2_trial_destroy(handle)
    dist.barrier(group=cpu_group)
    dist.destroy_process_group()


if __name__ == "__main__":
    main()
