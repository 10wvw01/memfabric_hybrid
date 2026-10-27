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

/*
 * This file derives from the xcclpp project (aicpu-aicore-310p3), which is
 * licensed under the MIT License:
 *
 * Copyright (c) xcclpp authors. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/* hybm_sdma_orch_kernel.cc — epoch 消费 kernel(310P 设备发起 SDMA)
 *
 * 执行模型(v11 起 launch json 为 KFCKernel/type 5, 按时长分轮 + 主机报时):
 * 设备侧系统监控对 type 5 任务不计时(28s 强制结束约束解除); 但该执行环境下
 * cntvct_el0/cntfrq_el0 均不可用(v7 时代 type 2 正常, type 5 两 die 全失效,
 * 2026-10-12 实测), 每轮时长改由主机报时驱动: 监督线程每 100ms 往工作区
 * HEARTBEAT_OFF 写递增计数, kernel 主循环读之, 到 budgetS×10 跳即自退;
 * 循环上限保留兜底(报时停喂时防永跑)。host 发现退出后重新拉起新一轮(环+游标零丢失)。
 * 为什么不一次下发跑到停止: host 侧全设备同步(AclrtSynchronizeDevice, torch_npu 分配
 * 显存时内部触发)等所有流的全部任务且无豁免——永不退出的 kernel 必阻塞它(serve 实测死因 507017)。
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

#include <cstdint>
#include <cstring>
#include <unistd.h>

#include "hybm_sdma_contract.h"

// 设备侧 HAL 弱符号 — CP1 dlopen 环境按名解析, 解析不到判 nullptr 报错
extern "C" {
__attribute__((weak)) int32_t halSqCqConfig(uint32_t devId, void *info);
__attribute__((weak)) int32_t halSqCqQuery(uint32_t devId, void *info);
__attribute__((weak)) int32_t drvMemSmmuQuery(uint32_t devId, uint32_t *ssid);
} // extern "C"

namespace {
// 常量(HAL 镜像词保持驱动侧命名)
constexpr uint32_t DRV_NORMAL_TYPE_HAL = 0;     // HAL 正常类型(查询/配置均填 0)
constexpr uint32_t DRV_SQCQ_PROP_SQ_STATUS = 0; // 属性编号: SQ 状态
constexpr uint32_t DRV_SQCQ_PROP_SQ_HEAD = 1;   // 属性编号: SQ head(TSFW 消费位置)
constexpr uint32_t DRV_SQCQ_PROP_SQ_TAIL = 2;   // 属性编号: SQ tail(提交位置)
constexpr uint32_t DRV_SQCQ_PROP_SQ_BASE = 6;   // 属性编号: SQ 环基址
constexpr uint32_t DRV_SQCQ_PROP_SQ_DEPTH = 7;  // 属性编号: SQ 环深度
constexpr uint32_t kInfoValueLen = 8;           // HAL 请求 value 数组长度(配置/查询同)
constexpr size_t kSqeSize = 64;                 // SQE 尺寸(64B = 8×u64)
constexpr uint32_t kSqFullRetry = 1000;              // SQ 满活查询自旋上限(×1ms)
constexpr uint32_t kSqFullBackoffUs = 1000;     // SQ 满退避粒度

// HAL 请求结构(HAL 侧按定长 value 数组解释; 前五域为定位五元组)
struct HalConfigInfo { // SQ/CQ 配置请求(doorbell 用)
    uint32_t type, tsId, sqId, cqId, prop;
    uint32_t value[kInfoValueLen]; // 属性参数(如 newTail)
};
struct HalQueryInfo { // SQ/CQ 查询请求
    uint32_t type, tsId, sqId, cqId, prop;
    uint32_t value[kInfoValueLen]; // 查询结果(如 head/base)
};

// cache 维护原语(AICPU 与 SDMA 同 domain)
inline void cacheFlushLine(uintptr_t addr) // cvac: 脏数据写回一致性点
{
    __asm__ __volatile__("dc cvac, %0" ::"r"(addr) : "memory");
}
inline void cacheInvalidateLine(uintptr_t addr) // civac: 读侧失效取最新值
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
    uint32_t ssidRaw = 0;
    if (drvMemSmmuQuery == nullptr || drvMemSmmuQuery(devId, &ssidRaw) != 0) {
        ssidRaw = 0xFFFFu; // 显式失败值(勿回退 0)
    }
    return static_cast<uint16_t>(ssidRaw);
}

// doorbell: 经 HAL 写 SQ tail(TSFW 即刻可见新 SQE)
int32_t doorbellViaHal(uint32_t devId, uint32_t sqId, uint32_t newTail)
{
    if (halSqCqConfig == nullptr) { // 弱符号未解析(CP1 外环境)
        return 260;                 // 错误码: 符号缺失
    }
    HalConfigInfo cfg = {};
    cfg.type = DRV_NORMAL_TYPE_HAL;
    cfg.tsId = 0;
    cfg.sqId = sqId;
    cfg.cqId = 0;
    cfg.prop = DRV_SQCQ_PROP_SQ_TAIL;
    cfg.value[0] = newTail;
    return halSqCqConfig(devId, &cfg);
}

// SQE 构造(64B, STARS V2 SDMA 布局; 数据与戳 SQE 同构, 仅地址/长度不同)。
// 逐字段依据 hcomm AddOneMemcpySqeV2 源码与真机成功样本比对; 构造前必须整体清零。
// sqeOpcode: off16 操作码 — 0x00=CPY 纯搬运(默认); 0x71/0x61=FP32/FP16 ADD(dst+=src,
//            SDMA inline reduce; 310P3 实测支持域 SUM×{FP32,FP16}, 见 contract 注)。
void buildStarsSdmaSqe(uint8_t sqe[kSqeSize], uint16_t streamId, uint16_t taskId, uint64_t src, uint64_t dst,
                       uint32_t len, uint32_t devId, uint64_t overflowAddr, uint16_t ssid,
                       uint8_t sqeOpcode = 0x00)
{
    memset(sqe, 0, kSqeSize);          // 全零打底(残留数据勿入环)
    sqe[0] = 0x09;                     // type: MEMCPY_ASYNC_SQE_V2(固件认的魔数)
    memcpy(&sqe[4], &streamId, 2);     // off4:  rt_stream_id
    memcpy(&sqe[6], &taskId, 2);       // off6:  task_id
    sqe[8] = 0x00;                     // off8-11: 全 0(固件严格校验该域)
    sqe[14] = 0xff;                    // off12-15: kernel_credit=0xff, 其余保留
    sqe[16] = sqeOpcode;               // off16: opcode(0x00=NOT_ATOMIC 搬运; reduce 由参数注入)
    sqe[17] = 0xde;
    sqe[18] = 0x0e;
    sqe[19] = 0x00;
    uint16_t smmuStreamId = (devId == 0) ? 0x7F45 : 0xBF45; // 按 die 区分(hcomm 分支)
    memcpy(&sqe[20], &smmuStreamId, 2); // off20: src_streamid
    memcpy(&sqe[22], &ssid, 2);         // off22: src_substreamid(实时查询)
    memcpy(&sqe[24], &smmuStreamId, 2); // off24: dst_streamid
    memcpy(&sqe[26], &ssid, 2);         // off26: dst_substreamid(与 src 对称)
    memcpy(&sqe[28], &len, 4);          // off28: length(字节)
    uint32_t lo = static_cast<uint32_t>(src & 0xFFFFFFFFu);
    uint32_t hi = static_cast<uint32_t>(src >> 32);
    memcpy(&sqe[32], &lo, 4);           // off32: src.lo
    memcpy(&sqe[36], &hi, 4);           // off36: src.hi
    lo = static_cast<uint32_t>(dst & 0xFFFFFFFFu);
    hi = static_cast<uint32_t>(dst >> 32);
    memcpy(&sqe[40], &lo, 4);           // off40: dst.lo
    memcpy(&sqe[44], &hi, 4);           // off44: dst.hi
    sqe[50] = 0x20;                     // off48-51: 模板位(0x20 在 off50)
    lo = static_cast<uint32_t>(overflowAddr & 0xFFFFFFFFu);
    hi = static_cast<uint32_t>(overflowAddr >> 32);
    memcpy(&sqe[52], &lo, 4);           // off52: overflow.lo
    memcpy(&sqe[56], &hi, 4);           // off56: overflow.hi
}

// 提交上下文: 通道元数据 + 设备号
struct SqContext {
    volatile ock::mf::HybmSdmaOrchResult *result; // 结果区(诊断)
    uint64_t sqBase;  // SQ 环基址
    uint32_t sqDepth; // SQ 环深度
    uint32_t sqHead;  // SQ head(满环时活查询刷新)
    uint32_t sqTail;  // SQ shadow tail
    uint32_t streamId; // SQE 的 rt_stream_id
    uint32_t devId;   // doorbell HAL 路径用
    uint32_t sqId;    // 目标 SQ
};

int32_t cp1QueryProp(uint32_t devId, uint32_t sqId, uint32_t prop, uint32_t *v0, uint32_t *v1);

// SQE 入环: 只写环 + 推进 shadow tail, 不敲铃(调用方统一 doorbell)。
// 满环检查活查询 head 重算(hcomm LaunchTask 同款): shadow 回绕后对 epoch 入口 head 快照
// 的模减会失明(used 恒小), 覆写固件未消费 SQE 造成邮件无声丢失(并发丢件挂死（507014） 根因);
// 活查询保证 used 始终以固件真实消费位置为基准。返回 0 成功; 1363=活查询超时环满。
int32_t submitSqe(const ock::mf::HybmSdmaOrchParam *p, SqContext *c, const uint8_t sqe[kSqeSize])
{
    uint32_t used = (c->sqTail >= c->sqHead) ? (c->sqTail - c->sqHead)
                                             : (c->sqDepth - c->sqHead + c->sqTail);
    if (c->sqDepth - used < 1) {
        uint32_t retry = 0;
        do { // 满环: 活查询固件 head 重算(非 epoch 快照), 1ms 退避, 上限 1s
            usleep(kSqFullBackoffUs);
            uint32_t liveHead = 0;
            if (cp1QueryProp(p->devId, c->sqId, DRV_SQCQ_PROP_SQ_HEAD, &liveHead, nullptr) != 0) {
                return 1363; // 错误码: head 活查询失败
            }
            c->sqHead = liveHead;
            used = (c->sqTail >= c->sqHead) ? (c->sqTail - c->sqHead)
                                            : (c->sqDepth - c->sqHead + c->sqTail);
        } while (c->sqDepth - used < 1 && ++retry < kSqFullRetry);
        if (c->sqDepth - used < 1) {
            return 1363; // 错误码: 活查询后仍满(消费侧停摆)
        }
    }
    volatile uint8_t *slot = reinterpret_cast<volatile uint8_t *>(c->sqBase) +
                             static_cast<uint64_t>(c->sqTail) * kSqeSize;
    const volatile uint64_t *src = reinterpret_cast<const volatile uint64_t *>(sqe);
    volatile uint64_t *dst = reinterpret_cast<volatile uint64_t *>(slot);
    for (uint32_t i = 0; i < kSqeSize / 8; i++) {
        dst[i] = src[i];
    }
    uintptr_t flushEnd = reinterpret_cast<uintptr_t>(slot) + kSqeSize;
    uintptr_t flushAddr = reinterpret_cast<uintptr_t>(slot) & ~static_cast<uintptr_t>(63);
    for (; flushAddr < flushEnd; flushAddr += 64) {
        cacheFlushLine(flushAddr); // cvac 刷出(SDMA 经一致性域读取)
    }
    /* 屏障纪律: 此处不 dsb(cvac 已写回一致性点, 逐封 dsb 4 次/封过重);
     * 由批末 doorbell 前的统一 memBarrier 保证全部 SQE 对 TSFW 可见 */
    c->sqTail = (c->sqTail + 1) % c->sqDepth;
    return 0;
}

