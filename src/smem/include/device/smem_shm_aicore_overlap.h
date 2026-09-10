/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

/* smem_shm_aicore_overlap.h — 310P AICore×AICPU 协同模式设备侧原语(AICore kernel 用)
 *
 * 机制(310p-aicore-aicpu-开发进展.md D5/D6 + E0c 实测定论):
 *   AICore 产数据 → GM 邮箱(16B {dataAddr, words}, vector 直写 + 行 clean)委托
 *   AICPU 编排 kernel 代投 SDMA SQE 对(数据 + 8B 标志, FIFO 保序保证标志到即数据到)
 *   → 对端 AICore dcci 轮询池内标志后消费。
 *
 * 工作区布局(与 hybm_overlap_prep.h 的 HYBM_OVERLAP_WS_* 常量一致):
 *   +0      mailbox ring(16B/slot × 64)
 *   +1024   哨兵 8B(SQE 对的标志源, host 侧 smem_shm_overlap_launch 预填)
 *   +1032   kernel result 64B
 *   +1096   SQE overflow 目标 64B
 *
 * 池内标志约定: 对称池每 rank 段的末尾 SMEM_SHM_OVERLAP_FLAG_REGION_SIZE(2MB)
 * 为协同保留区(用户数据请勿占用), 到达/结果标志在保留区内按 8B 槽位排布。
 *
 * 约束: 本头文件与 smem_shm_aicore_base_define.h 同风格, 供 bisheng 编译的
 * AICore kernel include; 依赖 AscendC 的 DataCacheCleanAndInvalid(dcci)。
 */
#ifndef __MEMFABRIC_SMEM_AI_CORE_OVERLAP_H__
#define __MEMFABRIC_SMEM_AI_CORE_OVERLAP_H__

#include "smem_shm_aicore_base_define.h"
#include "smem_shm_aicore_base_meta.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- meta 扩展字段偏移(与 host 侧 HybmDeviceMeta.overlapWorkspace/overlapFlags 对应) ---- */
/* overlapWorkspace @ reserved[0](meta 偏移 40), overlapFlags @ reserved[1](偏移 48) */
constexpr uint64_t SMEM_SHM_DEVICE_META_OVERLAP_WORKSPACE_OFFSET =
    SMEM_SHM_DEVICE_META_SDMA_WORKSPACE_OFFSET + sizeof(uint64_t);
constexpr uint64_t SMEM_SHM_DEVICE_META_OVERLAP_FLAGS_OFFSET =
    SMEM_SHM_DEVICE_META_OVERLAP_WORKSPACE_OFFSET + sizeof(uint64_t);

/* overlapFlags 能力位(与 hybm_define.h HYBM_OVERLAP_FLAG_READY 同值) */
constexpr uint64_t SMEM_SHM_OVERLAP_FLAG_READY = 0x1UL;

/* ---- 工作区布局常量(与 hybm_overlap_prep.h 一致) ---- */
constexpr uint64_t SMEM_SHM_OVERLAP_WS_MAILBOX_OFF = 0;        /* 邮箱 ring 基址偏移 */
constexpr uint64_t SMEM_SHM_OVERLAP_WS_MAILBOX_SLOT_SIZE = 16; /* 16B/slot {dataAddr, words} */
constexpr uint64_t SMEM_SHM_OVERLAP_WS_SENTINEL_OFF = 1024;    /* 哨兵 8B */

/* ---- 池内协同保留区 ----
 * 重要: 位置在本 rank 段的"物理映射区"末尾(而非 symmetricSize 末尾 —— 对称段大小
 * 经 1G 对齐, 尾部可能未映射物理页, SDMA/AICore 访问会失败)。
 * physSize = 本 rank 段实际物理大小(即 smem_shm_create 传入的 localSize),
 * 由 host 侧算好地址传入 kernel 更稳妥(推荐), 本助手供 kernel 侧自算场景。 */
constexpr uint64_t SMEM_SHM_OVERLAP_FLAG_REGION_SIZE = 2UL * 1024UL * 1024UL; /* 保留区 2MB */
constexpr uint64_t SMEM_SHM_OVERLAP_FLAG_ARRIVAL_IDX = 0;                     /* 到达标志槽序号 */
constexpr uint64_t SMEM_SHM_OVERLAP_FLAG_RESULT_IDX = 1;                      /* 结果标志槽序号 */
constexpr uint64_t SMEM_SHM_OVERLAP_FLAG_SLOT_SIZE = 8;                       /* 每槽 8B */

/* 轮询迭代上限(纯自旋 dcci, E0c 实测 ~139 万次完成八步往返; 上限给出充分裕量) */
constexpr uint64_t SMEM_SHM_OVERLAP_POLL_MAX_ITER = 100000000ULL;

