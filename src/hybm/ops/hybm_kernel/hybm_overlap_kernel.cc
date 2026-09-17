// MemFabric 310P AICore×AICPU 协同编排 kernel (310P3, 主构建产物)
//
// 溯源: fork 自 xcclpp main examples/tutorials/aicpu-aicore-310p3/aicpu_kernel/
// xcclpp_overlap_aicpu.cpp (MIT), 按 310p-aicore-aicpu-开发进展.md 迁移;
// 重命名 XcclppAicpuSqeRun → HybmOverlapRun (入口/json functionName),
// XcclppAicpuSqeParam/Result → HybmOverlapParam/Result
// (SQE 配方/提交链/轮询纪律均已 hcomm 源码级证实, 见计划附录 A)。
// P0 实证记录(2026-09-10 dev0/dev0↔dev1): E0a CP1 闭环/E0b 池 GVA 可译/
// E0c 八步全链 三实验一次通过(实测记录见 310p-aicore-aicpu-开发进展.md)。
// v2 精简(2026-09-16): 仅保留多 chunk 流水编排 —— mode13 闭环由 chunks=1 等价
// 覆盖, role1 编排方分支/未用弱符号/GM doorbell 直写/SQ 激活等 fork 遗留与实验
// 钩子全数移除。产物 so 名 libmf_overlap_aicpu.so — 与 libcann_hybm_kernel.so 独立
// (CP1 按文件名 dlopen 缓存, 改 kernel 必 bump 设备侧版本名 MF_OVERLAP_KVER)。
//
// 本文件仅支持 param.submitMode == SUBMIT_MODE_PIPELINE(14), 其余值拒绝 mainRet=950:
//   多 chunk 流水编排 (p->chunks = chunk 数, 通信被计算掩盖; chunks=1 即单块语义)
//   内嵌 CP1 活查询 (cp1LiveQuery) + ssid 实时查询 + SQE 直写提交全链。
//
// 完成检测: flag-SQE 哨兵轮询 (尾部 8B SDMA 搬运 + dc civac 轮询)。
// 提交路径: SQE 直写 sqBase + dc cvac 刷 PoC; doorbell 固定走 halSqCqConfig (ioctl)。
// 机制档案见 docs/310p/机制档案.md (SQE配方/通知链/cache纪律/状态机要点)。
//
// SPDX-License-Identifier: MIT

#include <cstdint>   // 定宽整型(uint8_t/uint32_t/uint64_t)
#include <cstring>   // memset/memcpy(SQE 逐字段填充)
#include <unistd.h>  // usleep(轮询退避)

// ============================================================================
// 常量
// ============================================================================
constexpr size_t RT_CMD_SIZE = 64; // SQE 尺寸(64B = 8×u64, 直写循环粒度)

// HAL 常量 (doorbell + 活查询)
constexpr uint32_t DRV_NORMAL_TYPE_HAL = 0;       // HAL 正常类型(查询/配置均填 0)
constexpr uint32_t DRV_SQCQ_PROP_SQ_STATUS = 0;   // 属性编号: SQ 状态
constexpr uint32_t DRV_SQCQ_PROP_SQ_HEAD = 1;     // 属性编号: SQ head(TSFW 消费位置)
constexpr uint32_t DRV_SQCQ_PROP_SQ_TAIL = 2;     // 属性编号: SQ tail(提交位置)
constexpr uint32_t DRV_SQCQ_PROP_SQ_BASE = 6;     // 属性编号: SQ 环基址
constexpr uint32_t DRV_SQCQ_PROP_SQ_DEPTH = 7;    // 属性编号: SQ 环深度
constexpr uint32_t SQCQ_CONFIG_INFO_LENGTH = 8;   // 配置请求 value 数组长度
constexpr uint32_t SQCQ_QUERY_INFO_LENGTH = 8;    // 查询请求 value 数组长度

