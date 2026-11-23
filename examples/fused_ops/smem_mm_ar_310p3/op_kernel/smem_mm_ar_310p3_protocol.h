// smem_mm_ar_310p3_protocol.h —— 协议层：常量、标记位域、轮询预算、
// 错误码、诊断 trace 布局。
// 分层：smem_mm_ar_310p3_tiling.h 池内区域偏移（唯一事实源，外部建池也依它）；
// 本头协议语义常量；smem_mm_ar_310p3_transport.h 协议动作原语；
// smem_mm_ar310p3.cpp 三侧逻辑编排。
//
// 读懂本协议的五个约定：
//   1) 每个通信组 = 1024 行，8 个 Cube 核各算 128 行切片，每组数据 4MiB；
//   2) 组完成通知（seats，池内）：Cube 核 → 调度器，本端核内同步。槽 64B
//      独占一个缓存行（防跨核共享缓存行互踩），槽内写本批次 epoch 即完成；
//   3) 就绪标记（peerRdy，池内）：对端调度器 → 本端归约分支，跨 rank 同步。
//      对端先 COPY 组数据到本端 scratch、再发 8B 标记，orch 按发送方保序 ⇒
//      标记落地即数据包全部落地，归约分支见到本批次 epoch 即可相加；
//   4) epoch：调度批次号，两端锁步递增；所有标记以"等于本批次 epoch"为
//      真值，天然防跨批次串扰；
//   5) 去填充契约：末尾组可不足 1024 行（rows%256==0 保证 128 行切片整片
//      实属或整片填充）；填充切片核免算，但组完成通知无条件发布——调度器
//      固定等 8 个通知，缺一个即死等。
#ifndef SMEM_MM_AR310P3_PROTOCOL_H
#define SMEM_MM_AR310P3_PROTOCOL_H

#include <cstdint>
#include "kernel_operator.h"
#include "smem_mm_ar_310p3_tiling.h"

namespace ar310p3_kernel {

using namespace AscendC;

// ---- 几何与角色 ----
constexpr uint32_t kCores = 8;          // Cube 核数（block 0..7）
constexpr uint32_t kCommRows = 1024;    // 通信组行数（与矩阵乘组同粒度）
constexpr uint32_t kMmCoreRows = kCommRows / kCores;  // 每核每组的 128 行切片

// ---- SDMA 分包 ----
constexpr uint32_t kPacket = 1024 * 1024;             // 分包粒度 1MiB
constexpr uint64_t kGroupBytes = kCommRows * kRowBytes;  // 每组 4MiB，恰 4 包

// ---- 标志槽位 ----
constexpr uint32_t kFlagSlotBytes = 64;    // 每个标志槽 64B，独占一个缓存行
constexpr uint32_t kFlagSlotQwords = kFlagSlotBytes / sizeof(int64_t);  // 8 个 64 位字
constexpr uint32_t kGroupSeatsBytes = kCores * kFlagSlotBytes;          // 每组组完成通知 512B

// 说明：memfabric 1.2.0 删除了随数据包携带完成通知附带数据（旧 imm/device_tag）
// 的机制，就绪标记改为 signal_set 直接写 8B 值（本批次 epoch），接收方轮询
// 槽值判等。旧协议中区分"哪个组哪个包"的阶段位域因此不再需要。

// ---- 轮询预算与退避（自旋等待共用，避免轮询流量占满访存带宽）----
constexpr uint32_t kPollBudget = 100000000;  // 死等保护：超过视为协议死锁
constexpr uint32_t kBackoffStart = 32;       // 空转初始间隔
constexpr uint32_t kBackoffStep = 256;       // 每 256 轮翻倍一次（判等用 mask）
constexpr uint32_t kBackoffMax = 1024;       // 空转间隔上限

// ---- 错误码（写 statusPtr，宿主可读定位失败）----
// 只含运行期失败路径；契约类（行数/池下限）由 op_host 在 tiling 期前置校验
// 并拒绝发射，kernel 侧不重复检查。
enum class ProtocolError : uint64_t {
    SignalPostFail = 102,    // SDMA 请求投递失败（请求环满环预算耗尽）
    GroupSeatsTimeout = 131, // 组完成通知死等（超过轮询预算）
    PeerRdyTimeout = 152,    // 对端就绪标记死等
    QuietFail = 105,         // 排空（quiet）超时
};

// ---- 诊断 trace 布局（tracePtr 指向 4KiB 区）----
// [0]                        magic（kTraceMagic），标识 trace 来源
// [1, 16)                    预留
// [16 + core*16, +16)        每个 Cube 核的诊断段
// [kTraceSchedBase, +8+4g)   调度器各组"等组完成通知"的轮数（g=组数）
// [kTraceSchedBase+64+lane)  每条归约分支的总轮询次数
constexpr uint32_t kTraceBytes = 4096;
constexpr uint32_t kTraceU64 = kTraceBytes / 8;
constexpr uint64_t kTraceMagic = 0x4D325431ULL;   // "M2T1"
constexpr uint32_t kTraceHeader = 16;             // 各核段起始
constexpr uint32_t kTraceCoreStride = 16;         // 每核段 16 个 64 位字
constexpr uint32_t kTraceSchedBase = kTraceHeader + kCores * kTraceCoreStride;

}  // namespace ar310p3_kernel

#endif  // SMEM_MM_AR310P3_PROTOCOL_H
