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

/* smem_shm_aicore_base_sdma.h — V5 邮箱化设备侧 API(AICore kernel 用, signal/wait/quiet)
 *
 * 使用形态(与 910B RDMADemo 同构): 用户 kernel 以池 gva 为参数, 三 API 均以 gva 为首参:
 *   smem_shm_sdma_signal(gva, src, dst, len, imm) → bool   投递搬运请求(满环自旋)
 *   smem_shm_sdma_wait(gva) → smem_shm_sdma_mail_t         按序消费到达邮件(阻塞自旋)
 *   smem_shm_sdma_quiet(gva) → bool                        等待全部发出落地(源缓冲复用前)
 *
 * 邮箱由 memfabric 全托管(双环在段尾保留区, host 初始化清零): 用户零槽位算术/零清零/
 * 零标志地址方案; 到达邮件 {dst,len,imm} 自描述, 接收方免预约定布局。
 *
 * 环境依赖: 段对称大小须 2 的幂(host 侧建池时校验); rank/symm/phys 从 meta 区自取
 * (310P meta 可读性已修复, 见平台事实档案)。
 *
 * dav-2002 工具链约束(见 base_meta.h 注): 本头全部设备内存访问 = gva 参数的指针算术
 * + GlobalTensor cache 维护(ex08 验证路径), 严禁运行时整数→指针 cast 与指针物化。
 */
#ifndef __MEMFABRIC_SMEM_AI_CORE_BASE_SDMA_H__ /* 头文件包含守卫(防重复包含) */
#define __MEMFABRIC_SMEM_AI_CORE_BASE_SDMA_H__ /* 同上, 与文件尾 #endif 配对 */

#include "smem_shm_aicore_base_define.h" // AICore 基础类型与内联宏(SMEM_SHM_INLINE_AICORE)
#include "smem_shm_aicore_base_meta.h"   // meta 区布局常量与设备侧读取原语
#include "smem_shm_sdma_layout.h"        // 段尾保留区布局单一事实源(三端共享)

#ifdef __cplusplus // C++ 编译环境
extern "C" {       // C 链接(供 bisheng 编译的 AICore kernel include)
#endif

/* ---- 邮件(wait 出参, 32B) ---- */
struct smem_shm_sdma_mail_t {
    uint64_t dst;    /* 数据落点(接收方直接定位, 免预约定布局) */
    uint64_t len;    /* 数据长度(字节) */
    uint64_t imm;    /* 用户标签(signal 的 imm 原样透传) */
    uint32_t seq;    /* 完成序号(诊断) */
    uint32_t status; /* 0=成功; 1=超时; 2=到达环溢出(生产方越界覆写) */
};

/* 轮询预算(探测次数上限; 与渐进退避配合, 超时为错误路径) */
constexpr uint64_t SMEM_SHM_SDMA_POLL_MAX_ITER = 100000000ULL;
/* 渐进退避: 前 SPIN_ITERS 轮纯自旋保低延迟, 之后插入空转退避并渐进加倍(上限 MAX),
 * 降低等待期间 AICore 周期浪费(AICore 无 usleep, 以编译屏障空转近似) */
constexpr uint64_t SMEM_SHM_SDMA_SPIN_ITERS = 4096;   /* 纯自旋轮数 */
constexpr uint32_t SMEM_SHM_SDMA_BACKOFF_START = 64;  /* 退避起始空转数 */
constexpr uint32_t SMEM_SHM_SDMA_BACKOFF_MAX = 16384; /* 退避上限空转数 */

/* 轮询退避(每轮探测后调用; delay 由调用方持有渐增) */
SMEM_SHM_INLINE_AICORE void SdmaPollBackoff(uint64_t iter, uint32_t &delay)
{
    if (iter < SMEM_SHM_SDMA_SPIN_ITERS) { /* 自旋期(保低延迟) */
        return;                            /* 不退避 */
    }
    for (uint32_t i = 0; i < delay; i++) { /* 空转退避 */
        __asm__ __volatile__("" ::: "memory"); /* 编译屏障防空转被优化 */
    }
    if ((iter & 0xFFu) == 0 && delay < SMEM_SHM_SDMA_BACKOFF_MAX) { /* 每 256 轮 */
        delay <<= 1; /* 退避加倍(至上限) */
    }
}

/* 到达邮件状态码 */
constexpr uint32_t SMEM_SHM_SDMA_MAIL_OK = 0;       /* 成功取件 */
constexpr uint32_t SMEM_SHM_SDMA_MAIL_TIMEOUT = 1;  /* 轮询预算耗尽(无邮件/传输失败) */
constexpr uint32_t SMEM_SHM_SDMA_MAIL_OVERRUN = 2;  /* 到达环溢出(消费落后超 1024 封) */

#ifdef __cplusplus // C++ 编译环境
}                  // extern "C" 结束
#endif

