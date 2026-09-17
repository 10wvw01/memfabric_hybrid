/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You can obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

/* hybm_sdma_orch_kernel.cc — 310P 设备发起 SDMA 的 AICPU 编排 kernel
 *
 * 职责: 消费 AICore 写入工作区邮箱 ring 的搬运描述符, 逐 chunk 构造 SDMA SQE 对
 * (数据搬运 + 8B 标志写入) 直投 TSFW 提交队列, 使搬运与 AICore 计算重叠。
 *
 * 提交链: SQE 直写 sqBase + dc cvac 刷 PoC; doorbell 走 halSqCqConfig;
 * 通道元数据(BASE/DEPTH/HEAD/TAIL)经 halSqCqQuery 从 CP1 进程 trs 表活查询。
 * 完成检测由标志 SQE 承载: 数据与标志 SQE 同队列 FIFO 保序, 消费侧轮询标志
 * 即知数据就绪, 无需 CQE。
 *
 * SQE 64B 配方为 STARS V2 布局, 经 hcomm 源码(AddOneMemcpySqeV2)逐字段比对
 * 与真机执行校验; 协同机制原型参见 xcclpp aicpu-aicore-310p3 教程(MIT)。
 * 配方与固件强耦合: 改动必须实机验证并 bump MF_SDMA_ORCH_KVER 重部署
 * (CP1 按文件名 dlopen 缓存, 同名不重载)。
 *
 * 入口 HybmSdmaOrchBatch 仅支持 SUBMIT_MODE_PIPELINE(多 chunk 流水,
 * chunks=1 即单块语义), 域外 submitMode 拒绝(mainRet=950)。
 */

#include <cstdint>   // 定宽整型(uint8_t/uint32_t/uint64_t)
#include <cstring>   // memset/memcpy(SQE 逐字段填充)
#include <unistd.h>  // usleep(轮询退避)

#include "hybm_sdma_contract.h" // wire 契约(host↔kernel 单一事实源, 布局锁)

// 设备侧 HAL 弱符号 — CP1 dlopen 环境按名解析, 解析不到判 nullptr 报错:
// doorbell 走 halSqCqConfig, 活查询走 halSqCqQuery(CP 流 SQ 条目可查),
// drvMemSmmuQuery 查 SMMU ssid(SQE src/dst_substreamid 必填, 否则翻译失败)。
extern "C" {
__attribute__((weak)) int32_t halSqCqConfig(uint32_t devId, void *info); // doorbell 配置入口
__attribute__((weak)) int32_t halSqCqQuery(uint32_t devId, void *info);  // SQ/CQ 属性查询入口
__attribute__((weak)) int32_t drvMemSmmuQuery(uint32_t devId, uint32_t *ssid); // SMMU ssid 查询
} // extern "C"

