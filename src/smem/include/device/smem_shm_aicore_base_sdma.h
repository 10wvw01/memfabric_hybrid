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

/* smem_shm_aicore_base_sdma.h — 邮箱化设备侧 API(AICore kernel 用, signal/wait/quiet)
 *
 * 使用形态(与 910B RDMADemo 同构), 三 API 均以池 gva 为首参:
 *   smem_shm_sdma_signal(gva, src, dst, len, imm) → bool   投递搬运请求(满环自旋背压)
 *   smem_shm_sdma_wait(gva) → smem_shm_sdma_mail_t         按序消费到达邮件(阻塞自旋)
 *   smem_shm_sdma_quiet(gva) → bool                        等待全部发出落地(源缓冲复用前)
 *
 * 邮箱由 memfabric 全托管(双环在段尾保留区, host 初始化清零): 用户零槽位算术/零清零;
 * 到达邮件 {dst,len,imm} 自描述, 接收方免预约定布局。
 *
 * 环境依赖: 段对称大小须 2 的幂(host 建池时校验); rank/symm/phys 从 meta 区自取。
 *
 * dav-2002 工具链约束(见 base_meta.h 注): 本头全部设备内存访问 = gva 参数的指针算术
 * + GlobalTensor cache 维护, 严禁运行时整数→指针 cast 与指针物化。
 */
#ifndef __MEMFABRIC_SMEM_AI_CORE_BASE_SDMA_H__
#define __MEMFABRIC_SMEM_AI_CORE_BASE_SDMA_H__

#include "smem_shm_aicore_base_define.h"
#include "smem_shm_aicore_base_meta.h"
#include "smem_shm_sdma_layout.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 邮件(wait 出参, 32B) ---- */
struct smem_shm_sdma_mail_t {
    uint64_t dst;    /* 数据落点(接收方直接定位) */
    uint64_t len;    /* 数据长度(字节) */
    uint64_t imm;    /* 用户标签(signal 的 imm 原样透传; reduce 时剥除操作编码后透传) */
    uint32_t seq;    /* 完成序号(诊断) */
    uint32_t status; /* 0=成功; 1=超时; 2=到达环溢出(生产方越界覆写) */
};

/* ---- 搬运操作模式(signal 可选参数; 不传 = 纯搬运, 原有用例零改动) ----
 * SDMA inline reduce: dst = dst op src (element-wise)。310P3 实测支持域: ADD+FP32
 * (实测 2026-10-15: SUM/fp32 PASS; i32/MAX/MIN/EQUAL 被能力位图拒绝 207000/107000)。
 * SQE 层实现: opcode 编码 = 数据类型(高4位)|操作(低4位), FP32=0x70 ADD=0x01 → 0x71。 */
typedef enum smem_shm_sdma_op {
    SMEM_SHM_SDMA_OP_COPY = 0,     /* 纯搬运 dst = src (默认, 现状路径) */
    SMEM_SHM_SDMA_OP_ADD_FP32 = 1, /* 搬运+reduce dst += src (fp32 逐元素) */
    SMEM_SHM_SDMA_OP_ADD_FP16 = 2, /* 搬运+reduce dst += src (fp16 逐元素) */
} smem_shm_sdma_op_t;

/* imm 操作编码 (仅 op != COPY 时 signal_at 内部使用; 用户标签保留低 40 位)
 * 布局: [63:48]=magic [47:40]=SQE opcode [39:0]=用户标签
 * magic 使旧调用(imm 为任意标签值)被消费方无误判 — 概率 < 1/65536 且撞上仅误 reduce */
constexpr uint64_t SMEM_SDMA_IMM_MAGIC = 0x5244ULL; /* 'RD' */
constexpr uint8_t SMEM_SDMA_SQE_OP_FP32_ADD = 0x71;  /* 0x70(FP32) | 0x01(ADD) */
constexpr uint8_t SMEM_SDMA_SQE_OP_FP16_ADD = 0x61;  /* 0x60(FP16) | 0x01(ADD) */

SMEM_SHM_INLINE_AICORE uint64_t SdmaEncodeOpImm(uint64_t imm, uint8_t sqeOpcode)
{
    return (SMEM_SDMA_IMM_MAGIC << 48) | (static_cast<uint64_t>(sqeOpcode) << 40) | (imm & 0xFFFFFFFFFFULL);
}