struct HalSqCqConfigInfo { // HAL SQ/CQ 配置请求(doorbell 用)
    uint32_t type, tsId, sqId, cqId, prop;   // 定位五元组(类型/TS/SQ/CQ/属性编号)
    uint32_t value[SQCQ_CONFIG_INFO_LENGTH]; // 属性参数(如 newTail)
};
struct HalSqCqQueryInfo { // HAL SQ/CQ 查询请求(活查询用)
    uint32_t type, tsId, sqId, cqId, prop;  // 定位五元组(同上)
    uint32_t value[SQCQ_QUERY_INFO_LENGTH]; // 查询结果(如 head/base)
};

// ============================================================================
// kernel 入参 (host 经 aclrtLaunchKernelWithHostArgs 传入)
// ============================================================================

// wire 契约 — 单一事实源(静态断言编译期锁布局)
#include "hybm_overlap_contract.h"


// ============================================================================
// cache 维护 (AICPU 与 SDMA 同 domain, zbal 同款)
// ============================================================================
inline void cacheFlushLine(uintptr_t addr) // 清除缓存行(脏数据写回一致性点, cvac)
{
    __asm__ __volatile__("dc cvac, %0" ::"r"(addr) : "memory");
}
inline void cacheInvalidateLine(uintptr_t addr) // 失效缓存行(读侧取最新值, civac)
{
    __asm__ __volatile__("dc civac, %0" ::"r"(addr) : "memory");
}
inline void memBarrier() // 全系统内存屏障(写序保证)
{
    __asm__ __volatile__("dsb ish" ::: "memory");
}

// P2 时间线打点: AICPU 侧 cycle 读取 (cntvct_el0, 各核独立计数器 — 只做本执行体内相对时长)
inline uint64_t readTimelineCycle() // 读 AICPU cycle 计数器(仅本执行体相对时长)
{
    uint64_t v = 0; // 结果暂存
    __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(v)); // 读通用定时器寄存器
    return v; // 返回 cycle 值
}

// timeline 写点 (p->timelineAddr = 本执行体 timeline 区, 16×u64 槽; 0 = 关闭打点)
inline void timelineMark(const HybmOverlapParam* p, uint32_t slot)
{
    if (p->timelineAddr == 0) return; // 0 = 关闭打点, 直接返回
    volatile uint64_t* tl = reinterpret_cast<volatile uint64_t*>(p->timelineAddr); // timeline 区基址
    tl[slot] = readTimelineCycle(); // 写当前 cycle 到槽位
    cacheFlushLine(reinterpret_cast<uintptr_t>(&tl[slot]) & ~static_cast<uintptr_t>(63)); // 刷出该行(host 可读)
}

// ============================================================================
// 设备侧 HAL 弱符号 (hcomm 模式) — doorbell 走 halSqCqConfig, 活查询走 halSqCqQuery
// (CP1 内 CP 流 SQ 条目可查); 310P3 专属 drvMemSmmuQuery 查 SMMU ssid。
// 弱符号经 CP1 dlopen 环境按名解析, 解析不到时判 nullptr 返回错误码。
// ============================================================================
extern "C" {
__attribute__((weak)) int32_t halSqCqConfig(uint32_t devId, HalSqCqConfigInfo* info); // doorbell 配置入口
__attribute__((weak)) int32_t halSqCqQuery(uint32_t devId, HalSqCqQueryInfo* info);   // SQ/CQ 属性查询入口
// 310P3 专属 (hcomm InitChipType: 仅 310P 分支调用): SMMU substream id —
// SDMA 数据搬运的 SMMU 页表路由, SQE src/dst_substreamid 必填, 否则翻译失败 SQE 不执行
__attribute__((weak)) int32_t drvMemSmmuQuery(uint32_t devId, uint32_t* ssid);
} // extern "C" 结束

