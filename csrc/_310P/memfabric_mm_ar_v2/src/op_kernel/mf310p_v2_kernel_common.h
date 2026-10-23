#pragma once
// T0 正式 Kernel 与诊断 Kernel 共同使用的 Ascend C 几何、分核及缓存操作。
#include <cstdint>
#include "acl/acl_rt.h"
#include "kernel_operator.h"
#include "lib/matmul/matmul_intf.h"
#include "smem_shm_aicore_base_sdma.h"

namespace {

using namespace AscendC;
using AT = MatmulType<TPosition::GM, CubeFormat::ND, float16_t, false>;
using BT = MatmulType<TPosition::GM, CubeFormat::NZ, float16_t, true>;
using CT = MatmulType<TPosition::GM, CubeFormat::ND, float16_t, false>;
using BiasT = MatmulType<TPosition::GM, CubeFormat::ND, float, false>;

constexpr uint32_t kCores = 8;
constexpr uint32_t kN = 2048, kK = 2048;
constexpr uint64_t kRowBytes = kN * sizeof(float16_t);
constexpr uint32_t kAddTile = 1024;
constexpr uint32_t kAddBytes = kAddTile * sizeof(float16_t);
constexpr uint64_t kTagBase = 0x4D46325600000000ULL;  // 通信标记 MF2V
constexpr uint64_t kTagDone = 0x4D463256444F4E45ULL;  // 完成标记 MF2VDONE
constexpr uint32_t kIBReady = 0;
constexpr uint32_t kIBDone = 1;
constexpr uint32_t kIBReleaseBase = 2;   // 核 1～7 对应事件编号 3～9
constexpr uint32_t kIBBytes = 8 * 32 * (kIBReleaseBase + 7) + 8 * 32;
constexpr uint32_t kUBForVector = 3 * kAddBytes;
constexpr uint32_t kUBForIB = 32;

constexpr MatmulQuantParams kQuant = {false, false};
constexpr MatmulBatchParams kBatch = {false, BatchMode::NONE};
constexpr MatmulFuncParams kFunc = {false, true, false, false, 0,
    IterateOrder::UNDEF, ScheduleType::INNER_PRODUCT, true, true};

#define V2_TILING(TAG, ROWS, BASIC)                                             \
    constexpr MatmulShapeParams shape##TAG = {ROWS / kCores, kN, kK,           \
        BASIC, 256, 64};                                                        \
    constexpr MatmulConfig cfg##TAG =                                          \
        GetMMConfig<MatmulConfigMode::CONFIG_NORM>(                             \
            shape##TAG, kQuant, kBatch, kFunc);                                 \
    constexpr static MatmulApiStaticTiling tiling##TAG =                        \
        GetMatmulApiTiling<AT, BT, CT, BiasT>(cfg##TAG, Impl::L1_SIZE);
V2_TILING(256, 256, 32)
V2_TILING(512, 512, 64)
V2_TILING(1024, 1024, 128)
#undef V2_TILING

__aicore__ inline void CleanLines(__gm__ uint8_t* p, uint64_t len)
{
    GlobalTensor<int32_t> g;
    for (uint64_t b = 0; b < len; b += 64) {
        g.SetGlobalBuffer(reinterpret_cast<__gm__ int32_t*>(p + b), 16);
        DataCacheCleanAndInvalid<int32_t, CacheLine::SINGLE_CACHE_LINE>(g);
    }
}

__aicore__ inline void CheckOrTrap(bool ok, __gm__ uint8_t* status, uint64_t code)
{
    if (ok) return;
    *reinterpret_cast<__gm__ volatile uint64_t*>(status) = code;
    CleanLines(status, 64);
    Trap();
}

/* 每个核独占 64B 对齐结果槽并清理缓存行。dav-2002 无跨核 L1 一致性：并发写同一 32B 缓冲可能导致回写丢失；V1 就绪单元同样按 64B 隔离。 */
__aicore__ inline void WriteResultSlot(
    __gm__ uint8_t* result, uint32_t core, uint32_t value)
{
    __gm__ uint8_t* slot = result + 64ULL * core;
    *reinterpret_cast<__gm__ volatile uint32_t*>(slot) = value;
    GlobalTensor<int32_t> g;
    g.SetGlobalBuffer(reinterpret_cast<__gm__ int32_t*>(slot), 8);
    DataCacheCleanAndInvalid<int32_t, CacheLine::SINGLE_CACHE_LINE>(g);
}

/* 核间 IB 协议：8→1 时工作核发送各自事件 0，Core0 等待；1→7 时 Core0 发布七个互不相同的事件，各由一个核消费；事件 1 表示 Add 完成。每次调用都须消费全部事件，IB GM 工作区先清零并验证 AICORE 模式。 */
__aicore__ inline void WaitReady(
    const GlobalTensor<int32_t>& ibGm, const LocalTensor<int32_t>& ibUb,
    uint32_t core)
{
    if (core != 0) {
        IBSet<true>(ibGm, ibUb, core, kIBReady);
        return;
    }
    for (uint32_t b = 1; b < kCores; ++b) {
        IBWait<true>(ibGm, ibUb, b, kIBReady);
    }
}

__aicore__ inline void ReleaseAdd(
    const GlobalTensor<int32_t>& ibGm, const LocalTensor<int32_t>& ibUb,
    uint32_t core)
{
    if (core == 0) {
        for (uint32_t b = 1; b < kCores; ++b) {
            IBSet<true>(ibGm, ibUb, 0, kIBReleaseBase + b);
        }
    } else {
        IBWait<true>(ibGm, ibUb, 0, kIBReleaseBase + core);
    }
}

__aicore__ inline void WaitAddDone(
    const GlobalTensor<int32_t>& ibGm, const LocalTensor<int32_t>& ibUb,
    uint32_t core)
{
    if (core != 0) {
        IBSet<true>(ibGm, ibUb, core, kIBDone);
    } else {
        for (uint32_t b = 1; b < kCores; ++b)
            IBWait<true>(ibGm, ibUb, b, kIBDone);
    }
}

} // 共用内部命名空间
