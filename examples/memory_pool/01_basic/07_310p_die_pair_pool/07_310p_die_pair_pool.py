#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# Copyright: (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
"""310P Duo 单卡双 die 2-rank HBM+DRAM 双池示例(Python 版, 需 memfabric_hybrid wheel)

与 06/03 号示例不同点:
- 仅适用于 Atlas 300I Duo(Ascend310P3), HBM 池走 310P 专用 ACL VMM 链路
  (MallocPhysical + shareable handle + die 对 SetContext 导入 + aclrtMemcpyAsync D2D)
- world_size=2 固定映射 dev0/dev1(同一张卡的两个 die)
- 双池: HBM 池(SDMA op 位) + DRAM 池(HOST_SHM), 两个独立 entity
"""

import os
import multiprocessing as mp

import torch

import memfabric_hybrid as mf
from memfabric_hybrid import bm

STORE_URL = "tcp://127.0.0.1:8576"
WORLD_SIZE = 2
RANK_DEV = {0: 0, 1: 1}  # rank0=dev0, rank1=dev1(同卡 die 对)
HBM_POOL_SIZE = 32 * 1024 * 1024
DRAM_POOL_SIZE = 32 * 1024 * 1024
TEST_LEN = 8 * 1024 * 1024
JOIN_TIMEOUT = 120  # 秒, 对齐 BmConfig 默认 init_timeout
TAG_HBM = 0x5A5AA5A500000000
TAG_DRAM = 0xA55A5AA500000000


def _rank_main(rank: int, joined: mp.Barrier, written: mp.Barrier):
    dev = RANK_DEV[rank]
    mf.initialize()
    cfg = bm.BmConfig()
    cfg.auto_ranking = False
    cfg.rank_id = rank
    cfg.start_store = rank == 0
    bm.initialize(STORE_URL, WORLD_SIZE, dev, cfg)

    # HBM 池: SDMA op 位 -> 310P 上自动路由到 AclVmm 段 + aclrtMemcpy 算子
    hbm = bm.create2(
        id=0,
        local_dram_size=0,
        max_dram_size=0,
        local_hbm_size=HBM_POOL_SIZE,
        max_hbm_size=HBM_POOL_SIZE,
        data_op_type=bm.BmDataOpType.SDMA,
    )
    assert hbm.join() == 0, "HBM join failed"
    own_hbm = hbm.peer_rank_ptr(rank, bm.BmMemType.DEVICE)
    peer_hbm = hbm.peer_rank_ptr(1 - rank, bm.BmMemType.DEVICE)

    # DRAM 池: HOST_SHM(同节点共享内存, 纯 POSIX 链路)
    dram = bm.create2(
        id=1,
        local_dram_size=DRAM_POOL_SIZE,
        max_dram_size=DRAM_POOL_SIZE,
        local_hbm_size=0,
        max_hbm_size=0,
        data_op_type=bm.BmDataOpType.HOST_SHM,
    )
    assert dram.join() == 0, "DRAM join failed"
    own_dram = dram.peer_rank_ptr(rank, bm.BmMemType.HOST)
    peer_dram = dram.peer_rank_ptr(1 - rank, bm.BmMemType.HOST)

    # join 为动态成员语义: 先 join 者需等后 join 者完成 join 且增量导入生效后,
    # 对端池地址才在本端 allocatedMap_ 中(否则 copy_data 方向校验失败)
    joined.wait(timeout=JOIN_TIMEOUT)
    _phase_check(hbm, own_hbm, peer_hbm, TAG_HBM, rank, written, "HBM")
    _phase_check(dram, own_dram, peer_dram, TAG_DRAM, rank, written, "DRAM")

    dram.destroy()
    hbm.destroy()
    bm.uninitialize(dev)
    mf.uninitialize()
    print(f"[rank{rank}] 07_310p_die_pair_pool phases ok")


def _phase_check(handle, own_gva, peer_gva, tag, rank, written, label):
    """跨 rank 写对端 + 对端写回本校验(H2G -> 对端 G2H 视角)

    copy_data 的 src/dst 均为整型地址(池 GVA 或 host 指针), host 侧缓冲
    用 torch tensor 提供连续内存并取 data_ptr()(对齐 01 号示例惯例)。
    """
    n_words = TEST_LEN // 8
    src = torch.arange(n_words, dtype=torch.int64) + (tag + rank * 0x10000)
    dst = torch.empty(n_words, dtype=torch.int64)
    h2g_type = bm.BmCopyType.H2G if label == "HBM" else bm.BmCopyType.H2GH
    g2h_type = bm.BmCopyType.G2H if label == "HBM" else bm.BmCopyType.GH2H
    # 写对端池内存
    assert handle.copy_data(src.data_ptr(), peer_gva, TEST_LEN, h2g_type, 0) == 0, f"{label} H2G failed"
    written.wait(timeout=JOIN_TIMEOUT)  # 双方都写完
    # 读本校验(对端写入的 pattern)
    assert handle.copy_data(own_gva, dst.data_ptr(), TEST_LEN, g2h_type, 0) == 0, f"{label} G2H failed"
    expect = torch.arange(n_words, dtype=torch.int64) + (tag + (1 - rank) * 0x10000)
    mismatch = int((dst != expect).sum())
    if mismatch != 0:
        raise RuntimeError(f"{label} pool cross check mismatch={mismatch}")


def main():
    ctx = mp.get_context("spawn")
    # Barrier 须由父进程创建并经 Process args 传递, 在子进程内各自创建不是共享对象
    joined = ctx.Barrier(WORLD_SIZE)
    written = ctx.Barrier(WORLD_SIZE)
    procs = [ctx.Process(target=_rank_main, args=(r, joined, written)) for r in range(WORLD_SIZE)]
    for p in procs:
        p.start()
    for p in procs:
        p.join()
    assert all(p.exitcode == 0 for p in procs), "rank process failed"
    print("07_310p_die_pair_pool ok")


if __name__ == "__main__":
    os.environ.setdefault("ASCEND_MF_STORE_URL", STORE_URL)
    main()
