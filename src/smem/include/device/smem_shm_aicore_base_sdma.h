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

/* 310P SHMEM 设备传输：AICPU 为各 AICore block 提交 SDMA 请求。
 * put/get 与 quiet 确认 GM 传输完成；调用方负责缓存维护，
 * 并通过应用协议协调远端数据就绪和缓冲区复用。
 * 每个 block 由一个执行主体提交请求，quiet 仅等待该 block 的传输。
 */
#ifndef __MEMFABRIC_SMEM_AI_CORE_BASE_SDMA_H__
#define __MEMFABRIC_SMEM_AI_CORE_BASE_SDMA_H__

#include "smem_shm_aicore_base_define.h"
#include "smem_shm_aicore_base_meta.h"
#include "smem_shm_sdma_layout.h"
typedef enum smem_shm_sdma_op {
    SMEM_SHM_SDMA_OP_COPY = 0,     /* dst = src */
    SMEM_SHM_SDMA_OP_ADD_FP32 = 1, /* 搬运+reduce dst += src (fp32 逐元素) */
    SMEM_SHM_SDMA_OP_ADD_FP16 = 2, /* 搬运+reduce dst += src (fp16 逐元素) */
} smem_shm_sdma_op_t;

struct smem_shm_sdma_ctx_t {
    __gm__ uint8_t *res; /* 本端段尾保留区基址 */
    uint64_t symm;       /* 对称段大小 */
    uint64_t phys;       /* 段物理大小 */
    uint32_t rank;       /* 本端 rank */
    uint32_t rankSize;   /* 世界大小 */
    uint32_t block;      /* 本 AICore block 号(GetBlockIdx, 队列下标 block 维) */
};