/* 轮询预算(探测次数上限; 超时为错误路径, 由调用方处置) */
constexpr uint64_t SMEM_SHM_SDMA_POLL_MAX_ITER = 100000000ULL;
/* 渐进退避: 前 SPIN_ITERS 轮纯自旋保低延迟, 之后空转退避渐进加倍(AICore 无 usleep) */
constexpr uint64_t SMEM_SHM_SDMA_SPIN_ITERS = 4096;
constexpr uint32_t SMEM_SHM_SDMA_BACKOFF_START = 64;
constexpr uint32_t SMEM_SHM_SDMA_BACKOFF_MAX = 16384;

/* 到达邮件状态码 */
constexpr uint32_t SMEM_SHM_SDMA_MAIL_OK = 0;
constexpr uint32_t SMEM_SHM_SDMA_MAIL_TIMEOUT = 1;
constexpr uint32_t SMEM_SHM_SDMA_MAIL_OVERRUN = 2;

/* 轮询退避(delay 由调用方持有渐增; 空转以编译屏障防优化) */
SMEM_SHM_INLINE_AICORE void SdmaPollBackoff(uint64_t iter, uint32_t &delay)
{
    if (iter < SMEM_SHM_SDMA_SPIN_ITERS) {
        return;
    }
    for (uint32_t i = 0; i < delay; i++) {
        __asm__ __volatile__("" ::: "memory");
    }
    if ((iter & 0xFFu) == 0 && delay < SMEM_SHM_SDMA_BACKOFF_MAX) {
        delay <<= 1;
    }
}

#ifdef __cplusplus
}
#endif

/* ---- 内部原语 ---- */

/* 预计算本端段尾保留区基址: kernel 入口一次计算, 三件套 _at 变体复用,
 * 免每调用 3 次 meta 区 GM 读(symm/rank/phys 在 kernel 生命周期内不变) */
SMEM_SHM_INLINE_AICORE __gm__ uint8_t *smem_shm_sdma_reserved(__gm__ void *gva)
{
    const uint64_t symm = smem_shm_get_symmetric_size(0);
    const uint64_t rank = smem_shm_get_global_rank(0);
    const uint64_t phys = smem_shm_get_phys_size(0);
    return static_cast<__gm__ uint8_t *>(gva) + symm * rank + (phys - SMEM_SHM_SDMA_RESERVED);
}

/* dcci 失效后读 8B(AICPU/SQE 写 → AICore 读的可见性正解, E0c 定论) */
SMEM_SHM_INLINE_AICORE uint64_t SdmaReadCell(__gm__ uint8_t *res, uint64_t off)
{
    __gm__ uint8_t *cell = res + off;
    AscendC::GlobalTensor<int32_t> t;
    t.SetGlobalBuffer(reinterpret_cast<__gm__ int32_t *>(cell), 2);
    AscendC::DataCacheCleanAndInvalid<int32_t, AscendC::CacheLine::SINGLE_CACHE_LINE>(t);
    return *reinterpret_cast<__gm__ volatile uint64_t *>(cell);
}

/* 写 8B 并 clean(AICore 写 → AICPU/SQE 读) */
SMEM_SHM_INLINE_AICORE void SdmaWriteCell(__gm__ uint8_t *res, uint64_t off, uint64_t v)
{
    __gm__ uint8_t *cell = res + off;
    *reinterpret_cast<__gm__ volatile uint64_t *>(cell) = v;
    AscendC::GlobalTensor<int32_t> t;
    t.SetGlobalBuffer(reinterpret_cast<__gm__ int32_t *>(cell), 2);
    AscendC::DataCacheCleanAndInvalid<int32_t, AscendC::CacheLine::SINGLE_CACHE_LINE>(t);
}

/* 读 32B 邮件槽(dcci 失效 + 4×volatile 读; 32B 对齐不跨行) */
SMEM_SHM_INLINE_AICORE void SdmaReadMail(__gm__ uint8_t *slot, uint64_t *f0, uint64_t *f1, uint64_t *f2,
                                         uint64_t *f3)
{
    AscendC::GlobalTensor<int32_t> t;
    t.SetGlobalBuffer(reinterpret_cast<__gm__ int32_t *>(slot), 8);
    AscendC::DataCacheCleanAndInvalid<int32_t, AscendC::CacheLine::SINGLE_CACHE_LINE>(t);
    __gm__ volatile uint64_t *q = reinterpret_cast<__gm__ volatile uint64_t *>(slot);
    *f0 = q[0];
    *f1 = q[1];
    *f2 = q[2];
    *f3 = q[3];
}