// ssid 实时查询 + 诊断戳 (result->pad 低 16 位; 0xFFFF = 符号缺失或查询失败)
// [W-k2 同步] 查询失败不回退 0 — ssid=0 → SMMU 静默翻译失败 (SQE 被消费但 SDMA 不执行,
// 症状: headAfter 部分前进 + flag 轮询超时 850)。母本 0903 已修 (72a6283), fork 当日分支
// 在先未同步 — 本例与 M7 同为 fork 同步缺失的实证 (检视 #988)。
static uint16_t querySsidAndStamp(const HybmOverlapParam* p, HybmOverlapResult* result)
{
    uint32_t ssidRaw = 0; // ssid 原始值
    if (drvMemSmmuQuery != nullptr && drvMemSmmuQuery(p->devId, &ssidRaw) == 0) { // 实时查询成功
        // 查询成功
    } else {
        ssidRaw = 0xFFFFu;  // 显式失败值 (0903 事故教训, 与母本 W-k2 对齐)
    }
    result->pad = (result->pad & 0xFFFF0000u) | (ssidRaw & 0xFFFFu); // pad 低 16 位记 ssid
    return static_cast<uint16_t>(ssidRaw); // 截断到 u16 返回
}

// [W1] 预算制轮询助手 — invalidate → 探测 16B 邮箱槽 → 命中即出, 未命中退避重试
// 命中 = slot[1] (words) != 0
// 混合退避: 前 kSpinIters 次纯自旋 (覆盖亚毫秒级等待, 见稳态周期优化),
// 之后 usleep(1000) (预算 ≈ 毫秒数, watchdog 30s 内)。命中后调用方直接读 slot 值。
static bool pollSlot(uint32_t* budget, volatile uint64_t* slot)
{
    const uint32_t kSpinIters = 128; // 纯自旋上限(前 128 次不睡)
    for (uint32_t r = 0; r < *budget; r++) { // 在剩余预算内重试
        cacheInvalidateLine(reinterpret_cast<uintptr_t>(slot) & ~static_cast<uintptr_t>(63)); // 失效行取新值
        if (slot[1] != 0) { // 探测槽有效(words 非零)
            *budget -= r; // 扣除本轮消耗
            return true;  // 命中
        }
        if (r >= kSpinIters) { // 自旋额度用完
            usleep(1000); // 退避 1ms
        }
    }
    *budget = 0; // 预算耗尽
    return false; // 未命中(超时语义)
}

static int32_t doorbellViaHal(uint32_t devId, uint32_t sqId, uint32_t newTail) // HAL 敲铃(ioctl 路径)
{
    if (halSqCqConfig == nullptr) { // 弱符号未解析(CP1 外环境)
        return 260; // 错误码: 符号缺失
    }
    HalSqCqConfigInfo cfg = {};  // 配置请求(零初始化)
    cfg.type = DRV_NORMAL_TYPE_HAL;   // 正常 HAL 类型
    cfg.tsId = 0;                     // TS 0
    cfg.sqId = sqId;                  // 目标 SQ
    cfg.cqId = 0;                     // CQ 不涉及
    cfg.prop = DRV_SQCQ_PROP_SQ_TAIL; // 属性: 写 tail
    cfg.value[0] = newTail;           // 新 tail 值
    return halSqCqConfig(devId, &cfg); // 原始返回码透传
}

// ============================================================================
// SQE 构造 (64B, 单格式: STARS V2 SDMA; 数据与标志 SQE 同构, 仅地址/长度不同)
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
    uint32_t lo = static_cast<uint32_t>(src & 0xFFFFFFFFu); // src 低 32 位
    uint32_t hi = static_cast<uint32_t>(src >> 32);         // src 高 32 位
    memcpy(&sqe[32], &lo, 4); // src.lo → off32
    memcpy(&sqe[36], &hi, 4); // src.hi → off36
    lo = static_cast<uint32_t>(dst & 0xFFFFFFFFu); // dst 低 32 位
    hi = static_cast<uint32_t>(dst >> 32);         // dst 高 32 位
    memcpy(&sqe[40], &lo, 4); // dst.lo → off40
    memcpy(&sqe[44], &hi, 4); // dst.hi → off44
    // off48-51: 模板原样 00 00 20 00; off52-59: overflow_addr 32/32
    sqe[50] = 0x20; // off48-51 模板原样(0x20 在 off50)
    lo = static_cast<uint32_t>(overflowAddr & 0xFFFFFFFFu); // overflow 低 32 位
    hi = static_cast<uint32_t>(overflowAddr >> 32);         // overflow 高 32 位
    memcpy(&sqe[52], &lo, 4); // overflow.lo → off52
    memcpy(&sqe[56], &hi, 4); // overflow.hi → off56
}



