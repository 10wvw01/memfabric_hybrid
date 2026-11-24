/*
 * reg_sync_aicpu_kernel.cc — 寄存器同步端到端验证 AICPU 侧 kernel
 *
 * 验证 C1(AICore set_cond→AICPU 扫描唤醒) + C2(AICPU 写 0xA0→AICore 读 DATA_MAIN_BASE):
 *   1. 扫全部 10 核的 0x4C8, lastCond 判变化(memfabric orch 等价模式), 命中 AICore set_cond 值(0xABCD),
 *      记录命中核下标(与 Send kernel 上报核对照) + 残留噪声普查
 *   2. 写 regBase[recvCoreId]+0xA0 = 0x1234(供 AICore 第二轮 get_data_main_base 读)
 *   3. 结果写 GM buffer(host D2H 读回验证)
 *
 * 编译: hcc 交叉编译 -shared -fPIC → libreg_sync_aicpu.so
 * 运行: host 侧 aclrtLoadKernelFromBinary + aclrtLaunchKernelWithConfig
 */
#include <cstdint>
#include <cstring>
#include <unistd.h>

/* AICPU 侧寄存器读写原语(抄 hybm_sdma_orch_kernel.cc + pypto aicore_hal.h) */
constexpr uint32_t REG_SPR_DATA_MAIN_BASE = 0xA0;  /* AICPU 写→AICore 读(C2 背压) */
constexpr uint32_t REG_SPR_COND = 0x4C8;           /* AICore 写→AICPU 读(C1 请求通知) */

inline void AicpuWriteReg64(uint64_t regAddr, uint64_t val)
{
    *reinterpret_cast<volatile uint64_t *>(regAddr) = val;
}
inline uint64_t AicpuReadReg64(uint64_t regAddr)
{
    return *reinterpret_cast<volatile uint64_t *>(regAddr);
}

/* cache 维护(GM buffer 写回) */
inline void cacheFlushLine(uintptr_t addr)
{
    __asm__ __volatile__("dc cvac, %0" ::"r"(addr) : "memory");
}
inline void memBarrier()
{
    __asm__ __volatile__("dsb ish" ::: "memory");
}

/* AICPU kernel 入口:
 *   regBaseAddr — regBase 数组设备地址(10×u64, host H2D)
 *   resultAddr  — GM buffer 结果区(设备地址, 32×u64)
 *   expectCond  — 期望从 0x4C8 读到的值(AICore set_cond 写的)
 *   writeDmb    — 要写 0xA0 的值(供 AICore get_data_main_base 读)
 *   coreId      — AICore Send kernel 运行的物理核 ID(host 从 GM slot 4 读出后传入)
 *   recvCoreId  — AICore Recv kernel 实际所在核 ID(从 GM slot 7 读出后传入) — 0xA0 必须写
 *                 "Recv 所在核"的 regBase! 旧版按 Send 的 coreId 写, 而 Send/Recv 不同核
 *                 (Send=4, Recv=5), 写读异核 → SPR 恒 0 = 探针假阴性根因
 */