// HAL 单属性查询(弱符号缺失返回 9xx 段错误码; v0/v1 可空出参)
int32_t cp1QueryProp(uint32_t devId, uint32_t sqId, uint32_t prop, uint32_t *v0, uint32_t *v1)
{
    if (halSqCqQuery == nullptr) { // 弱符号缺失
        return 900 + prop;         // 错误码: 9xx 段(带属性号)
    }
    HalQueryInfo q = {};
    q.type = DRV_NORMAL_TYPE_HAL;
    q.tsId = 0;
    q.sqId = sqId;
    q.cqId = 0;
    q.prop = prop;
    int32_t r = halSqCqQuery(devId, &q);
    if (r == 0) {
        if (v0 != nullptr) {
            *v0 = q.value[0];
        }
        if (v1 != nullptr) {
            *v1 = q.value[1];
        }
    }
    return r;
}

// 活查询 + 提交上下文装配(失败码写入返回值, 语义见各 1xxx 分支)
int32_t cp1LiveQuery(const ock::mf::HybmSdmaOrchParam *p, SqContext *ctx)
{
    uint32_t lo = 0, hi = 0, status = 0, head = 0, tail = 0;
    int32_t rStatus = cp1QueryProp(p->devId, p->sqId, DRV_SQCQ_PROP_SQ_STATUS, &status, nullptr);
    int32_t rBase = cp1QueryProp(p->devId, p->sqId, DRV_SQCQ_PROP_SQ_BASE, &lo, &hi);
    int32_t rDepth = cp1QueryProp(p->devId, p->sqId, DRV_SQCQ_PROP_SQ_DEPTH, &ctx->sqDepth, nullptr);
    int32_t rHead = cp1QueryProp(p->devId, p->sqId, DRV_SQCQ_PROP_SQ_HEAD, &head, nullptr);
    int32_t rTail = cp1QueryProp(p->devId, p->sqId, DRV_SQCQ_PROP_SQ_TAIL, &tail, nullptr);
    (void)rStatus; // 状态仅诊断
    if (rTail != 0) { // TAIL 失败: sqTail 是 SQE 写入位置, 失败即退出防覆写固件未消费 SQE
        return 1400 + (rTail & 0xFF);
    }
    if (rBase != 0) {
        return 1000 + (rBase & 0xFF); // 10xx: BASE 失败
    }
    if (rHead != 0) {
        return 1300 + (rHead & 0xFF); // 13xx: HEAD 失败
    }
    if (rDepth != 0) {
        return 1100 + (rDepth & 0xFF); // 11xx: DEPTH 失败
    }
    const uint64_t base = (static_cast<uint64_t>(hi) << 32) | lo;
    if (base == 0 || ctx->sqDepth == 0) {
        return 1200; // 12xx: 无效通道
    }
    ctx->sqBase = base;
    ctx->sqHead = head;
    ctx->sqTail = tail;
    return 0;
}

