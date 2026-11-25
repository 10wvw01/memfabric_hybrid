/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT ANY KIND OF EITHER EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY OR FIT FOR A
 * PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

/* AICore 生产端与 AICPU 消费端共用的 310P SDMA 保留区布局。
 * 请求、游标和完成戳的偏移及 2 MiB 保留区大小保持一致，各写入方独占 64 字节缓存行。
 * 原到达区的一部分存放通知参数，维持内存池的地址布局。
 * 暂存槽在前次 quiet 完成戳可见之前仍是 DMA 源，不能覆盖。
 */
#ifndef __MEMFABRIC_SMEM_SHM_SDMA_LAYOUT_H__
#define __MEMFABRIC_SMEM_SHM_SDMA_LAYOUT_H__

#include <cstdint>

/* host/AICore 双侧可调标记: bisheng 统一编译(__CCE__, host+AICore 双 pass)下
 * __host__ __aicore__ 双属性(设备 kernel 与 host 代码均可调); 纯 g++ 环境
 * (AICPU kernel 交叉编译/host UT/orchestrator)无执行模型, 退化为普通 inline */
#if defined(__CCE__)
#define SMEM_SHM_SDMA_LAYOUT_FN __host__ __aicore__ inline
#else
#define SMEM_SHM_SDMA_LAYOUT_FN static inline
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 段尾保留区(48 套请求/完成/暂存队列 + 48 个到达子环) ---- */
constexpr uint64_t SMEM_SHM_SDMA_RESERVED = 2048UL * 1024UL; /* 保留区总大小(2MB) */
/* 保留区内偏移(见文件头布局图; 跨写者字段按 64B 行分隔——v23 布局纪律) */
constexpr uint64_t SMEM_SHM_SDMA_RS_REQ_RING = 0x0000;   /* 请求环数组基址(AICore 写) */
constexpr uint64_t SMEM_SHM_SDMA_RS_REQ_SLOTS = 64;      /* 每队列请求槽数 */
constexpr uint64_t SMEM_SHM_SDMA_RS_REQ_RING_STRIDE = 0x800; /* 队列间距(64×32B) */
constexpr uint64_t SMEM_SHM_SDMA_RS_REQ_TAIL = 0x18000;  /* 生产游标数组(AICore 写, 各独占行) */
constexpr uint64_t SMEM_SHM_SDMA_RS_REQ_HEAD = 0x18C00;  /* 消费游标数组(AICPU 写, 各独占行) */
constexpr uint64_t SMEM_SHM_SDMA_RS_CURSOR_STRIDE = 0x40; /* 游标间距(独占 64B 行) */
constexpr uint64_t SMEM_SHM_SDMA_RS_QUIET = 0x19800;     /* 完成戳数组基址(SQE 写) */
constexpr uint64_t SMEM_SHM_SDMA_RS_QUIET_SLOTS = 64;    /* 每队列完成戳数 */
constexpr uint64_t SMEM_SHM_SDMA_RS_QUIET_STRIDE = 0x200; /* 队列间距(64×8B) */
/* 暂存槽保持32字节间距：首个uint64_t保存完成序号，第二个保存通知值。 */
constexpr uint64_t SMEM_SHM_SDMA_RS_STAGE = 0x1F800;    /* 邮件像暂存数组基址(AICPU 写本端, SDMA 本端读) */
constexpr uint32_t SMEM_SHM_SDMA_RS_STAGE_SLOTS = 512;   /* 每队列暂存槽数(每封 32B) */
constexpr uint64_t SMEM_SHM_SDMA_RS_STAGE_STRIDE = 0x4000; /* 队列间距(512×32B) */

// 通知参数保存在原到达区，每请求16字节，随请求tail一起发布。
constexpr uint64_t SMEM_SHM_SDMA_RS_SIGNAL = 0xDF800;
constexpr uint64_t SMEM_SHM_SDMA_RS_SIGNAL_STRIDE = 0x400;
constexpr uint64_t SMEM_SHM_SDMA_SIGNAL_FLAG = 1ULL << 39;
constexpr uint64_t SMEM_SHM_SDMA_SIGNAL32_FLAG = 1ULL << 38;
// 初始化后保持不变，与游标及通知参数分开占用缓存行。
constexpr uint64_t SMEM_SHM_SDMA_RS_PRODUCER_MASK = 0xEF800;
static_assert(SMEM_SHM_SDMA_RS_SIGNAL + 48 * SMEM_SHM_SDMA_RS_SIGNAL_STRIDE <=
              SMEM_SHM_SDMA_RS_PRODUCER_MASK, "producer mask overlaps signal parameters");
static_assert(SMEM_SHM_SDMA_RS_PRODUCER_MASK + 64 <= SMEM_SHM_SDMA_RESERVED, "producer mask exceeds reserved area");

