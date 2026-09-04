/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

#include "kernel_operator.h"
#include "smem_shm_aicore_base_api.h"

constexpr int32_t BLOCK_LEN = SMEM_SHM_ALIGN_SIZE / sizeof(int64_t);

class KernelShmShift {
public:
    __aicore__ inline KernelShmShift() {}
    __aicore__ inline void Init(GM_ADDR gva, GM_ADDR localInput, GM_ADDR localOutput)
    {
        gvaSt = (__gm__ int64_t *)gva;
        inputGm = (__gm__ int64_t *)localInput;
        outputGm = (__gm__ int64_t *)localOutput;
        pipe.InitBuffer(bufQueue, 1, SMEM_SHM_ALIGN_SIZE);
    }
    __aicore__ inline void Process()
    {
        AscendC::LocalTensor<int64_t> bufTensor = bufQueue.AllocTensor<int64_t>();

        uint32_t rank = smem_shm_get_global_rank(0);
        uint32_t rankSize = smem_shm_get_global_rank_size(0);
        uint64_t symSize = smem_shm_get_symmetric_size(0);
        uint32_t targetRank = (rank + 1) % rankSize;
        uint32_t srcRank = (rank + rankSize - 1) % rankSize;

        __gm__ int64_t *targetSlot = reinterpret_cast<__gm__ int64_t *>(
            reinterpret_cast<uint64_t>(gvaSt) + symSize * targetRank);
        __gm__ int64_t *srcSlot = reinterpret_cast<__gm__ int64_t *>(
            reinterpret_cast<uint64_t>(gvaSt) + symSize * srcRank);

        AscendC::GlobalTensor<int64_t> inputGmT;
        inputGmT.SetGlobalBuffer(inputGm);
        AscendC::GlobalTensor<int64_t> targetSlotT;
        targetSlotT.SetGlobalBuffer(targetSlot);
        AscendC::GlobalTensor<int64_t> srcSlotT;
        srcSlotT.SetGlobalBuffer(srcSlot);
        AscendC::GlobalTensor<int64_t> outputGmT;
        outputGmT.SetGlobalBuffer(outputGm);

        AscendC::DataCopy(bufTensor, inputGmT, BLOCK_LEN);
        AscendC::DataCopy(targetSlotT, bufTensor, BLOCK_LEN);

        AscendC::PipeBarrier<PIPE_ALL>();

        AscendC::DataCopy(bufTensor, srcSlotT, BLOCK_LEN);
        AscendC::DataCopy(outputGmT, bufTensor, BLOCK_LEN);

        bufQueue.FreeTensor(bufTensor);
    }

private:
    AscendC::TPipe pipe;
    AscendC::TQue<AscendC::TPosition::VECIN, 1> bufQueue;
    __gm__ int64_t *gvaSt, *inputGm, *outputGm;
};

extern "C" __global__ __aicore__ void shm_shift_kernel(GM_ADDR gva, GM_ADDR localInput, GM_ADDR localOutput)
{
    KernelShmShift op;
    op.Init(gva, localInput, localOutput);
    op.Process();
}

void shm_shift_kernel_do(void *stream, uint8_t *gva, int64_t *localInput, int64_t *localOutput)
{
    shm_shift_kernel<<<1, nullptr, stream>>>(gva, (uint8_t *)localInput, (uint8_t *)localOutput);
}