/* ============================ 公开 API ============================ */

/* signal 的预计算基址变体(res = smem_shm_sdma_reserved(gva) 缓存值)
 * op: 可选搬运操作(不传 = SMEM_SHM_SDMA_OP_COPY 纯搬运); ADD_FP32 时 dst += src(fp32) */
SMEM_SHM_INLINE_AICORE bool smem_shm_sdma_signal_at(__gm__ uint8_t *res, __gm__ void *src, __gm__ void *dst,
                                                    uint64_t len, uint64_t imm,
                                                    smem_shm_sdma_op_t op = SMEM_SHM_SDMA_OP_COPY)
{
    const uint64_t tail = SdmaReadCell(res, SMEM_SHM_SDMA_RS_REQ_TAIL);
    uint32_t delay = SMEM_SHM_SDMA_BACKOFF_START;
    for (uint64_t i = 0; i < SMEM_SHM_SDMA_POLL_MAX_ITER; i++) {
        const uint64_t head = SdmaReadCell(res, SMEM_SHM_SDMA_RS_REQ_HEAD);
        if (tail - head < SMEM_SHM_SDMA_RS_REQ_SLOTS) {
            break;
        }
        SdmaPollBackoff(i, delay);
    }
    const uint64_t head = SdmaReadCell(res, SMEM_SHM_SDMA_RS_REQ_HEAD);
    if (tail - head >= SMEM_SHM_SDMA_RS_REQ_SLOTS) {
        return false; /* 满环预算耗尽, 邮件未投递(消费方 wait 超时暴露) */
    }
    __gm__ uint8_t *slot = res + SMEM_SHM_SDMA_RS_REQ_RING + (tail % SMEM_SHM_SDMA_RS_REQ_SLOTS) * 32;
    uint8_t sqeOp = 0;
    if (op == SMEM_SHM_SDMA_OP_ADD_FP32) {
        sqeOp = SMEM_SDMA_SQE_OP_FP32_ADD;
    } else if (op == SMEM_SHM_SDMA_OP_ADD_FP16) {
        sqeOp = SMEM_SDMA_SQE_OP_FP16_ADD;
    }
    const uint64_t immOut = (sqeOp != 0) ? SdmaEncodeOpImm(imm, sqeOp) : imm;
    *reinterpret_cast<__gm__ volatile uint64_t *>(slot + 0) = reinterpret_cast<uint64_t>(src);
    *reinterpret_cast<__gm__ volatile uint64_t *>(slot + 8) = reinterpret_cast<uint64_t>(dst);
    *reinterpret_cast<__gm__ volatile uint64_t *>(slot + 16) = len;
    *reinterpret_cast<__gm__ volatile uint64_t *>(slot + 24) = immOut;
    AscendC::GlobalTensor<int32_t> t;
    t.SetGlobalBuffer(reinterpret_cast<__gm__ int32_t *>(slot), 8);
    AscendC::DataCacheCleanAndInvalid<int32_t, AscendC::CacheLine::SINGLE_CACHE_LINE>(t);
    SdmaWriteCell(res, SMEM_SHM_SDMA_RS_REQ_TAIL, tail + 1);
    return true;
}

