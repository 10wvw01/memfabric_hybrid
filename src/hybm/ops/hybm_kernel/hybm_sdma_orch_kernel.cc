/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

/* hybm_sdma_orch_kernel.cc — V5 epoch 消费 kernel(310P 设备发起 SDMA)
 *
 * 执行模型(watchdog ~30s 实证约束): 每 epoch 自限 ~22s, host 监督线程到期续命;
 * 环+游标语义使续命间隙无损(邮件只延迟不丢失)。
 *
 * 职责: 按序消费本端段尾保留区请求环的邮件 {src,dst,len,imm}, 逐封构造 SQE 三连:
 *   ① 数据 SQE(src→dst)
 *   ② 到达戳 SQE(8B seq ← 邮件像首字段 → 对端到达戳槽)——FIFO 保序, 戳到即数据到
 *   ③ 完成戳 SQE(8B seq ← 同源 → 本端完成戳槽, quiet 语义)
 * 同队列 FIFO 保证 ②③ 晚于 ① 落地; 对端 AICore 经 smem_shm_sdma_wait 按序消费。
 *
 * SQE 64B 配方为 STARS V2 布局(hcomm AddOneMemcpySqeV2 逐字段比对+真机校验);
 * 配方与固件强耦合: 改动必须实机验证并 bump MF_SDMA_ORCH_KVER 重部署
 * (CP1 按文件名 dlopen 缓存, 同名不重载)。
 */

#include <cstdint>   // 定宽整型
#include <cstring>   // memset/memcpy(SQE 逐字段填充)
#include <unistd.h>  // usleep(空闲退避/SQ 满重试)

#include "hybm_sdma_contract.h" // wire 契约(host↔kernel 单一事实源, 布局锁)

// 设备侧 HAL 弱符号 — CP1 dlopen 环境按名解析, 解析不到判 nullptr 报错
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
constexpr uint32_t kSqFullRetry = 1000;         // SQ 满重试次数(×1ms)
constexpr uint32_t kSqFullBackoffUs = 1000;     // SQ 满退避粒度

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

// ssid 实时查询(SQE src/dst_substreamid 必填, 失败显式记 0xFFFF 不回退 0)
uint16_t querySsid(uint32_t devId)
{
    uint32_t ssidRaw = 0;                                   // ssid 原始值
    if (drvMemSmmuQuery == nullptr || drvMemSmmuQuery(devId, &ssidRaw) != 0) {
        ssidRaw = 0xFFFFu;                                  // 显式失败值(勿回退 0)
    }
    return static_cast<uint16_t>(ssidRaw);                  // 截断到 u16 返回
}

// doorbell: 经 HAL 写 SQ tail(TSFW 即刻可见新 SQE)
int32_t doorbellViaHal(uint32_t devId, uint32_t sqId, uint32_t newTail)
{
    if (halSqCqConfig == nullptr) { // 弱符号未解析(CP1 外环境)
        return 260;                 // 错误码: 符号缺失
    }
    HalConfigInfo cfg = {};         // 配置请求(零初始化)
    cfg.type = DRV_NORMAL_TYPE_HAL; // 正常 HAL 类型
    cfg.tsId = 0;                   // TS 0
    cfg.sqId = sqId;                // 目标 SQ
    cfg.cqId = 0;                   // CQ 不涉及
    cfg.prop = DRV_SQCQ_PROP_SQ_TAIL; // 属性: 写 tail
    cfg.value[0] = newTail;         // 新 tail 值
    return halSqCqConfig(devId, &cfg); // 原始返回码透传
}

