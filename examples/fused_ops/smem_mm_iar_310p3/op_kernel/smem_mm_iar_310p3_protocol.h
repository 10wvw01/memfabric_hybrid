// smem_mm_iar_310p3_protocol.h —— 协议层：常量、标记位域、轮询预算、
// 错误码、诊断 trace 布局。
// 分层：smem_mm_iar_310p3_tiling.h 池内区域偏移（唯一事实源，外部建池也依它）；
// 本头协议语义常量；smem_mm_iar_310p3_transport.h 协议动作原语；
// smem_mm_iar310p3.cpp 三侧逻辑编排。
//
// 读懂本协议的五个约定：
//   1) 矩阵乘组 = 1024 行，8 个 Cube 核各算 128 行切片；通信子组与矩阵乘组
//      同粒度（kSubRows = kCommRows，subsPerGroup = 1）；
//   2) 组完成通知（seats，池内）：Cube 核 → 调度器，本端核内同步。槽 64B
//      独占一个缓存行（防跨核共享缓存行互踩），槽内写本批次 epoch 即完成；
//   3) 标记族（8B，池内）：seatUp（本端子组的组完成通知齐了，告知对端调度器）、
//      RDONE（REDUCE 数据包全部发出）、GDONE（GATHER 数据包全部发出）。
//      orch 按发送方保序 ⇒ 尾部拷出分支见 GDONE 即该子组两个半区都已收敛
//      为终值（依赖"同一子组按 REDUCE→RDONE→GATHER→GDONE 顺序发出"）；
//   4) epoch：调度批次号，两端锁步递增；所有标记以"等于本批次 epoch"为
//      真值，天然防跨批次串扰；
//   5) 去填充契约：末尾组/末子组可不足整组（rows%256==0 保证子组字节数是
//      1MiB 的整数倍）；填充切片核免算，但组完成通知无条件发布——调度器
//      固定按槽位数等齐，缺一个即死等。
#ifndef SMEM_MM_IAR310P3_PROTOCOL_H
#define SMEM_MM_IAR310P3_PROTOCOL_H

#include <cstdint>
#include "kernel_operator.h"
#include "smem_mm_iar_310p3_tiling.h"

namespace iar310p3_kernel {

using namespace AscendC;

// ---- 几何与角色 ----
constexpr uint32_t kCores = 8;          // Cube 核数（block 0..7）
constexpr uint32_t kCommRows = 1024;    // 矩阵乘组行数
constexpr uint32_t kMmCoreRows = kCommRows / kCores;  // 每核每组的 128 行切片
constexpr uint32_t kSubRows = kCommRows;              // 通信子组行数（与矩阵乘组同粒度）

// ---- SDMA 分包 ----
constexpr uint32_t kPacket = 1024 * 1024;                // 分包粒度 1MiB
constexpr uint64_t kGroupBytes = kCommRows * kRowBytes;  // 每组 4MiB

// ---- 标志槽位 ----
constexpr uint32_t kFlagSlotBytes = 64;    // 每个标志槽 64B，独占一个缓存行
constexpr uint32_t kFlagSlotQwords = kFlagSlotBytes / sizeof(int64_t);  // 8 个 64 位字
constexpr uint32_t kGroupSeatsBytes = kCores * kFlagSlotBytes;          // 每组组完成通知 512B
constexpr uint32_t kSeatUpOff = 512;       // seatUp 槽段相对 kReadyOff 的偏移（GDONE 槽段后预留）

// 说明：memfabric 1.2.0 删除了随数据包携带完成通知附带数据（旧 imm/device_tag）
// 的机制，就绪标记改为 signal_set 直接写 8B 值（本批次 epoch），接收方轮询
// 槽值判等。旧协议中用于诊断区分各标记族的阶段号不再需要。

// ---- 轮询预算与退避（自旋等待共用，避免轮询流量占满访存带宽）----
constexpr uint32_t kPollBudget = 100000000;  // 死等保护：超过视为协议死锁
constexpr uint32_t kBackoffStart = 32;       // 空转初始间隔
constexpr uint32_t kBackoffStep = 256;       // 每 256 轮翻倍一次（判等用 mask）
constexpr uint32_t kBackoffMax = 1024;       // 空转间隔上限

// ---- 错误码（写 statusPtr，宿主可读定位失败）----
// 只含运行期失败路径；契约类（行数/池下限）由 op_host 在 tiling 期前置校验
// 并拒绝发射，kernel 侧不重复检查。
enum class ProtocolError : uint64_t {
    SignalMarkFail = 101,    // 标记包（seatUp/RDONE/GDONE）投递失败
    SignalDataFail = 102,    // 数据包（REDUCE/GATHER）投递失败
    GdoneWaitTimeout = 155,  // 尾部拷出分支等 GDONE 死等
    SchedLoopTimeout = 157,  // 调度器事件循环死等
    QuietFail = 105,         // 排空（quiet）超时
};

// ---- 诊断 trace 布局（tracePtr 指向 4KiB 区）----
// [0, 8)                     批次头：magic/rows/subRows/rank/epoch/subCount
// [8, 16)                    预留
// [16 + core*16, +16)        每个 Cube 核的诊断段
// [kTraceSchedBase, +8+4s)   调度器三游标各子组的发射空转轮数（s=子组数）
// [kTraceSchedBase+64+lane)  每条尾部拷出分支的总轮询次数
constexpr uint32_t kTraceBytes = 4096;
constexpr uint32_t kTraceU64 = kTraceBytes / 8;
constexpr uint64_t kTraceMagic = 0x4D325431ULL;   // "M2T1"
constexpr uint32_t kTraceHeader = 16;             // 各核段起始
constexpr uint32_t kTraceCoreStride = 16;         // 每核段 16 个 64 位字
constexpr uint32_t kTraceSchedBase = kTraceHeader + kCores * kTraceCoreStride;

}  // namespace iar310p3_kernel

#endif  // SMEM_MM_IAR310P3_PROTOCOL_H
