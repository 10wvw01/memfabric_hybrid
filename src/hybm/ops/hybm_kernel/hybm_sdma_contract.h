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

/* Host 与 AICPU 共用的 SDMA 请求格式；队列偏移统一使用设备布局头中的定义。 */
#ifndef HYBM_SDMA_CONTRACT_H
#define HYBM_SDMA_CONTRACT_H

#include <cstdint>

#include "smem_shm_sdma_layout.h" // 段尾保留区布局单一事实源(与设备侧共用)

namespace ock {
namespace mf {

/* 布局常量沿用共享头命名(SMEM_SHM_SDMA_*), hybm 侧经 include 直接使用 */

/* ---- 邮件格式(32B/封, 8B 对齐字段) ---- */
struct HybmSdmaReqMail {  /* 传输请求(AICore→AICPU) */
    uint64_t src;         /* 源地址(Put 本端 GM / Get 远端共享内存) */
    uint64_t dst;         /* 目标地址(Put 远端共享内存 / Get 本端 GM) */
    uint64_t len;         /* 低 32 位为实际字节数；bit 63 标识纯传输，bit 32–62 必须为零，不补齐尾部 */
    uint64_t imm;         /* 数据操作编码，bit39表示携带通知参数 */
};

/* 操作编码：COPY 为零；ADD 的 [63:48] 为协议标识，[47:40] 为硬件操作码。
 * bit39表示携带SET通知；消费端去掉通知标记后校验数据操作编码。
 */
constexpr uint64_t HYBM_SDMA_IMM_MAGIC = 0x5244ULL; /* 'RD' */
constexpr uint8_t HYBM_SDMA_SQE_OP_FP32_ADD = 0x02; /* FP32 累加: dst += src */
constexpr uint8_t HYBM_SDMA_SQE_OP_FP16_ADD = 0x03; /* FP16 累加: dst += src */

static inline uint8_t HybmSdmaDecodeOp(uint64_t encodedOp)
{
    return encodedOp == 0 ? 0 : static_cast<uint8_t>((encodedOp >> 40) & 0xFFULL);
}

/* ---- host→epoch kernel 参数(值透传, 布局锁) ---- */
struct HybmSdmaOrchParam {
    uint32_t devId;       /* host 设备号(SQE/doorbell 路径用) */
    uint32_t sqId;        /* CP 流 SQ id */
    int32_t streamId;     /* CP 流 id(SQE 的 rt_stream_id) */
    uint32_t epochBudgetS;/* 每轮时长上限(秒); 0=默认 15s; 0xFFFFFFFF=永不退出(跑到停止/错误, 诊断用) */
    uint64_t resBase;     /* 本端保留区基址(= ownSeg + phys - RESERVED; 请求环/游标/完成戳/暂存定位原点) */
    uint64_t stopAddr;    /* 本端停止标志地址(epoch 退出) */
    uint64_t overflowAddr;/* SQE overflow 目标(工作区) */
    uint64_t resultAddr;  /* 工作区结果区地址(诊断) */
    uint64_t symSize;     /* 对称段大小(对端段基址掩码算术用, 须 2 的幂) */
    uint64_t physSize;    /* 段物理大小(对端保留区定位用) */
    uint32_t rank;        /* 本端 rank 号(请求环下标 = PeerRingIdx(rank, 目的 rank)) */
    uint32_t rankCount;   /* 世界大小，决定有效对端队列范围 */
    uint64_t poolBaseGva; /* 池窗口基址(对端段基 → 对端 rank = (dstSeg - poolBaseGva)/symSize) */
    uint32_t producerMask; /* 允许提交的逻辑block，与设备元数据一致 */
    uint32_t reserved;
    /* 寄存器通知(commByReg): 默认 0 走原 GM 路径; 1 时 AICore 入口自注册核号并在提交后
     * set_cond, AICPU 空闲时只读该注册核的 COND(不扫其它核), 数据面(邮件/游标)仍走 GM。 */
    uint8_t commByReg;    /* 1=启用寄存器通知; 0=GM(默认) */
    uint8_t rsv2[7];      /* 填充对齐(显式置 0) */
    uint64_t regBaseAddr; /* regBase 数组设备地址(10×u64, host 经 halMemCtl 查出后 H2D) */
};
static_assert(sizeof(HybmSdmaOrchParam) == 104, "Param 尺寸漂移"); /* 布局锁 104B(88B + 8B + 8B) */

// 部署与清理确认使用独立参数，结果必须携带本次nonce。
struct HybmSdmaDeployCheck {
    uint64_t expectedAddr;
    uint64_t expectedSize; // 0表示确认SO已删除
    uint64_t nonce;
    uint64_t resultAddr;
    char path[256];
};
struct HybmSdmaDeployResult {
    uint64_t nonce;
    uint64_t compared;
    uint32_t status;
    uint32_t reserved;
};

/* ---- kernel→host 结果(诊断, 工作区) ---- */
struct HybmSdmaOrchResult {
    uint32_t magic;       /* 存活魔数(kernel 执行过) */
    uint32_t mainRet;     /* 主返回码(0=成功/1=失败) */
    uint32_t consumed;    /* 本 epoch 消费邮件数(低 16 位有效) */
    uint32_t exitReason;  /* 退出原因(0=时限/1=停止/2=错误) */
    uint64_t headAfter;   /* 逐单日志: 退出时 SQ head 采样 */
};
static_assert(sizeof(HybmSdmaOrchResult) == 24, "Result 尺寸漂移"); /* 布局锁 24B */

constexpr uint32_t HYBM_SDMA_ORCH_RESULT_MAGIC = 0x5D0A000Au; /* 结果区存活魔数 */
constexpr uint32_t HYBM_SDMA_ORCH_EXIT_RUNNING = 0xFFFFFFFFu; /* 运行中(kernel 入口写, host 轮询判据) */
constexpr uint32_t HYBM_SDMA_ORCH_EXIT_DEADLINE = 0;          /* 时限退出(正常换轮) */
constexpr uint32_t HYBM_SDMA_ORCH_EXIT_STOP = 1;              /* 停止退出(析构) */
constexpr uint32_t HYBM_SDMA_ORCH_EXIT_ERROR = 2;             /* 错误退出 */

/* SQE 配方与 HAL 弱符号(STARS V2 64B, hcomm 源码比对定论), 见 kernel 实现 */
/* 每轮时长机制(2026-10-12 定案, v11): KFCKernel(type 5) 执行环境下 cntvct_el0/cntfrq_el0
 * 均不可用(v7 时代 type 2 实测正常, 换 type 5 后两 die 全失效——回退循环计数 22 万圈
 * 实测 34s/15s 预算, 负载下漂移至 42~102s)。正解 = 主机报时: host 监督线程每
 * 100ms 以同步 AclrtMemcpy 往工作区 HEARTBEAT_OFF 写递增计数(同步调用返回即完成,
 * 不给任何全设备同步留 pending 任务), kernel 主循环读该计数, 到 budgetS×10 即自退。
 * 精度 100ms, 不受设备负载影响, 不依赖设备寄存器。 */
constexpr uint32_t HYBM_SDMA_ORCH_HEARTBEAT_HZ = 10;  /* host 报时频率(每 100ms 一跳) */
/* 工作区报时槽偏移(工作区布局: resultAddr 即基址, RESULT 0 / OVF 64 / STOP 128): */
constexpr uint64_t HYBM_SDMA_ORCH_WS_HEARTBEAT_OFF = 136; /* host 心跳计数 8B(每 100ms +1, kernel 读) */
/* ---- 诊断逐单日志(工作区, host D2H 可读; 每封搬运记录请求原文/计算地址/SQ 游标) ----
 * 逐单定位邮件丢失类故障: 单条记录即可裁决"读到空请求(写到错误地址)/ SQE 未执行 / doorbell 异常"。
 * 开销: 每封 ~1 次缓存行维护(微秒级), 常驻。 */
constexpr uint64_t HYBM_SDMA_ORCH_WS_DBG_DOORBELL_OFF = 144; /* u64: 最近一次 doorbell 尾值 */
constexpr uint64_t HYBM_SDMA_ORCH_WS_DBG_DBRET_OFF = 152;    /* u64: 低 32 位=该次 doorbell 返回码 */
constexpr uint64_t HYBM_SDMA_ORCH_WS_DBG_RING_CNT_OFF = 176; /* u64: 累计逐单日志条数(环形游标) */
constexpr uint64_t HYBM_SDMA_ORCH_WS_DBG_RING_OFF = 184;     /* 60 × 64B 环形逐单日志 */
constexpr uint32_t HYBM_SDMA_ORCH_DBG_RING_SLOTS = 60;       /* (4096-184-32)/64 */
constexpr uint64_t HYBM_SDMA_ORCH_WS_PAD_SCRATCH_OFF = 4088; /* 8B: 尾值防 0 占位 SQE 的暂存地址 */
struct HybmSdmaOrchDbgMail {                                  /* 单条逐单日志(64B, 单缓存行) */
    uint64_t seq;         /* 消费的请求序号 */
    uint64_t reqF0;       /* 请求槽读回 4 字(原文; 全 0=读到空请求, 写到错误地址特征) */
    uint64_t reqF1;
    uint64_t reqF2;
    uint64_t reqF3;
    uint64_t reserved; /* 保留为零，维持诊断记录大小 */
    uint64_t sqTailBefore;/* SQE 入环前 shadow 尾 */
    uint64_t sqTailAfter; /* SQE 入环后 shadow 尾 */
};
static_assert(sizeof(HybmSdmaOrchDbgMail) == 64, "DbgMail 尺寸漂移"); /* 布局锁 64B */

/* ---- 寄存器通知(commByReg)常量 ---- */
constexpr uint32_t HYBM_SDMA_ORCH_REG_CORES = 10;        /* 310P3 DAV_2002 物理 AICore 数 */
constexpr uint64_t HYBM_SDMA_ORCH_REG_CORE_STRIDE = 0x200000ULL; /* 每核寄存器窗口 2MB */
constexpr uint32_t HYBM_SDMA_ORCH_REG_SPR_COND = 0x4C8;  /* AICore set_cond 写, AICPU 读 */
constexpr uint32_t HYBM_SDMA_ORCH_ACK_TIMEOUT_MS_DEFAULT = 100;  /* AICore 出口等待回执默认 100ms */
constexpr uint32_t HYBM_SDMA_ORCH_ACK_TIMEOUT_MS_MAX = 60000;    /* 硬上限 1 分钟 */
constexpr uint32_t HYBM_SDMA_ORCH_EPOCH_IDLE_US = 100;   /* 寄存器通知空闲休眠(仅启用时) */
constexpr uint32_t HYBM_SDMA_ORCH_REG_FORCE_SCAN = 4096; /* 连续空转后强制 GM 全扫(通知丢失兜底) */
/* 寄存器通知阶段码(写 result->headAfter, 退出前被真实 head 覆盖; 挂死时 host D2H 可读):
 * 0x3001 读通知槽 / 0x3100+coreId 即将读该核 COND / 0x3200 读完成 / 0x3300 写回执 */
constexpr uint64_t HYBM_SDMA_ORCH_STAGE_REG_CELLS = 0x3001;
constexpr uint64_t HYBM_SDMA_ORCH_STAGE_REG_COND_BASE = 0x3100;
constexpr uint64_t HYBM_SDMA_ORCH_STAGE_REG_DONE = 0x3200;
constexpr uint64_t HYBM_SDMA_ORCH_STAGE_REG_ACK = 0x3300;

constexpr uint32_t HYBM_SDMA_ORCH_EPOCH_TIME_BUDGET_S = 15;
constexpr uint32_t HYBM_SDMA_ORCH_EPOCH_BUDGET_RESIDENT = 0xFFFFFFFFu; /* 永不退出的专用值(不判报时, 诊断用) */
constexpr uint32_t HYBM_SDMA_ORCH_EPOCH_CLOCK_INTERVAL = 64; /* 稀疏读取单调时钟，限制每轮实际时长 */

// epoch入口生成允许提交的队列列表，批次消费时直接按列表轮转。
inline uint32_t HybmSdmaBuildScanQueues(uint32_t ranks, uint32_t mask, uint32_t *queues)
{
    if (ranks == 0 || ranks > SMEM_SHM_SDMA_MAX_RANKS || !SmemShmSdmaProducerMaskValid(mask)) {
        return 0;
    }
    uint32_t count = 0;
    for (uint32_t peer = 0; peer < ranks - 1; ++peer) {
        for (uint32_t block = 0; block < SMEM_SHM_SDMA_QUEUES_PER_PEER; ++block) {
            if (SmemShmSdmaProducerAllowed(mask, block)) {
                queues[count++] = SmemShmSdmaQueueIdx(peer, block);
            }
        }
    }
    return count;
}

inline uint32_t HybmSdmaNextScan(uint32_t ordinal, uint32_t count)
{
    return ordinal + 1 == count ? 0 : ordinal + 1;
}

} // namespace mf
} // namespace ock

#endif // HYBM_SDMA_CONTRACT_H
