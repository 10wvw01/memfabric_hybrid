#pragma once

template<class T>
class LocalCopy {
public:
    __aicore__ inline LocalCopy()
    {
        pipe.InitBuffer(storage, 4096);
    }
    __aicore__ inline void Run(__gm__ T *src, __gm__ T *dst, uint64_t count, bool clear, bool add)
    {
        auto tmp = storage.Get<T>();
        for (uint64_t pos = 0; pos < count; pos += 4096 / sizeof(T)) {
            uint32_t n = static_cast<uint32_t>(count - pos);
            if (n > 4096 / sizeof(T))
                n = 4096 / sizeof(T);
            AscendC::GlobalTensor<T> out, in;
            out.SetGlobalBuffer(dst + pos, n);
            if (clear) {
                AscendC::Duplicate(tmp, static_cast<T>(0), n);
            } else {
                in.SetGlobalBuffer(src + pos, n);
                AscendC::DataCopy(tmp, in, n);
            }
            AscendC::PipeBarrier<PIPE_ALL>();
            if (add)
                AscendC::SetAtomicAdd<T>();
            AscendC::DataCopy(out, tmp, n);
            AscendC::PipeBarrier<PIPE_ALL>();
            if (add)
                AscendC::SetAtomicNone();
        }
    }

private:
    AscendC::TPipe pipe;
    AscendC::TBuf<AscendC::TPosition::VECCALC> storage;
};

__aicore__ inline void Maintain(__gm__ void *ptr, uint64_t bytes, bool enabled)
{
    AscendC::PipeBarrier<PIPE_ALL>();
    if (enabled)
        SdmaMaintainCache(ptr, bytes);
    AscendC::PipeBarrier<PIPE_ALL>();
}

template<class T>
__aicore__ inline void ConsumeRows(LocalCopy<T> &copy, __gm__ T *src, __gm__ T *out, uint32_t rows, uint32_t tile,
                                   bool maintain)
{
    for (uint32_t row = 0; row < rows; row += tile) {
        uint32_t n = rows - row < tile ? rows - row : tile;
        Maintain(src + row * 2048, uint64_t(n) * 2048 * sizeof(T), maintain);
        copy.Run(src + row * 2048, out + row * 2048, uint64_t(n) * 2048, false, false);
    }
}

template<class T>
__global__ __aicore__ void Consume(__gm__ T *src, __gm__ T *out, uint32_t rows, uint32_t tile)
{
    KERNEL_TASK_TYPE_DEFAULT(KERNEL_TYPE_AICORE);
    LocalCopy<T> copy;
    ConsumeRows(copy, src, out, rows, tile, false);
}

template<class T>
__aicore__ inline bool Transfer(const smem_shm_sdma_ctx_t &ctx, __gm__ T *src, __gm__ T *peer, uint64_t bytes,
                                __gm__ uint64_t *done, uint64_t epoch, bool fused)
{
    auto op = sizeof(T) == 4 ? SMEM_SHM_SDMA_OP_ADD_FP32 : SMEM_SHM_SDMA_OP_ADD_FP16;
    if (fused)
        return smem_shm_sdma_put_signal_nbi_at(ctx, src, peer, bytes, done, epoch, op) && smem_shm_sdma_quiet_at(ctx);
    return smem_shm_sdma_put_at(ctx, src, peer, bytes, op) && smem_shm_sdma_signal_set_at(ctx, done, epoch);
}

template<class T>
__global__ __aicore__ void Collective(__gm__ uint8_t *gva, __gm__ T *out, __gm__ uint32_t *status, uint32_t rows,
                                      uint32_t tile, uint32_t mode, uint64_t epoch)
{
    KERNEL_TASK_TYPE_DEFAULT(KERNEL_TYPE_AICORE);
    LocalCopy<T> copy;
    auto ctx = smem_shm_sdma_ctx(gva);
    auto own = gva + ctx.rank * ctx.symm;
    auto peer = gva + (1 - ctx.rank) * ctx.symm;
    const bool maintain = mode != 3;
    const uint32_t step = mode == 0 ? tile : rows;
    for (uint32_t row = 0, piece = 0; row < rows; row += step, ++piece) {
        uint32_t valid = rows - row < step ? rows - row : step;
        uint64_t count = uint64_t(valid) * 2048;
        auto src = reinterpret_cast<__gm__ T *>(own) + row * 2048;
        auto acc = reinterpret_cast<__gm__ T *>(own + ACC_BASE) + row * 2048;
        auto dst = reinterpret_cast<__gm__ T *>(peer + ACC_BASE) + row * 2048;
        uint64_t flag = FLAG_BASE + piece * 256;
        status[0] = 1;
        copy.Run(src, acc, count, true, false);
        Maintain(acc, count * sizeof(T), maintain);
        if (!smem_shm_sdma_signal_set_at(ctx, reinterpret_cast<__gm__ uint64_t *>(peer + flag), epoch) ||
            !SdmaWaitReady(own, flag, epoch))
            return;
        status[0] = 2;
        copy.Run(src, acc, count, false, true);
        Maintain(src, count * sizeof(T), maintain);
        if (!Transfer(ctx, src, dst, count * sizeof(T), reinterpret_cast<__gm__ uint64_t *>(peer + flag + 128), epoch,
                      mode >= 2))
            return;
        status[0] = 3;
        if (!SdmaWaitReady(own, flag + 128, epoch))
            return;
        ConsumeRows(copy, acc, out + row * 2048, valid, tile, maintain);
    }
    status[0] = 0;
    SdmaMaintainCache(status, 4);
}