// 主机报时槽: 工作区 HEARTBEAT_OFF(resultAddr 即工作区基址, RESULT_OFF=0)。
// type 5 下 cntvct_el0/cntfrq_el0 均不可用(见文件头), 时限由 host 每 100ms 一跳驱动。
inline uint64_t LoadHeartbeat(const volatile ock::mf::HybmSdmaOrchResult *result)
{
    const uint64_t hbAddr =
        reinterpret_cast<uint64_t>(result) + ock::mf::HYBM_SDMA_ORCH_WS_HEARTBEAT_OFF;
    __asm__ __volatile__("dc civac, %0" ::"r"(hbAddr & ~static_cast<uint64_t>(63)) : "memory");
    return *reinterpret_cast<volatile uint64_t *>(hbAddr);
}

// 结果写回并刷出(host 经 D2H 可读)
void finishResult(volatile ock::mf::HybmSdmaOrchResult *result)
{
    cacheFlushLine(reinterpret_cast<uintptr_t>(result) & ~static_cast<uintptr_t>(63));
    memBarrier();
}


// 读请求游标/停止标志(AICore/SQE 写 → AICPU 读, 失效取新值)
inline uint64_t loadCell(uint64_t addr)
{
    cacheInvalidateLine(addr & ~static_cast<uintptr_t>(63));
    return *reinterpret_cast<volatile uint64_t *>(addr);
}

