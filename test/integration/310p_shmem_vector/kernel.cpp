#include "kernel_operator.h"
#include "smem_shm_aicore_base_api.h"

constexpr uint64_t kBlockBytes = 4096;
constexpr uint64_t kFlagBase = 1024 * 1024;

__aicore__ inline void Maintain(__gm__ void *ptr)
{
    AscendC::GlobalTensor<int32_t> gm;
    gm.SetGlobalBuffer(reinterpret_cast<__gm__ int32_t *>(ptr), 16);
    AscendC::DataCacheCleanAndInvalid<int32_t, AscendC::CacheLine::SINGLE_CACHE_LINE>(gm);
}

__aicore__ inline bool RejectDisabled(const smem_shm_sdma_ctx_t &ctx, __gm__ uint8_t *own,
                                     __gm__ uint8_t *peer, __gm__ uint64_t *flag)
{
    return !smem_shm_sdma_put_nbi_at(ctx, own, peer, 64) &&
           !smem_shm_sdma_get_nbi_at(ctx, peer, own, 64) &&
           !smem_shm_sdma_put_signal_nbi_at(ctx, own, peer, 64, flag, 1) &&
           !smem_shm_sdma_signal_set_nbi_at(ctx, flag, 1) && !smem_shm_sdma_quiet_at(ctx);
}

__aicore__ inline bool Transfer(const smem_shm_sdma_ctx_t &ctx, __gm__ uint8_t *own,
                              __gm__ uint8_t *peer, __gm__ uint64_t *flag, uint32_t rounds)
{
    for (uint32_t i = 1; i <= rounds; ++i) {
        Maintain(own);
        if (!smem_shm_sdma_put_nbi_at(ctx, own, peer + 256, 64) ||
            !smem_shm_sdma_get_nbi_at(ctx, peer, own + 512, 64) ||
            !smem_shm_sdma_put_signal_nbi_at(ctx, own, peer + 768, 64, flag, i) ||
            !smem_shm_sdma_signal_set_nbi_at(ctx, flag + 8, i) || !smem_shm_sdma_quiet_at(ctx)) {
            return false;
        }
    }
    return true;
}

extern "C" __global__ __aicore__ void vectorQueues(GM_ADDR base, GM_ADDR status, uint32_t mask, uint32_t rounds)
{
    KERNEL_TASK_TYPE_DEFAULT(KERNEL_TYPE_MIX_VECTOR_CORE);
    const auto ctx = smem_shm_sdma_ctx(base);
    auto *row = reinterpret_cast<__gm__ uint32_t *>(status) + AscendC::GetBlockIdx() * 32;
    row[0] = 0xC04E1234;
    row[1] = ctx.block;
#ifdef __ENABLE_VECTOR_CORE__
    row[2] = 1;
#else
    row[2] = 0;
#endif
    row[3] = ctx.producerMask;
    row[4] = 99;
    row[5] = 1;
    Maintain(row);
    auto *gva = reinterpret_cast<__gm__ uint8_t *>(base);
    auto *own = gva + ctx.rank * ctx.symm + ctx.block * kBlockBytes;
    auto *peer = gva + (1 - ctx.rank) * ctx.symm + ctx.block * kBlockBytes;
    auto *flag = reinterpret_cast<__gm__ uint64_t *>(gva + (1 - ctx.rank) * ctx.symm + kFlagBase + ctx.block * 128);
    row[5] = 2;
    Maintain(row);
    bool ok = ctx.producerMask == mask;
    if (ctx.rank == 0) {
        ok = ok && ((mask & (1U << ctx.block)) ? Transfer(ctx, own, peer, flag, rounds)
                                                   : RejectDisabled(ctx, own, peer, flag));
        row[5] = 3;
        Maintain(row);
        auto invalid = ctx;
        invalid.block = 15;
        invalid.producerMask |= SMEM_SHM_SDMA_RESERVED_PRODUCER_MASK;
        ok = ok && RejectDisabled(invalid, own, peer, flag);
        invalid.block = UINT32_MAX;
        ok = ok && RejectDisabled(invalid, own, peer, flag);
    }
    row[4] = ok ? 0 : 1;
    Maintain(row);
}
