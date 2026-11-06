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

/* smem_shm_sdma_layout.h — 310P SDMA 协同段尾保留区布局(单一事实源)
 *
 * 本头为纯常量与纯函数声明(不含 AscendC 原语), 供三端共同 include 消除双头镜像漂移:
 *   - 设备侧原语头 smem_shm_aicore_base_sdma.h(用户 AICore kernel)
 *   - hybm wire 契约头 hybm_sdma_contract.h(host↔epoch kernel)
 *   - host UT(布局不变式交叉校验)
 *
 * 保留区位于每 rank 段尾(段基 + physSize - SMEM_SHM_SDMA_RESERVED), host 建池时
 * 初始化清零。**布局纪律(v23, 并发丢件挂死（507014） 修复): 不同写者的字段严禁共用 64B
 * 缓存行**——AICore/AICPU 双处理器均为软件维护缓存(dcci/cvac), 共行字段的回写
 * 会以本核陈旧行内容覆盖对端新写内容(实测: 游标与邮件共行时并发突发下内容被
 * 确定性清零——SQE 完好执行但到达戳源为零, 邮件无声丢失)。各数组/游标
 * 按 64B 边界对齐分隔; 多 AICore block 并发写同一数组区时各自槽位/游标亦独占行。
 *
 * 变更史(按版本):
 * - v29: 请求环与到达环按对端划分(多发送方槽位互相覆盖 + 序号空洞修复);
 * - v30: 邮件像经本端暂存由 SQE ⓪ 投递(AICPU 对对端零直写——跨卡直写无通路);
 * - v31: 环按对端×AICore block 扩展为多队列(每对端 8 个 AICore block 各有
 *   独立收发队列, hcomm 多队列同族设计)——多 AICore block 并发 signal/wait 各走
 *   各队, SPSC 与换轮重消费幂等性逐队列保持; 到达子环深度 1024→512
 *   (在途上界: 请求环未消费 64 + SQ 在途 256 < 512)。
 * - v32: 队列数对齐 hcomm 规模——每对端 16 个队列槽位(block 0..7 在用,
 *   8..15 预留冗余), 3 对端 × 16 = 48 队列(hcomm MAX_QUE_NUM=48 同规模;
 *   队列长度 64 槽/队列亦对齐 hcomm HCCL_MSG_CNT=64)。
 * 布局图(本端区域写者均为本端, 与对端无关):
 *   +0x00000  请求环 × 48, 每队列 64 槽 × 32B(本端 AICore 写, 本端 AICPU 读)
 *   +0x18000  reqTail × 48 × u64(每队列生产游标, AICore 写)[各独占行]
 *   +0x18C00  reqHead × 48 × u64(每队列消费游标, AICPU 写)[各独占行]
 *   +0x19800  完成戳 × 48, 每队列 64 × u64(SQE 写, AICore quiet 轮询)
 *   +0x1F800  邮件像暂存 × 48, 每队列 512 × 32B(AICPU 写本端, 本端 SDMA 读)[⓪②③源]
 *   +0xDF800  到达子环 × 48, 每队列: 戳 512 × u64 + 邮件 512 × 32B + 游标[独占行]
 * 队列下标 = 对端环下标 × 每对端队列数(16) + block 下标(收发双方同一编码)。
 * 注意: 戳值 1 基(序号+1), 0 保留为"空"——序号 0 的邮件与从未写入的清零槽
 * 读出的值相同, 从 1 起才能区分。
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
/* 邮件像暂存: 每队列 512 槽 × 32B。每封一个 32B 槽 = 邮件像 [序号+1][dst][len][imm]——
 * ⓪ 邮件(32B)、② 到达戳(8B, 源=槽首即序号+1)、③ 完成戳(8B, 同源)三支 SQE 的共同
 * 拷贝源(v25: 跨芯片写在途读回旧值会无声失效, 源一律本端)。
 * 单队列在途上界 = SQ 环深 1024 SQE ÷ 4(封) = 256 < 512, 暂存槽不会被提前复用。 */
constexpr uint64_t SMEM_SHM_SDMA_RS_STAGE = 0x1F800;    /* 邮件像暂存数组基址(AICPU 写本端, SDMA 本端读) */
constexpr uint32_t SMEM_SHM_SDMA_RS_STAGE_SLOTS = 512;   /* 每队列暂存槽数(每封 32B) */
constexpr uint64_t SMEM_SHM_SDMA_RS_STAGE_STRIDE = 0x4000; /* 队列间距(512×32B) */

/* ---- 协同模式规模上限 ---- */
constexpr uint32_t SMEM_SHM_SDMA_MAX_RANKS = 4;  /* rank 上限(8-rank 全互联被每卡 8 个 P2P 槽位硬约束否定) */
constexpr uint32_t SMEM_SHM_SDMA_MAX_PEERS = SMEM_SHM_SDMA_MAX_RANKS - 1;
constexpr uint32_t SMEM_SHM_SDMA_MAX_BLOCKS = 8; /* 每 die AICore block 数(<<<N>>> 启动上限) */
constexpr uint32_t SMEM_SHM_SDMA_QUEUES_PER_PEER = 16; /* 每对端队列槽位(block 0..7 在用, 8..15 预留) */
constexpr uint32_t SMEM_SHM_SDMA_QUEUE_COUNT = 48; /* 邮箱队列总数(对端 × 槽位, hcomm 48 同规模) */

/* ---- 到达子环区(v31: 每对端 × block 一队列, 单一远端写者; v32: 48 队列) ---- */
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

/* (对端环下标, block 下标) → 队列下标: 对端环下标 × 每对端队列数(16) + block。
 * 16 为 2 的幂, 乘除编译为移位/与(AICore 侧无除法开销); 预留槽位(8..15)无邮件,
 * AICPU 轮询恒空扫, 无路由副作用。 */
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