/* wait 的预计算基址变体(res = smem_shm_sdma_reserved(gva) 缓存值) */
SMEM_SHM_INLINE_AICORE smem_shm_sdma_mail_t smem_shm_sdma_wait_at(__gm__ uint8_t *res)
{
    smem_shm_sdma_mail_t m = {0, 0, 0, 0, SMEM_SHM_SDMA_MAIL_TIMEOUT};
    const uint64_t head = SdmaReadCell(res, SMEM_SHM_SDMA_RS_ARR_HEAD);
    const uint64_t expect = head + 1; /* 戳=seq+1(1 基), 0 表空——防 seq0 幻影 */
    uint64_t stamp = 0;
    uint32_t delay = SMEM_SHM_SDMA_BACKOFF_START;
    for (uint64_t i = 0; i < SMEM_SHM_SDMA_POLL_MAX_ITER; i++) {
        stamp = SdmaReadCell(res, SMEM_SHM_SDMA_RS_ARR_STAMP + (head % SMEM_SHM_SDMA_RS_ARR_SLOTS) * 8);
        if (stamp == expect) {
            break;
        }
        /* 溢出判定: 戳超前期望即该槽已被越环覆写(正常时序槽值只能 ==expect 或上一轮
         * 旧值 <expect; 任何超前都意味着消费落后至少一整环, 差值大小不影响结论) */
        if (stamp > expect) {
            m.status = SMEM_SHM_SDMA_MAIL_OVERRUN;
            m.seq = static_cast<uint32_t>(head);
            return m;
        }
        SdmaPollBackoff(i, delay);
    }
    if (stamp != expect) {
        m.status = SMEM_SHM_SDMA_MAIL_TIMEOUT;
        m.seq = static_cast<uint32_t>(head);
        return m;
    }
    __gm__ uint8_t *slot = res + SMEM_SHM_SDMA_RS_ARR_MAIL + (head % SMEM_SHM_SDMA_RS_ARR_SLOTS) * 32;
    uint64_t f0 = 0, f1 = 0, f2 = 0, f3 = 0;
    SdmaReadMail(slot, &f0, &f1, &f2, &f3);
    m.dst = f1;
    m.len = f2;
    m.imm = f3;
    m.seq = static_cast<uint32_t>(f0 - 1); /* f0 为 1 基戳 */
    m.status = SMEM_SHM_SDMA_MAIL_OK;
    SdmaWriteCell(res, SMEM_SHM_SDMA_RS_ARR_HEAD, head + 1);
    return m;
}

/* quiet 的预计算基址变体(res = smem_shm_sdma_reserved(gva) 缓存值) */
SMEM_SHM_INLINE_AICORE bool smem_shm_sdma_quiet_at(__gm__ uint8_t *res)
{
    const uint64_t tail = SdmaReadCell(res, SMEM_SHM_SDMA_RS_REQ_TAIL);
    if (tail == 0) {
        return true;
    }
    const uint64_t expect = tail; /* 末封戳(1 基: 末封 seq=tail-1 → 戳=tail) */
    uint32_t delay = SMEM_SHM_SDMA_BACKOFF_START;
    for (uint64_t i = 0; i < SMEM_SHM_SDMA_POLL_MAX_ITER; i++) {
        const uint64_t stamp = SdmaReadCell(res, SMEM_SHM_SDMA_RS_QUIET +
                                             ((tail - 1) % SMEM_SHM_SDMA_RS_QUIET_SLOTS) * 8);
        if (stamp == expect) { /* 末封已落地(FIFO 序即全部) */
            return true;
        }
        SdmaPollBackoff(i, delay);
    }
    return false;
}

/* ======================= gva 便捷封装(转发 _at 变体) ======================= */

/* 投递一封搬运请求邮件(源→目的, 带用户标签)。返回 true=已投递。
 * 满环时渐进退避自旋(背压); 预算耗尽返回 false。
 * len 须 4 字节对齐(SDMA 搬运粒度, 长度直传 SQE 不做补齐)。
 * 高频路径可 kernel 入口 smem_shm_sdma_reserved(gva) 预计算后走 _at 变体。 */
SMEM_SHM_INLINE_AICORE bool smem_shm_sdma_signal(__gm__ void *gva, __gm__ void *src, __gm__ void *dst,
                                                 uint64_t len, uint64_t imm,
                                                 smem_shm_sdma_op_t op = SMEM_SHM_SDMA_OP_COPY)
{
    return smem_shm_sdma_signal_at(smem_shm_sdma_reserved(gva), src, dst, len, imm, op);
}

/* 按序消费下一封到达邮件(阻塞自旋+渐进退避, 超时/溢出由 status 表达)。
 * 邮件 {dst,len,imm} 自描述, 接收方直接定位数据。 */
SMEM_SHM_INLINE_AICORE smem_shm_sdma_mail_t smem_shm_sdma_wait(__gm__ void *gva)
{
    return smem_shm_sdma_wait_at(smem_shm_sdma_reserved(gva));
}

/* 等待本端全部已投递请求落地(源缓冲复用/改写前调用)。
 * 依据: 完成戳按 SQE FIFO 序落地, 末封戳到即全部到。 */
SMEM_SHM_INLINE_AICORE bool smem_shm_sdma_quiet(__gm__ void *gva)
{
    return smem_shm_sdma_quiet_at(smem_shm_sdma_reserved(gva));
}

#endif // __MEMFABRIC_SMEM_AI_CORE_BASE_SDMA_H__
