// MemFabric 310P AICore×AICPU 协同编排 kernel (310P3, 主构建产物)
//
// 溯源: fork 自 xcclpp main examples/tutorials/aicpu-aicore-310p3/aicpu_kernel/
// xcclpp_overlap_aicpu.cpp (MIT), 按 310p-aicore-aicpu-开发进展.md 迁移;
// 重命名 XcclppAicpuSqeRun → HybmOverlapRun (入口/json functionName),
// XcclppAicpuSqeParam/Result → HybmOverlapParam/Result, 其余逐行保留
// (SQE 配方/提交链/轮询纪律均已 hcomm 源码级证实, 见计划附录 A)。
// P0 实证记录(2026-09-10 dev0/dev0↔dev1): E0a CP1 闭环/E0b 池 GVA 可译/
// E0c 八步全链 三实验一次通过(实测记录见 310p-aicore-aicpu-开发进展.md)。
// 产物 so 名 libmf_overlap_aicpu.so — 与 libcann_hybm_kernel.so 独立
// (CP1 按文件名 dlopen 缓存, 改 kernel 必 bump 设备侧版本名 MF_OVERLAP_KVER)。
//
// 本文件提供 (param.submitMode, 支持域 13/14, 其余值拒绝 mainRet=950):
//   13 = 八步闭环编排状态机 (单 chunk, role0 发送方 / role1 编排方)
//   14 = 多 chunk 流水编排 (roleRsv[1] = chunk 数, 通信被计算掩盖)
// 两模式内嵌 CP1 活查询 (cp1LiveQuery) + ssid 实时查询 + SQE 直写提交全链。
//
// 完成检测: flag-SQE 哨兵轮询 (尾部 8B SDMA 搬运 + dc civac 轮询)。
// 提交路径: SQE 直写 sqBase + dc cvac 刷 PoC; doorbell 走 halSqCqConfig (ioctl)。
// 机制档案见本目录 README 与 xcclpp aicpu-310p3/README.md。
//
// SPDX-License-Identifier: MIT

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <sys/ioctl.h>
#include <dlfcn.h>

// ============================================================================
// 常量
// ============================================================================
constexpr size_t RT_CMD_SIZE = 64;
constexpr uint64_t AICPU_MAX_SQE_LEN = 1ull << 20;  // 单 SQE ≤ 1MB (纪律 7)


// doorbell 寄存器偏移 (shmem: 2201 → sqRegBase+0x8)
constexpr uint64_t STARS_SQ_TAIL_OFFSET = 0x8;

// HAL 常量 (doorbell + mode 3 活查询)
constexpr uint32_t DRV_NORMAL_TYPE_HAL = 0;
constexpr uint32_t DRV_SQCQ_PROP_SQ_STATUS = 0;
constexpr uint32_t DRV_SQCQ_PROP_SQ_HEAD = 1;
constexpr uint32_t DRV_SQCQ_PROP_SQ_TAIL = 2;
constexpr uint32_t DRV_SQCQ_PROP_SQ_BASE = 6;
constexpr uint32_t DRV_SQCQ_PROP_SQ_DEPTH = 7;
constexpr uint32_t SQCQ_CONFIG_INFO_LENGTH = 8;
constexpr uint32_t SQCQ_QUERY_INFO_LENGTH = 8;

struct HalSqCqConfigInfo {
    uint32_t type, tsId, sqId, cqId, prop;
    uint32_t value[SQCQ_CONFIG_INFO_LENGTH];
};
struct HalSqCqQueryInfo {
    uint32_t type, tsId, sqId, cqId, prop;
    uint32_t value[SQCQ_QUERY_INFO_LENGTH];
};

// ============================================================================
// kernel 入参 (host 经 aclrtLaunchKernelWithHostArgs 传入)
// ============================================================================

// wire 契约 — 单一事实源 ../overlap_wire.hpp (static_assert 编译期锁布局)
#include "hybm_overlap_contract.h"


// ============================================================================
// cache 维护 (AICPU 与 SDMA 同 domain, zbal 同款)
// ============================================================================
inline void cacheFlushLine(uintptr_t addr)
{
    __asm__ __volatile__("dc cvac, %0" ::"r"(addr) : "memory");
}
inline void cacheInvalidateLine(uintptr_t addr)
{
    __asm__ __volatile__("dc civac, %0" ::"r"(addr) : "memory");
}
inline void memBarrier()
{
    __asm__ __volatile__("dsb ish" ::: "memory");
}

// P2 时间线打点: AICPU 侧 cycle 读取 (cntvct_el0, 各核独立计数器 — 只做本执行体内相对时长)
inline uint64_t readTimelineCycle()
{
    uint64_t v = 0;
    __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(v));
    return v;
}

// timeline 写点 (p->timelineAddr = 本执行体 timeline 区, 16×u64 槽; 0 = 关闭打点)
inline void timelineMark(const HybmOverlapParam* p, uint32_t slot)
{
    if (p->timelineAddr == 0) return;
    volatile uint64_t* tl = reinterpret_cast<volatile uint64_t*>(p->timelineAddr);
    tl[slot] = readTimelineCycle();
    cacheFlushLine(reinterpret_cast<uintptr_t>(&tl[slot]) & ~static_cast<uintptr_t>(63));
}