/* ---- 内部原语(仅本头使用; 全部 gva 指针算术 + GlobalTensor cache 维护) ---- */

/* 预计算本端段尾保留区基址(kernel 入口一次计算, 三件套 _at 变体复用;
 * 免每调用 3 次 meta 区 GM 读——symm/rank/phys 在 kernel 生命周期内不变) */
SMEM_SHM_INLINE_AICORE __gm__ uint8_t *smem_shm_sdma_reserved(__gm__ void *gva)
{
    const uint64_t symm = smem_shm_get_symmetric_size(0);            // 对称段大小(meta)
    const uint64_t rank = smem_shm_get_global_rank(0);               // 本端 rank(meta)
    const uint64_t phys = smem_shm_get_phys_size(0);                 // 段物理大小(meta)
    return static_cast<__gm__ uint8_t *>(gva) + symm * rank + (phys - SMEM_SHM_SDMA_RESERVED); // 指针算术
}

/* dcci 失效后读 8B(AICPU/SQE 写 → AICore 读的可见性正解, E0c 定论) */
SMEM_SHM_INLINE_AICORE uint64_t SdmaReadCell(__gm__ uint8_t *res, uint64_t off)
{
    __gm__ uint8_t *cell = res + off;                                        // 指针算术定位
    AscendC::GlobalTensor<int32_t> t;                                        // GM 张量(cache 指令寻址)
    t.SetGlobalBuffer(reinterpret_cast<__gm__ int32_t *>(cell), 2);          // 绑定该 8B
    AscendC::DataCacheCleanAndInvalid<int32_t, AscendC::CacheLine::SINGLE_CACHE_LINE>(t); // 失效取新
    return *reinterpret_cast<__gm__ volatile uint64_t *>(cell);              // volatile 读
}

/* 写 8B 并 clean(AICore 写 → AICPU/SQE 读) */
SMEM_SHM_INLINE_AICORE void SdmaWriteCell(__gm__ uint8_t *res, uint64_t off, uint64_t v)
{
    __gm__ uint8_t *cell = res + off;                                        // 指针算术定位
    *reinterpret_cast<__gm__ volatile uint64_t *>(cell) = v;                 // volatile 写
    AscendC::GlobalTensor<int32_t> t;                                        // GM 张量
    t.SetGlobalBuffer(reinterpret_cast<__gm__ int32_t *>(cell), 2);          // 绑定该 8B
    AscendC::DataCacheCleanAndInvalid<int32_t, AscendC::CacheLine::SINGLE_CACHE_LINE>(t); // 刷出
}

/* 读 32B 邮件槽(dcci 失效 + 4×volatile 读; 32B 对齐不跨行) */
SMEM_SHM_INLINE_AICORE void SdmaReadMail(__gm__ uint8_t *slot, uint64_t *f0, uint64_t *f1, uint64_t *f2,
                                         uint64_t *f3)
{
    AscendC::GlobalTensor<int32_t> t;                                        // GM 张量
    t.SetGlobalBuffer(reinterpret_cast<__gm__ int32_t *>(slot), 8);          // 绑定 32B(4 字)
    AscendC::DataCacheCleanAndInvalid<int32_t, AscendC::CacheLine::SINGLE_CACHE_LINE>(t); // 失效取新
    __gm__ volatile uint64_t *q = reinterpret_cast<__gm__ volatile uint64_t *>(slot); // 槽视角
    *f0 = q[0]; // 字段 0
    *f1 = q[1]; // 字段 1
    *f2 = q[2]; // 字段 2
    *f3 = q[3]; // 字段 3
}

/* ============================ 公开 API ============================ */