#ifdef __cplusplus
extern "C" {
#endif

constexpr uint64_t SMEM_SDMA_IMM_MAGIC = 0x5244ULL;
constexpr uint8_t SMEM_SDMA_SQE_OP_FP32_ADD = 0x02;
constexpr uint8_t SMEM_SDMA_SQE_OP_FP16_ADD = 0x03;

SMEM_SHM_INLINE_AICORE uint64_t SdmaEncodeOp(uint8_t sqeOpcode)
{
    return (SMEM_SDMA_IMM_MAGIC << 48) | (static_cast<uint64_t>(sqeOpcode) << 40);
}

/* 轮询预算(探测次数上限; 超时为错误路径, 由调用方处置) */
constexpr uint64_t SMEM_SHM_SDMA_POLL_MAX_ITER = 100000000ULL;
/* 渐进退避: 前 SPIN_ITERS 轮纯自旋保低延迟, 之后空转退避渐进加倍(AICore 无 usleep) */
constexpr uint64_t SMEM_SHM_SDMA_SPIN_ITERS = 4096;
constexpr uint32_t SMEM_SHM_SDMA_BACKOFF_START = 64;
constexpr uint32_t SMEM_SHM_SDMA_BACKOFF_MAX = 16384;

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

/* 失效缓存后读取 AICPU 或 SDMA 写入的状态。 */
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

/* ============================ 公开 API ============================ */

SMEM_SHM_INLINE_AICORE smem_shm_sdma_ctx_t smem_shm_sdma_ctx(__gm__ void *gva)
{
    smem_shm_sdma_ctx_t c;
    c.symm = smem_shm_get_symmetric_size(0);
    c.rank = smem_shm_get_global_rank(0);
    c.rankSize = smem_shm_get_global_rank_size(0);
    c.phys = smem_shm_get_phys_size(0);
    c.res = static_cast<__gm__ uint8_t *>(gva) + c.symm * c.rank + (c.phys - SMEM_SHM_SDMA_RESERVED);
    c.block = static_cast<uint32_t>(AscendC::GetBlockIdx()); /* v31: 队列按 block 划分 */
    return c;
}

/* 本端有效对端环数(rankSize 异常值封顶于布局上限) */
SMEM_SHM_INLINE_AICORE uint32_t smem_shm_sdma_peer_cnt(const smem_shm_sdma_ctx_t &c)
{
    return (c.rankSize < SMEM_SHM_SDMA_MAX_RANKS) ? (c.rankSize - 1U) : (SMEM_SHM_SDMA_MAX_RANKS - 1U);
}

SMEM_SHM_INLINE_AICORE bool SdmaSubmitAt(const smem_shm_sdma_ctx_t &ctx, __gm__ void *src,
                                                    __gm__ void *dst, uint64_t len,
                                                    smem_shm_sdma_op_t op, bool get)
{
    if (len == 0) { return true; }
    if (len > UINT32_MAX || ((reinterpret_cast<uint64_t>(src) |
        reinterpret_cast<uint64_t>(dst) | len) & 3) != 0 ||
        static_cast<uint32_t>(op) > SMEM_SHM_SDMA_OP_ADD_FP16) { return false; }
    // 根据远端地址选择队列；本端缓冲区允许使用普通设备内存。
    const uint64_t ownSeg = reinterpret_cast<uint64_t>(ctx.res) - (ctx.phys - SMEM_SHM_SDMA_RESERVED);
    const uint64_t srcSeg = reinterpret_cast<uint64_t>(src) & ~(ctx.symm - 1);
    const uint64_t dstSeg = reinterpret_cast<uint64_t>(dst) & ~(ctx.symm - 1);
    const uint64_t poolBase = ownSeg - ctx.symm * ctx.rank;
    const uint64_t poolEnd = poolBase + ctx.symm * ctx.rankSize;
    const bool srcRemote = srcSeg >= poolBase && srcSeg < poolEnd && srcSeg != ownSeg;
    const bool dstRemote = dstSeg >= poolBase && dstSeg < poolEnd && dstSeg != ownSeg;
    const bool isGet = srcRemote;
    const uint64_t remoteSeg = isGet ? srcSeg : dstSeg;
    const int64_t segOff = static_cast<int64_t>(remoteSeg - ownSeg) / static_cast<int64_t>(ctx.symm);
    const int64_t peerQ = static_cast<int64_t>(ctx.rank) + segOff;
    const int64_t rankBound = static_cast<int64_t>((ctx.rankSize < SMEM_SHM_SDMA_MAX_RANKS)
                                                        ? ctx.rankSize : SMEM_SHM_SDMA_MAX_RANKS);
    if (segOff == 0 || peerQ < 0 || peerQ >= rankBound ||
        ctx.block >= SMEM_SHM_SDMA_MAX_BLOCKS ||
        srcRemote == dstRemote || isGet != get || (isGet && op != SMEM_SHM_SDMA_OP_COPY)) {
        return false;
    }
    const uint64_t remoteAddr = reinterpret_cast<uint64_t>(isGet ? src : dst);
    const uint64_t usable = ctx.phys - SMEM_SHM_SDMA_RESERVED;
    const uint64_t offset = remoteAddr - remoteSeg;
    if (offset >= usable || len > usable - offset) { return false; }
    const uint32_t peerRank = static_cast<uint32_t>(peerQ);
    /* v31: 队列 = (对端环下标, 本 block)——各 AICore block 各走各队, 背压仅限本队列 */
    const uint32_t r = SmemShmSdmaQueueIdx(SmemShmSdmaPeerRingIdx(ctx.rank, peerRank), ctx.block);
    const uint64_t ringOff = r * SMEM_SHM_SDMA_RS_REQ_RING_STRIDE;
    const uint64_t tailOff = SMEM_SHM_SDMA_RS_REQ_TAIL + r * SMEM_SHM_SDMA_RS_CURSOR_STRIDE;
    const uint64_t headOff = SMEM_SHM_SDMA_RS_REQ_HEAD + r * SMEM_SHM_SDMA_RS_CURSOR_STRIDE;
    const uint64_t tail = SdmaReadCell(ctx.res, tailOff);
    uint32_t delay = SMEM_SHM_SDMA_BACKOFF_START;
    for (uint64_t i = 0; i < SMEM_SHM_SDMA_POLL_MAX_ITER; i++) {
        const uint64_t head = SdmaReadCell(ctx.res, headOff);
        if (tail - head < SMEM_SHM_SDMA_RS_REQ_SLOTS) {
            break;
        }
        SdmaPollBackoff(i, delay); /* 该环满: 仅背压本环(其他对端环不受影响) */
    }
    const uint64_t head = SdmaReadCell(ctx.res, headOff);
    if (tail - head >= SMEM_SHM_SDMA_RS_REQ_SLOTS) {
        return false; /* 满环预算耗尽，请求未提交 */
    }
    __gm__ uint8_t *slot = ctx.res + SMEM_SHM_SDMA_RS_REQ_RING + ringOff +
                           (tail % SMEM_SHM_SDMA_RS_REQ_SLOTS) * 32;
    uint8_t sqeOp = 0;
    if (op == SMEM_SHM_SDMA_OP_ADD_FP32) {
        sqeOp = SMEM_SDMA_SQE_OP_FP32_ADD;
    } else if (op == SMEM_SHM_SDMA_OP_ADD_FP16) {
        sqeOp = SMEM_SDMA_SQE_OP_FP16_ADD;
    }
    const uint64_t immOut = (sqeOp != 0) ? SdmaEncodeOp(sqeOp) : 0;
    *reinterpret_cast<__gm__ volatile uint64_t *>(slot + 0) = reinterpret_cast<uint64_t>(src);
    *reinterpret_cast<__gm__ volatile uint64_t *>(slot + 8) = reinterpret_cast<uint64_t>(dst);
    *reinterpret_cast<__gm__ volatile uint64_t *>(slot + 16) = len | (1ULL << 63);
    *reinterpret_cast<__gm__ volatile uint64_t *>(slot + 24) = immOut;
    AscendC::GlobalTensor<int32_t> t;
    t.SetGlobalBuffer(reinterpret_cast<__gm__ int32_t *>(slot), 8);
    AscendC::DataCacheCleanAndInvalid<int32_t, AscendC::CacheLine::SINGLE_CACHE_LINE>(t);
    SdmaWriteCell(ctx.res, tailOff, tail + 1);
    return true;
}

/* put/get 的零长度请求直接成功，不修改队列。
 * 远端地址须位于有效共享内存，本端地址允许普通 GM。
 * 地址与长度按 4 字节对齐，长度不超过 UINT32_MAX。
 * NBI 成功表示请求已提交；quiet 或同步接口成功表示 GM 传输完成。
 * 调用方负责源和目标缓存维护，并确认远端源数据就绪。
 * quiet 仅等待调用 block 的传输；远端消费完成由应用协议确认。 */
SMEM_SHM_INLINE_AICORE bool smem_shm_sdma_put_nbi_at(const smem_shm_sdma_ctx_t &ctx,
    __gm__ void *src, __gm__ void *dst, uint64_t bytes,
    smem_shm_sdma_op_t op = SMEM_SHM_SDMA_OP_COPY)
{
    return SdmaSubmitAt(ctx, src, dst, bytes, op, false);
}

SMEM_SHM_INLINE_AICORE bool smem_shm_sdma_get_nbi_at(const smem_shm_sdma_ctx_t &ctx,
    __gm__ void *src, __gm__ void *dst, uint64_t bytes)
{
    return SdmaSubmitAt(ctx, src, dst, bytes, SMEM_SHM_SDMA_OP_COPY, true);
}

/* quiet 的上下文变体: 等待本端全部已投递请求落地(源缓冲复用前)。
 * v29: 逐环判定——每环末封戳到即该环全部到(SQ FIFO 序), 全环到齐即全部到。 */
SMEM_SHM_INLINE_AICORE bool smem_shm_sdma_quiet_at(const smem_shm_sdma_ctx_t &ctx)
{
    const uint32_t peerCnt = smem_shm_sdma_peer_cnt(ctx);
    for (uint32_t pr = 0; pr < peerCnt; pr++) {
        /* v31: 逐发送队列判定——队列 = (对端环下标, 本 block), 各队列末封戳到即该队列全部到 */
        const uint64_t tail = SdmaReadCell(ctx.res, SMEM_SHM_SDMA_RS_REQ_TAIL +
                                                        static_cast<uint64_t>(SmemShmSdmaQueueIdx(pr,
                                                                                                  ctx.block)) *
                                                            SMEM_SHM_SDMA_RS_CURSOR_STRIDE);
        if (tail == 0) {
            continue; /* 该环从未投递 */
        }
        const uint64_t expect = tail; /* 末封戳(1 基: 末封序号=tail-1 → 戳=tail) */
        uint32_t delay = SMEM_SHM_SDMA_BACKOFF_START;
        bool landed = false;
        for (uint64_t i = 0; i < SMEM_SHM_SDMA_POLL_MAX_ITER; i++) {
            const uint64_t stamp = SdmaReadCell(ctx.res,
                                                 SMEM_SHM_SDMA_RS_QUIET +
                                                     static_cast<uint64_t>(SmemShmSdmaQueueIdx(pr,
                                                                                               ctx.block)) *
                                                         SMEM_SHM_SDMA_RS_QUIET_STRIDE +
                                                     ((tail - 1) % SMEM_SHM_SDMA_RS_QUIET_SLOTS) * 8);
            if (stamp == expect) { /* 该环末封已落地(FIFO 序即该环全部) */
                landed = true;
                break;
            }
            SdmaPollBackoff(i, delay);
        }
        if (!landed) {
            return false; /* 该环预算耗尽 */
        }
    }
    return true; /* 全部有投递的环均已到齐 */
}

SMEM_SHM_INLINE_AICORE bool smem_shm_sdma_put_at(const smem_shm_sdma_ctx_t &ctx,
    __gm__ void *src, __gm__ void *dst, uint64_t bytes, smem_shm_sdma_op_t op = SMEM_SHM_SDMA_OP_COPY)
{
    return bytes == 0 || (smem_shm_sdma_put_nbi_at(ctx, src, dst, bytes, op) && smem_shm_sdma_quiet_at(ctx));
}

SMEM_SHM_INLINE_AICORE bool smem_shm_sdma_put_nbi(__gm__ void *gva,
    __gm__ void *src, __gm__ void *dst, uint64_t bytes, smem_shm_sdma_op_t op = SMEM_SHM_SDMA_OP_COPY)
{
    return smem_shm_sdma_put_nbi_at(smem_shm_sdma_ctx(gva), src, dst, bytes, op);
}

SMEM_SHM_INLINE_AICORE bool smem_shm_sdma_put(__gm__ void *gva,
    __gm__ void *src, __gm__ void *dst, uint64_t bytes, smem_shm_sdma_op_t op = SMEM_SHM_SDMA_OP_COPY)
{
    return smem_shm_sdma_put_at(smem_shm_sdma_ctx(gva), src, dst, bytes, op);
}

SMEM_SHM_INLINE_AICORE bool smem_shm_sdma_get_at(const smem_shm_sdma_ctx_t &ctx,
    __gm__ void *src, __gm__ void *dst, uint64_t bytes)
{
    return bytes == 0 || (smem_shm_sdma_get_nbi_at(ctx, src, dst, bytes) && smem_shm_sdma_quiet_at(ctx));
}

SMEM_SHM_INLINE_AICORE bool smem_shm_sdma_get_nbi(__gm__ void *gva,
    __gm__ void *src, __gm__ void *dst, uint64_t bytes)
{
    return smem_shm_sdma_get_nbi_at(smem_shm_sdma_ctx(gva), src, dst, bytes);
}

SMEM_SHM_INLINE_AICORE bool smem_shm_sdma_get(__gm__ void *gva,
    __gm__ void *src, __gm__ void *dst, uint64_t bytes)
{
    return smem_shm_sdma_get_at(smem_shm_sdma_ctx(gva), src, dst, bytes);
}

/* ======================= gva 便捷封装(转发 _at 变体) ======================= */

/* 等待本端全部已投递请求落地(源缓冲复用/改写前调用)。
 * 依据: 每环完成戳按 SQE FIFO 序落地, 各环末封戳到即全部到。 */
SMEM_SHM_INLINE_AICORE bool smem_shm_sdma_quiet(__gm__ void *gva)
{
    return smem_shm_sdma_quiet_at(smem_shm_sdma_ctx(gva));
}

#endif // __MEMFABRIC_SMEM_AI_CORE_BASE_SDMA_H__