#ifdef __cplusplus
}
#endif

/* 查询本 shm 的协同工作区地址(mailbox/哨兵所在, 普通设备内存, AICore 可直接 GM 访问) */
SMEM_SHM_INLINE_AICORE __gm__ void *smem_shm_overlap_get_workspace(uint32_t shmemId)
{
    if (shmemId >= SMEM_OBJECT_NUM_MAX) {
        return NULL;
    }
    uint64_t metaAddr =
        SMEM_SHM_DEVICE_META_ADDR + SMEM_SHM_DEVICE_GLOBAL_META_SIZE + shmemId * SMEM_SHM_DEVICE_PRE_META_SIZE;
    return *(__gm__ void **)(metaAddr + SMEM_SHM_DEVICE_META_OVERLAP_WORKSPACE_OFFSET);
}

/* 查询协同模式是否就绪(310P + 开关 + 冒烟通过; 未就绪时上层不应使用本组原语) */
SMEM_SHM_INLINE_AICORE bool smem_shm_overlap_ready(uint32_t shmemId)
{
    if (shmemId >= SMEM_OBJECT_NUM_MAX) {
        return false;
    }
    uint64_t metaAddr =
        SMEM_SHM_DEVICE_META_ADDR + SMEM_SHM_DEVICE_GLOBAL_META_SIZE + shmemId * SMEM_SHM_DEVICE_PRE_META_SIZE;
    uint64_t flags = *(__gm__ uint64_t *)(metaAddr + SMEM_SHM_DEVICE_META_OVERLAP_FLAGS_OFFSET);
    return (flags & SMEM_SHM_OVERLAP_FLAG_READY) != 0;
}

/* 计算本 rank 池段内协同保留区标志地址
 * (segBase = 本 rank 段基址(gva + symmetricSize*rankId), physSize = 段物理大小, idx = 槽序号) */
SMEM_SHM_INLINE_AICORE __gm__ void *smem_shm_overlap_get_flag_addr(__gm__ void *segBase, uint64_t physSize,
                                                                   uint64_t idx)
{
    uint64_t base = reinterpret_cast<uint64_t>(segBase) + physSize - SMEM_SHM_OVERLAP_FLAG_REGION_SIZE;
    return reinterpret_cast<__gm__ void *>(base + idx * SMEM_SHM_OVERLAP_FLAG_SLOT_SIZE);
}

/* 邮箱通知: AICore → AICPU(写 16B 描述符 + 单行 clean; AICore→AICPU 可见性天然一致) */
SMEM_SHM_INLINE_AICORE void smem_shm_overlap_notify(__gm__ void *mailboxSlot, uint64_t dataAddr, uint64_t dataWords)
{
    __gm__ uint64_t *slot = reinterpret_cast<__gm__ uint64_t *>(mailboxSlot);
    slot[0] = dataAddr;
    slot[1] = dataWords;
    AscendC::GlobalTensor<int32_t> mbGm;
    mbGm.SetGlobalBuffer(reinterpret_cast<__gm__ int32_t *>(slot), 4);
    AscendC::DataCacheCleanAndInvalid<int32_t, AscendC::CacheLine::SINGLE_CACHE_LINE>(mbGm);
}

/* 标志轮询: dcci + volatile 读(AICPU/SDMA 写 → AICore 读的可见性正解, E0c 实测),
 * 命中返回非零标志值, 超时返回 0; iters 出参为迭代数(时延近似度量, 可传 NULL) */
SMEM_SHM_INLINE_AICORE uint64_t smem_shm_overlap_poll_flag(__gm__ void *flagAddr, uint64_t *iters)
{
    __gm__ volatile uint64_t *f = reinterpret_cast<__gm__ volatile uint64_t *>(flagAddr);
    AscendC::GlobalTensor<int32_t> fg;
    fg.SetGlobalBuffer(reinterpret_cast<__gm__ int32_t *>(flagAddr), 2);
    for (uint64_t i = 0; i < SMEM_SHM_OVERLAP_POLL_MAX_ITER; i++) {
        AscendC::DataCacheCleanAndInvalid<int32_t, AscendC::CacheLine::SINGLE_CACHE_LINE>(fg);
        uint64_t v = *f;
        if (v != 0) {
            if (iters != NULL) {
                *iters = i;
            }
            return v;
        }
    }
    if (iters != NULL) {
        *iters = SMEM_SHM_OVERLAP_POLL_MAX_ITER;
    }
    return 0;
}

#endif // __MEMFABRIC_SMEM_AI_CORE_OVERLAP_H__