// SQE 构造(64B, STARS V2 SDMA 布局; 数据与戳 SQE 同构, 仅地址/长度不同)。
// 逐字段依据 hcomm AddOneMemcpySqeV2 源码与真机成功样本比对; 构造前必须整体清零。
void buildStarsSdmaSqe(uint8_t sqe[kSqeSize], uint16_t streamId, uint16_t taskId, uint64_t src, uint64_t dst,
                       uint32_t len, uint32_t devId, uint64_t overflowAddr, uint16_t ssid)
{
    memset(sqe, 0, kSqeSize); // 全零打底(残留数据勿入环)
    sqe[0] = 0x09; // type: MEMCPY_ASYNC_SQE_V2(固件认的魔数)
    memcpy(&sqe[4], &streamId, 2); // off4: rt_stream_id
    memcpy(&sqe[6], &taskId, 2);   // off6: task_id
    sqe[8] = 0x00;                 // off8-11: 全 0(固件严格校验该域)
    sqe[14] = 0xff;                // off12-15: kernel_credit=0xff, 其余保留
    sqe[16] = 0x00;                // off16-19: 位域模板(opcode=NOT_ATOMIC + ie_dma 等)
    sqe[17] = 0xde;
    sqe[18] = 0x0e;
    sqe[19] = 0x00;
    uint16_t smmuStreamId = (devId == 0) ? 0x7F45 : 0xBF45; // 按 die 区分(hcomm 分支)
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

// 提交上下文: 通道元数据 + 设备号
struct SqContext {
    volatile ock::mf::HybmSdmaOrchResult *result; // 结果区(诊断)
    uint64_t sqBase;                              // SQ 环基址
    uint32_t sqDepth;                             // SQ 环深度
    uint32_t sqHead;                              // SQ head(活查询初值)
    uint32_t sqTail;                              // SQ shadow tail
    uint32_t streamId;                            // SQE 的 rt_stream_id
    uint32_t devId;                               // doorbell HAL 路径用
    uint32_t sqId;                                // 目标 SQ
};

// SQE 入环: 只写环 + 推进 shadow tail, 不敲铃(调用方统一 doorbell); SQ 满返回 240
int32_t submitSqe(SqContext *c, const uint8_t sqe[kSqeSize])
{
    uint32_t used = (c->sqTail >= c->sqHead) ? (c->sqTail - c->sqHead)
                                             : (c->sqDepth - c->sqHead + c->sqTail); // 已占用槽数
    if (c->sqDepth - used < 1) {
        return 240; // 环满(调用方退避重试)
    }
    volatile uint8_t *slot = reinterpret_cast<volatile uint8_t *>(c->sqBase) + // 环槽基址
                             static_cast<uint64_t>(c->sqTail) * kSqeSize;
    const volatile uint64_t *src = reinterpret_cast<const volatile uint64_t *>(sqe); // 源(本地 SQE)
    volatile uint64_t *dst = reinterpret_cast<volatile uint64_t *>(slot);            // 目的(环槽)
    for (uint32_t i = 0; i < kSqeSize / 8; i++) { // 8×u64 拷贝 64B
        dst[i] = src[i];                          // 逐字写入
    }
    uintptr_t flushEnd = reinterpret_cast<uintptr_t>(slot) + kSqeSize;             // 刷写区间末尾
    uintptr_t flushAddr = reinterpret_cast<uintptr_t>(slot) & ~static_cast<uintptr_t>(63); // 行对齐起点
    for (; flushAddr < flushEnd; flushAddr += 64) { // 逐 64B 行
        cacheFlushLine(flushAddr);                  // cvac 刷出(SDMA 经一致性域读取)
    }
    memBarrier();                                   // 写序屏障
    c->sqTail = (c->sqTail + 1) % c->sqDepth;       // 推进 shadow tail(环形 +1)
    return 0;                                       // 入环成功
}

// HAL 单属性查询(弱符号缺失返回 9xx 段错误码; v0/v1 可空出参)
int32_t cp1QueryProp(uint32_t devId, uint32_t sqId, uint32_t prop, uint32_t *v0, uint32_t *v1)
{
    if (halSqCqQuery == nullptr) { // 弱符号缺失
        return 900 + prop;         // 错误码: 9xx 段(带属性号)
    }
    HalQueryInfo q = {};          // 查询请求(零初始化)
    q.type = DRV_NORMAL_TYPE_HAL; // 正常 HAL 类型
    q.tsId = 0;                   // TS 0
    q.sqId = sqId;                // 目标 SQ
    q.cqId = 0;                   // CQ 不涉及
    q.prop = prop;                // 查询属性编号
    int32_t r = halSqCqQuery(devId, &q); // 发起查询
    if (r == 0) {                         // 查询成功
        if (v0 != nullptr) {              // 出参 0
            *v0 = q.value[0];             // 回传
        }
        if (v1 != nullptr) {              // 出参 1
            *v1 = q.value[1];             // 回传
        }
    }
    return r; // 原始 drvError
}

// 活查询 + 提交上下文装配(诊断码写入 result)
int32_t cp1LiveQuery(const ock::mf::HybmSdmaOrchParam *p, SqContext *ctx)
{
    uint32_t lo = 0, hi = 0, status = 0, head = 0, tail = 0; // 查询暂存
    int32_t rStatus = cp1QueryProp(p->devId, p->sqId, DRV_SQCQ_PROP_SQ_STATUS, &status, nullptr); // 状态
    int32_t rBase = cp1QueryProp(p->devId, p->sqId, DRV_SQCQ_PROP_SQ_BASE, &lo, &hi);             // 基址(双出参)
    int32_t rDepth = cp1QueryProp(p->devId, p->sqId, DRV_SQCQ_PROP_SQ_DEPTH, &ctx->sqDepth, nullptr); // 深度
    int32_t rHead = cp1QueryProp(p->devId, p->sqId, DRV_SQCQ_PROP_SQ_HEAD, &head, nullptr);       // head
    (void)cp1QueryProp(p->devId, p->sqId, DRV_SQCQ_PROP_SQ_TAIL, &tail, nullptr);                 // tail(诊断)
    (void)rStatus;   // 状态仅诊断
    if (rBase != 0) { // BASE 查询失败
        return 1000 + (rBase & 0xFF); // 10xx: BASE 失败
    }
    if (rHead != 0) { // HEAD 查询失败
        return 1300 + (rHead & 0xFF); // 13xx: HEAD 失败
    }
    if (rDepth != 0) {        // DEPTH 查询失败
        return 1100 + (rDepth & 0xFF); // 11xx: DEPTH 失败
    }
    uint64_t base = (static_cast<uint64_t>(hi) << 32) | lo; // 拼接 64 位环基址
    if (base == 0 || ctx->sqDepth == 0) { // 基址或深度无效
        return 1200;                      // 12xx: 无效通道
    }
    ctx->sqBase = base;   // 填通道: 环基址
    ctx->sqHead = head;   // 填通道: head
    ctx->sqTail = tail;   // 填通道: tail
    return 0;             // 活查询成功
}

// 结果写回并刷出(host 经 D2H 可读)
void finishResult(volatile ock::mf::HybmSdmaOrchResult *result)
{
    cacheFlushLine(reinterpret_cast<uintptr_t>(result) & ~static_cast<uintptr_t>(63)); // 刷出结果行
    memBarrier();                                                                     // 屏障
}

// 读请求游标/停止标志(AICore/SQE 写 → AICPU 读, 失效取新值)
inline uint64_t loadCell(uint64_t addr)
{
    cacheInvalidateLine(addr & ~static_cast<uintptr_t>(63)); // 失效该行
    return *reinterpret_cast<volatile uint64_t *>(addr);     // volatile 读
}

// 写游标并刷出(AICPU 写 → AICore 读)
inline void storeCell(uint64_t addr, uint64_t v)
{
    *reinterpret_cast<volatile uint64_t *>(addr) = v;        // volatile 写
    cacheFlushLine(addr & ~static_cast<uintptr_t>(63));      // 刷出该行
}

// SQE 三连入环 + doorbell: ① 数据(src→dst) ② 到达戳(邮件像 seq 字段→对端戳槽)
// ③ 完成戳(同源→本端戳槽); 同队列 FIFO 保证 ②③ 晚于 ① 落地。
// 返回 0 成功; 非 0 失败码。
int32_t issueMailSqes(const ock::mf::HybmSdmaOrchParam *p, SqContext *ctx, uint16_t ssid, uint64_t seq,
                      const ock::mf::HybmSdmaReqMail &mail, uint64_t arrMailSlot, uint64_t peerArrStampSlot)
{
    const uint16_t streamId = static_cast<uint16_t>(ctx->streamId); // SQE 的 rt_stream_id
    uint8_t sqe[kSqeSize];                                          // SQE 构造缓冲
    buildStarsSdmaSqe(sqe, streamId, static_cast<uint16_t>(seq), mail.src,            // ① 数据 SQE
                      mail.dst, static_cast<uint32_t>(mail.len), p->devId, p->overflowAddr, ssid);
    for (uint32_t retry = 0; submitSqe(ctx, sqe) == 240; retry++) {                   // SQ 满退避重试
        if (retry >= kSqFullRetry) {
            return 1360;                              // 错误码: SQ 持续满
        }
        usleep(kSqFullBackoffUs);                     // 1ms 退避
    }
    buildStarsSdmaSqe(sqe, streamId, static_cast<uint16_t>(seq + 1), arrMailSlot,     // ② 到达戳(8B 戳值)
                      peerArrStampSlot, 8, p->devId, p->overflowAddr, ssid);
    if (submitSqe(ctx, sqe) != 0) {                   // 入环失败
        return 1361;                                  // 错误码: 到达戳失败
    }
    buildStarsSdmaSqe(sqe, streamId, static_cast<uint16_t>(seq + 2), arrMailSlot,     // ③ 完成戳(8B 戳值)
                      p->quietBase + (seq % ock::mf::HYBM_SDMA_ORCH_RS_QUIET_SLOTS) * 8, 8, p->devId,
                      p->overflowAddr, ssid);
    if (submitSqe(ctx, sqe) != 0) {                   // 入环失败
        return 1362;                                  // 错误码: 完成戳失败
    }
    memBarrier();                                     // SQE 写序后
    if (doorbellViaHal(p->devId, ctx->sqId, ctx->sqTail) != 0) { // 每 mail 一次敲铃(时延优先)
        return 1390;                                  // 错误码: 敲铃失败
    }
    return 0;                                         // 三连成功
}

// 消费一封请求邮件: 写到达邮件像 → SQE 三连 → reqHead 推进。
// 返回 0 成功; 非 0 失败码(调用方记 result 并退出 epoch)。
int32_t consumeOneMail(const ock::mf::HybmSdmaOrchParam *p, SqContext *ctx, uint16_t ssid, uint64_t seq)
{
    using namespace ock::mf; // 契约类型
    const uint64_t reqSlot = p->reqBase + (seq % HYBM_SDMA_ORCH_RS_REQ_SLOTS) * sizeof(HybmSdmaReqMail);
    cacheInvalidateLine(reqSlot & ~static_cast<uintptr_t>(63));       // 失效取邮件
    const volatile HybmSdmaReqMail *rm = reinterpret_cast<const volatile HybmSdmaReqMail *>(reqSlot);
    HybmSdmaReqMail mail = {rm->src, rm->dst, rm->len, rm->imm};      // 快照(后续槽可复用)
    const uint64_t peerSeg = mail.dst & ~(p->symSize - 1);            // 对端段基址(掩码算术)
    const uint64_t peerRes = peerSeg + (p->physSize - HYBM_SDMA_ORCH_RESERVED); // 对端保留区基址
    const uint64_t arrMailSlot = peerRes + HYBM_SDMA_ORCH_RS_ARR_MAIL +
                                 (seq % HYBM_SDMA_ORCH_RS_ARR_SLOTS) * sizeof(HybmSdmaArrMail);
    volatile HybmSdmaArrMail *am = reinterpret_cast<volatile HybmSdmaArrMail *>(arrMailSlot);
    am->seq = seq + 1;  // 戳值(1 基, 供两支戳 SQE 复用为拷贝源; 0 保留为"空", 防幻影)
    am->dst = mail.dst; // 数据落点(接收方免预约定布局)
    am->len = mail.len; // 数据长度
    am->imm = mail.imm; // 用户标签透传
    cacheFlushLine(arrMailSlot & ~static_cast<uintptr_t>(63));        // 刷出邮件像(SQE 可读)
    const uint64_t peerArrStamp = peerRes + HYBM_SDMA_ORCH_RS_ARR_STAMP +
                                  (seq % HYBM_SDMA_ORCH_RS_ARR_SLOTS) * 8; // 对端到达戳槽
    auto ret = issueMailSqes(p, ctx, ssid, seq, mail, arrMailSlot, peerArrStamp); // SQE 三连
    if (ret != 0) {                                                   // 三连失败
        return ret;                                                   // 透传失败码
    }
    storeCell(p->reqBase - HYBM_SDMA_ORCH_RS_REQ_RING + HYBM_SDMA_ORCH_RS_REQ_HEAD, seq + 1); // reqHead 推进
    return 0;                                                         // 消费成功
}
} // namespace

