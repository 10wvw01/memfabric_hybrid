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

/* smem_shm_sdma_layout.h — 310P SDMA 协同段尾保留区布局(单一事实源)
 *
 * 本头为纯常量声明(不含 AscendC 原语), 供三端共同 include 消除双头镜像漂移:
 *   - 设备侧原语头 smem_shm_aicore_base_sdma.h(用户 AICore kernel)
 *   - hybm wire 契约头 hybm_sdma_contract.h(host↔epoch kernel)
 *   - host UT(布局不变式交叉校验)
 *
 * 保留区位于每 rank 段尾(段基 + physSize - SMEM_SHM_SDMA_RESERVED), host 建池时
 * 初始化清零; 布局图:
 *   +0x0000  请求环 64 槽 × 32B(本端 AICore 写, AICPU 读)
 *   +0x0800  reqTail u64(生产游标, AICore 写)
 *   +0x0808  reqHead u64(消费游标, AICPU 写, AICore 背压读)
 *   +0x0810  完成戳 64 × u64(SQE 写, AICore quiet 轮询)
 *   +0x0A10  (保留)
 *   +0x0A20  到达戳 1024 × u64(SQE 写, 对端 AICore wait 轮询)
 *   +0x2A20  到达邮件 1024 × 32B(对端 AICPU 写, 本端 AICore 读)
 *   +0xAA20  arrHead u64(到达消费游标, 本端 AICore 写)
 * 注意: 戳值 1 基(seq+1), 0 保留为"空"(防 seq0 与清零槽不可辨的幻影邮件)。
 */
#ifndef __MEMFABRIC_SMEM_SHM_SDMA_LAYOUT_H__ /* 头文件包含守卫(防重复包含) */
#define __MEMFABRIC_SMEM_SHM_SDMA_LAYOUT_H__ /* 同上, 与文件尾 #endif 配对 */

#include <cstdint> /* 定宽整型 */

#ifdef __cplusplus /* C++ 编译环境 */
extern "C" {       /* C 链接(供 bisheng kernel 与 host UT 双端 include) */
#endif

/* ---- 段尾保留区(替代 V4 的 2MB 标志区, 用户段几乎全量归还) ---- */
constexpr uint64_t SMEM_SHM_SDMA_RESERVED = 48UL * 1024UL; /* 保留区总大小 */
/* 保留区内偏移(见文件头布局图) */
constexpr uint64_t SMEM_SHM_SDMA_RS_REQ_RING = 0x0000;   /* 请求环基址 */
constexpr uint64_t SMEM_SHM_SDMA_RS_REQ_SLOTS = 64;      /* 请求环槽数 */
constexpr uint64_t SMEM_SHM_SDMA_RS_REQ_TAIL = 0x0800;   /* 生产游标 */
constexpr uint64_t SMEM_SHM_SDMA_RS_REQ_HEAD = 0x0808;   /* 消费游标 */
constexpr uint64_t SMEM_SHM_SDMA_RS_QUIET = 0x0810;      /* 完成戳数组 */
constexpr uint64_t SMEM_SHM_SDMA_RS_QUIET_SLOTS = 64;    /* 完成戳数 */
constexpr uint64_t SMEM_SHM_SDMA_RS_STOP = 0x0A10;       /* (保留)停止标志槽位 */
constexpr uint64_t SMEM_SHM_SDMA_RS_ARR_STAMP = 0x0A20;  /* 到达戳数组 */
constexpr uint64_t SMEM_SHM_SDMA_RS_ARR_SLOTS = 1024;    /* 到达环深度 */
constexpr uint64_t SMEM_SHM_SDMA_RS_ARR_MAIL = 0x2A20;   /* 到达邮件数组 */
constexpr uint64_t SMEM_SHM_SDMA_RS_ARR_HEAD = 0xAA20;   /* 到达消费游标 */

#ifdef __cplusplus /* C++ 编译环境 */
}                  /* extern "C" 结束 */
#endif

#endif // __MEMFABRIC_SMEM_SHM_SDMA_LAYOUT_H__