/* signal 的预计算基址变体(res = smem_shm_sdma_reserved(gva) 缓存值) */
SMEM_SHM_INLINE_AICORE bool smem_shm_sdma_signal_at(__gm__ uint8_t *res, __gm__ void *src, __gm__ void *dst,
                                                    uint64_t len, uint64_t imm)
{
    const uint64_t tail = SdmaReadCell(res, SMEM_SHM_SDMA_RS_REQ_TAIL); // 生产游标(自写, 直接读)
    uint32_t delay = SMEM_SHM_SDMA_BACKOFF_START;                       // 退避档位(渐进)
    for (uint64_t i = 0; i < SMEM_SHM_SDMA_POLL_MAX_ITER; i++) {        // 满环自旋背压
        const uint64_t head = SdmaReadCell(res, SMEM_SHM_SDMA_RS_REQ_HEAD); // AICPU 消费游标
        if (tail - head < SMEM_SHM_SDMA_RS_REQ_SLOTS) {                 // 有空槽
            break;                                                      // 退出自旋
        }
        SdmaPollBackoff(i, delay);                                      // 渐进退避
    }
    const uint64_t head = SdmaReadCell(res, SMEM_SHM_SDMA_RS_REQ_HEAD); // 复查(预算耗尽路径)
    if (tail - head >= SMEM_SHM_SDMA_RS_REQ_SLOTS) {                    // 仍满(预算耗尽)
        return false;                                                   // 投递失败(上层可见)
    }
    __gm__ uint8_t *slot = res + SMEM_SHM_SDMA_RS_REQ_RING +            // 请求环槽(指针算术)
                           (tail % SMEM_SHM_SDMA_RS_REQ_SLOTS) * 32;
    *reinterpret_cast<__gm__ volatile uint64_t *>(slot + 0) = reinterpret_cast<uint64_t>(src); // 源地址
    *reinterpret_cast<__gm__ volatile uint64_t *>(slot + 8) = reinterpret_cast<uint64_t>(dst); // 目的地址
    *reinterpret_cast<__gm__ volatile uint64_t *>(slot + 16) = len;                            // 字节数
    *reinterpret_cast<__gm__ volatile uint64_t *>(slot + 24) = imm;                            // 用户标签
    AscendC::GlobalTensor<int32_t> t;                                   // GM 张量
    t.SetGlobalBuffer(reinterpret_cast<__gm__ int32_t *>(slot), 8);     // 绑定 32B 槽
    AscendC::DataCacheCleanAndInvalid<int32_t, AscendC::CacheLine::SINGLE_CACHE_LINE>(t); // 刷出(AICPU 可见)
    SdmaWriteCell(res, SMEM_SHM_SDMA_RS_REQ_TAIL, tail + 1);            // 推进生产游标
    return true;                                                        // 投递成功
}


/* wait 的预计算基址变体(res = smem_shm_sdma_reserved(gva) 缓存值) */
SMEM_SHM_INLINE_AICORE smem_shm_sdma_mail_t smem_shm_sdma_wait_at(__gm__ uint8_t *res)
{
    smem_shm_sdma_mail_t m = {0, 0, 0, 0, SMEM_SHM_SDMA_MAIL_TIMEOUT};   // 出参(默认超时态)
    const uint64_t head = SdmaReadCell(res, SMEM_SHM_SDMA_RS_ARR_HEAD); // 消费游标(自写, 直接读)
    const uint64_t expect = head + 1; /* 期望戳(1 基: 戳=seq+1, 0=空——防 seq0 幻影) */
    uint64_t stamp = 0;                // 戳快照
    uint32_t delay = SMEM_SHM_SDMA_BACKOFF_START; // 退避档位(渐进)
    for (uint64_t i = 0; i < SMEM_SHM_SDMA_POLL_MAX_ITER; i++) {        // 戳轮询(SQE 写 → dcci)
        stamp = SdmaReadCell(res, SMEM_SHM_SDMA_RS_ARR_STAMP +
                                    (head % SMEM_SHM_SDMA_RS_ARR_SLOTS) * 8);
        if (stamp == expect) {                                          // 第 head 封已落地
            break;                                                      // 命中
        }
        /* 溢出判定: 戳超前期望即该槽已被越环覆写(正常时序槽值只能 ==expect 或上一轮
         * 旧值 <expect; 任何超前都意味着消费落后至少一整环, 差值大小不影响结论) */
        if (stamp > expect) {                                           // 越界覆写
            m.status = SMEM_SHM_SDMA_MAIL_OVERRUN;                      // 溢出错(不静默)
            m.seq = static_cast<uint32_t>(head);                        // 诊断序号
            return m;                                                   // 立即返回
        }
        SdmaPollBackoff(i, delay);                                      // 渐进退避
    }
    if (stamp != expect) {                                              // 预算耗尽仍无邮件
        m.status = SMEM_SHM_SDMA_MAIL_TIMEOUT;                          // 超时
        m.seq = static_cast<uint32_t>(head);                            // 诊断序号
        return m;                                                       // 返回超时态
    }
    __gm__ uint8_t *slot = res + SMEM_SHM_SDMA_RS_ARR_MAIL +            // 到达邮件槽(指针算术)
                           (head % SMEM_SHM_SDMA_RS_ARR_SLOTS) * 32;
    uint64_t f0 = 0, f1 = 0, f2 = 0, f3 = 0;                            // 字段暂存
    SdmaReadMail(slot, &f0, &f1, &f2, &f3);                             // 读 32B 邮件
    m.dst = f1;                                                         // 数据落点(f0=戳值诊断用)
    m.len = f2;                                                         // 数据长度
    m.imm = f3;                                                         // 用户标签
    m.seq = static_cast<uint32_t>(f0 - 1);                              // 序号(0 基, 诊断; f0 为 1 基戳)
    m.status = SMEM_SHM_SDMA_MAIL_OK;                                   // 成功
    SdmaWriteCell(res, SMEM_SHM_SDMA_RS_ARR_HEAD, head + 1);            // 推进消费游标
    return m;                                                           // 返回邮件
}


