#ifndef MEMFABRIC_EXAMPLE_SDMA_READY_H
#define MEMFABRIC_EXAMPLE_SDMA_READY_H

#include "smem_shm_aicore_base_sdma.h"

// Application-owned slots use separate cache lines for incoming state and outgoing scratch.
#define SDMA_EXAMPLE_INLINE __attribute__((always_inline)) __aicore__ inline
constexpr uint64_t SDMA_EXAMPLE_READY_STRIDE = 128;

SDMA_EXAMPLE_INLINE void SdmaExampleCache(__gm__ void *base, uint64_t bytes)
{
    for (uint64_t off = 0; off < bytes; off += 64) {
        AscendC::GlobalTensor<int32_t> gm;
        gm.SetGlobalBuffer(reinterpret_cast<__gm__ int32_t *>(static_cast<__gm__ uint8_t *>(base) + off), 2);
        AscendC::DataCacheCleanAndInvalid<int32_t, AscendC::CacheLine::SINGLE_CACHE_LINE>(gm);
    }
}

// Call only after the payload quiet has succeeded. Each slot has one remote writer.
SDMA_EXAMPLE_INLINE bool SdmaExamplePublish(const smem_shm_sdma_ctx_t &ctx, __gm__ uint8_t *own,
    __gm__ uint8_t *peer, uint64_t sourceOffset, uint64_t targetOffset, uint64_t sequence)
{
    auto scratch = own + sourceOffset + 64;
    *reinterpret_cast<__gm__ volatile uint64_t *>(scratch) = sequence;
    SdmaExampleCache(scratch, 8);
    return smem_shm_sdma_put_at(ctx, scratch, peer + targetOffset, 8);
}

SDMA_EXAMPLE_INLINE bool SdmaExampleWait(__gm__ uint8_t *own, uint64_t offset, uint64_t sequence)
{
    auto flag = own + offset;
    for (uint64_t i = 0; i < SMEM_SHM_SDMA_POLL_MAX_ITER; ++i) {
        SdmaExampleCache(flag, 8);
        if (*reinterpret_cast<__gm__ volatile uint64_t *>(flag) == sequence) {
            return true;
        }
    }
    return false;
}

#undef SDMA_EXAMPLE_INLINE
#endif