// ============================================================================
// 设备侧 HAL 弱符号 (hcomm 模式) — doorbell 走 halSqCqConfig; mode 3 活查询走
// halSqCqQuery (CP1 内 CP 流 SQ 条目可查); mode 1 提交走 halSqMemGet/halSqMsgSend
// ============================================================================
extern "C" {
__attribute__((weak)) int32_t halSqCqConfig(uint32_t devId, HalSqCqConfigInfo* info);
__attribute__((weak)) int32_t halSqCqQuery(uint32_t devId, HalSqCqQueryInfo* info);
__attribute__((weak)) int32_t halSqMemGet(uint32_t devId, void* in, void* out);
__attribute__((weak)) int32_t halSqMsgSend(uint32_t devId, void* msg);
__attribute__((weak)) int32_t halSqCqAllocate(uint32_t devId, void* in, void* out);
__attribute__((weak)) int32_t halSqCqFree(uint32_t devId, void* info);
// 310P3 专属 (hcomm InitChipType: 仅 310P 分支调用): SMMU substream id —
// SDMA 数据搬运的 SMMU 页表路由, SQE src/dst_substreamid 必填, 否则翻译失败 SQE 不执行
__attribute__((weak)) int32_t drvMemSmmuQuery(uint32_t devId, uint32_t* ssid);
// 内部变体 (trs_sqcq.c "internal interface, running in dev cp proc") — 绕过闭源 wrapper
__attribute__((weak)) int32_t _halSqCqAllocate(uint32_t devId, void* in, void* out);
__attribute__((weak)) int32_t _halSqCqFree(uint32_t devId, void* info);
}

// ssid 实时查询 + 诊断戳 (result->pad 低 16 位; 0xFFFF = 符号缺失或查询失败)
// reserved[3] 非零则覆盖 (dump 对照实验用; 严禁生产硬编码)
// [W-k2 同步] 查询失败不再回退 0 — ssid=0 → SMMU 静默翻译失败 (SQE 被消费但 SDMA 不执行,
// 症状: headAfter 部分前进 + flag 轮询超时 850)。母本 0903 已修 (72a6283), fork 当日分支
// 在先未同步 — 本例与 M7 同为 fork 同步缺失的实证 (检视 #988)。
static uint16_t querySsidAndStamp(const HybmOverlapParam* p, HybmOverlapResult* result)
{
    uint32_t ssidRaw = 0;
    if (p->reserved[3] != 0) {
        ssidRaw = p->reserved[3];
    } else if (drvMemSmmuQuery != nullptr && drvMemSmmuQuery(p->devId, &ssidRaw) == 0) {
        // 查询成功
    } else {
        ssidRaw = 0xFFFFu;  // 显式失败值 (0903 事故教训, 与母本 W-k2 对齐)
    }
    result->pad = (result->pad & 0xFFFF0000u) | (ssidRaw & 0xFFFFu);
    return static_cast<uint16_t>(ssidRaw);
}

// [W1] 预算制轮询助手 — invalidate → 探测 slot → 命中即出, 未命中退避重试
// hasSecond=false: 单值槽 (arrival/ready/resultFlag, 命中 = slot[0] != 0)
// hasSecond=true : 16B 邮箱槽 (命中 = slot[1] (words) != 0)
// 混合退避: 前 kSpinIters 次纯 dcci 自旋 (覆盖亚毫秒级等待, 见稳态周期优化),
// 之后 usleep(1000) (预算 ≈ 毫秒数, watchdog 30s 内)。命中后调用方直接读 slot 值。
static bool pollSlot(uint32_t* budget, volatile uint64_t* slot, bool hasSecond)
{
    const uint32_t kSpinIters = 128;
    for (uint32_t r = 0; r < *budget; r++) {
        cacheInvalidateLine(reinterpret_cast<uintptr_t>(slot) & ~static_cast<uintptr_t>(63));
        if (hasSecond ? (slot[1] != 0) : (slot[0] != 0)) {
            *budget -= r;
            return true;
        }
        if (r >= kSpinIters) {
            usleep(1000);
        }
    }
    *budget = 0;
    return false;
}

// host 配方结构 (port_channel.cc 同款)
struct HalSqMemGetInputT { uint32_t type, tsId, sqId, cmdCount, res[8]; };
struct HalSqMemGetOutputT { uint32_t cmdCount; volatile void* cmdPtr; uint32_t pos; uint32_t res[7]; };
struct HalSqMsgInfoT { uint32_t type, tsId, sqId, cmdCount, reportCount, res[8]; };

// halSqCqAllocate 结构 (trs_pkg.h / ascend_hal_define.h 同款)
struct HalSqCqInputInfoT {
    uint32_t type;        // DRV_NORMAL_TYPE = 0
    uint32_t tsId;
    uint32_t sqeSize;     // 64
    uint32_t cqeSize;     // 12
    uint32_t sqeDepth;    // 1024
    uint32_t cqeDepth;    // 1024
    uint32_t grpId;
    uint32_t flag;        // 0 = 全新分配 (非 remote)
    uint32_t cqId;
    uint32_t sqId;
    uint32_t info[5];     // rtStreamAllocInfo_t: streamId/priority/bits/threadDisable/shareSqId
    uint32_t ext_info_len;
    void* ext_info;
    uint32_t res[5];
};
struct HalSqCqOutputInfoT {
    uint32_t sqId;
    uint32_t cqId;
    uint64_t queueVAddr;
    uint32_t flag;
    uint32_t res[5];
};
struct HalSqCqFreeInfoT {
    uint32_t type;
    uint32_t tsId;
    uint32_t sqId;
    uint32_t cqId;
    uint32_t flag;
    uint32_t res[8];
};

static int32_t doorbellViaHal(uint32_t devId, uint32_t sqId, uint32_t newTail)
{
    if (halSqCqConfig == nullptr) {
        return 260;
    }
    HalSqCqConfigInfo cfg = {};
    cfg.type = DRV_NORMAL_TYPE_HAL;
    cfg.tsId = 0;
    cfg.sqId = sqId;
    cfg.cqId = 0;
    cfg.prop = DRV_SQCQ_PROP_SQ_TAIL;
    cfg.value[0] = newTail;
    return halSqCqConfig(devId, &cfg);
}

