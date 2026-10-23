#pragma once
// G1-C 只读取证：每个 AI Core 独占 64B GM 槽，记录最后到达的阶段。
// 仅在 MF310P_V2_G1C_TRACE 编译期开启；不占用 MemFabric 邮箱或 IB 工作区。
// 注意：这是前进进度戳，不是性能计时器；Host 采样可能读到相邻阶段。
#ifdef MF310P_V2_G1C_TRACE
constexpr uint64_t kG1cTraceOffset = 64;
constexpr uint32_t kG1cTraceSlotBytes = 64;
constexpr uint32_t kG1cTraceBytes = kCores * kG1cTraceSlotBytes;
__aicore__ inline void G1cStage(__gm__ uint8_t* status, uint32_t core,
                                uint32_t chunk, uint32_t stage)
{
    __gm__ uint8_t* slot = status + kG1cTraceOffset +
                          static_cast<uint64_t>(core) * kG1cTraceSlotBytes;
    // slot+8 为每核调用代际，仅在实际进入 kernel 时递增；可区分上次已完成
    // 的阶段 120 和本次尚未启动、正在排队的 Kernel。
    if (stage == 10) {
        CleanLines(slot, kG1cTraceSlotBytes);
        __gm__ volatile uint64_t* generation =
            reinterpret_cast<__gm__ volatile uint64_t*>(slot + 8);
        *generation = *generation + 1;
    }
    // 单一对齐 8B 阶段字：高 32 位 chunk，下 32 位 stage；每核独占缓存行。
    *reinterpret_cast<__gm__ volatile uint64_t*>(slot) =
        (static_cast<uint64_t>(chunk) << 32) | stage;
    CleanLines(slot, kG1cTraceSlotBytes);
}
#else
__aicore__ inline void G1cStage(__gm__ uint8_t*, uint32_t, uint32_t, uint32_t) {}
#endif

// 阶段码：10 入口；20/21 MM 前/后；30/31 IB ready 前/后；
// 40/41 signal 前/后；50/51 mail wait 前/后；60/61 release 前/后；
// 70/71 Add 前/后；80/81 AddDone 前/后；90/91 quiet 前/后；
// 100/101 DONE signal 前/后；110/111 DONE wait 前/后；120 退出。
// 本头独立于正式 MM/SDMA 算法，禁止从这里访问协议控制环。