namespace {
// ============================================================================
// 常量(HAL 镜像词保持驱动侧命名)
// ============================================================================
constexpr uint32_t DRV_NORMAL_TYPE_HAL = 0;     // HAL 正常类型(查询/配置均填 0)
constexpr uint32_t DRV_SQCQ_PROP_SQ_STATUS = 0; // 属性编号: SQ 状态
constexpr uint32_t DRV_SQCQ_PROP_SQ_HEAD = 1;   // 属性编号: SQ head(TSFW 消费位置)
constexpr uint32_t DRV_SQCQ_PROP_SQ_TAIL = 2;   // 属性编号: SQ tail(提交位置)
constexpr uint32_t DRV_SQCQ_PROP_SQ_BASE = 6;   // 属性编号: SQ 环基址
constexpr uint32_t DRV_SQCQ_PROP_SQ_DEPTH = 7;  // 属性编号: SQ 环深度
constexpr uint32_t kInfoValueLen = 8;           // HAL 请求 value 数组长度(配置/查询同)
constexpr size_t kSqeSize = 64;                 // SQE 尺寸(64B = 8×u64, 直写循环粒度)
constexpr uint32_t kSpinIters = 128;            // 邮箱轮询纯自旋上限(覆盖亚毫秒等待)
constexpr uint32_t kPollBackoffUs = 100;        // 轮询退避粒度(保亚毫秒级/chunk 流水节拍)
constexpr uint32_t kPollBudgetPerChunk = 200000; // 单 chunk 轮询预算(×100µs ≈ 20s < watchdog 30s)
constexpr uint32_t kHeadPollRounds = 20000;     // head 前进采样轮数上限(×100µs ≈ 2s)
constexpr uint32_t kHeadPollBackoffUs = 100;    // head 采样退避粒度

// HAL 请求结构(HAL 侧按定长 value 数组解释)
struct HalConfigInfo { // SQ/CQ 配置请求(doorbell 用)
    uint32_t type, tsId, sqId, cqId, prop; // 定位五元组(类型/TS/SQ/CQ/属性编号)
    uint32_t value[kInfoValueLen];         // 属性参数(如 newTail)
};
struct HalQueryInfo { // SQ/CQ 查询请求(活查询用)
    uint32_t type, tsId, sqId, cqId, prop; // 定位五元组(同上)
    uint32_t value[kInfoValueLen];         // 查询结果(如 head/base)
};

// ============================================================================
// cache 维护原语(AICPU 与 SDMA 同 domain)
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

// timeline 打点: p->timelineAddr 为本执行体 timeline 区(16×u64), 0=关闭
inline void timelineMark(const HybmSdmaOrchParam *p, uint32_t slot)
{
    if (p->timelineAddr == 0) {
        return; // 关闭打点直接返回
    }
    uint64_t v = 0; // cycle 暂存
    __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(v)); // 读通用定时器(本执行体相对时长)
    volatile uint64_t *tl = reinterpret_cast<volatile uint64_t *>(p->timelineAddr); // timeline 区基址
    tl[slot] = v; // 写当前 cycle 到槽位
    cacheFlushLine(reinterpret_cast<uintptr_t>(&tl[slot]) & ~static_cast<uintptr_t>(63)); // 刷出该行(host 可读)
}

// ssid 实时查询 + 诊断戳(result->pad 低 16 位)。
// 查询失败不回退 0: ssid=0 会导致 SMMU 静默翻译失败(SQE 被消费但 SDMA 不执行),
// 故显式记 0xFFFF 让错误可见。
uint16_t querySsidAndStamp(const HybmSdmaOrchParam *p, HybmSdmaOrchResult *result)
{
    uint32_t ssidRaw = 0; // ssid 原始值
    if (drvMemSmmuQuery == nullptr || drvMemSmmuQuery(p->devId, &ssidRaw) != 0) {
        ssidRaw = 0xFFFFu; // 显式失败值(勿回退 0)
    }
    result->pad = (result->pad & 0xFFFF0000u) | (ssidRaw & 0xFFFFu); // pad 低 16 位记 ssid
    return static_cast<uint16_t>(ssidRaw); // 截断到 u16 返回
}

// 预算制邮箱轮询: invalidate → 探测 16B 槽(命中=words 非零) → 命中即出。
// 混合退避: 前 kSpinIters 次纯自旋, 之后 100µs 细粒度退避(粗退避会抬升流水节拍)。
bool pollSlot(uint32_t *budget, volatile uint64_t *slot)
{
    for (uint32_t r = 0; r < *budget; r++) { // 在剩余预算内重试
        cacheInvalidateLine(reinterpret_cast<uintptr_t>(slot) & ~static_cast<uintptr_t>(63)); // 失效行取新值
        if (slot[1] != 0) { // 槽有效(words 非零)
            *budget -= r; // 扣除本轮消耗
            return true;  // 命中
        }
        if (r >= kSpinIters) { // 自旋额度用完
            usleep(kPollBackoffUs); // 退避 100µs
        }
    }
    *budget = 0; // 预算耗尽
    return false; // 未命中(超时语义)
}

// doorbell: 经 HAL 写 SQ tail(TSFW 即刻可见新 SQE)
int32_t doorbellViaHal(uint32_t devId, uint32_t sqId, uint32_t newTail)
{
    if (halSqCqConfig == nullptr) { // 弱符号未解析(CP1 外环境)
        return 260; // 错误码: 符号缺失
    }
    HalConfigInfo cfg = {}; // 配置请求(零初始化)
    cfg.type = DRV_NORMAL_TYPE_HAL;   // 正常 HAL 类型
    cfg.tsId = 0;                     // TS 0
    cfg.sqId = sqId;                  // 目标 SQ
    cfg.cqId = 0;                     // CQ 不涉及
    cfg.prop = DRV_SQCQ_PROP_SQ_TAIL; // 属性: 写 tail
    cfg.value[0] = newTail;           // 新 tail 值
    return halSqCqConfig(devId, &cfg); // 原始返回码透传
}

