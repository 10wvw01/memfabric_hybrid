#ifndef MEMFABRIC_SDMA_READY_H
#define MEMFABRIC_SDMA_READY_H

#include "smem_shm_aicore_base_sdma.h"

// 应用状态槽将接收标志和发送暂存区放在不同缓存行，避免相互覆盖。
#define SDMA_READY_INLINE __attribute__((always_inline)) __aicore__ inline
constexpr uint64_t SDMA_READY_SLOT_STRIDE = 128;

SDMA_READY_INLINE void SdmaMaintainCache(__gm__ void *base, uint64_t bytes)
{
    for (uint64_t off = 0; off < bytes; off += 64) {
        AscendC::GlobalTensor<int32_t> gm;
        gm.SetGlobalBuffer(reinterpret_cast<__gm__ int32_t *>(static_cast<__gm__ uint8_t *>(base) + off), 2);
        AscendC::DataCacheCleanAndInvalid<int32_t, AscendC::CacheLine::SINGLE_CACHE_LINE>(gm);
    }
}

// 数据 quiet 成功后才能发布就绪状态；每个接收槽仅由一个对端写入。
SDMA_READY_INLINE bool SdmaPublishReady(const smem_shm_sdma_ctx_t &ctx, __gm__ uint8_t *own,
    __gm__ uint8_t *peer, uint64_t sourceOffset, uint64_t targetOffset, uint64_t sequence)
{
    auto scratch = own + sourceOffset + 64;
    *reinterpret_cast<__gm__ volatile uint64_t *>(scratch) = sequence;
    SdmaMaintainCache(scratch, 8);
    return smem_shm_sdma_put_at(ctx, scratch, peer + targetOffset, 8);
}

SDMA_READY_INLINE bool SdmaWaitReady(__gm__ uint8_t *own, uint64_t offset, uint64_t sequence)
{
    auto flag = own + offset;
    for (uint64_t i = 0; i < SMEM_SHM_SDMA_POLL_MAX_ITER; ++i) {
        SdmaMaintainCache(flag, 8);
        if (*reinterpret_cast<__gm__ volatile uint64_t *>(flag) == sequence) {
            return true;
        }
    }
    return false;
}

#undef SDMA_READY_INLINE
#endif