// ============================================================================
// SQE 构造 (64B, 双格式)
// ============================================================================

// format 0: STARS V2 SDMA SQE (hcomm rtStarsMemcpyAsyncSqeV2_t / shmem stars_sdma_sqe_t 布局)
void buildStarsSdmaSqe(uint8_t sqe[64], uint16_t streamId, uint16_t taskId, uint64_t src, uint64_t dst,
                       uint32_t len, uint32_t devId, uint64_t overflowAddr, uint16_t ssid)
{
    memset(sqe, 0, 64);
    // ===== 逐字节复刻 hcomm 成功 SQE 模板 (SQ 环残留 dump 的 slot2, hcomm 同 sqId 真机执行成功) =====
    // slot2: 09|000000|f1 00|02 00 | 00000000 | 0000ff00 | 00de0e00 | 457f0400 | 457f0400 | 00100000 | ...
    // 0902 die1 dump: off8=f0 00 (die0=f1) — die1 该 bit 不同, 照抄模板
    sqe[0] = 0x09;  // ★ MEMCPY_ASYNC_SQE_V2 真实 type (旧 STARS_SQE_TYPE_SDMA=1 是错的 → 固件不识别 → 假成功)
    memcpy(&sqe[4], &streamId, 2);   // rt_stream_id
    memcpy(&sqe[6], &taskId, 2);     // task_id
    // off8-11: 全 0 (0903 fresh dump 勘正: hcomm 真实提交的 SQE off8=0x00;
    // 此前 f1/f0 来自 SQ 环残留叠加数据, 非本次提交内容 — die1 固件严格校验时被拒)
    sqe[8] = 0x00;
    // off12-15: res3:16 | kernel_credit:8(=0xff) | res4:8
    sqe[14] = 0xff;
    // off16-19: 模板原样 00 de 0e 00 (opcode=0 NOT_ATOMIC + ie_dma/sssv/dssv/sns/dns/qos/sro/dro/overflow_en 位域)
    sqe[16] = 0x00; sqe[17] = 0xde; sqe[18] = 0x0e; sqe[19] = 0x00;
    // off20-27: src_streamid=0x7f45 | src_substreamid=ssid | dst_streamid=0x7f45 | dst_substreamid=ssid
    // [协议常量-有源码出处] SMMU streamid 按 die 区分 (hcomm aicpu_hccl_sqcqv2.cc:101-106
    // AddOneMemcpySqeV2 原文分支, 非实测猜值): die0→0x7F45, die1→0xBF45
    uint16_t smmuStreamId = (devId == 0) ? 0x7F45 : 0xBF45;
    memcpy(&sqe[20], &smmuStreamId, 2);
    // off22-23: src_substreamid = ssid (0903 fresh dump 勘正: hcomm die1 真实提交 src/dst ssid 对称=4;
    // 此前 "die1 src=0" 为 SQ 环残留误读)
    memcpy(&sqe[22], &ssid, 2);

    memcpy(&sqe[24], &smmuStreamId, 2);
    // off26-27: dst_substreamid = ssid
    memcpy(&sqe[26], &ssid, 2);
    // off28-31: length
    memcpy(&sqe[28], &len, 4);
    // off32-39: src 32/32; off40-47: dst 32/32
    uint32_t lo = static_cast<uint32_t>(src & 0xFFFFFFFFu);
    uint32_t hi = static_cast<uint32_t>(src >> 32);
    memcpy(&sqe[32], &lo, 4);
    memcpy(&sqe[36], &hi, 4);
    lo = static_cast<uint32_t>(dst & 0xFFFFFFFFu);
    hi = static_cast<uint32_t>(dst >> 32);
    memcpy(&sqe[40], &lo, 4);
    memcpy(&sqe[44], &hi, 4);
    // off48-51: 模板原样 00 00 20 00; off52-59: overflow_addr 32/32
    sqe[50] = 0x20;
    lo = static_cast<uint32_t>(overflowAddr & 0xFFFFFFFFu);
    hi = static_cast<uint32_t>(overflowAddr >> 32);
    memcpy(&sqe[52], &lo, 4);
    memcpy(&sqe[56], &hi, 4);
}



// ============================================================================
// 提交: 直写 sqBase + GM 写 doorbell (shmem aclshmemi_sdma_post_send 同款, 无需 HAL)
// ============================================================================

struct ChanCtx {
    volatile StarsChannelInfo* ch;  // GM 中的通道元数据 (shadow tail 也要回写)
    uint32_t devId;                 // doorbell hal 路径用
};

static bool g_noDoorbellForTest = false;  // reserved[2]&2: 不敲铃实验