// 写游标并刷出(AICPU 写 → AICore 读)
inline void storeCell(uint64_t addr, uint64_t v)
{
    *reinterpret_cast<volatile uint64_t *>(addr) = v;
    cacheFlushLine(addr & ~static_cast<uintptr_t>(63));
}

// v15 诊断逐单日志: 逐单落工作区环形缓冲(读回原文/计算地址/SQ 游标), host D2H 可读。
// 单线程 kernel 无并发; 计数持久于工作区(跨 epoch 累计, 环形覆盖最旧)。
void dbgLogMail(const ock::mf::HybmSdmaOrchParam *p, uint64_t seq, const ock::mf::HybmSdmaReqMail &mail,
                uint64_t arrMailSlot, uint64_t sqTailBefore, uint64_t sqTailAfter)
{
    using namespace ock::mf;
    const uintptr_t ws = static_cast<uintptr_t>(p->resultAddr);
    const uint64_t cnt = loadCell(ws + ock::mf::HYBM_SDMA_ORCH_WS_DBG_RING_CNT_OFF);
    volatile ock::mf::HybmSdmaOrchDbgMail *e = reinterpret_cast<volatile ock::mf::HybmSdmaOrchDbgMail *>(
        ws + ock::mf::HYBM_SDMA_ORCH_WS_DBG_RING_OFF +
        (cnt % ock::mf::HYBM_SDMA_ORCH_DBG_RING_SLOTS) * sizeof(ock::mf::HybmSdmaOrchDbgMail));
    e->seq = seq;
    e->reqF0 = mail.src;
    e->reqF1 = mail.dst;
    e->reqF2 = mail.len;
    e->reqF3 = mail.imm;
    e->arrMailSlot = arrMailSlot;
    e->sqTailBefore = sqTailBefore;
    e->sqTailAfter = sqTailAfter;
    cacheFlushLine(reinterpret_cast<uintptr_t>(e)); /* 64B 恰一行 */
    storeCell(ws + ock::mf::HYBM_SDMA_ORCH_WS_DBG_RING_CNT_OFF, cnt + 1);
}

