// smem_mm_ar_310p3_transport.h —— 传输层：SDMA 搬运、协议等待、就绪标记收发。
// 单次调用的控制写/通知排空留在调度器主循环（smem_mm_ar310p3.cpp）。
#ifndef SMEM_MM_AR310P3_TRANSPORT_H
#define SMEM_MM_AR310P3_TRANSPORT_H

#include "kernel_operator.h"
#include "smem_mm_ar_310p3_protocol.h"
#include "smem_shm_aicore_base_sdma.h"

namespace ar310p3_kernel {

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

// real_rows_in_group：组 group 的实属行数（去填充契约）。整组返回 1024，
// 末尾组只返回剩余行数；rows%256==0 保证实属行数是 128 行切片的整数倍。
__aicore__ inline uint32_t real_rows_in_group(uint32_t rows, uint32_t group)
{
    const uint32_t firstRow = group * kCommRows;
    return firstRow + kCommRows <= rows ? kCommRows : rows - firstRow;
}

// group_bytes_of：组 group 的实际传输字节数。组基址固定为 group*kGroupBytes，
// 末尾组只把长度缩短到实属行数（不搬填充区）。
__aicore__ inline uint64_t group_bytes_of(uint32_t rows, uint32_t group)
{
    return uint64_t{real_rows_in_group(rows, group)} * kRowBytes;
}

// wait_seats：等某组的一批组完成通知到齐（槽内首 64 位字 == 本批次 epoch）。
// 整段经 MTE 搬进 UB 判断（不做标量全局内存读）；指数退避防轮询流量占满
// 访存带宽；超过 kPollBudget 视为死等。返回总轮询次数（写 trace 用）。
__aicore__ inline uint32_t wait_seats(LocalTensor<int64_t>& poll, __gm__ uint8_t* seats,
                                      uint32_t group, uint32_t firstSlot, uint32_t count,
                                      uint32_t epoch, __gm__ uint8_t* status, ProtocolError timeoutCode)
{
    GlobalTensor<int64_t> seatsGm;
    seatsGm.SetGlobalBuffer(reinterpret_cast<__gm__ int64_t*>(seats + uint64_t{group} * kGroupSeatsBytes),
                       kFlagSlotQwords * kCores);
    uint32_t spins = 0, delay = kBackoffStart;
    while (true) {
        DataCopy(poll, seatsGm, kFlagSlotQwords * kCores);
        uint32_t ok = 1;
        for (uint32_t slotIdx = 0; slotIdx < count; ++slotIdx) {
            if (static_cast<uint32_t>(poll.GetValue((firstSlot + slotIdx) * kFlagSlotQwords)) != epoch) {
                ok = 0;
                break;
            }
        }
        if (ok) return spins;
        check(++spins < kPollBudget, status, timeoutCode);
        for (uint32_t i = 0; i < delay; ++i) __asm__ __volatile__("" ::: "memory");
        if ((spins & (kBackoffStep - 1u)) == 0 && delay < kBackoffMax) delay <<= 1;
    }
}

// wait_peer_rdy：轮询本端 peerRdy 槽（对端写回的"组数据已到我方 scratch"
// 标记）直到值等于本批次 epoch。对端先 COPY 组数据再发 8B 标记、orch 按发送
// 方保序 ⇒ 见标记时对端数据必已全部落位。死等保护与 wait_seats 相同；
// 返回总轮询次数。
__aicore__ inline uint32_t wait_peer_rdy(LocalTensor<int64_t>& poll, __gm__ uint8_t* work,
                                         uint32_t group, uint32_t epoch, __gm__ uint8_t* status)
{
    GlobalTensor<int64_t> peerRdyGm;
    peerRdyGm.SetGlobalBuffer(reinterpret_cast<__gm__ int64_t*>(work + kPeerRdyOff + uint64_t{group} * kFlagSlotBytes),
                       kFlagSlotQwords);
    uint32_t spins = 0, delay = kBackoffStart;
    while (true) {
        DataCopy(poll, peerRdyGm, kFlagSlotQwords);
        if (static_cast<uint32_t>(poll.GetValue(0)) == epoch) break;
        check(++spins < kPollBudget, status, ProtocolError::PeerRdyTimeout);
        for (uint32_t i = 0; i < delay; ++i) __asm__ __volatile__("" ::: "memory");
        if ((spins & (kBackoffStep - 1u)) == 0 && delay < kBackoffMax) delay <<= 1;
    }
    return spins;
}

// Transport：SDMA 传输封装。sdmaRes 为保留区基址句柄（kernel 入口预计算），
// work/peer 为本端与对端池地址，status 写失败错误码，epoch 参与完成通知标记，
// packet 为分包粒度。
struct Transport {
    __gm__ uint8_t *sdmaRes, *work, *peer, *status;
    uint32_t epoch, packet;

    // send_packetized：把 work 区 [srcOff, srcOff+bytes) 按 packet 粒度分包
    // 复制到对端 dstOff；每包完成通知携带 device_tag（phase/group/包序号）。
    // 任一包失败即写错误码并终止。
    __aicore__ inline void send_packetized(Phase phase, uint32_t group, uint64_t srcOff, uint64_t dstOff,
                                           uint64_t bytes, smem_shm_sdma_op_t op)
    {
        for (uint64_t pos = 0; pos < bytes; pos += packet) {
            const uint64_t chunkBytes = bytes - pos < packet ? bytes - pos : packet;
            check(smem_shm_sdma_signal_at(sdmaRes, work + srcOff + pos, peer + dstOff + pos, chunkBytes,
                  device_tag(phase, epoch, group, static_cast<uint32_t>(pos / packet)), op), status,
                  ProtocolError::SignalPostFail);
        }
    }

    // send_group_copy：组 group 本端数据全量复制进对端 scratch 区同偏移，
    // 末尾组按实属行数缩短。
    __aicore__ inline void send_group_copy(Phase phase, uint32_t group, uint32_t rows)
    {
        send_packetized(phase, group, uint64_t{group} * kGroupBytes, kScratchOff + uint64_t{group} * kGroupBytes,
              group_bytes_of(rows, group), SMEM_SHM_SDMA_OP_COPY);
    }

    // signal_peer_ready：发 8B 包把就绪标记写进对端 peerRdy 槽（载荷为本
    // 批次 epoch），对端归约分支轮询到该值即知数据已到。
    __aicore__ inline void signal_peer_ready(uint32_t group)
    {
        check(smem_shm_sdma_signal_at(sdmaRes, work + kCtrlOff, peer + kPeerRdyOff + uint64_t{group} * kFlagSlotBytes,
              8, device_tag(Phase::READY, epoch, group, 0), SMEM_SHM_SDMA_OP_COPY), status,
              ProtocolError::SignalPostFail);
    }
};

}  // namespace ar310p3_kernel

#endif  // SMEM_MM_AR310P3_TRANSPORT_H