// SQE 构造(64B, STARS V2 SDMA 布局; 数据与标志 SQE 同构, 仅地址/长度不同)。
// 逐字段依据 hcomm AddOneMemcpySqeV2 源码与真机成功样本比对; SQ 环残留会
// 叠加旧数据, 构造前必须整体清零, 勿按环内容反推字段值。
void buildStarsSdmaSqe(uint8_t sqe[kSqeSize], uint16_t streamId, uint16_t taskId, uint64_t src, uint64_t dst,
                       uint32_t len, uint32_t devId, uint64_t overflowAddr, uint16_t ssid)
{
    memset(sqe, 0, kSqeSize); // 全零打底(残留数据勿入环)
    sqe[0] = 0x09; // type: MEMCPY_ASYNC_SQE_V2(固件认的魔数, 旧值 1 不被识别)
    memcpy(&sqe[4], &streamId, 2); // off4: rt_stream_id
    memcpy(&sqe[6], &taskId, 2);   // off6: task_id
    sqe[8] = 0x00;                 // off8-11: 全 0(固件严格校验该域)
    sqe[14] = 0xff;                // off12-15: kernel_credit=0xff, 其余保留
    sqe[16] = 0x00;                // off16-19: 位域模板(opcode=NOT_ATOMIC + ie_dma 等)
    sqe[17] = 0xde;
    sqe[18] = 0x0e;
    sqe[19] = 0x00;
    // SMMU streamid 按 die 区分(hcomm AddOneMemcpySqeV2 分支): die0→0x7F45, die1→0xBF45
    uint16_t smmuStreamId = (devId == 0) ? 0x7F45 : 0xBF45; // src/dst 同值
    memcpy(&sqe[20], &smmuStreamId, 2); // off20: src_streamid
    memcpy(&sqe[22], &ssid, 2);         // off22: src_substreamid(实时查询)
    memcpy(&sqe[24], &smmuStreamId, 2); // off24: dst_streamid
    memcpy(&sqe[26], &ssid, 2);         // off26: dst_substreamid(与 src 对称)
    memcpy(&sqe[28], &len, 4);          // off28: length(字节)
    uint32_t lo = static_cast<uint32_t>(src & 0xFFFFFFFFu); // src 低 32 位
    uint32_t hi = static_cast<uint32_t>(src >> 32);         // src 高 32 位
    memcpy(&sqe[32], &lo, 4); // off32: src.lo
    memcpy(&sqe[36], &hi, 4); // off36: src.hi
    lo = static_cast<uint32_t>(dst & 0xFFFFFFFFu); // dst 低 32 位
    hi = static_cast<uint32_t>(dst >> 32);         // dst 高 32 位
    memcpy(&sqe[40], &lo, 4); // off40: dst.lo
    memcpy(&sqe[44], &hi, 4); // off44: dst.hi
    sqe[50] = 0x20; // off48-51: 模板位(0x20 在 off50)
    lo = static_cast<uint32_t>(overflowAddr & 0xFFFFFFFFu); // overflow 低 32 位
    hi = static_cast<uint32_t>(overflowAddr >> 32);         // overflow 高 32 位
    memcpy(&sqe[52], &lo, 4); // off52: overflow.lo
    memcpy(&sqe[56], &hi, 4); // off56: overflow.hi
}

// 提交上下文: 通道元数据(GM) + 设备号
struct SqContext {
    volatile StarsChannelInfo *ch; // 通道元数据(shadow tail 也要回写)
    uint32_t devId;                // doorbell HAL 路径用
};

