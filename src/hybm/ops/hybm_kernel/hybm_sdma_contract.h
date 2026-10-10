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

/* hybm_sdma_contract.h — wire 契约(host↔epoch kernel 单一事实源)
 *
 * 段尾保留区布局经共享布局头 smem_shm_sdma_layout.h 与设备侧
 * 原语头共用单一事实源(无镜像漂移)。
 *
 * 邮箱化契约: 双环邮箱 + epoch 常驻消费(批处理 chunks/dstBase/flagBase 形态已废止);
 * host 数据面 API 废止(signal/wait/quiet 设备侧自驱)。
 */
#ifndef HYBM_SDMA_CONTRACT_H
#define HYBM_SDMA_CONTRACT_H

#include <cstdint>

#include "smem_shm_sdma_layout.h" // 段尾保留区布局单一事实源(与设备侧共用)

namespace ock {
namespace mf {

/* 布局常量沿用共享头命名(SMEM_SHM_SDMA_*), hybm 侧经 include 直接使用 */

/* ---- 版本(wire 变更必须 bump, CP1 按文件名 dlopen 缓存) ---- */
constexpr uint32_t HYBM_SDMA_ORCH_KVER = 9; /* 设备侧产物版本号: wire 行为变更时递增(CP1 按文件名 dlopen 缓存刷新) */

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
    uint32_t epochBudgetS;/* epoch 壁钟预算(秒); 0=默认 15s; 0xFFFFFFFF=常驻(跑到停止/错误) */
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

constexpr uint32_t HYBM_SDMA_ORCH_RESULT_MAGIC = 0x5D0A0005u; /* 结果区存活魔数 */
constexpr uint32_t HYBM_SDMA_ORCH_EXIT_DEADLINE = 0;          /* 时限退出(正常续命) */
constexpr uint32_t HYBM_SDMA_ORCH_EXIT_STOP = 1;              /* 停止退出(析构) */
constexpr uint32_t HYBM_SDMA_ORCH_EXIT_ERROR = 2;             /* 错误退出 */

/* SQE 配方与 HAL 弱符号(STARS V2 64B, hcomm 源码比对定论), 见 kernel 实现 */
/* epoch 壁钟预算(秒): cntvct_el0 到点即 DEADLINE 自退。
 * v9 起 launch json 为 KFCKernel(type 5): aicpu-sd monitor 对该类型不计时
 * (ExecuteHWTSKFCEventTask 无 SetTaskStartTime, 与 CANN 自带 HcclKernelEntrance 同路径),
 * 28s 击杀约束解除——host 下发常驻哨兵, kernel 一次下发跑到停止标志/错误退出,
 * 不再有到期重拉(监督线程已删除)。预算字段保留兼容: 0=本默认值(旧 host),
 * 有限值=到期自退(诊断用)。禁止用循环次数×假设每轮耗时估算自退——usleep(100)
 * 实测 ≈130µs */
constexpr uint32_t HYBM_SDMA_ORCH_EPOCH_TIME_BUDGET_S = 15;
constexpr uint32_t HYBM_SDMA_ORCH_EPOCH_BUDGET_RESIDENT = 0xFFFFFFFFu; /* 常驻哨兵(无墙钟/循环上限) */
constexpr uint32_t HYBM_SDMA_ORCH_EPOCH_IDLE_US = 100;       /* 空闲退避 100µs(实测 ≈130µs) */
constexpr uint32_t HYBM_SDMA_ORCH_EPOCH_MAX_LOOPS = 220000;  /* 每 15s 预算的循环上限兜底(有限预算模式按比例放大) */

} // namespace mf
} // namespace ock

#endif // HYBM_SDMA_CONTRACT_H