// ============================================================================
// 提交: SQE 直写 sqBase (攒批语义, doorbell 由调用方统一敲 — hcomm LaunchTask 同款)
// ============================================================================

struct ChanCtx {
    volatile StarsChannelInfo* ch;  // GM 中的通道元数据 (shadow tail 也要回写)
    uint32_t devId;                 // doorbell hal 路径用
};

// SQE 入环 (攒批): 只写环 + 推进 shadow tail, 不敲铃 — 调用方整批写完后统一 doorbellViaHal。
// tail 不显式 flush: 下次 submitSqe 入口 / 调用方 doorbell 前的 cacheInvalidateLine
// (civac = clean+invalidate) 会把脏 tail 行自动写回, 语义等价且省一次屏障
int32_t submitSqe(ChanCtx* c, const uint8_t sqe[64])
{
    uint32_t depth = c->ch->sqDepth; // 环深度
    uint32_t head = c->ch->sqHead;   // 当前 head
    // 读 shadow tail 须先 invalidate (shmem: dcci_cacheline(channel_info + 4))
    cacheInvalidateLine(reinterpret_cast<uintptr_t>(&c->ch->sqTail) & ~static_cast<uintptr_t>(63));
    uint32_t tail = c->ch->sqTail; // 当前 shadow tail

    // 流控: used = tail - head (环形)
    uint32_t used = (tail >= head) ? (tail - head) : (depth - head + tail); // 已占用槽数(环形差)
    if (depth - used < 2) {
        return 240;  // 环满 (SQE 对需 2 槽)
    }

    // 直写 SQE
    volatile uint8_t* slot = reinterpret_cast<volatile uint8_t*>(c->ch->sqBase) + static_cast<uint64_t>(tail) * 64;
    {
        const volatile uint64_t* src = reinterpret_cast<const volatile uint64_t*>(sqe); // 源(本地 SQE)
        volatile uint64_t* dst = reinterpret_cast<volatile uint64_t*>(slot);            // 目的(环槽)
        for (uint32_t i = 0; i < RT_CMD_SIZE / 8; i++) { // 8×u64 拷贝 64B
            dst[i] = src[i]; // 逐字写入
        }
    }
    // 刷 SQE 到 PoC (SDMA 经一致性域读取)
    uintptr_t flushEnd = reinterpret_cast<uintptr_t>(slot) + RT_CMD_SIZE; // 刷写区间末尾
    uintptr_t flushAddr = reinterpret_cast<uintptr_t>(slot) & ~static_cast<uintptr_t>(63); // 起始(行对齐)
    for (; flushAddr < flushEnd; flushAddr += 64) { // 逐 64B 行
        cacheFlushLine(flushAddr); // cvac 刷出一行
    }
    memBarrier(); // 写序屏障

    c->ch->sqTail = (tail + 1) % depth; // 推进 shadow tail(环形 +1)
    return 0;
}


// ============================================================================
// CP1 活查询 (hcomm InitStreamInfo/QuerySqBaseAddr 同款):
// 本 kernel 若经 type-2(AICPUKernel) 路由进 CP1, CP 流的 SQ 元数据就在本进程
// trs 表内 — 直接 halSqCqQuery 查 BASE/DEPTH/HEAD/TAIL (诊断+提交二合一)
// ============================================================================