int32_t submitSqe(ChanCtx* c, const uint8_t sqe[64], bool batchDoorbell = false)
{
    uint32_t depth = c->ch->sqDepth;
    uint32_t head = c->ch->sqHead;
    // 读 shadow tail 须先 invalidate (shmem: dcci_cacheline(channel_info + 4))
    cacheInvalidateLine(reinterpret_cast<uintptr_t>(&c->ch->sqTail) & ~static_cast<uintptr_t>(63));
    uint32_t tail = c->ch->sqTail;

    // 流控: used = tail - head (环形)
    uint32_t used = (tail >= head) ? (tail - head) : (depth - head + tail);
    if (depth - used < 2) {
        return 240;  // 环满
    }

    // 直写 SQE
    volatile uint8_t* slot = reinterpret_cast<volatile uint8_t*>(c->ch->sqBase) + static_cast<uint64_t>(tail) * 64;
    {
        const volatile uint64_t* src = reinterpret_cast<const volatile uint64_t*>(sqe);
        volatile uint64_t* dst = reinterpret_cast<volatile uint64_t*>(slot);
        for (uint32_t i = 0; i < RT_CMD_SIZE / 8; i++) {
            dst[i] = src[i];
        }
    }
    // 刷 SQE 到 PoC (SDMA 经一致性域读取)
    uintptr_t flushEnd = reinterpret_cast<uintptr_t>(slot) + RT_CMD_SIZE;
    uintptr_t flushAddr = reinterpret_cast<uintptr_t>(slot) & ~static_cast<uintptr_t>(63);
    for (; flushAddr < flushEnd; flushAddr += 64) {
        cacheFlushLine(flushAddr);
    }
    memBarrier();

    uint32_t newTail = (tail + 1) % depth;
    if (batchDoorbell) {
        // 攒批模式: 只推进 shadow tail, 不敲铃 (调用方整批写完后一次 ConfigSqTail)。
        // tail 不显式 flush: 下次 submitSqe 入口 / 调用方 doorbell 前的 cacheInvalidateLine
        // (civac = clean+invalidate) 会把脏 tail 行自动写回, 语义等价且省一次屏障
        c->ch->sqTail = newTail;
        return 0;
    }
    // doorbell: sqRegBase 有效 → GM 直写 (shmem 同款); 否则 halSqCqConfig (ioctl 路径)
    if (g_noDoorbellForTest) {
        c->ch->sqTail = newTail;
        return 0;  // 实验模式: 只写环+推进影子 tail, 不敲铃
    }
    if (c->ch->sqRegBase != 0) {
        volatile uint32_t* db = reinterpret_cast<volatile uint32_t*>(c->ch->sqRegBase + STARS_SQ_TAIL_OFFSET);
        *db = newTail;
    } else {
        int32_t r = doorbellViaHal(c->devId, c->ch->sqId, newTail);
        if (r != 0) {
            return 260 + (r < 0 ? -r : r);
        }
    }
    // 回写 shadow tail 供下次读取 (刷 cache)
    c->ch->sqTail = newTail;
    cacheFlushLine(reinterpret_cast<uintptr_t>(&c->ch->sqTail) & ~static_cast<uintptr_t>(63));
    memBarrier();
    return 0;
}


// ============================================================================
// CP1 活查询 (submitMode=3, hcomm InitStreamInfo/QuerySqBaseAddr 同款):
// 本 kernel 若经 type-2(AICPUKernel) 路由进 CP1, CP 流的 SQ 元数据就在本进程
// trs 表内 — 直接 halSqCqQuery 查 BASE/DEPTH/HEAD/TAIL (诊断+提交二合一)
// ============================================================================

// 弱符号查询原语 (halSqCqQuery 弱声明见文件头; CP1 内可用)
namespace {

int32_t cp1QueryProp(uint32_t devId, uint32_t sqId, uint32_t prop, uint32_t* v0, uint32_t* v1)
{
    if (halSqCqQuery == nullptr) {
        return 900 + prop;
    }
    HalSqCqQueryInfo q = {};
    q.type = DRV_NORMAL_TYPE_HAL;
    q.tsId = 0;
    q.sqId = sqId;
    q.cqId = 0;
    q.prop = prop;
    int32_t r = halSqCqQuery(devId, &q);
    if (r == 0) {
        if (v0) *v0 = q.value[0];
        if (v1) *v1 = q.value[1];
    }
    return r;  // 原始 drvError (诊断关键: 65534=NOT_SUPPORT, 3=INVALID_VALUE, 0=OK)
}

// mode 3: 活查询 + hcomm 式提交; 失败时把四属性查询结果写入 result 供诊断
// reserved[1] bit0: 提交前尝试 SQ_DISABLE_TO_ENABLE; bit1: 尝试 SQ_RESUME; bit2: 尝试 SQ_PAUSE
int32_t cp1LiveQuery(HybmOverlapParam* p, HybmOverlapResult* result, StarsChannelInfo* localCh)
{
    uint32_t lo = 0, hi = 0, depth = 0, head = 0, tail = 0, status = 0;
    int32_t rStatus = cp1QueryProp(p->devId, p->sqId, DRV_SQCQ_PROP_SQ_STATUS, &status, nullptr);
    int32_t rBase = cp1QueryProp(p->devId, p->sqId, DRV_SQCQ_PROP_SQ_BASE, &lo, &hi);
    int32_t rDepth = cp1QueryProp(p->devId, p->sqId, DRV_SQCQ_PROP_SQ_DEPTH, &depth, nullptr);
    int32_t rHead = cp1QueryProp(p->devId, p->sqId, DRV_SQCQ_PROP_SQ_HEAD, &head, nullptr);
    int32_t rTail = cp1QueryProp(p->devId, p->sqId, DRV_SQCQ_PROP_SQ_TAIL, &tail, nullptr);
    // 诊断打包: 五属性返回码各占一 nibble (sqeCount 高→低: STATUS/BASE/DEPTH/HEAD/TAIL)
    // 0=OK 3=INVALID_VALUE 0xE=NOT_SUPPORT(65534低4位)
    result->sqeCount = ((static_cast<uint32_t>(rStatus) & 0xFu) << 16) |
                       ((static_cast<uint32_t>(rBase) & 0xFu) << 12) | ((static_cast<uint32_t>(rDepth) & 0xFu) << 8) |
                       ((static_cast<uint32_t>(rHead) & 0xFu) << 4) | (static_cast<uint32_t>(rTail) & 0xFu);
    // 尝试激活 (remote SQ 可能创建于禁用态)
    if (halSqCqConfig != nullptr && p->reserved[1] != 0) {
        struct HalSqCqCfg {
            uint32_t type, tsId, sqId, cqId;
            uint32_t prop;
            uint32_t value[8];
        };
        if ((p->reserved[1] & 0x1u) != 0) {
            HalSqCqCfg cfg = {};  // DRV_SQCQ_PROP_SQ_DISABLE_TO_ENABLE = 3
            cfg.type = 0; cfg.tsId = 0; cfg.sqId = p->sqId; cfg.cqId = 0; cfg.prop = 3; cfg.value[0] = 1;
            (void)halSqCqConfig(p->devId, reinterpret_cast<HalSqCqConfigInfo*>(&cfg));
        }
        if ((p->reserved[1] & 0x2u) != 0) {
            HalSqCqCfg cfg = {};  // DRV_SQCQ_PROP_SQ_RESUME = 11
            cfg.type = 0; cfg.tsId = 0; cfg.sqId = p->sqId; cfg.cqId = 0; cfg.prop = 11; cfg.value[0] = 1;
            (void)halSqCqConfig(p->devId, reinterpret_cast<HalSqCqConfigInfo*>(&cfg));
        }
    }
    if (rBase != 0) {
        return 1000 + (rBase & 0xFF);  // 10xx: BASE 查询失败 (drvError 低 8 位)
    }
    if (rDepth != 0) {
        return 1100 + (rDepth & 0xFF);
    }
    uint64_t base = (static_cast<uint64_t>(hi) << 32) | lo;
    if (base == 0 || depth == 0) {
        return 1200;
    }
    localCh->sqBase = base;
    localCh->sqDepth = depth;
    localCh->sqHead = head;
    localCh->sqTail = tail;
    localCh->sqId = p->sqId;
    // zbal/shmem 配方: 查 SQ_REG_BASE (prop=5) → doorbell 直写 sqRegBase+0x8
    uint32_t regLo = 0, regHi = 0;
    int32_t rReg = cp1QueryProp(p->devId, p->sqId, 5 /*DRV_SQCQ_PROP_SQ_REG_BASE*/, &regLo, &regHi);
    if (rReg == 0) {
        localCh->sqRegBase = (static_cast<uint64_t>(regHi) << 32) | regLo;  // 有效则 submitSqe 走直写 doorbell
    } else {
        localCh->sqRegBase = 0;  // 未知 → doorbell 走 halSqCqConfig
    }
    // reserved[1] bit3: 强制 HAL doorbell (hcomm LaunchTask 固定 halSqCqConfig, 不走 GM 直写)
    if ((p->reserved[1] & 0x8u) != 0) {
        localCh->sqRegBase = 0;
    }
    localCh->streamId = static_cast<uint32_t>(p->streamId);
    return 0;
}

}  // namespace