extern "C" __attribute__((visibility("default"))) uint32_t RegSyncAicpuKernel(
    uint64_t regBaseAddr, uint64_t resultAddr, uint64_t expectCond, uint64_t writeDmb,
    uint64_t coreId, uint64_t recvCoreId)
{
    /* 0. 诊断: 先写参数到 GM(若后续 regBase 读取 crash, host 也能看到参数) */
    volatile uint64_t *result = reinterpret_cast<volatile uint64_t *>(resultAddr);
    result[8]  = 0xBEEF0001ULL;  /* 诊断: kernel 入口 */
    result[9]  = regBaseAddr;    /* 诊断: regBaseAddr 参数 */
    result[10] = resultAddr;     /* 诊断: resultAddr 参数 */
    result[11] = expectCond;     /* 诊断: expectCond */
    result[12] = writeDmb;       /* 诊断: writeDmb */
    result[13] = coreId;         /* 诊断: coreId */
    memBarrier();
    cacheFlushLine(reinterpret_cast<uintptr_t>(&result[8]) & ~static_cast<uintptr_t>(63));

    /* 1. 从 regBase 数组读 coreId 对应的基址 */
    const volatile uint64_t *regBaseArr = reinterpret_cast<const volatile uint64_t *>(regBaseAddr);
    const uint64_t regBase = regBaseArr[coreId];

    result[8]  = 0xBEEF0002ULL;  /* 诊断: regBase 读取完成 */
    result[14] = regBase;        /* 诊断: regBase 值 */
    memBarrier();
    cacheFlushLine(reinterpret_cast<uintptr_t>(&result[8]) & ~static_cast<uintptr_t>(63));

    /* 2. C1(扫描版): memfabric orch kernel 等价唤醒——不按指定核寻址,
     *    lastCond 起步全 0(与 hybm_sdma_orch_kernel 每个 epoch 的初始化一致),
     *    每轮扫全部 10 核的 0x4C8, 任一核值变化即"有活动"; 记录首个变化核与值,
     *    并普查首轮各核 COND 残留(orch 的 lastCond=0 语义会把残留非 0 判为一次活动, 属噪声)。
     *    纯忙等(不用 usleep: KFC kernel 有 28s 系统监控击杀, usleep 会拖长触发击杀) */
    uint64_t lastCond[10] = {0};
    uint64_t condVal = 0;
    uint64_t scanCore = 0xFFFF;   /* 首个变化核下标(裁决: 应 == Send kernel 上报核) */
    uint64_t noiseCores = 0;      /* 首轮 COND 非 0 的核数(残留噪声普查) */
    uint32_t pollCnt = 0;
    bool hit = false;
    for (uint32_t i = 0; i < 100000000; i++) { /* 上限 ~数秒(纯忙等) */
        bool woken = false;
        for (uint32_t c = 0; c < 10; c++) {
            const uint64_t v = AicpuReadReg64(regBaseArr[c] + REG_SPR_COND);
            if (i == 0 && v != 0) {
                noiseCores++; /* 首轮普查: 该核 COND 有残留(上一进程 set_cond 留下) */
            }
            if (v != lastCond[c]) {
                lastCond[c] = v;
                woken = true;
                if (scanCore == 0xFFFF) { /* 只记首个变化核, 其余变化仅计入噪声统计 */
                    scanCore = c;
                    condVal = v;
                }
            }
            if (v == expectCond) { /* 任一核读到目标值即命中(expectCond 每次运行唯一, 不会撞残留) */
                scanCore = c;
                condVal = v;
                hit = true;
                break;
            }
        }
        pollCnt++;
        if (hit) {
            break; /* 扫描唤醒命中 Send kernel 的 set_cond 值 */
        }
    }

    result[21] = scanCore;       /* 扫描唤醒命中的核下标(应 == result[13] coreId) */
    result[22] = noiseCores;     /* 首轮 COND 非 0 核数(残留噪声普查) */
    result[23] = 0xBEEF0003ULL;  /* 诊断: 扫描轮询完成 */

    result[8]  = 0xBEEF0003ULL;  /* 诊断: 扫描轮询完成 */
    result[11] = pollCnt;        /* 轮询次数 */
    memBarrier();
    cacheFlushLine(reinterpret_cast<uintptr_t>(&result[8]) & ~static_cast<uintptr_t>(63));

    /* 3a. C2 精确写: Recv 所在核 regBase[recvCoreId]+0xA0 = writeDmb(核心修复点) */
    uint64_t recvBase = 0;
    if (recvCoreId < 10) {
        recvBase = regBaseArr[recvCoreId];
        AicpuWriteReg64(recvBase + REG_SPR_DATA_MAIN_BASE, writeDmb);
        memBarrier();
    }
    uint64_t recvCoreRb = (recvBase != 0) ? AicpuReadReg64(recvBase + REG_SPR_DATA_MAIN_BASE) : 0;

    /* 3b. C2 兜底: 写所有核 regBase[i]+0xA0 和 +0xD0 = writeDmb(AICore get_data_main_base 读)。
     *    0xD0 是 DAV_3510 的 DMB 偏移, 一并写入排除 310P3 SPR 实际背书在 0xD0 的可能。 */
    constexpr uint32_t REG_DMB_ALT = 0xD0;
    for (uint32_t c = 0; c < 10; c++) {
        AicpuWriteReg64(regBaseArr[c] + REG_SPR_DATA_MAIN_BASE, writeDmb);
        AicpuWriteReg64(regBaseArr[c] + REG_DMB_ALT, writeDmb);
    }
    memBarrier(); /* 写后屏障: 确保落到总线再读回 */

    /* 诊断: 写后读回 0xA0/0xD0/0x4C8, 区分"写未落 MMIO"与"落了但 AICore SPR 不取此值" */
    uint64_t dmbRb  = AicpuReadReg64(regBase + REG_SPR_DATA_MAIN_BASE);
    uint64_t dmbRbAlt = AicpuReadReg64(regBase + REG_DMB_ALT);
    uint64_t condRb = AicpuReadReg64(regBase + REG_SPR_COND);

    /* 4. 结果写 GM buffer(写到 slot 8..15 + 19..20, 避免与 AICore kernel 的 slot 0..7/17..18 冲突) */
    result[8]  = condVal;       /* 从 0x4C8 读到的值(应 == expectCond) */
    result[9]  = (condVal == expectCond) ? 1 : 0;  /* C1 通过标志 */
    result[10] = dmbRb;         /* 全核写后读回 0xA0 的值(关键诊断) */
    result[11] = pollCnt;       /* 轮询次数(诊断) */
    result[12] = condRb;        /* 写后再读回 0x4C8(对照) */
    result[13] = coreId;        /* Send coreId(诊断) */
    result[14] = dmbRbAlt;      /* 写后读回 0xD0 的值(偏移排查) */
    result[19] = recvCoreRb;    /* 精确写 Recv 核后读回 0xA0 的值 */
    result[20] = recvCoreId;    /* Recv coreId(诊断) */
    result[15] = 0xDEADBEEFULL; /* 尾标记(kernel 完整执行) */
    memBarrier();
    cacheFlushLine(reinterpret_cast<uintptr_t>(&result[8]) & ~static_cast<uintptr_t>(63));

    return (condVal == expectCond) ? 0 : 1;
}