// SQE 三连入环(不敲铃): ① 数据(src→dst, 可带 reduce opcode) ② 到达戳(本端暂存单号→对端戳槽)
// ③ 完成戳(同源→本端戳槽); 同队列 FIFO 保证 ②③ 晚于 ① 落地。返回 0 成功。
// v25: ②③ 拷贝数据源=本端暂存(原为对端邮件内容首字段——跨芯片写在途时 SDMA 读回旧值, 并发丢件挂死（507014）)。
int32_t issueMailSqes(const ock::mf::HybmSdmaOrchParam *p, SqContext *ctx, uint16_t ssid, uint64_t seq,
                      const ock::mf::HybmSdmaReqMail &mail, uint64_t stageSlot, uint64_t peerArrStampSlot,
                      uint8_t sqeOpcode)
{
    const uint16_t streamId = static_cast<uint16_t>(ctx->streamId);
    uint8_t sqe[kSqeSize];
    /* 尾值防 0(v21, 并发丢件挂死（507014） 修复): 本封三连若恰好使 shadow 尾落到 0, 该批 doorbell
     * 值为 0——固件将其当"空环"哨兵静默丢弃, SQE 完好在环但永不执行(实测确定性丢件)。
     * 先插入一条无害占位 SQE(工作区 8B 自拷贝), 尾值落到 1; 小正值回绕形态实测安全。 */
    if ((ctx->sqTail + 3) % ctx->sqDepth == 0) {
        const uint64_t scratch = p->resultAddr + ock::mf::HYBM_SDMA_ORCH_WS_PAD_SCRATCH_OFF;
        buildStarsSdmaSqe(sqe, streamId, static_cast<uint16_t>(seq + 3),
                          scratch, scratch, 8,
                          p->devId, p->overflowAddr, ssid);
        int32_t padRet = submitSqe(p, ctx, sqe);
        if (padRet != 0) {
            return 1365; // 错误码: 占位 SQE 入环失败
        }
    }
    buildStarsSdmaSqe(sqe, streamId, static_cast<uint16_t>(seq), mail.src, mail.dst,
                      static_cast<uint32_t>(mail.len), p->devId, p->overflowAddr, ssid, sqeOpcode);
    int32_t r = submitSqe(p, ctx, sqe); // 满环活查询在 submitSqe 内自旋(上限 1s)
    if (r != 0) {
        return 1360; // 错误码: SQ 持续满/查询失败
    }
    buildStarsSdmaSqe(sqe, streamId, static_cast<uint16_t>(seq + 1), stageSlot,
                      peerArrStampSlot, 8, p->devId, p->overflowAddr, ssid); // ② 到达戳(源=本端暂存)
    if (submitSqe(p, ctx, sqe) != 0) {
        return 1361; // 错误码: 到达戳失败
    }
    buildStarsSdmaSqe(sqe, streamId, static_cast<uint16_t>(seq + 2), stageSlot,
                      p->quietBase + (seq % SMEM_SHM_SDMA_RS_QUIET_SLOTS) * 8, 8, p->devId,
                      p->overflowAddr, ssid); // ③ 完成戳(源=本端暂存)
    if (submitSqe(p, ctx, sqe) != 0) {
        return 1362; // 错误码: 完成戳失败
    }
    return 0;
}