// 弱符号查询原语 (halSqCqQuery 弱声明见文件头; CP1 内可用)
namespace {

int32_t cp1QueryProp(uint32_t devId, uint32_t sqId, uint32_t prop, uint32_t* v0, uint32_t* v1)
{
    if (halSqCqQuery == nullptr) { // 弱符号缺失
        return 900 + prop; // 错误码: 9xx 段(带属性号)
    }
    HalSqCqQueryInfo q = {};      // 查询请求(零初始化)
    q.type = DRV_NORMAL_TYPE_HAL; // 正常 HAL 类型
    q.tsId = 0;                   // TS 0
    q.sqId = sqId;                // 目标 SQ
    q.cqId = 0;                   // CQ 不涉及
    q.prop = prop;                // 查询属性编号
    int32_t r = halSqCqQuery(devId, &q); // 发起查询
    if (r == 0) { // 查询成功
        if (v0) *v0 = q.value[0]; // 出参 0(可空)
        if (v1) *v1 = q.value[1]; // 出参 1(可空)
    }
    return r;  // 原始 drvError (诊断关键: 65534=NOT_SUPPORT, 3=INVALID_VALUE, 0=OK)
}

// 活查询 + 提交上下文装配; 失败时把五属性查询结果写入 result 供诊断
int32_t cp1LiveQuery(HybmOverlapParam* p, HybmOverlapResult* result, StarsChannelInfo* localCh)
{
    uint32_t lo = 0, hi = 0, depth = 0, head = 0, tail = 0, status = 0; // 五属性查询结果暂存
    int32_t rStatus = cp1QueryProp(p->devId, p->sqId, DRV_SQCQ_PROP_SQ_STATUS, &status, nullptr); // 查状态
    int32_t rBase = cp1QueryProp(p->devId, p->sqId, DRV_SQCQ_PROP_SQ_BASE, &lo, &hi);             // 查基址
    int32_t rDepth = cp1QueryProp(p->devId, p->sqId, DRV_SQCQ_PROP_SQ_DEPTH, &depth, nullptr);    // 查深度
    int32_t rHead = cp1QueryProp(p->devId, p->sqId, DRV_SQCQ_PROP_SQ_HEAD, &head, nullptr);       // 查 head
    int32_t rTail = cp1QueryProp(p->devId, p->sqId, DRV_SQCQ_PROP_SQ_TAIL, &tail, nullptr);       // 查 tail
    // 诊断打包: 五属性返回码各占一 nibble (sqeCount 高→低: STATUS/BASE/DEPTH/HEAD/TAIL)
    // 0=OK 3=INVALID_VALUE 0xE=NOT_SUPPORT(65534低4位)
    result->sqeCount = ((static_cast<uint32_t>(rStatus) & 0xFu) << 16) |
                       ((static_cast<uint32_t>(rBase) & 0xFu) << 12) | ((static_cast<uint32_t>(rDepth) & 0xFu) << 8) |
                       ((static_cast<uint32_t>(rHead) & 0xFu) << 4) | (static_cast<uint32_t>(rTail) & 0xFu);
    if (rBase != 0) { // BASE 查询失败
        return 1000 + (rBase & 0xFF);  // 10xx: BASE 查询失败 (drvError 低 8 位)
    }
    if (rDepth != 0) { // DEPTH 查询失败
        return 1100 + (rDepth & 0xFF); // 11xx: DEPTH 失败
    }
    uint64_t base = (static_cast<uint64_t>(hi) << 32) | lo; // 拼接 64 位环基址
    if (base == 0 || depth == 0) { // 基址或深度无效
        return 1200; // 12xx: 无效通道
    }
    localCh->sqBase = base;   // 填通道: 环基址
    localCh->sqDepth = depth; // 填通道: 深度
    localCh->sqHead = head;   // 填通道: head
    localCh->sqTail = tail;   // 填通道: tail
    localCh->sqId = p->sqId;  // 填通道: sqId
    localCh->streamId = static_cast<uint32_t>(p->streamId); // 填通道: 流 id
    return 0; // 活查询成功
}

}  // namespace


