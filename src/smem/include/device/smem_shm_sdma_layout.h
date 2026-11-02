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
 * 会以本核陈旧行内容覆盖对端新写内容(实测: ARR_HEAD 与 ARR_MAIL[1023] 共行时,
 * 双 rank 并发突发下邮件内容被确定性清零——SQE 完好执行但到达戳源为零, 邮件无声
 * 丢失; 旧布局 [0x800) 行更是 REQ_TAIL/REQ_HEAD/QUIET 三写者混行)。各数组/游标
 * 按 64B 边界对齐分隔。
 *
 * v29 4-rank 化(三处协议级变更):
 * 1. 请求环与到达环均**按对端划分**(每对端一套环, 各自独立序号)——旧布局请求序号
 *    跨全部目的地共享, 多发送方写同一到达环槽位互相覆盖, 且发往不同对端的邮件在对端
 *    环上序号有空洞, 接收方 head+1 连续性期望失配(仅支持 2-rank)。按对端划分后
 *    槽位/戳值/暂存值均为环序号的纯函数——逐环完整继承 2-rank 已验证的 SPSC 语义
 *    与换轮重消费幂等性(epoch 错误重拉重写同槽同值)。
 * 2. 邮件像经本端暂存由 SDMA SQE 投递(⓪), AICPU 对对端内存**零直写**——AICPU 跨卡
 *    直写实测不返回(EX09 4-rank 实机: epoch 停留在首封跨卡邮件的写入上, 设备侧 CPU 核的
 *    跨卡 store 无通路; hcomm 生产配方同以 SDMA 承载全部跨卡写), 同卡一并统一走
 *    SDMA, 消除 AICPU 的全部跨端直写路径。
 * 布局图(本端区域写者均为本端 AICore/AICPU/本端 SQE, 与对端无关):
 *   +0x0000  请求环 × 3, 每环 64 槽 × 32B(本端 AICore 写, 本端 AICPU 读)
 *   +0x1800  reqTail × 3 × u64(每环生产游标, AICore 写)[各独占行]
 *   +0x18C0  reqHead × 3 × u64(每环消费游标, AICPU 写, AICore 背压读)[各独占行]
 *   +0x1980  完成戳 × 3, 每环 64 × u64(SQE 写, AICore quiet 轮询)
 *   +0x1F80  邮件像暂存 × 3, 每区 512 × 32B(AICPU 写本端, 本端 SDMA 读)[⓪②③拷贝源]
 *   +0xDF80  对端到达环 × 3, 每环 0xA040:
 *              +0x0000 到达戳 1024 × u64(对端 SQE 写, 本端 AICore wait 轮询)
 *              +0x2000 到达邮件 1024 × 32B(对端 SQE 写, 本端 AICore 读)
 *              +0xA000 arrHead u64(到达消费游标, 本端 AICore 写)[独占行]
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

/* ---- 段尾保留区(用户段几乎全量归还; 含 3 套请求环/完成戳/暂存 + 3 个到达环) ---- */
constexpr uint64_t SMEM_SHM_SDMA_RESERVED = 192UL * 1024UL; /* 保留区总大小 */
/* 保留区内偏移(见文件头布局图; 跨写者字段按 64B 行分隔——v23 布局纪律) */
constexpr uint64_t SMEM_SHM_SDMA_RS_REQ_RING = 0x0000;   /* 请求环数组基址(AICore 写) */
constexpr uint64_t SMEM_SHM_SDMA_RS_REQ_SLOTS = 64;      /* 每环请求槽数 */
constexpr uint64_t SMEM_SHM_SDMA_RS_REQ_RING_STRIDE = 0x800; /* 环间距(64×32B) */
constexpr uint64_t SMEM_SHM_SDMA_RS_REQ_TAIL = 0x1800;   /* 生产游标数组(AICore 写, 各独占行) */
constexpr uint64_t SMEM_SHM_SDMA_RS_REQ_HEAD = 0x18C0;   /* 消费游标数组(AICPU 写, 各独占行) */
constexpr uint64_t SMEM_SHM_SDMA_RS_CURSOR_STRIDE = 0x40; /* 游标间距(独占 64B 行) */
constexpr uint64_t SMEM_SHM_SDMA_RS_QUIET = 0x1980;      /* 完成戳数组基址(SQE 写) */
constexpr uint64_t SMEM_SHM_SDMA_RS_QUIET_SLOTS = 64;    /* 每环完成戳数 */
constexpr uint64_t SMEM_SHM_SDMA_RS_QUIET_STRIDE = 0x200; /* 环间距(64×8B) */
/* v25(并发丢件挂死（507014） 残余竞态修复): 戳 SQE 的拷贝数据源一律本端暂存——原设计复用对端
 * "到达邮件"首字段为源, AICPU 跨芯片写面单后同 die 的 SDMA 立即跨芯片读回自己刚写的值,
 * 写在途时读到上一圈旧值(实测: ② 抄旧值盖旧值无声失效; 双向同窗各丢一封, ~2 万封/次)。
 * 改为本端暂存后跨芯片只剩"写", 与从未失败的 SQE① 同一路径。
 * v29: 每环一区, 每封一个 32B 槽 = 邮件像 [序号+1][dst][len][imm]——⓪ 邮件(32B)、
 * ② 到达戳(8B, 源=槽首即序号+1)、③ 完成戳(8B, 同源)三支 SQE 的共同拷贝源。
 * 每区 512 槽: 单环在途上界 = SQ 环深 1024 SQE ÷ 4(封) = 256 < 512, 暂存槽不会被
 * 提前复用(复用距离 512 封, 远超在途上界)。 */