/* quiet 的预计算基址变体(res = smem_shm_sdma_reserved(gva) 缓存值) */
SMEM_SHM_INLINE_AICORE bool smem_shm_sdma_quiet_at(__gm__ uint8_t *res)
{
    const uint64_t tail = SdmaReadCell(res, SMEM_SHM_SDMA_RS_REQ_TAIL); // 已投递总数
    if (tail == 0) {                                                    // 无投递
        return true;                                                    // 平凡完成
    }
    const uint64_t expect = tail; /* 末封戳(1 基: 末封 seq=tail-1 → 戳=tail) */
    uint32_t delay = SMEM_SHM_SDMA_BACKOFF_START; // 退避档位(渐进)
    for (uint64_t i = 0; i < SMEM_SHM_SDMA_POLL_MAX_ITER; i++) {        // 完成戳轮询(SQE 写 → dcci)
        const uint64_t stamp = SdmaReadCell(res, SMEM_SHM_SDMA_RS_QUIET +
                                            ((tail - 1) % SMEM_SHM_SDMA_RS_QUIET_SLOTS) * 8);
        if (stamp == expect) {                                          // 末封已落地(FIFO 即全部)
            return true;                                                // 全部落地
        }
        SdmaPollBackoff(i, delay);                                      // 渐进退避
    }
    return false;                                                       // 预算耗尽(传输异常)
}

/* ======================= gva 便捷封装(转发 _at 变体) ======================= */

/* 投递一封搬运请求邮件(源→目的, 带用户标签)。
 * 满环时渐进退避自旋(背压); 预算耗尽返回 false(邮件未投递, 消费方 wait 将超时暴露)。
 * 返回 true=已投递。len 须 4 字节对齐(SDMA 搬运粒度, 长度直传 SQE 不做补齐)。
 * 高频路径可 kernel 入口 smem_shm_sdma_reserved(gva) 预计算后走 _at 变体,
 * 免每调用 3 次 meta 区 GM 读(值 kernel 生命周期内不变)。 */
SMEM_SHM_INLINE_AICORE bool smem_shm_sdma_signal(__gm__ void *gva, __gm__ void *src, __gm__ void *dst,
                                                 uint64_t len, uint64_t imm)
{
    return smem_shm_sdma_signal_at(smem_shm_sdma_reserved(gva), src, dst, len, imm); // 预计算基址转发
}

/* 按序消费下一封到达邮件(阻塞自旋+渐进退避, 预算耗尽/溢出由 status 表达)。
 * 邮件 {dst,len,imm} 自描述: 接收方直接定位数据, 免预约定布局。
 * 高频路径走 _at 变体(见 signal 注)。 */
SMEM_SHM_INLINE_AICORE smem_shm_sdma_mail_t smem_shm_sdma_wait(__gm__ void *gva)
{
    return smem_shm_sdma_wait_at(smem_shm_sdma_reserved(gva)); // 预计算基址转发
}

/* 等待本端全部已投递请求落地(quiet: 源缓冲复用/改写前调用)。
 * 依据: 完成戳按 SQE FIFO 序落地, 末封戳到即全部到。返回 true=全部落地。 */
SMEM_SHM_INLINE_AICORE bool smem_shm_sdma_quiet(__gm__ void *gva)
{
    return smem_shm_sdma_quiet_at(smem_shm_sdma_reserved(gva)); // 预计算基址转发
}

#endif // __MEMFABRIC_SMEM_AI_CORE_BASE_SDMA_H__