// kernel 入口: epoch 消费(自限 ~22s, host 监督续命)。
// 返回 0 正常退出(时限/停止); 1 错误退出(诊断见 result)。
extern "C" __attribute__((visibility("default"))) uint32_t HybmSdmaOrchEpoch(void *args)
{
    using namespace ock::mf;                                          // 契约类型
    HybmSdmaOrchParam *p = static_cast<HybmSdmaOrchParam *>(args);    // 入参(72B wire 结构)
    volatile HybmSdmaOrchResult *result =                             // 结果(24B, 工作区)
        reinterpret_cast<volatile HybmSdmaOrchResult *>(p->resultAddr);
    result->magic = HYBM_SDMA_ORCH_RESULT_MAGIC;                      // 存活魔数
    result->mainRet = 0;                                              // 主返回码清零
    result->consumed = 0;                                             // 消费计数清零
    result->exitReason = HYBM_SDMA_ORCH_EXIT_DEADLINE;                // 默认时限退出
    SqContext ctx = {};                                               // 提交上下文
    ctx.result = result;                                              // 诊断挂接
    ctx.streamId = static_cast<uint32_t>(p->streamId);                // 流 id(SQE rt_stream_id)
    ctx.sqId = p->sqId;                                               // SQ id
    ctx.devId = p->devId;                                             // 设备号
    auto ret = cp1LiveQuery(p, &ctx);                                 // CP1 活查询(失败码入 ctx 路径)
    if (ret != 0) {                                                   // 活查询失败
        result->mainRet = 1300 + (ret & 0xFF);                        // 13xx: 活查询失败
        result->exitReason = HYBM_SDMA_ORCH_EXIT_ERROR;               // 错误退出
        finishResult(result);                                         // 刷出
        return 1;                                                     // 失败退出
    }
    const uint16_t ssid = querySsid(p->devId);                        // ssid 实时查询(严禁硬编码)
    uint64_t head = loadCell(p->reqBase - HYBM_SDMA_ORCH_RS_REQ_RING + HYBM_SDMA_ORCH_RS_REQ_HEAD); // 消费起点
    for (uint32_t loops = 0; loops < HYBM_SDMA_ORCH_EPOCH_MAX_LOOPS; loops++) { // epoch 主循环
        if (loadCell(p->stopAddr) != 0) {                             // 停止标志(host 析构)
            result->exitReason = HYBM_SDMA_ORCH_EXIT_STOP;            // 停止退出
            break;                                                    // 正常退出
        }
        const uint64_t tail = loadCell(p->reqBase - HYBM_SDMA_ORCH_RS_REQ_RING + HYBM_SDMA_ORCH_RS_REQ_TAIL);
        if (head == tail) {                                           // 环空
            usleep(HYBM_SDMA_ORCH_EPOCH_IDLE_US);                     // 100µs 空闲退避
            continue;                                                 // 继续监听
        }
        ret = consumeOneMail(p, &ctx, ssid, head);                    // 消费一封(head 即序号)
        if (ret != 0) {                                               // 消费失败
            result->mainRet = static_cast<uint32_t>(ret);             // 失败码(13xx 段)
            result->exitReason = HYBM_SDMA_ORCH_EXIT_ERROR;           // 错误退出
            break;                                                    // 退出 epoch
        }
        head++;                                                       // 前进
        result->consumed++;                                           // 计数
    }
    uint32_t headA = ctx.sqHead; // head 采样初值(查询失败时兜底)
    (void)cp1QueryProp(p->devId, p->sqId, DRV_SQCQ_PROP_SQ_HEAD, &headA, nullptr); // 面包屑: head 采样
    result->headAfter = headA;   // 回写结果
    finishResult(result);        // 刷出结果
    return (result->exitReason == HYBM_SDMA_ORCH_EXIT_ERROR) ? 1 : 0; // 错误退出返回 1
}