// SQE 入环: 只写环 + 推进 shadow tail, 不敲铃(调用方统一 doorbell)。
// tail 不显式 flush: 下次入环/doorbell 前的 cacheInvalidateLine(civac=clean+invalidate)
// 会把脏 tail 行自动写回, 语义等价且省一次屏障。
int32_t submitSqe(SqContext *c, const uint8_t sqe[kSqeSize])
{
    uint32_t depth = c->ch->sqDepth; // 环深度
    uint32_t head = c->ch->sqHead;   // 当前 head
    cacheInvalidateLine(reinterpret_cast<uintptr_t>(&c->ch->sqTail) & ~static_cast<uintptr_t>(63)); // 失效取 tail
    uint32_t tail = c->ch->sqTail; // 当前 shadow tail
    uint32_t used = (tail >= head) ? (tail - head) : (depth - head + tail); // 已占用槽数(环形差)
    if (depth - used < 2) {
        return 240; // 环满(SQE 对需 2 槽)
    }
    volatile uint8_t *slot = reinterpret_cast<volatile uint8_t *>(c->ch->sqBase) + // 环槽基址
                             static_cast<uint64_t>(tail) * kSqeSize;
    const volatile uint64_t *src = reinterpret_cast<const volatile uint64_t *>(sqe); // 源(本地 SQE)
    volatile uint64_t *dst = reinterpret_cast<volatile uint64_t *>(slot);            // 目的(环槽)
    for (uint32_t i = 0; i < kSqeSize / 8; i++) { // 8×u64 拷贝 64B
        dst[i] = src[i]; // 逐字写入
    }
    uintptr_t flushEnd = reinterpret_cast<uintptr_t>(slot) + kSqeSize;             // 刷写区间末尾
    uintptr_t flushAddr = reinterpret_cast<uintptr_t>(slot) & ~static_cast<uintptr_t>(63); // 起始(行对齐)
    for (; flushAddr < flushEnd; flushAddr += 64) { // 逐 64B 行
        cacheFlushLine(flushAddr); // cvac 刷出(SDMA 经一致性域读取)
    }
    memBarrier(); // 写序屏障
    c->ch->sqTail = (tail + 1) % depth; // 推进 shadow tail(环形 +1)
    return 0;
}

// HAL 单属性查询(弱符号缺失返回 9xx 段错误码)
int32_t cp1QueryProp(uint32_t devId, uint32_t sqId, uint32_t prop, uint32_t *v0, uint32_t *v1)
{
    if (halSqCqQuery == nullptr) { // 弱符号缺失
        return 900 + prop; // 错误码: 9xx 段(带属性号)
    }
    HalQueryInfo q = {};      // 查询请求(零初始化)
    q.type = DRV_NORMAL_TYPE_HAL; // 正常 HAL 类型
    q.tsId = 0;                   // TS 0
    q.sqId = sqId;                // 目标 SQ
    q.cqId = 0;                   // CQ 不涉及
    q.prop = prop;                // 查询属性编号
    int32_t r = halSqCqQuery(devId, &q); // 发起查询
    if (r == 0) { // 查询成功
        if (v0 != nullptr) {
            *v0 = q.value[0]; // 出参 0(可空)
        }
        if (v1 != nullptr) {
            *v1 = q.value[1]; // 出参 1(可空)
        }
    }
    return r; // 原始 drvError(诊断: 65534=NOT_SUPPORT, 3=INVALID_VALUE, 0=OK)
}