/* 批末敲铃: 统一屏障 + doorbell(环内积压时 N 封一铃, 降 HAL 调用开销)。
 * 尾值永不为 0 由 issueMailSqes 的占位 SQE 保证(0 会被固件当"空环"哨兵丢弃,
 * 并发丢件挂死（507014） 根因)。尾值与返回码落诊断区(v15 逐单日志)。 */
int32_t flushBatchDoorbell(const ock::mf::HybmSdmaOrchParam *p, SqContext *ctx)
{
    memBarrier();                                                // SQE 写序后(批内唯一 dsb)
    const int32_t r = doorbellViaHal(p->devId, ctx->sqId, ctx->sqTail); // 批末一次敲铃
    const uintptr_t ws = static_cast<uintptr_t>(p->resultAddr);
    storeCell(ws + ock::mf::HYBM_SDMA_ORCH_WS_DBG_DOORBELL_OFF, ctx->sqTail);
    storeCell(ws + ock::mf::HYBM_SDMA_ORCH_WS_DBG_DBRET_OFF, static_cast<uint64_t>(static_cast<uint32_t>(r)));
    return (r != 0) ? 1390 : 0;                                 // 错误码: 敲铃失败
}

// 消费一封请求邮件: 写到达邮件像 → SQE 三连 → reqHead 推进。
// 返回 0 成功; 非 0 失败码(调用方记 result 并退出 epoch)。
int32_t consumeOneMail(const ock::mf::HybmSdmaOrchParam *p, SqContext *ctx, uint16_t ssid, uint64_t seq)
{
    using namespace ock::mf;
    const uint64_t reqSlot = p->reqBase + (seq % SMEM_SHM_SDMA_RS_REQ_SLOTS) * sizeof(HybmSdmaReqMail);
    cacheInvalidateLine(reqSlot & ~static_cast<uintptr_t>(63));
    const volatile HybmSdmaReqMail *rm = reinterpret_cast<const volatile HybmSdmaReqMail *>(reqSlot);
    HybmSdmaReqMail mail = {rm->src, rm->dst, rm->len, rm->imm}; // 快照(后续槽可复用)
    uint64_t immTag = mail.imm;                                  // 解码 reduce 编码 → 用户标签 + SQE opcode
    const uint8_t sqeOpcode = HybmSdmaDecodeOp(mail.imm, &immTag);
    const uint64_t peerSeg = mail.dst & ~(p->symSize - 1);       // 对端段基址(掩码算术)
    const uint64_t peerRes = peerSeg + (p->physSize - SMEM_SHM_SDMA_RESERVED); // 对端保留区基址
    const uint64_t arrMailSlot = peerRes + SMEM_SHM_SDMA_RS_ARR_MAIL +
                                 (seq % SMEM_SHM_SDMA_RS_ARR_SLOTS) * sizeof(HybmSdmaArrMail);
    volatile HybmSdmaArrMail *am = reinterpret_cast<volatile HybmSdmaArrMail *>(arrMailSlot);
    am->seq = seq + 1; // 戳值(1 基, 供两支戳 SQE 复用为拷贝源; 0 保留为"空", 防幻影)
    am->dst = mail.dst;
    am->len = mail.len;
    am->imm = immTag;  // 还原后的用户标签(reduce 编码不透传, wait 方兼容)
    cacheFlushLine(arrMailSlot & ~static_cast<uintptr_t>(63)); // 刷出邮件像(SQE 可读)
    const uint64_t peerArrStamp = peerRes + SMEM_SHM_SDMA_RS_ARR_STAMP +
                                  (seq % SMEM_SHM_SDMA_RS_ARR_SLOTS) * 8; // 对端到达戳槽
    /* v25: 单号写本端暂存(本地写+刷出, 本端 SDMA 立即可见)——②③ 的拷贝数据源 */
    const uint64_t stageSlot = p->reqBase - SMEM_SHM_SDMA_RS_REQ_RING + SMEM_SHM_SDMA_RS_STAGE +
                               (seq % SMEM_SHM_SDMA_RS_STAGE_SLOTS) * 8;
    storeCell(stageSlot, seq + 1);
    const uint64_t tailBefore = ctx->sqTail;
    auto ret = issueMailSqes(p, ctx, ssid, seq, mail, stageSlot, peerArrStamp, sqeOpcode);
    if (ret != 0) {
        return ret;
    }
    dbgLogMail(p, seq, mail, arrMailSlot, tailBefore, ctx->sqTail); // v15 逐单日志(最后写, 失败路径不记)
    storeCell(p->reqBase - SMEM_SHM_SDMA_RS_REQ_RING + SMEM_SHM_SDMA_RS_REQ_HEAD, seq + 1); // reqHead 推进
    return 0;
}
} // namespace