// ============================================================================
// kernel 入口
// ============================================================================
extern "C" __attribute__((visibility("default"))) uint32_t HybmOverlapRun(void* args)
{
    HybmOverlapParam* p = static_cast<HybmOverlapParam*>(args); // 入参指针(host 装配的 120B wire 结构)
    HybmOverlapResult* result = reinterpret_cast<HybmOverlapResult*>(p->resultAddr); // 结果指针(kernel 写/host 读)
    result->magic = HYBM_OVERLAP_RESULT_MAGIC; // 存活魔数(证明 kernel 执行过)
    result->stage = STAGE_ENTRY;               // 阶段=入口
    result->mainRet = 0;                       // 主返回码清零
    result->subRet = 0;                        // 子返回码清零
    result->sqBaseLow = 0;                     // 环基址低位清零
    result->sqDepth = 0;                       // 环深度清零
    result->sqTail = 0;                        // tail 面包屑清零
    result->sqeCount = 0;                      // 计数清零

    if (p->submitMode != SUBMIT_MODE_PIPELINE) { // 域外模式(仅支持流水 14)
        result->mainRet = 950;  // 支持域外的 submitMode (支持域: 仅 14)
        cacheFlushLine(reinterpret_cast<uintptr_t>(result) & ~static_cast<uintptr_t>(63)); // 刷出结果
        memBarrier(); // 屏障
        return 1;     // 失败退出
    }

    // ===== 多 chunk 流水编排 (mode14; p->chunks = chunk 数, 0 视为 1) =====
    // 邮箱 ring: mailboxAddr[i] = {addr, words} (16B/slot, AICore 按序写)
    // 标志数组: flagDoneAddr+i*8 (SDMA flag-SQE 逐 chunk 写)
    // FIFO 保序: 发送侧只需等最后一块结果标志即全量完成 (对端链路因果保序)
    const uint32_t chunks = p->chunks ? p->chunks : 1; // chunk 数(0 视为 1)
    StarsChannelInfo localCh = {};                     // 通道上下文(活查询填充)
    if (cp1LiveQuery(p, result, &localCh) != 0) {      // CP1 活查询失败
        result->mainRet = 1300; // 13xx: 活查询失败
        cacheFlushLine(reinterpret_cast<uintptr_t>(result) & ~static_cast<uintptr_t>(63)); // 刷出结果
        memBarrier(); // 屏障
        return 1;     // 失败退出
    }
    ChanCtx ctx = {&localCh, p->devId}; // 提交上下文(通道+设备)
    const uint16_t ssid = querySsidAndStamp(p, result);  // 严禁硬编码, 实时查询
    const uint16_t sqeStreamId = static_cast<uint16_t>(p->streamId); // SQE 的 rt_stream_id
    result->sqTail = localCh.sqHead;  // headBefore 面包屑
    // [W1] 累计轮询预算: 按 chunk 数放大 (单 chunk 24000 ≈ 20s 同等裕度;
    // 共享预算不放大时, 前 chunk 偏慢会耗尽后续 chunk 预算造成误判超时)
    uint32_t pollBudget = 24000u * chunks;
    timelineMark(p, 0);  // [0] 入口
    uint16_t taskBase = static_cast<uint16_t>(localCh.sqTail);
    volatile uint64_t* mb = reinterpret_cast<volatile uint64_t*>(p->mailboxAddr);

    // ---- 逐 chunk 轮询邮箱 → SQE 对 → doorbell (流水不停顿) ----
    for (uint32_t c = 0; c < chunks; c++) {
        volatile uint64_t* slot = mb + c * 2;  // 16B/slot(邮箱 ring 第 c 槽)
        uint64_t src = 0, words = 0; // 槽内容暂存
        if (pollSlot(&pollBudget, slot)) { // 轮询本 chunk 邮箱槽
            src = slot[0];   // 取数据地址
            words = slot[1]; // 取字数
        } else { // 轮询超时
            src = 0; words = 0; // 清零标记未命中
        }
        if (words == 0) { // 邮箱未命中
            result->mainRet = 1350 + c;  // chunk c 邮箱 20s 未到
            cacheFlushLine(reinterpret_cast<uintptr_t>(result) & ~static_cast<uintptr_t>(63)); // 刷出结果
            memBarrier(); // 屏障
            return 1;     // 失败退出
        }
        uint8_t sqe[64];
        buildStarsSdmaSqe(sqe, sqeStreamId, taskBase, src,
                          p->dstAddr + static_cast<uint64_t>(c) * (words * 4),  // dst 逐 chunk 递增
                          static_cast<uint32_t>(words * 4), p->devId, p->overflowAddr, ssid);
        if (submitSqe(&ctx, sqe) != 0) { // 数据 SQE 入环失败
            result->mainRet = 1360 + c; // 13xx+c: 数据失败
            cacheFlushLine(reinterpret_cast<uintptr_t>(result) & ~static_cast<uintptr_t>(63)); // 刷出结果
            memBarrier(); // 屏障
            return 1;     // 失败退出
        }
        buildStarsSdmaSqe(sqe, sqeStreamId, static_cast<uint16_t>(taskBase + 1), p->flagSentinelAddr,
                          p->flagDoneAddr + static_cast<uint64_t>(c) * 8, 8, p->devId, p->overflowAddr,
                          ssid);
        if (submitSqe(&ctx, sqe) != 0) { // 标志 SQE 入环失败
            result->mainRet = 1361 + c; // 13xx+c: 标志失败
            cacheFlushLine(reinterpret_cast<uintptr_t>(result) & ~static_cast<uintptr_t>(63)); // 刷出结果
            memBarrier(); // 屏障
            return 1;     // 失败退出
        }
        taskBase = static_cast<uint16_t>(taskBase + 2); // task_id 前进 2
        // 每 chunk 一次 doorbell (流水时延优先)
        // 读最新 shadow tail
        cacheInvalidateLine(reinterpret_cast<uintptr_t>(&ctx.ch->sqTail) & ~static_cast<uintptr_t>(63));
        if (doorbellViaHal(p->devId, p->sqId, ctx.ch->sqTail) != 0) { // 每 chunk 一次敲铃
            result->mainRet = 1390 + c; // 13xx+c: 敲铃失败
            cacheFlushLine(reinterpret_cast<uintptr_t>(result) & ~static_cast<uintptr_t>(63)); // 刷出结果
            memBarrier(); // 屏障
            return 1;     // 失败退出
        }
        if (c == 0) timelineMark(p, 1);  // [1] 首对 SQE+doorbell
    }
    timelineMark(p, 2);  // [2]: 全部 chunk 编排完
    // head 前进诊断 (TSFW 消费证据, 2s 上限)
    {
        uint32_t headA = localCh.sqHead; // 轮询值初值(headBefore)
        for (int i = 0; i < 20; i++) { // 最多 20 轮(约 2s)
            usleep(100000); // 每轮 100ms
            if (cp1QueryProp(p->devId, p->sqId, DRV_SQCQ_PROP_SQ_HEAD, &headA, nullptr) == 0 &&
                headA != localCh.sqHead) { // head 前进即出
                break;
            }
        }
        result->headAfter = headA; // 面包屑: 消费后 head
    }
    result->stage = STAGE_DONE;   // 阶段=完成
    result->mainRet = 0;          // 主返回码=成功
    result->sqeCount = chunks;  // 面包屑: chunk 数
    cacheFlushLine(reinterpret_cast<uintptr_t>(result) & ~static_cast<uintptr_t>(63)); // 刷出结果
    memBarrier(); // 屏障
    return 0;     // 成功退出
}
