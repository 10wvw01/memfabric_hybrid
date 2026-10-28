/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You can obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

/* smem_shm_sdma_layout.h — 310P SDMA 协同段尾保留区布局(单一事实源)
 *
 * 本头为纯常量声明(不含 AscendC 原语), 供三端共同 include 消除双头镜像漂移:
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
 * 按 64B 边界对齐分隔。布局图:
 *   +0x0000  请求环 64 槽 × 32B(本端 AICore 写, AICPU 读)
 *   +0x0800  reqTail u64(生产游标, AICore 写)[独占行]
 *   +0x0840  reqHead u64(消费游标, AICPU 写, AICore 背压读)[独占行]
 *   +0x0880  完成戳 64 × u64(SQE 写, AICore quiet 轮询)[独占区]
 *   +0x0A80  到达戳 1024 × u64(SQE 写, 对端 AICore wait 轮询)
 *   +0x2A80  到达邮件 1024 × 32B(对端 AICPU 写, 本端 AICore 读)
 *   +0xAA80  arrHead u64(到达消费游标, 本端 AICore 写)[独占行]
 *   +0xAB00  单号暂存 512 × u64(本端 AICPU 写, 本端 SDMA 读)[v25: 戳 SQE 拷贝数据源]
 * 注意: 戳值 1 基(seq+1), 0 保留为"空"(防 seq0 与清零槽不可辨的幻影邮件)。
 */
#ifndef __MEMFABRIC_SMEM_SHM_SDMA_LAYOUT_H__
#define __MEMFABRIC_SMEM_SHM_SDMA_LAYOUT_H__

#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 段尾保留区(用户段几乎全量归还, 仅占用 48KB) ---- */
constexpr uint64_t SMEM_SHM_SDMA_RESERVED = 48UL * 1024UL; /* 保留区总大小 */
/* 保留区内偏移(见文件头布局图; 跨写者字段按 64B 行分隔——v23 布局纪律) */
constexpr uint64_t SMEM_SHM_SDMA_RS_REQ_RING = 0x0000;   /* 请求环基址(AICore 写) */
constexpr uint64_t SMEM_SHM_SDMA_RS_REQ_SLOTS = 64;      /* 请求环槽数 */
constexpr uint64_t SMEM_SHM_SDMA_RS_REQ_TAIL = 0x0800;   /* 生产游标(AICore 写, 独占行) */
constexpr uint64_t SMEM_SHM_SDMA_RS_REQ_HEAD = 0x0840;   /* 消费游标(AICPU 写, 独占行) */
constexpr uint64_t SMEM_SHM_SDMA_RS_QUIET = 0x0880;      /* 完成戳数组(SQE 写, 独占区) */
constexpr uint64_t SMEM_SHM_SDMA_RS_QUIET_SLOTS = 64;    /* 完成戳数 */
constexpr uint64_t SMEM_SHM_SDMA_RS_ARR_STAMP = 0x0A80;  /* 到达戳数组(SQE 写) */
constexpr uint64_t SMEM_SHM_SDMA_RS_ARR_SLOTS = 1024;    /* 到达环深度 */
constexpr uint64_t SMEM_SHM_SDMA_RS_ARR_MAIL = 0x2A80;   /* 到达邮件数组(AICPU 写) */
constexpr uint64_t SMEM_SHM_SDMA_RS_ARR_HEAD = 0xAA80;   /* 到达消费游标(AICore 写, 独占行) */
/* v25(并发丢件挂死（507014） 残余竞态修复): 戳 SQE 的单号拷贝数据源。原设计复用对端"到达邮件"首字段
 * 为源——AICPU 跨芯片写面单后, 同 die 的 SDMA 立即跨芯片读回自己刚写的值, 写在途时
 * 读到上一圈旧值(实测: ② 抄旧值盖旧值无声失效, ③ 数微秒后读到新值正确落地; 双向
 * 同窗各丢一封, ~2 万封/次)。改为本端暂存后跨芯片只剩"写", 与从未失败的 SQE① 同一路径。
 * 512 槽: 在途上界 = SQ 环深 1024 SQE ÷ 3 = 341 封 < 512, 暂存槽不会被提前复用。 */
constexpr uint64_t SMEM_SHM_SDMA_RS_STAGE = 0xAB00;      /* 单号暂存数组(AICPU 写本端, SDMA 本端读) */
constexpr uint32_t SMEM_SHM_SDMA_RS_STAGE_SLOTS = 512;   /* 暂存槽数(> 在途邮件上界 341) */

#ifdef __cplusplus
}
#endif

#endif // __MEMFABRIC_SMEM_SHM_SDMA_LAYOUT_H__