/* ---- 协同模式规模上限 ---- */
constexpr uint32_t SMEM_SHM_SDMA_MAX_RANKS = 4;  /* rank 上限(8-rank 全互联被每卡 8 个 P2P 槽位硬约束否定) */
constexpr uint32_t SMEM_SHM_SDMA_MAX_PEERS = SMEM_SHM_SDMA_MAX_RANKS - 1;
constexpr uint32_t SMEM_SHM_SDMA_MAX_BLOCKS = 8; /* 默认 AI Core 生产者数 */
constexpr uint32_t SMEM_SHM_SDMA_AICORE_PRODUCER_MASK = 0x00FF; /* bit 0..7: AI Core */
constexpr uint32_t SMEM_SHM_SDMA_VECTOR_PRODUCER_MASK = 0xFF00; /* bit 8..15: Vector */
constexpr uint32_t SMEM_SHM_SDMA_RESERVED_PRODUCER_MASK = 0x8000; /* bit 15 保留，禁止提交 */
constexpr uint32_t SMEM_SHM_SDMA_DEFAULT_PRODUCER_MASK = SMEM_SHM_SDMA_AICORE_PRODUCER_MASK;
constexpr uint32_t SMEM_SHM_SDMA_VALID_PRODUCER_MASK =
    (SMEM_SHM_SDMA_AICORE_PRODUCER_MASK | SMEM_SHM_SDMA_VECTOR_PRODUCER_MASK) &
    ~SMEM_SHM_SDMA_RESERVED_PRODUCER_MASK;
constexpr uint32_t SMEM_SHM_SDMA_QUEUES_PER_PEER = 16; /* 每对端队列容量，block 15 保留 */
constexpr uint32_t SMEM_SHM_SDMA_QUEUE_COUNT = 48; /* 邮箱队列总数(对端 × 槽位, hcomm 48 同规模) */

/* 原到达区偏移保留用于布局兼容；通知参数使用其起始部分。 */
constexpr uint64_t SMEM_SHM_SDMA_RS_PEER_RINGS = 0xDF800; /* 到达子环数组基址 */
constexpr uint64_t SMEM_SHM_SDMA_RS_PEER_STAMP = 0x0000; /* 队列内: 到达戳数组(SQE 写) */
constexpr uint64_t SMEM_SHM_SDMA_RS_PEER_MAIL = 0x1000;   /* 队列内: 到达邮件数组(SQE 写) */
constexpr uint64_t SMEM_SHM_SDMA_RS_PEER_HEAD = 0x5000;   /* 队列内: 到达消费游标(AICore 写, 独占行) */
constexpr uint64_t SMEM_SHM_SDMA_RS_PEER_SLOTS = 512;     /* 每队列到达深度(戳与邮件同深) */
constexpr uint64_t SMEM_SHM_SDMA_RS_PEER_STRIDE = 0x5040; /* 队列间距(戳 0x1000+邮件 0x4000+游标行 0x40) */

/* 对端 rank → 本端对端环下标(收发双方共用; peerRank 须 != ownRank 且 < rankSize)。
 * 去自身压缩: 小于本端的对端原样, 大于等于本端的对端前移一位, 恒 < MAX_PEERS。 */
SMEM_SHM_SDMA_LAYOUT_FN uint32_t SmemShmSdmaPeerRingIdx(uint32_t ownRank, uint32_t peerRank)
{
    return (peerRank < ownRank) ? peerRank : (peerRank - 1U);
}

/* 反演: 对端环下标 → 对端 rank(signal 侧从请求队列反推目的/接收方 wait 定位来源) */
SMEM_SHM_SDMA_LAYOUT_FN uint32_t SmemShmSdmaPeerRankOf(uint32_t ownRank, uint32_t ringIdx)
{
    return (ringIdx < ownRank) ? ringIdx : (ringIdx + 1U);
}

SMEM_SHM_SDMA_LAYOUT_FN bool SmemShmSdmaProducerMaskValid(uint32_t mask)
{
    return mask != 0 && (mask & ~SMEM_SHM_SDMA_VALID_PRODUCER_MASK) == 0;
}

SMEM_SHM_SDMA_LAYOUT_FN bool SmemShmSdmaProducerAllowed(uint32_t mask, uint32_t block)
{
    return block < SMEM_SHM_SDMA_QUEUES_PER_PEER &&
           (mask & SMEM_SHM_SDMA_VALID_PRODUCER_MASK & (1U << block)) != 0;
}

/* (对端环下标, block 下标) → 队列下标: 对端环下标 × 每对端队列数(16) + block。
 * block 0..14 的提交权限由初始化掩码决定。 */
SMEM_SHM_SDMA_LAYOUT_FN uint32_t SmemShmSdmaQueueIdx(uint32_t peerRingIdx, uint32_t blockIdx)
{
    return peerRingIdx * SMEM_SHM_SDMA_QUEUES_PER_PEER + blockIdx;
}

/* 队列下标 → block 下标(epoch kernel 消费侧分解: 该邮件由哪个 AICore block 发起) */
SMEM_SHM_SDMA_LAYOUT_FN uint32_t SmemShmSdmaQueueBlock(uint32_t queueIdx)
{
    return queueIdx % SMEM_SHM_SDMA_QUEUES_PER_PEER;
}

/* 队列下标 → 对端环下标(消费侧分解: 该队列的目的对端) */
SMEM_SHM_SDMA_LAYOUT_FN uint32_t SmemShmSdmaQueuePeerRing(uint32_t queueIdx)
{
    return queueIdx / SMEM_SHM_SDMA_QUEUES_PER_PEER;
}

#ifdef __cplusplus
}
#endif

#endif // __MEMFABRIC_SMEM_SHM_SDMA_LAYOUT_H__