/* ---- CANN extend lib loader 入口同名导出 ----
 * 与主链路(hybm_sdma_orch_kernel.cc:623-645)相同的 pypto 机制: host 把本 so 字节经
 * DeviceArgs.aicpuSoBin 传给 CP1 预加载的 libaicpu_extend_kernels.so, 其
 * DynTileFwkKernelServerInit 在 CP1 内 memfd 加载本 so 并 dlsym 下列三个固定符号;
 * 之后 DynTileFwkKernelServer 在 CP1 内分发到这里.
 * 参数经 DeviceKernelArgs.cfgdata(偏移 40)传入 RegSyncAicpuParams 设备指针. */
struct RegSyncAicpuParams {
    uint64_t regBaseAddr;
    uint64_t resultAddr;
    uint64_t expectCond;
    uint64_t writeDmb;
    uint64_t coreId;
    uint64_t recvCoreId;
};

extern "C" __attribute__((visibility("default"))) uint32_t StaticTileFwkBackendKernelServer(void *args)
{
    (void)args;
    return 0;
}

extern "C" __attribute__((visibility("default"))) uint32_t DynTileFwkBackendKernelServerInit(void *args)
{
    (void)args;
    return 0;
}

/* loader 填函数表时按这个名字 dlsym(与主链路 so 导出一致) */
extern "C" __attribute__((visibility("default"))) uint32_t DynTileFwkBackendKernelServer(void *args)
{
    void *param = *reinterpret_cast<void **>(reinterpret_cast<char *>(args) + 40); // cfgdata
    const auto *p = reinterpret_cast<const RegSyncAicpuParams *>(param);
    return RegSyncAicpuKernel(p->regBaseAddr, p->resultAddr, p->expectCond, p->writeDmb, p->coreId,
                              p->recvCoreId);
}