// ============================================================================
// kernel 入口
// ============================================================================
extern "C" __attribute__((visibility("default"))) uint32_t HybmOverlapRun(void* args)
{
    HybmOverlapParam* p = static_cast<HybmOverlapParam*>(args);
    g_noDoorbellForTest = (p->reserved[2] & 0x2u) != 0;
    HybmOverlapResult* result = reinterpret_cast<HybmOverlapResult*>(p->resultAddr);
    result->magic = HYBM_OVERLAP_RESULT_MAGIC;
    result->stage = STAGE_ENTRY;
    result->mainRet = 0;
    result->subRet = 0;
    result->sqBaseLow = 0;
    result->sqDepth = 0;
    result->sqTail = 0;
    result->sqeCount = 0;

    // 通道上下文 (mode 13/14 在各自 case 块内活查询填充/使用)
    StarsChannelInfo localCh = {};
    volatile StarsChannelInfo* ch = nullptr;
    // switch dispatch (SubmitMode 命名 case, 定义于 overlap_wire.hpp)
    switch (p->submitMode) {
    case SUBMIT_MODE_CHAIN: {
        // ===== 八步闭环编排状态机 (通知链: flag-SQE + GM 轮询, 无 notify) =====
        // 邮箱描述符 (AICore 写, 16B): {u64 dataAddr, u64 dataWords} — dataWords!=0 即有效。
        // SQE 对 (两角色同构): 数据(flagSentinelAddr 不涉及) src→dstAddr + 标志 哨兵8B→flagDoneAddr
        //   同一 SQ FIFO 保序: 数据搬完才写标志 (硬件时序)。
        //   哨兵/远端标志初值均 host 预置 (sentinel=magic, 远端 flag=0), 本 kernel 只搬运。
        //
        // role 0 (卡0 发送方): civac 轮询 mailbox → {src,len} → SQE 对(正向) → doorbell → 退
        // role 1 (卡1 编排方): civac 轮询本地 arrival(==magic) → cvac 写本地 ready(magic)
        //                     → civac 轮询 mailbox → SQE 对(回程) → doorbell → 退
        // 轮询上限 20s (< 30s watchdog); 各环节面包屑: mainRet 13xx/14xx 段。
        {
            if (cp1LiveQuery(p, result, &localCh) != 0) {
                result->mainRet = 1300;
                cacheFlushLine(reinterpret_cast<uintptr_t>(result) & ~static_cast<uintptr_t>(63));
                memBarrier();
                return 1;
            }
            ch = &localCh;
            ChanCtx ctx = {ch, p->devId};
            // ssid 实时查询 (主流程同款, 严禁硬编码)
            const uint16_t ssid = querySsidAndStamp(p, result);  // 严禁硬编码, 实时查询
            const uint16_t sqeStreamId = static_cast<uint16_t>(p->streamId);
            result->sqTail = localCh.sqHead;  // headBefore 面包屑 (诊断: headAfter 与之对比)
            // [W1] 累计轮询预算: 每轮 usleep(1ms) → 24000 次 ≈ 24s < 30s watchdog
            // (原每段独立 20000 次, role1 串行最坏 40s — 失败路径被 watchdog 抢杀,
            //  面包屑写不出 → 失败诊断不可靠)
            uint32_t pollBudget = 24000;

            if (p->role == 0) {
                // ---- 卡0: 轮询 AICore 邮箱 (AICore 已产数据+flush) ----
                timelineMark(p, 0);  // [0] 入口
                volatile uint64_t* mb = reinterpret_cast<volatile uint64_t*>(p->mailboxAddr);
                uint64_t src = 0, words = 0;
                if (pollSlot(&pollBudget, mb, true)) {
                    src = mb[0];
                    words = mb[1];
                } else {
                    src = 0; words = 0;
                }
                if (words == 0) {
                    result->mainRet = 1310;  // 20s 未见邮箱 (AICore 未产数据?)
                    cacheFlushLine(reinterpret_cast<uintptr_t>(result) & ~static_cast<uintptr_t>(63));
                    memBarrier();
                    return 1;
                }
                result->sqeCount = static_cast<uint32_t>(words);  // 面包屑: 邮箱内容
                timelineMark(p, 1);  // [1] 邮箱命中 (等待结束)
                // SQE 对: 数据 (邮箱 src → 远端 dst) + 标志 (哨兵 → 远端到达标志)
                uint16_t taskBase = static_cast<uint16_t>(localCh.sqTail);
                uint8_t sqe[64];
                buildStarsSdmaSqe(sqe, sqeStreamId, taskBase, src, p->dstAddr,
                                  words * sizeof(uint32_t), p->devId, p->overflowAddr, ssid);
                if (submitSqe(&ctx, sqe, true) != 0) {
                    result->mainRet = 1320;
                    cacheFlushLine(reinterpret_cast<uintptr_t>(result) & ~static_cast<uintptr_t>(63));
                    memBarrier();
                    return 1;
                }
                buildStarsSdmaSqe(sqe, sqeStreamId, static_cast<uint16_t>(taskBase + 1), p->flagSentinelAddr,
                                  p->flagDoneAddr, 8, p->devId, p->overflowAddr, ssid);
                if (submitSqe(&ctx, sqe, true) != 0) {
                    result->mainRet = 1321;
                    cacheFlushLine(reinterpret_cast<uintptr_t>(result) & ~static_cast<uintptr_t>(63));
                    memBarrier();
                    return 1;
                }
            } else {
                // ---- 卡1: 轮询本地到达标志 (卡0 SDMA flag-SQE 写) ----
                timelineMark(p, 0);  // [0] 入口
                volatile uint64_t* arr = reinterpret_cast<volatile uint64_t*>(p->arrivalAddr);
                uint64_t arrV = 0;
                arrV = pollSlot(&pollBudget, arr, false) ? *arr : 0;
                if (arrV == 0) {
                    result->mainRet = 1410;  // 20s 数据未到达 (卡0 SDMA 未执行/标志未写)
                    cacheFlushLine(reinterpret_cast<uintptr_t>(result) & ~static_cast<uintptr_t>(63));
                    memBarrier();
                    return 1;
                }
                result->sqeCount = static_cast<uint32_t>(arrV & 0xFFFFFFFFu);  // 面包屑: 到达标志值
                timelineMark(p, 1);  // [1] 到达命中 (等待结束)
                // 通知卡1 AICore: 写本地 ready 标志 (cvac 刷出)
                volatile uint64_t* ready = reinterpret_cast<volatile uint64_t*>(p->readyAddr);
                *ready = arrV;  // 透传到达 magic 作为 ready 值 (AICore 判 !=0)
                cacheFlushLine(reinterpret_cast<uintptr_t>(ready) & ~static_cast<uintptr_t>(63));
                memBarrier();
                timelineMark(p, 2);  // [2] ready 写完
                // 轮询 AICore 回程邮箱 (AICore 已计算+flush)
                volatile uint64_t* mb = reinterpret_cast<volatile uint64_t*>(p->mailboxAddr);
                uint64_t src = 0, words = 0;
                if (pollSlot(&pollBudget, mb, true)) {
                    src = mb[0];
                    words = mb[1];
                } else {
                    src = 0; words = 0;
                }
                if (words == 0) {
                    result->mainRet = 1420;  // AICore 未完成计算
                    cacheFlushLine(reinterpret_cast<uintptr_t>(result) & ~static_cast<uintptr_t>(63));
                    memBarrier();
                    return 1;
                }
                result->sqDepth = static_cast<uint32_t>(words);  // 面包屑: 回程邮箱
                timelineMark(p, 3);  // [3] 回程邮箱命中
                // SQE 对: 数据 (本地 dataBuf → 远端卡0 结果区) + 标志 (哨兵 → 卡0 结果标志)
                uint16_t taskBase = static_cast<uint16_t>(localCh.sqTail);
                uint8_t sqe[64];
                buildStarsSdmaSqe(sqe, sqeStreamId, taskBase, src, p->dstAddr,
                                  words * sizeof(uint32_t), p->devId, p->overflowAddr, ssid);
                if (submitSqe(&ctx, sqe, true) != 0) {
                    result->mainRet = 1430;
                    cacheFlushLine(reinterpret_cast<uintptr_t>(result) & ~static_cast<uintptr_t>(63));
                    memBarrier();
                    return 1;
                }
                buildStarsSdmaSqe(sqe, sqeStreamId, static_cast<uint16_t>(taskBase + 1), p->flagSentinelAddr,
                                  p->flagDoneAddr, 8, p->devId, p->overflowAddr, ssid);
                if (submitSqe(&ctx, sqe, true) != 0) {
                    result->mainRet = 1431;
                    cacheFlushLine(reinterpret_cast<uintptr_t>(result) & ~static_cast<uintptr_t>(63));
                    memBarrier();
                    return 1;
                }
            }
            // [2](role0)/[4](role1): SQE 对+doorbell 完 (role1 的 [2] 是 ready 写完, 勿覆盖)
            timelineMark(p, (p->role == 0) ? 2 : 4);
            // 攒批 doorbell: 两条 SQE 已入环, 一次敲铃 (hcomm LaunchTask 同款)
            cacheInvalidateLine(reinterpret_cast<uintptr_t>(&ctx.ch->sqTail) & ~static_cast<uintptr_t>(63));
            uint32_t tailNow = ctx.ch->sqTail;
            if (doorbellViaHal(p->devId, p->sqId, tailNow) != 0) {
                result->mainRet = 1390;
                cacheFlushLine(reinterpret_cast<uintptr_t>(result) & ~static_cast<uintptr_t>(63));
                memBarrier();
                return 1;
            }
            result->stage = STAGE_SUBMITTED;
            // head 前进诊断 (TSFW 消费证据, 2s 上限)
            {
                const uint32_t headBefore = localCh.sqHead;
                uint32_t headA = headBefore;
                for (int i = 0; i < 20; i++) {
                    usleep(100000);
                    if (cp1QueryProp(p->devId, p->sqId, DRV_SQCQ_PROP_SQ_HEAD, &headA, nullptr) == 0 &&
                        headA != headBefore) {
                        break;
                    }
                }
                result->headAfter = headA;
                result->tailAfter = tailNow;
            }
            result->stage = STAGE_DONE;
            result->mainRet = 0;
            cacheFlushLine(reinterpret_cast<uintptr_t>(result) & ~static_cast<uintptr_t>(63));
            memBarrier();
            return 0;
        }
        break;  // mode 13 全路径 return, 此 break 为防御
    }
    case SUBMIT_MODE_PIPELINE: {
        // ===== 多 chunk 流水编排 (mode13 的循环化; roleRsv[1] = chunk 数) =====
        // 邮箱 ring: mailboxAddr[i] = {addr, words} (16B/slot, AICore 按序写)
        // 标志数组: arrivalAddr[i] / flagDoneAddr+i*8 (SDMA flag-SQE 逐 chunk 写)
        // ready 数组: readyAddr[i] (role1 写, AICore dcci 轮询)
        // FIFO 保序: 卡0 只需等 result[N-1] 即全量完成 (卡1 侧链路因果保序)
        {
            const uint32_t chunks = p->roleRsv[1] ? p->roleRsv[1] : 1;
            if (cp1LiveQuery(p, result, &localCh) != 0) {
                result->mainRet = 1300;
                cacheFlushLine(reinterpret_cast<uintptr_t>(result) & ~static_cast<uintptr_t>(63));
                memBarrier();
                return 1;
            }
            ch = &localCh;
            ChanCtx ctx = {ch, p->devId};
            const uint16_t ssid = querySsidAndStamp(p, result);  // 严禁硬编码, 实时查询
            const uint16_t sqeStreamId = static_cast<uint16_t>(p->streamId);
            result->sqTail = localCh.sqHead;  // headBefore
            // [W1] 累计轮询预算: 按 chunk 数放大 (mode13 单 chunk 24000 ≈ 20s 同等裕度;
            // 共享预算不放大时, 前 chunk 偏慢会耗尽后续 chunk 预算造成误判超时)
            uint32_t pollBudget = 24000u * chunks;
            timelineMark(p, 0);               // [0] 入口
            uint16_t taskBase = static_cast<uint16_t>(localCh.sqTail);
            volatile uint64_t* mb = reinterpret_cast<volatile uint64_t*>(p->mailboxAddr);

            if (p->role == 0) {
                // ---- 卡0: 逐 chunk 轮询邮箱 → SQE 对 → doorbell (流水不停顿) ----
                for (uint32_t c = 0; c < chunks; c++) {
                    volatile uint64_t* slot = mb + c * 2;  // 16B/slot
                    uint64_t src = 0, words = 0;
                    if (pollSlot(&pollBudget, slot, true)) {
                        src = slot[0];
                        words = slot[1];
                    } else {
                        src = 0; words = 0;
                    }
                    if (words == 0) {
                        result->mainRet = 1350 + c;  // chunk c 邮箱 20s 未到
                        cacheFlushLine(reinterpret_cast<uintptr_t>(result) & ~static_cast<uintptr_t>(63));
                        memBarrier();
                        return 1;
                    }
                    uint8_t sqe[64];
                    buildStarsSdmaSqe(sqe, sqeStreamId, taskBase, src,
                                      p->dstAddr + static_cast<uint64_t>(c) * (words * 4),  // dst 逐 chunk 递增
                                      static_cast<uint32_t>(words * 4), p->devId, p->overflowAddr, ssid);
                    if (submitSqe(&ctx, sqe, true) != 0) {
                        result->mainRet = 1360 + c;
                        cacheFlushLine(reinterpret_cast<uintptr_t>(result) & ~static_cast<uintptr_t>(63));
                        memBarrier();
                        return 1;
                    }
                    buildStarsSdmaSqe(sqe, sqeStreamId, static_cast<uint16_t>(taskBase + 1), p->flagSentinelAddr,
                                      p->flagDoneAddr + static_cast<uint64_t>(c) * 8, 8, p->devId, p->overflowAddr,
                                      ssid);
                    if (submitSqe(&ctx, sqe, true) != 0) {
                        result->mainRet = 1361 + c;
                        cacheFlushLine(reinterpret_cast<uintptr_t>(result) & ~static_cast<uintptr_t>(63));
                        memBarrier();
                        return 1;
                    }
                    taskBase = static_cast<uint16_t>(taskBase + 2);
                    // 每 chunk 一次 doorbell (流水时延优先)
                    cacheInvalidateLine(reinterpret_cast<uintptr_t>(&ctx.ch->sqTail) & ~static_cast<uintptr_t>(63));
                    if (doorbellViaHal(p->devId, p->sqId, ctx.ch->sqTail) != 0) {
                        result->mainRet = 1390 + c;
                        cacheFlushLine(reinterpret_cast<uintptr_t>(result) & ~static_cast<uintptr_t>(63));
                        memBarrier();
                        return 1;
                    }
                    if (c == 0) timelineMark(p, 1);  // [1] 首对 SQE+doorbell
                }
            } else {
                // ---- 卡1: 逐 chunk [到达→ready→回程邮箱→SQE 对] ----
                volatile uint64_t* arr = reinterpret_cast<volatile uint64_t*>(p->arrivalAddr);
                volatile uint64_t* ready = reinterpret_cast<volatile uint64_t*>(p->readyAddr);
                for (uint32_t c = 0; c < chunks; c++) {
                    volatile uint64_t* arrC = arr + c;  // 8B/entry
                    uint64_t arrV = 0;
                    arrV = pollSlot(&pollBudget, arrC, false) ? *arrC : 0;
                    if (arrV == 0) {
                        result->mainRet = 1450 + c;  // chunk c 20s 未到达
                        cacheFlushLine(reinterpret_cast<uintptr_t>(result) & ~static_cast<uintptr_t>(63));
                        memBarrier();
                        return 1;
                    }
                    if (c == 0) timelineMark(p, 1);  // [1] 首块到达
                    volatile uint64_t* readyC = ready + c;
                    *readyC = arrV;
                    cacheFlushLine(reinterpret_cast<uintptr_t>(readyC) & ~static_cast<uintptr_t>(63));
                    memBarrier();
                    volatile uint64_t* slot = mb + c * 2;
                    uint64_t src = 0, words = 0;
                    if (pollSlot(&pollBudget, slot, true)) {
                        src = slot[0];
                        words = slot[1];
                    } else {
                        src = 0; words = 0;
                    }
                    if (words == 0) {
                        result->mainRet = 1460 + c;
                        cacheFlushLine(reinterpret_cast<uintptr_t>(result) & ~static_cast<uintptr_t>(63));
                        memBarrier();
                        return 1;
                    }
                    if (c == 0) timelineMark(p, 2);  // [2] 首块回程邮箱命中
                    uint8_t sqe[64];
                    buildStarsSdmaSqe(sqe, sqeStreamId, taskBase, src,
                                      p->dstAddr + static_cast<uint64_t>(c) * (words * 4),
                                      static_cast<uint32_t>(words * 4), p->devId, p->overflowAddr, ssid);
                    if (submitSqe(&ctx, sqe, true) != 0) {
                        result->mainRet = 1470 + c;
                        cacheFlushLine(reinterpret_cast<uintptr_t>(result) & ~static_cast<uintptr_t>(63));
                        memBarrier();
                        return 1;
                    }
                    buildStarsSdmaSqe(sqe, sqeStreamId, static_cast<uint16_t>(taskBase + 1), p->flagSentinelAddr,
                                      p->flagDoneAddr + static_cast<uint64_t>(c) * 8, 8, p->devId, p->overflowAddr,
                                      ssid);
                    if (submitSqe(&ctx, sqe, true) != 0) {
                        result->mainRet = 1471 + c;
                        cacheFlushLine(reinterpret_cast<uintptr_t>(result) & ~static_cast<uintptr_t>(63));
                        memBarrier();
                        return 1;
                    }
                    taskBase = static_cast<uint16_t>(taskBase + 2);
                    cacheInvalidateLine(reinterpret_cast<uintptr_t>(&ctx.ch->sqTail) & ~static_cast<uintptr_t>(63));
                    if (doorbellViaHal(p->devId, p->sqId, ctx.ch->sqTail) != 0) {
                        result->mainRet = 1490 + c;
                        cacheFlushLine(reinterpret_cast<uintptr_t>(result) & ~static_cast<uintptr_t>(63));
                        memBarrier();
                        return 1;
                    }
                }
            }
            timelineMark(p, (p->role == 0) ? 2 : 4);  // [2]/[4]: 全部 chunk 编排完
            // head 前进诊断
            {
                uint32_t headA = localCh.sqHead;
                for (int i = 0; i < 20; i++) {
                    usleep(100000);
                    if (cp1QueryProp(p->devId, p->sqId, DRV_SQCQ_PROP_SQ_HEAD, &headA, nullptr) == 0 &&
                        headA != localCh.sqHead) {
                        break;
                    }
                }
                result->headAfter = headA;
            }
            result->stage = STAGE_DONE;
            result->mainRet = 0;
            result->sqeCount = chunks;  // 面包屑: chunk 数
            cacheFlushLine(reinterpret_cast<uintptr_t>(result) & ~static_cast<uintptr_t>(63));
            memBarrier();
            return 0;
        }
        break;  // mode 14 全路径 return, 此 break 为防御
    }
    default:
        result->mainRet = 950;  // 支持域外的 submitMode (支持域: 13/14)
        cacheFlushLine(reinterpret_cast<uintptr_t>(result) & ~static_cast<uintptr_t>(63));
        memBarrier();
        return 1;
    }  // switch
    return 1;  // 不可达 (全部 case 均在块内 return)
}

