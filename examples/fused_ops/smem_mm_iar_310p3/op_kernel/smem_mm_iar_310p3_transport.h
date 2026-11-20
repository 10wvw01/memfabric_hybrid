// smem_mm_iar_310p3_transport.h —— 传输层：SDMA 搬运、标记轮询/探测、
// REDUCE/GATHER 传输段发射。
// 游标推进与事件循环留在调度器主循环（smem_mm_iar310p3.cpp）。
#ifndef SMEM_MM_IAR310P3_TRANSPORT_H
#define SMEM_MM_IAR310P3_TRANSPORT_H

#include "kernel_operator.h"
#include "smem_mm_iar_310p3_protocol.h"
#include "smem_shm_aicore_base_sdma.h"

namespace iar310p3_kernel {

using namespace AscendC;

// flush：整缓存写回并失效（ENTIRE_DATA_CACHE，非单缓存行）。用于 Cube 写完
// work 区、错误码落盘后，保证写入对其他核/对端经 MTE 可见。已知代价：顺带
// 逐出权重缓存行，属正确性换取。
__aicore__ inline void flush(__gm__ uint8_t* valid)
{
    GlobalTensor<int32_t> fence;
    fence.SetGlobalBuffer(reinterpret_cast<__gm__ int32_t*>(valid), 16);
    DataCacheCleanAndInvalid<int32_t, CacheLine::ENTIRE_DATA_CACHE>(fence);
}

// check：协议自检断言。失败时错误码写入 status 落盘并 Trap 终止，
// 宿主从 status 读码定位失败原因（ProtocolError）。
__aicore__ inline void check(bool ok, __gm__ uint8_t* status, ProtocolError code)
{
    if (ok) return;
    *reinterpret_cast<__gm__ volatile uint64_t*>(status) = static_cast<uint64_t>(code);
    flush(status);
    Trap();
}

// poll_flag：阻塞轮询单个 8 字节标记槽，直到槽值等于本批次 epoch（该槽由
// 对端以 8 字节 COPY 信号写入）。经 MTE 搬运 + 指数退避；超过 kPollBudget
// 视为死等。返回总轮询次数（写 trace 用）。
__aicore__ inline uint32_t poll_flag(LocalTensor<int64_t>& poll, __gm__ uint8_t* slot,
                                     uint32_t epoch, __gm__ uint8_t* status, ProtocolError timeoutCode)
{
    GlobalTensor<int64_t> flagGm;
    flagGm.SetGlobalBuffer(reinterpret_cast<__gm__ int64_t*>(slot), kFlagSlotQwords);
    uint32_t spins = 0, delay = kBackoffStart;
    while (true) {
        DataCopy(poll, flagGm, kFlagSlotQwords);
        if (static_cast<uint32_t>(poll.GetValue(0)) == epoch) return spins;
        check(++spins < kPollBudget, status, timeoutCode);
        for (uint32_t i = 0; i < delay; ++i) __asm__ __volatile__("" ::: "memory");
        if ((spins & (kBackoffStep - 1u)) == 0 && delay < kBackoffMax) delay <<= 1;
    }
}

// probe_flag：单槽非阻塞探测——只搬一次、只判一次。调度器的事件循环每轮
// 把所有门重探一遍，而不是阻塞在单个门上。
__aicore__ inline bool probe_flag(LocalTensor<int64_t>& poll, __gm__ uint8_t* slot, uint32_t epoch)
{
    GlobalTensor<int64_t> flagGm;
    flagGm.SetGlobalBuffer(reinterpret_cast<__gm__ int64_t*>(slot), kFlagSlotQwords);
    DataCopy(poll, flagGm, kFlagSlotQwords);
    return static_cast<uint32_t>(poll.GetValue(0)) == epoch;
}

// probe_seats：非阻塞探测一个矩阵乘组的部分组完成通知槽，返回已到齐的
// 数量（调度器据此判断该子组的生产核是否全部完成）。
__aicore__ inline uint32_t probe_seats(LocalTensor<int64_t>& poll, __gm__ uint8_t* seats,
                                        uint32_t group, uint32_t firstSlot, uint32_t count,
                                        uint32_t epoch)
{
    GlobalTensor<int64_t> seatsGm;
    seatsGm.SetGlobalBuffer(reinterpret_cast<__gm__ int64_t*>(seats + uint64_t{group} * kGroupSeatsBytes),
                       kFlagSlotQwords * kCores);
    DataCopy(poll, seatsGm, kFlagSlotQwords * kCores);
    uint32_t doneCount = 0;
    for (uint32_t slotIdx = 0; slotIdx < count; ++slotIdx) {
        if (static_cast<uint32_t>(poll.GetValue((firstSlot + slotIdx) * kFlagSlotQwords)) == epoch) ++doneCount;
    }
    return doneCount;
}

// real_rows_in_group：组 group 的实属行数（去填充契约）。整组返回 1024，
// 末尾组只返回剩余行数；rows%256==0 保证实属行数是 128 行切片的整数倍。
__aicore__ inline uint32_t real_rows_in_group(uint32_t rows, uint32_t group)
{
    const uint32_t firstRow = group * kCommRows;
    return firstRow + kCommRows <= rows ? kCommRows : rows - firstRow;
}

// sub_bytes_of：通信子组 subGroup 的实际字节数（末子组可不足整子组）。
// rows%256==0 保证子组字节数是 1MiB 的整数倍：半区不小于 512KiB 且按 512
// 对齐，尾部拷出分支的四分之一不少于 256KiB，都在包长与搬运对齐约束内。
__aicore__ inline uint64_t sub_bytes_of(uint32_t rows, uint64_t subBytes, uint32_t subGroup)
{
    const uint64_t total = uint64_t{rows} * kRowBytes;
    const uint64_t firstByte = uint64_t{subGroup} * subBytes;
    return firstByte + subBytes <= total ? subBytes : total - firstByte;
}

// issue_reduce_leg：发出 REDUCE 传输段——把子组 subGroup 中本端不拥有的
// 半区（rank 决定上下哪半）按 packet 粒度分包，以 ADD_FP16 加到对端同偏移
// 位置（SDMA 在传输途中完成加法，即"硬件归约"），数据包全部发出后再发
// 8 字节 RDONE 标记。orch 按发送方保序 ⇒ 对端见 RDONE 即整个数据段已落位。
// subLen 为子组实属字节数（末子组可不足），半区 = subLen/2。
__aicore__ inline void issue_reduce_leg(__gm__ uint8_t* sdmaRes, __gm__ uint8_t* work, __gm__ uint8_t* peer,
                                        __gm__ uint8_t* status, uint32_t subGroup, uint64_t subBytes,
                                        uint64_t subLen, uint64_t packet, uint32_t rank, uint32_t epoch)
{
    const uint64_t halfBytes = subLen / 2;
    const uint64_t reduceOff = uint64_t{subGroup} * subBytes + uint64_t{1 - rank} * halfBytes;
    for (uint64_t pos = 0; pos < halfBytes; pos += packet) {
        const uint64_t chunkBytes = halfBytes - pos < packet ? halfBytes - pos : packet;
        check(smem_shm_sdma_signal_at(sdmaRes, work + reduceOff + pos, peer + reduceOff + pos, chunkBytes,
              device_tag(REDUCE, epoch, subGroup, static_cast<uint32_t>(pos / packet)),
              SMEM_SHM_SDMA_OP_ADD_FP16), status, ProtocolError::SignalDataFail);
    }
    check(smem_shm_sdma_signal_at(sdmaRes, work + kCtrlOff, peer + kPeerRdyOff + uint64_t{subGroup} * kFlagSlotBytes,
          8, device_tag(RDONE, epoch, subGroup, 0), SMEM_SHM_SDMA_OP_COPY), status, ProtocolError::SignalMarkFail);
}

// issue_gather_leg：发出 GATHER 传输段——把子组 subGroup 中本端拥有的半区
//（其终值已在 REDUCE 阶段收敛完毕）按 packet 粒度复制回对端同偏移位置，随后
// 发 8 字节 GDONE 标记。发射门是对端 RDONE（由调度器在调用前探测确认）。
// subLen 为子组实属字节数（末子组可不足），半区 = subLen/2。
__aicore__ inline void issue_gather_leg(__gm__ uint8_t* sdmaRes, __gm__ uint8_t* work, __gm__ uint8_t* peer,
                                        __gm__ uint8_t* status, uint32_t subGroup, uint64_t subBytes,
                                        uint64_t subLen, uint64_t packet, uint32_t rank, uint32_t epoch)
{
    const uint64_t halfBytes = subLen / 2;
    const uint64_t gatherOff = uint64_t{subGroup} * subBytes + uint64_t{rank} * halfBytes;
    for (uint64_t pos = 0; pos < halfBytes; pos += packet) {
        const uint64_t chunkBytes = halfBytes - pos < packet ? halfBytes - pos : packet;
        check(smem_shm_sdma_signal_at(sdmaRes, work + gatherOff + pos, peer + gatherOff + pos, chunkBytes,
              device_tag(GATHER, epoch, subGroup, static_cast<uint32_t>(pos / packet)),
              SMEM_SHM_SDMA_OP_COPY), status, ProtocolError::SignalDataFail);
    }
    check(smem_shm_sdma_signal_at(sdmaRes, work + kCtrlOff, peer + kReadyOff + uint64_t{subGroup} * kFlagSlotBytes,
          8, device_tag(GDONE, epoch, subGroup, 0), SMEM_SHM_SDMA_OP_COPY), status, ProtocolError::SignalMarkFail);
}

}  // namespace iar310p3_kernel

#endif  // SMEM_MM_IAR310P3_TRANSPORT_H
