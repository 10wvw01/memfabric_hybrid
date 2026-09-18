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

/* hybm_sdma_contract.h — V5 wire 契约(host↔epoch kernel 单一事实源)
 *
 * V5 邮箱化重构(2026-09-18): 批处理契约(chunks/dstBase/flagBase)废止, 改为
 * 双环邮箱 + epoch 常驻消费:
 *   - 请求环(段尾保留区): AICore 经 smem_shm_sdma_signal 投递 {src,dst,len,imm}
 *   - 到达环(对端段尾保留区): AICPU 写邮件内容, SQE 戳(seq)保序落地, 对端 AICore
 *     经 smem_shm_sdma_wait 按序消费
 *   - 完成戳(本端段尾保留区): quiet 语义(发送方确认全部落地)
 *   - 执行模型: epoch kernel(~25s 自限时, watchdog 30s 实证约束) + host 监督续命
 *
 * 保留区布局(两端对称, 段基 + physSize - HYBM_SDMA_ORCH_RESERVED 起):
 *   +0x0000  请求环 64 槽 × 32B(本端 AICore 写, AICPU 读)
 *   +0x0800  reqTail u64(生产游标, AICore 写)
 *   +0x0808  reqHead u64(消费游标, AICPU 写, AICore 背压读)
 *   +0x0810  完成戳 64 × u64(SQE 写, AICore quiet 轮询)
 *   +0x0A10  stopFlag u64(host 写, AICPU epoch 退出)
 *   +0x0A20  到达戳 1024 × u64(SQE 写, 对端 AICore wait 轮询)
 *   +0x2A20  到达邮件 1024 × 32B(对端 AICPU 写, 本端 AICore 读)
 *   +0xAA20  arrHead u64(到达消费游标, 本端 AICore 写)
 * 注意: 本布局与 smem_shm_aicore_sdma.h 的设备侧镜像常量必须一致
 * (UT WorkspaceLayoutInvariants 双头 static_assert 锁定)。
 */
#ifndef HYBM_SDMA_CONTRACT_V5_H
#define HYBM_SDMA_CONTRACT_V5_H

#include <cstdint> // 定宽整型