// kernel 入口: 按序消费请求环邮件(诊断模式跑到停止/错误; 正常模式到点自退)。
// 返回 0 正常退出(停止); 1 错误退出(诊断见 result)。
extern "C" __attribute__((visibility("default"))) uint32_t HybmSdmaOrchEpoch(void *args)
{
    using namespace ock::mf;
    HybmSdmaOrchParam *p = static_cast<HybmSdmaOrchParam *>(args);
    volatile HybmSdmaOrchResult *result =
        reinterpret_cast<volatile HybmSdmaOrchResult *>(p->resultAddr);
    result->magic = HYBM_SDMA_ORCH_RESULT_MAGIC;
    result->mainRet = 0;
    result->consumed = 0;
    result->exitReason = HYBM_SDMA_ORCH_EXIT_RUNNING; // 运行中(host 轮询判据: 变为终值即退出)
    finishResult(result);                             // 入口状态刷出(host 立即可见 RUNNING)
    SqContext ctx = {};
    ctx.result = result;
    ctx.streamId = static_cast<uint32_t>(p->streamId);
    ctx.sqId = p->sqId;
    ctx.devId = p->devId;
    auto ret = cp1LiveQuery(p, &ctx);
    if (ret != 0) {
        result->mainRet = 1300 + (ret & 0xFF); // 13xx: 活查询失败
        result->exitReason = HYBM_SDMA_ORCH_EXIT_ERROR;
        finishResult(result);
        return 1;
    }
    const uint16_t ssid = querySsid(p->devId); // ssid 实时查询(严禁硬编码)
    uint64_t head = loadCell(p->reqBase - SMEM_SHM_SDMA_RS_REQ_RING + SMEM_SHM_SDMA_RS_REQ_HEAD); // 消费起点
    /* 报时预算(v11): 永不退出的专用值不判报时(诊断模式); 0 用默认 15s(兼容旧 host);
     * 到 budgetS×10 跳自退——host 每 100ms 写一次递增计数, 精度 100ms, 不受设备负载影响。
     * 循环上限保留兜底(报时停喂时防永跑, 名额按 15s 预算折算)。 */
    const bool resident = (p->epochBudgetS == HYBM_SDMA_ORCH_EPOCH_BUDGET_RESIDENT);
    const uint32_t budgetS =
        (p->epochBudgetS == 0) ? HYBM_SDMA_ORCH_EPOCH_TIME_BUDGET_S : p->epochBudgetS;
    const uint64_t budgetTicks = static_cast<uint64_t>(budgetS) * HYBM_SDMA_ORCH_HEARTBEAT_HZ;
    const uint64_t maxLoops = resident ? UINT64_MAX
        : (static_cast<uint64_t>(HYBM_SDMA_ORCH_EPOCH_MAX_LOOPS) * budgetS +
           HYBM_SDMA_ORCH_EPOCH_TIME_BUDGET_S - 1) / HYBM_SDMA_ORCH_EPOCH_TIME_BUDGET_S;
    constexpr uint32_t kDoorbellBatch = 8; // 攒批上限(环内积压时 N 封一铃降 HAL 开销; 单封场景批=1)
    for (uint64_t loops = 0; loops < maxLoops; loops++) {
        if (!resident && LoadHeartbeat(result) >= budgetTicks) {
            result->exitReason = HYBM_SDMA_ORCH_EXIT_DEADLINE; // 时限到点(正常换轮)
            break;
        }
        if (loadCell(p->stopAddr) != 0) { // 停止标志(host 析构)
            result->exitReason = HYBM_SDMA_ORCH_EXIT_STOP;
            break;
        }
        uint64_t tail = loadCell(p->reqBase - SMEM_SHM_SDMA_RS_REQ_RING + SMEM_SHM_SDMA_RS_REQ_TAIL);
        if (head == tail) { // 环空
            usleep(HYBM_SDMA_ORCH_EPOCH_IDLE_US);
            continue;
        }
        uint32_t batch = 0;
        while (head != tail && batch < kDoorbellBatch) {
            ret = consumeOneMail(p, &ctx, ssid, head); // head 即序号, 不敲铃
            if (ret != 0) {
                result->mainRet = static_cast<uint32_t>(ret);
                result->exitReason = HYBM_SDMA_ORCH_EXIT_ERROR;
                (void)flushBatchDoorbell(p, &ctx); // 已入环部分尽力冲出
                head = 0;                          // 标记错误(跳出双循环)
                break;
            }
            head++;
            result->consumed++;
            batch++;
            if (batch < kDoorbellBatch) { // 批未满再查尾
                tail = loadCell(p->reqBase - SMEM_SHM_SDMA_RS_REQ_RING + SMEM_SHM_SDMA_RS_REQ_TAIL);
            }
        }
        if (head == 0) { // 错误标记(双循环跳出)
            break;
        }
        ret = flushBatchDoorbell(p, &ctx); // 批末统一敲铃
        if (ret != 0) {
            result->mainRet = static_cast<uint32_t>(ret);
            result->exitReason = HYBM_SDMA_ORCH_EXIT_ERROR;
            break;
        }
    }
    uint32_t headA = ctx.sqHead; // head 采样初值(查询失败时兜底)
    (void)cp1QueryProp(p->devId, p->sqId, DRV_SQCQ_PROP_SQ_HEAD, &headA, nullptr); // 逐单日志: head 采样
    result->headAfter = headA;
    finishResult(result);
    return (result->exitReason == HYBM_SDMA_ORCH_EXIT_ERROR) ? 1 : 0;
}
