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
constexpr uint32_t HYBM_SDMA_ORCH_KVER = 9; /* 设备侧产物版本号: kernel 变更需重部署时递增(CP1 按文件名 dlopen 缓存刷新) */

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

constexpr uint32_t HYBM_SDMA_ORCH_RESULT_MAGIC = 0x5D0A0005u; /* 结果区存活魔数 */
constexpr uint32_t HYBM_SDMA_ORCH_EXIT_DEADLINE = 0;          /* 时限退出(正常续命) */
constexpr uint32_t HYBM_SDMA_ORCH_EXIT_STOP = 1;              /* 停止退出(析构) */
constexpr uint32_t HYBM_SDMA_ORCH_EXIT_ERROR = 2;             /* 错误退出 */

/* SQE 配方与 HAL 弱符号(STARS V2 64B, hcomm 源码比对定论), 见 kernel 实现 */
constexpr uint32_t HYBM_SDMA_ORCH_EPOCH_IDLE_US = 100;        /* 空闲退避 100µs */
/* epoch 自限双层设计(实机复盘 2026-09-21):
 *   主限 = 墙钟 WALL_S(cntvct_el0 纪内差值, 频率 usleep 下界标定): 忙/闲节拍漂移
 *         (实测空闲即有 116→140µs/循环漂移)下墙钟恒定, 循环数预算本质不可靠。
 *   兜底 = MAX_LOOPS: 标定失败(计数器不走)时退化为纯循环上限, 150000×~140µs≈21s。
 * 约束链: WALL_S=18s < 击杀边界(板间漂移 28~36s, 本机实测 35.9s)。
 * 教训(勿再犯): ①epoch 超时击杀终局为 hdc 断链/DEV_RUNNING_DOWN, 进程级永久不可
 * 恢复(90s launch 探针零恢复)——续命只能靠"从未被杀"; ②launch cfg 空属性(numAttrs=0)
 * 被 runtime 拒绝(107000), attr 仅作占位且取值须大于击杀边界; ③cntfrq_el0 可读性
 * 无实机依据, 频率以 usleep 下界标定反推(只依赖已验证的 cntvct_el0)。 */
constexpr uint32_t HYBM_SDMA_ORCH_EPOCH_WALL_S = 18;           /* epoch 墙钟自限(主限, 节拍漂移免疫) */
constexpr uint32_t HYBM_SDMA_ORCH_EPOCH_MAX_LOOPS = 150000;   /* epoch 循环上限(兜底, 时钟失效时 ~21s) */

} // namespace mf
} // namespace ock

#endif // HYBM_SDMA_CONTRACT_H