namespace ock { // MemFabric 总命名空间
namespace mf {  // hybm 内存池引擎子命名空间

/* ---- 版本(wire 变更必须 bump, CP1 按文件名 dlopen 缓存) ---- */
constexpr uint32_t HYBM_SDMA_ORCH_KVER = 5; /* V5: 双环邮箱 + epoch 消费 */

/* ---- 段尾保留区(替代 V4 的 2MB 标志区, 用户段几乎全量归还) ---- */
constexpr uint64_t HYBM_SDMA_ORCH_RESERVED = 48UL * 1024UL; /* 保留区总大小 */
/* 保留区内偏移(见文件头布局图) */
constexpr uint64_t HYBM_SDMA_ORCH_RS_REQ_RING = 0x0000;   /* 请求环基址 */
constexpr uint64_t HYBM_SDMA_ORCH_RS_REQ_SLOTS = 64;      /* 请求环槽数 */
constexpr uint64_t HYBM_SDMA_ORCH_RS_REQ_TAIL = 0x0800;   /* 生产游标 */
constexpr uint64_t HYBM_SDMA_ORCH_RS_REQ_HEAD = 0x0808;   /* 消费游标 */
constexpr uint64_t HYBM_SDMA_ORCH_RS_QUIET = 0x0810;      /* 完成戳数组 */
constexpr uint64_t HYBM_SDMA_ORCH_RS_QUIET_SLOTS = 64;    /* 完成戳数 */
constexpr uint64_t HYBM_SDMA_ORCH_RS_STOP = 0x0A10;       /* epoch 停止标志 */
constexpr uint64_t HYBM_SDMA_ORCH_RS_ARR_STAMP = 0x0A20;  /* 到达戳数组 */
constexpr uint64_t HYBM_SDMA_ORCH_RS_ARR_SLOTS = 1024;    /* 到达环深度 */
constexpr uint64_t HYBM_SDMA_ORCH_RS_ARR_MAIL = 0x2A20;   /* 到达邮件数组 */
constexpr uint64_t HYBM_SDMA_ORCH_RS_ARR_HEAD = 0xAA20;   /* 到达消费游标 */

/* ---- 邮件格式(32B/封, 8B 对齐字段) ---- */
struct HybmSdmaReqMail {  /* 请求邮件(AICore→AICPU, signal 投递) */
    uint64_t src;         /* 搬运源地址(本端段 GVA) */
    uint64_t dst;         /* 搬运目的地址(对端段 GVA) */
    uint64_t len;         /* 字节数(框架内部按粒度补齐) */
    uint64_t imm;         /* 用户标签(原样回带到达到达邮件) */
};

struct HybmSdmaArrMail {  /* 到达邮件(AICPU→对端 AICore, wait 消费) */
    uint64_t seq;         /* 序号(=请求序号; 首字段供到达戳 SQE 复用为拷贝源) */
    uint64_t dst;         /* 数据落点(接收方免预约定布局, 直接定位) */
    uint64_t len;         /* 数据长度 */
    uint64_t imm;         /* 用户标签(来自 signal 的 imm, 原样透传) */
};

/* ---- host→epoch kernel 参数(值透传, 布局锁) ---- */
struct HybmSdmaOrchParam {
    uint32_t devId;       /* host 设备号(SQE/doorbell 路径用) */
    uint32_t sqId;        /* CP 流 SQ id */
    int32_t streamId;     /* CP 流 id(SQE 的 rt_stream_id) */
    uint32_t rsv0;        /* 对齐保留 */
    uint64_t reqBase;     /* 本端请求环基址(= ownSeg + phys - RESERVED + RS_REQ_RING) */
    uint64_t quietBase;   /* 本端完成戳基址(quiet SQE 的目标) */
    uint64_t stopAddr;    /* 本端停止标志地址(epoch 退出) */
    uint64_t overflowAddr;/* SQE overflow 目标(工作区) */
    uint64_t resultAddr;  /* 工作区结果区地址(诊断) */
    uint64_t symSize;     /* 对称段大小(对端段基址掩码算术用, 须 2 的幂) */
    uint64_t physSize;    /* 段物理大小(对端保留区定位用) */
};
static_assert(sizeof(HybmSdmaOrchParam) == 72, "Param 尺寸漂移"); /* 布局锁 72B(4×u32 头 + 7×u64) */

/* ---- kernel→host 结果(诊断, 工作区) ---- */
struct HybmSdmaOrchResult {
    uint32_t magic;       /* 存活魔数(kernel 执行过) */
    uint32_t mainRet;     /* 主返回码(0=成功/1=失败) */
    uint32_t consumed;    /* 本 epoch 消费邮件数(低 16 位有效) */
    uint32_t exitReason;  /* 退出原因(0=时限/1=停止/2=错误) */
    uint64_t headAfter;   /* 面包屑: 退出时 SQ head 采样 */
};
static_assert(sizeof(HybmSdmaOrchResult) == 24, "Result 尺寸漂移"); /* 布局锁 24B */

constexpr uint32_t HYBM_SDMA_ORCH_RESULT_MAGIC = 0x5D0A0005u; /* V5 魔数 */
constexpr uint32_t HYBM_SDMA_ORCH_EXIT_DEADLINE = 0;          /* 时限退出(正常续命) */
constexpr uint32_t HYBM_SDMA_ORCH_EXIT_STOP = 1;              /* 停止退出(析构) */
constexpr uint32_t HYBM_SDMA_ORCH_EXIT_ERROR = 2;             /* 错误退出 */

/* SQE 配方与 HAL 弱符号沿用 V4(STARS V2 64B, hcomm 源码比对定论), 见 kernel 实现 */
constexpr uint32_t HYBM_SDMA_ORCH_EPOCH_IDLE_US = 100;        /* 空闲退避 100µs */
constexpr uint32_t HYBM_SDMA_ORCH_EPOCH_MAX_LOOPS = 220000;   /* epoch 循环上限(≈22s 空闲) */

} // namespace mf
} // namespace ock

#endif // HYBM_SDMA_CONTRACT_V5_H
