/*
 * reg_sync_aicore_dev.cpp — 寄存器同步端到端验证 AICore 侧 kernel
 *
 * 两个 kernel:
 *   1. RegSyncAicoreSend: set_cond(expectCond) 通知 AICPU(C1), 结果(含 coreId)写 GM
 *   2. RegSyncAicoreRecv: get_data_main_base() 读 AICPU 写的值(C2), 结果写 GM
 *
 * 关键: set_cond 写的是"当前执行 kernel 的 AICore"的 COND 寄存器,
 *       get_data_main_base 读的也是"当前 AICore"的 DATA_MAIN_BASE。
 *       AICPU 必须用 regBase[coreId] 而非 regBase[0] 来访问正确的核心。
 *       所以 Send kernel 把 coreId 写到 GM slot 4, host 读出后传给 AICPU kernel。
 *
 * 编译: ascendc_library, SOC_VERSION=Ascend310P3
 */
#include "kernel_operator.h"

constexpr uint32_t RESULT_SLOTS = 32;

/* kernel 1: AICore set_cond 写 COND 寄存器(C1 请求通知) + 记录 coreId */
extern "C" __global__ __aicore__ void RegSyncAicoreSend(GM_ADDR resultBuf, uint64_t expectCond)
{
    /* C1: set_cond(expectCond) — 写当前 AICore 的 COND 寄存器
     * cce_aicore_intrinsics_3101.h:790 set_cond(int64_t config) */
    set_cond(static_cast<int64_t>(expectCond));

    /* 记录当前 coreId(供 host 传给 AICPU kernel 用 regBase[coreId]) */
    uint64_t coreId = static_cast<uint64_t>(get_coreid());

    /* 结果写 GM */
    AscendC::GlobalTensor<uint64_t> t;
    t.SetGlobalBuffer(reinterpret_cast<__gm__ uint64_t*>(resultBuf), RESULT_SLOTS);
    t.SetValue(0, expectCond);      /* set_cond 写入的值(回显) */
    t.SetValue(1, 0xDEADBEEFULL);   /* 尾标记(kernel 完整执行) */
    t.SetValue(4, coreId);          /* coreId(AICPU 用 regBase[coreId] 访问正确核心) */
    AscendC::DataCacheCleanAndInvalid<uint64_t, AscendC::CacheLine::SINGLE_CACHE_LINE>(t);
}

/* kernel 2: AICore get_data_main_base 轮询 DATA_MAIN_BASE 寄存器(C2 背压)
 * 必须先于 AICPU 写 0xA0 启动并轮询: 若先写后派发, 派发时 runtime 会重置该 SPR. */
extern "C" __global__ __aicore__ void RegSyncAicoreRecv(GM_ADDR resultBuf, uint64_t slotOff)
{
    AscendC::GlobalTensor<uint64_t> t;
    t.SetGlobalBuffer(reinterpret_cast<__gm__ uint64_t*>(resultBuf), RESULT_SLOTS);

    /* 先把自己所在核写到 GM slot 7, host 读出后传给 AICPU kernel:
     * AICPU 必须写 "本 Recv kernel 所在核" 的 regBase[core]+0xA0, 不能按 Send 的 coreId 写
     * (实测 Send/Recv 不保证同核: Send=core4, Recv=core5 → 旧版核错配读到恒 0 = 假阴性根因) */
    uint64_t myCoreId = static_cast<uint64_t>(get_coreid());
    t.SetValue(7, myCoreId);
    /* 启动标记(host 轮询到后再让 AICPU 写 0xA0) */
    t.SetValue(6, 0xAAAA0001ULL);
    AscendC::DataCacheCleanAndInvalid<uint64_t, AscendC::CacheLine::SINGLE_CACHE_LINE>(t);

    /* C2 路径 1: intrinsic 读 (cce_aicore_intrinsics_3101.h:2362, 同条 MOV 指令, 对照用)
     * 注意: get_data_main_base() builtin 无 volatile, 实测编译器会把纯轮询循环整个消掉
     * (intPollCnt=0) — 必须把每次读值写入 volatile 内存池(memory sink)强制保留循环 */
    uint64_t intVal = 0;
    uint64_t intPollCnt = 0;
    volatile uint64_t intSink = 0;
    for (uint32_t i = 0; i < 100000000; i++) {
        uint64_t v = static_cast<uint64_t>(get_data_main_base());
        intSink = v;
        intPollCnt++;
        if (v != 0) {
            intVal = v;
            break;
        }
    }

    /* C2 路径 2: pypto 同款裸 asm 读 DATA_MAIN_BASE (aicore_entry.h:160-165) */
    uint64_t asmVal = 0;
    uint64_t asmPollCnt = 0;
    for (uint32_t i = 0; i < 100000000; i++) {
        uint64_t coreStatus = 0;
        __asm__ volatile("MOV %0, DATA_MAIN_BASE\n" : "+l"(coreStatus));
        asmPollCnt++;
        if (coreStatus != 0) {
            asmVal = coreStatus;
            break;
        }
    }

    /* 结果写 GM(写到 resultBuf + slotOff 对应槽) */
    t.SetValue(slotOff, asmVal);            /* 裸 asm 从 DATA_MAIN_BASE 读到的值 */
    t.SetValue(slotOff + 1, 0xDEADBEEFULL); /* 尾标记 */
    t.SetValue(5, asmPollCnt);              /* 裸 asm 轮询次数 */
    t.SetValue(17, intVal);                 /* intrinsic 读到的值(对照) */
    t.SetValue(18, intPollCnt);             /* intrinsic 轮询次数(对照) */
    AscendC::DataCacheCleanAndInvalid<uint64_t, AscendC::CacheLine::SINGLE_CACHE_LINE>(t);
}

/* ---- launch 封装(host 侧 triple-chevron 调用) ---- */
#include "reg_sync_aicore_dev.h"

void RegSyncAicoreSend_do(void *stream, uint8_t *resultBuf, uint64_t expectCond)
{
    RegSyncAicoreSend<<<1, nullptr, stream>>>(resultBuf, expectCond);
}

void RegSyncAicoreRecv_do(void *stream, uint8_t *resultBuf, uint64_t slotOff)
{
    RegSyncAicoreRecv<<<1, nullptr, stream>>>(resultBuf, slotOff);
}