constexpr uint64_t SMEM_SHM_SDMA_RS_STAGE = 0x1F80;      /* 邮件像暂存数组基址(AICPU 写本端, SDMA 本端读) */
constexpr uint32_t SMEM_SHM_SDMA_RS_STAGE_SLOTS = 512;   /* 每区暂存槽数(每封 32B) */
constexpr uint64_t SMEM_SHM_SDMA_RS_STAGE_STRIDE = 0x4000; /* 区间距(512×32B) */

/* ---- 协同模式 rank 上限(布局按 MAX_RANKS-1 套环设计; 8-rank 全互联被
 * 每卡 8 个 P2P 槽位硬约束否定, 4-rank 为全 mesh 安全上限) ---- */
constexpr uint32_t SMEM_SHM_SDMA_MAX_RANKS = 4;
constexpr uint32_t SMEM_SHM_SDMA_MAX_PEERS = SMEM_SHM_SDMA_MAX_RANKS - 1;

/* ---- per-peer 到达环区(v29 4-rank 化: 每对端一环, 单一远端写者) ---- */
constexpr uint64_t SMEM_SHM_SDMA_RS_PEER_RINGS = 0xDF80; /* 到达环数组基址 */
constexpr uint64_t SMEM_SHM_SDMA_RS_PEER_STAMP = 0x0000; /* 环内: 到达戳数组(SQE 写) */
constexpr uint64_t SMEM_SHM_SDMA_RS_PEER_MAIL = 0x2000;   /* 环内: 到达邮件数组(AICPU 写) */
constexpr uint64_t SMEM_SHM_SDMA_RS_PEER_HEAD = 0xA000;   /* 环内: 到达消费游标(AICore 写, 独占行) */
constexpr uint64_t SMEM_SHM_SDMA_RS_PEER_SLOTS = 1024;    /* 每环到达深度(戳与邮件同深) */
constexpr uint64_t SMEM_SHM_SDMA_RS_PEER_STRIDE = 0xA040; /* 环间距(戳 0x2000+邮件 0x8000+游标行 0x40) */

/* 对端 rank → 本端环下标(请求环与到达环共用; peerRank 须 != ownRank 且 < rankSize)。
 * 去自身压缩: 小于本端的对端原样, 大于等于本端的对端前移一位, 恒 < MAX_PEERS。
 * 请求环(发送方视角)与到达环(接收方视角)各自以本公式定位, 参数序天然区分。 */
SMEM_SHM_SDMA_LAYOUT_FN uint32_t SmemShmSdmaPeerRingIdx(uint32_t ownRank, uint32_t peerRank)
{
    return (peerRank < ownRank) ? peerRank : (peerRank - 1U);
}

/* 反演: 环下标 → 对端 rank(发送方从请求环反推目的/接收方 wait_any 定位来源) */
SMEM_SHM_SDMA_LAYOUT_FN uint32_t SmemShmSdmaPeerRankOf(uint32_t ownRank, uint32_t ringIdx)
{
    return (ringIdx < ownRank) ? ringIdx : (ringIdx + 1U);
}

#ifdef __cplusplus
}
#endif

#endif // __MEMFABRIC_SMEM_SHM_SDMA_LAYOUT_H__