// 活查询 + 提交上下文装配; 五属性查询返回码打包进 result 供诊断(nibble 各占 4 位)
int32_t cp1LiveQuery(const HybmSdmaOrchParam *p, HybmSdmaOrchResult *result, StarsChannelInfo *localCh)
{
    uint32_t lo = 0, hi = 0, depth = 0, head = 0, tail = 0, status = 0; // 五属性查询结果暂存
    int32_t rStatus = cp1QueryProp(p->devId, p->sqId, DRV_SQCQ_PROP_SQ_STATUS, &status, nullptr); // 查状态
    int32_t rBase = cp1QueryProp(p->devId, p->sqId, DRV_SQCQ_PROP_SQ_BASE, &lo, &hi);             // 查基址
    int32_t rDepth = cp1QueryProp(p->devId, p->sqId, DRV_SQCQ_PROP_SQ_DEPTH, &depth, nullptr);    // 查深度
    int32_t rHead = cp1QueryProp(p->devId, p->sqId, DRV_SQCQ_PROP_SQ_HEAD, &head, nullptr);       // 查 head
    int32_t rTail = cp1QueryProp(p->devId, p->sqId, DRV_SQCQ_PROP_SQ_TAIL, &tail, nullptr);       // 查 tail
    // 诊断打包: 五属性返回码各占一 nibble(sqeCount 高→低: STATUS/BASE/DEPTH/HEAD/TAIL)
    result->sqeCount = ((static_cast<uint32_t>(rStatus) & 0xFu) << 16) |
                       ((static_cast<uint32_t>(rBase) & 0xFu) << 12) | ((static_cast<uint32_t>(rDepth) & 0xFu) << 8) |
                       ((static_cast<uint32_t>(rHead) & 0xFu) << 4) | (static_cast<uint32_t>(rTail) & 0xFu);
    if (rBase != 0) { // BASE 查询失败
        return 1000 + (rBase & 0xFF); // 10xx: BASE 失败(drvError 低 8 位)
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

// 结果写回并刷出(host 经 D2H 可读)
void finishResult(HybmSdmaOrchResult *result)
{
    cacheFlushLine(reinterpret_cast<uintptr_t>(result) & ~static_cast<uintptr_t>(63)); // 刷出结果行
    memBarrier(); // 屏障
}

// 失败收尾: 记 mainRet + 刷出 + 返回失败码
uint32_t failResult(HybmSdmaOrchResult *result, int32_t mainRet)
{
    result->mainRet = mainRet; // 主返回码(13xx 段定位步骤)
    finishResult(result);      // 刷出
    return 1;                  // 失败退出
}

// 逐 chunk 编排: 轮询邮箱 → SQE 对构造提交 → doorbell(流水不停顿)。
// 返回 0 成功; 失败时 result->mainRet 已置(1350+c 邮箱超时/1360+c 数据 SQE/1361+c
// 标志 SQE/1390+c doorbell), 返回 1。
uint32_t orchestrateChunks(const HybmSdmaOrchParam *p, HybmSdmaOrchResult *result, SqContext *ctx, uint16_t ssid,
                           uint32_t chunks)
{
    const uint16_t sqeStreamId = static_cast<uint16_t>(p->streamId); // SQE 的 rt_stream_id
    uint32_t pollBudget = kPollBudgetPerChunk * chunks; // 累计轮询预算(按 chunk 放大防误判超时)
    uint16_t taskBase = static_cast<uint16_t>(ctx->ch->sqTail); // task_id 基(从当前 tail 起)
    volatile uint64_t *mb = reinterpret_cast<volatile uint64_t *>(p->mailboxAddr); // 邮箱 ring 基址
    uint8_t sqe[kSqeSize]; // SQE 构造缓冲
    timelineMark(p, 0); // 打点[0]: 入口
    for (uint32_t c = 0; c < chunks; c++) { // 逐 chunk 流水
        volatile uint64_t *slot = mb + c * 2; // 16B/slot(邮箱 ring 第 c 槽)
        uint64_t src = 0, words = 0; // 槽内容暂存
        if (pollSlot(&pollBudget, slot)) { // 轮询本 chunk 邮箱槽
            src = slot[0];   // 数据源地址
            words = slot[1]; // 数据字数
        }
        if (words == 0) { // 邮箱未命中(预算耗尽)
            return failResult(result, static_cast<int32_t>(1350 + c)); // chunk c 邮箱超时
        }
        buildStarsSdmaSqe(sqe, sqeStreamId, taskBase, src, // 数据 SQE: 邮箱源 → 目的基址+chunk 偏移
                          p->dstAddr + static_cast<uint64_t>(c) * (words * 4), static_cast<uint32_t>(words * 4),
                          p->devId, p->overflowAddr, ssid);
        if (submitSqe(ctx, sqe) != 0) { // 数据 SQE 入环失败
            return failResult(result, static_cast<int32_t>(1360 + c)); // 13xx+c: 数据失败
        }
        buildStarsSdmaSqe(sqe, sqeStreamId, static_cast<uint16_t>(taskBase + 1), // 标志 SQE: 哨兵 8B → 标志槽
                          p->flagSentinelAddr, p->flagDoneAddr + static_cast<uint64_t>(c) * 8, 8, p->devId,
                          p->overflowAddr, ssid);
        if (submitSqe(ctx, sqe) != 0) { // 标志 SQE 入环失败
            return failResult(result, static_cast<int32_t>(1361 + c)); // 13xx+c: 标志失败
        }
        taskBase = static_cast<uint16_t>(taskBase + 2); // task_id 前进 2(一对)
        cacheInvalidateLine(reinterpret_cast<uintptr_t>(&ctx->ch->sqTail) & ~static_cast<uintptr_t>(63)); // 取最新 tail
        if (doorbellViaHal(p->devId, p->sqId, ctx->ch->sqTail) != 0) { // 每 chunk 一次敲铃(时延优先)
            return failResult(result, static_cast<int32_t>(1390 + c)); // 13xx+c: 敲铃失败
        }
        if (c == 0) {
            timelineMark(p, 1); // 打点[1]: 首对 SQE+doorbell
        }
    }
    timelineMark(p, 2); // 打点[2]: 全部 chunk 编排完
    return 0; // 编排完成
}

// head 前进采样(TSFW 消费证据; 诊断用, 不阻塞成功返回)。
// 关键路径纪律: 先查后睡 + 100µs 细粒度退避 — 本采样在每次 launch 尾部,
// 粗粒度睡眠会以固定时延封顶流水吞吐, 严禁回退。
void recordHeadAdvance(const HybmSdmaOrchParam *p, HybmSdmaOrchResult *result, const StarsChannelInfo *localCh)
{
    uint32_t headA = localCh->sqHead; // 采样值初值(headBefore)
    for (uint32_t i = 0; i < kHeadPollRounds; i++) { // 最多 20000 轮(≈2s 上限)
        if (cp1QueryProp(p->devId, p->sqId, DRV_SQCQ_PROP_SQ_HEAD, &headA, nullptr) == 0 &&
            headA != localCh->sqHead) { // head 前进即出(首轮即查, 典型 µs~ms 命中)
            break;
        }
        usleep(kHeadPollBackoffUs); // 退避 100µs
    }
    result->headAfter = headA; // 面包屑: 消费后 head(未前进时为 headBefore)
}
} // namespace

// kernel 入口: 编排一批设备发起的 SDMA 搬运(chunks 个邮箱描述符 → SQE 对)。
// 返回 0 成功; 1 失败(诊断见 result: mainRet 段码/五属性 nibble/面包屑)。
extern "C" __attribute__((visibility("default"))) uint32_t HybmSdmaOrchBatch(void *args)
{
    HybmSdmaOrchParam *p = static_cast<HybmSdmaOrchParam *>(args); // 入参(120B wire 结构)
    HybmSdmaOrchResult *result = reinterpret_cast<HybmSdmaOrchResult *>(p->resultAddr); // 结果(56B)
    result->magic = HYBM_SDMA_ORCH_RESULT_MAGIC; // 存活魔数(kernel 执行过)
    result->stage = STAGE_ENTRY; // 阶段=入口
    result->mainRet = 0;         // 主返回码清零
    result->subRet = 0;          // 子返回码清零
    result->sqBaseLow = 0;       // 环基址低位清零
    result->sqDepth = 0;         // 环深度清零
    result->sqTail = 0;          // headBefore 面包屑清零
    result->sqeCount = 0;        // 计数清零
    if (p->submitMode != SUBMIT_MODE_PIPELINE) { // 域外模式(仅支持流水 14)
        return failResult(result, 950); // 支持域外 submitMode
    }
    const uint32_t chunks = p->chunks ? p->chunks : 1; // chunk 数(0 视为 1)
    StarsChannelInfo localCh = {};                     // 通道上下文(活查询填充)
    if (cp1LiveQuery(p, result, &localCh) != 0) {      // CP1 活查询失败(诊断已入 result)
        return failResult(result, 1300); // 13xx: 活查询失败
    }
    SqContext ctx = {&localCh, p->devId}; // 提交上下文
    const uint16_t ssid = querySsidAndStamp(p, result); // ssid 实时查询(严禁硬编码)
    result->sqTail = localCh.sqHead; // 面包屑: headBefore
    if (orchestrateChunks(p, result, &ctx, ssid, chunks) != 0) { // 逐 chunk 编排(失败已记录)
        return 1; // 失败退出(mainRet 已置)
    }
    recordHeadAdvance(p, result, &localCh); // head 前进采样(诊断)
    result->stage = STAGE_DONE; // 阶段=完成
    result->mainRet = 0;        // 主返回码=成功
    result->sqeCount = chunks;  // 面包屑: chunk 数
    finishResult(result);       // 刷出结果
    return 0;                   // 成功退出
}
