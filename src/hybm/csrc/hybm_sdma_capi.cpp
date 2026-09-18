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

/* hybm_sdma_capi.cpp — 310P 设备发起 SDMA 的 hybm 层 C API 实现(smem_shm 层消费, 声明见 hybm_big_mem.h)
 * 资源与提交全部经进程级单例 SdmaOrchestrator(初始化在 InitTransManager 挂载点完成)。
 */
#include "hybm_sdma_orchestrator.h" // 单例(Ready/Get*/Launch/Wait)与工作区布局常量
#include "hybm_big_mem.h"      // 本文件实现的 C API 声明(hybm_sdma_*)

using namespace ock::mf; // 引入 SdmaOrchestrator/BM_LOG 所在命名空间

namespace { // 匿名命名空间(编译单元私有)
/* SQE 对的哨兵值(每次 submit 幂等预填; 对端 AICore 判非零, 值本身无语义) */
constexpr uint64_t HYBM_SDMA_ORCH_SUBMIT_SENTINEL = 0xE0ECA001ULL; // 提交哨兵魔数(0xE0ECA001)

/* 流水提交: 装配 param(经 prep 单例) + 哨兵预填 + launch */
int32_t SdmaSubmitCommon(uint64_t dstAddr, uint64_t flagDoneAddr, uint32_t chunks)
{
    auto &prep = SdmaOrchestrator::GetInstance(); // 单例引用(就绪判断/工作区/launch 均经它)
    if (!prep.Ready()) {                         // 未就绪(开关关/初始化失败)
        // 根因日志: 目的地址/标志地址/chunks
        BM_LOG_ERROR("sdma orch not ready, dstAddr: 0x" << std::hex << dstAddr << " flagDoneAddr: 0x"
                                                      << flagDoneAddr << " chunks: " << std::dec << chunks);
        return BM_NOT_INITIALIZED; // 未就绪直接拒绝(不静默降级)
    }
    /* 哨兵幂等预填(8B H2D, SQE 对标志源) */
    const uint64_t sent = HYBM_SDMA_ORCH_SUBMIT_SENTINEL; // 栈上暂存待拷入工作区哨兵槽
    // H2D: 哨兵写入工作区 +1024 槽(SQE 对第二支搬运的 8B 数据源)
    auto ret = DlAclApi::AclrtMemcpy(reinterpret_cast<void *>(prep.GetWorkspace() + HYBM_SDMA_ORCH_WS_SENTINEL_OFF),
                                     8, &sent, 8, ACL_MEMCPY_HOST_TO_DEVICE);
    if (ret != BM_OK) { // 哨兵预填失败
        BM_LOG_ERROR("fill sentinel failed, ret: " << ret << " sqId: " << prep.GetSqId()); // 根因日志: ret/sqId
        return ret;                                                                        // 透传失败(根因已记录)
    }
    HybmSdmaOrchParam p = {};                                // kernel 入参(逐字段装配, 见下)
    p.devId = prep.GetDieId();                              // CP1 内 HAL devId = die 号
    p.sqId = prep.GetSqId();                                // CP 流 SQ id(kernel 内活查询)
    p.streamId = prep.GetStreamId();                        // CP 流 id(SQE 的 rt_stream_id)
    p.submitMode = SUBMIT_MODE_PIPELINE;                    // 流水编排(kernel 内逐 chunk, chunks=1 即单块)
    p.chunks = chunks;                                      // chunk 数(邮箱槽序号即 chunk 序号)
    p.dstAddr = dstAddr;                                    // 数据目的基址(kernel 内按 chunk 递增)
    p.overflowAddr = prep.GetWorkspace() + HYBM_SDMA_ORCH_WS_OVF_OFF; // SQE overflow 目标槽
    p.flagSentinelAddr = prep.GetWorkspace() + HYBM_SDMA_ORCH_WS_SENTINEL_OFF; // 标志源 = 哨兵槽
    p.flagDoneAddr = flagDoneAddr;                          // 标志数组目的基址(kernel 内逐 chunk +8)
    p.resultAddr = prep.GetWorkspace() + HYBM_SDMA_ORCH_WS_RESULT_OFF; // 结果写回槽
    p.mailboxAddr = prep.GetWorkspace() + HYBM_SDMA_ORCH_WS_MAILBOX_OFF; /* 本端 AICore 写(ring) */
    return prep.LaunchOrchestrator(p); // 提交编排 kernel(结果即函数结果, 失败根因已记录)
} // namespace 结束前的公共提交函数
} // namespace

HYBM_API int32_t hybm_sdma_get_info(hybm_sdma_info_t *info) // 查询就绪信息
{
    if (info == nullptr) { // 出参指针空校验
        BM_LOG_ERROR("sdma orch get info failed: info is null"); // 根因日志: 空指针
        return BM_INVALID_PARAM;                               // 参数非法
    }
    auto &prep = SdmaOrchestrator::GetInstance(); // 单例引用
    if (!prep.Ready()) {                         // 未就绪
        BM_LOG_ERROR("sdma orch not ready (switch off or init failed)"); // 根因日志: 未就绪原因(开关/初始化)
        return BM_NOT_INITIALIZED;                                     // 未初始化
    }
    info->sqId = prep.GetSqId();           // 填 CP 流 SQ id
    info->streamId = prep.GetStreamId();   // 填 CP 流 id
    info->dieId = prep.GetDieId();         // 填 die 号(devId%2)
    info->workspace = prep.GetWorkspace(); // 填工作区设备地址
    return BM_OK;                          // 查询成功
}

// 流水提交(chunks 1~64)
HYBM_API int32_t hybm_sdma_submit(uint64_t dstAddr, uint64_t flagDoneAddr, uint32_t chunks)
{
    if (chunks == 0 || chunks > HYBM_SDMA_ORCH_WS_MAILBOX_SLOTS) { // 边界: 0 与超邮箱槽数(64)均非法
        // 根因日志: 非法 chunks 与合法区间/目的地址
        BM_LOG_ERROR("invalid chunks: " << chunks << " (1~" << HYBM_SDMA_ORCH_WS_MAILBOX_SLOTS
                                        << ") dstAddr: 0x" << std::hex << dstAddr);
        return BM_INVALID_PARAM; // 参数非法
    }
    return SdmaSubmitCommon(dstAddr, flagDoneAddr, chunks); // 参数合法后统一提交
}

HYBM_API int32_t hybm_sdma_wait(void) // 等待搬运完成
{
    return SdmaOrchestrator::GetInstance().WaitOrchestrator(); // 同步等待提交流完成
}

HYBM_API int32_t hybm_sdma_get_result(uint32_t *mainRet, uint32_t *stage, uint32_t *headAfter) // 诊断读回
{
    auto &prep = SdmaOrchestrator::GetInstance(); // 单例引用
    if (!prep.Ready()) {                         // 未就绪时结果不可读
        BM_LOG_ERROR("sdma orch not ready, result unavailable"); // 根因日志: 未就绪
        return BM_NOT_INITIALIZED;                              // 未初始化
    }
    HybmSdmaOrchResult r = {}; // 结果读回缓冲(零初始化)
    // D2H: 从工作区 +1032 槽读回 56B kernel 结果
    auto ret = DlAclApi::AclrtMemcpy(&r, sizeof(r),
                                     reinterpret_cast<void *>(prep.GetWorkspace() + HYBM_SDMA_ORCH_WS_RESULT_OFF),
                                     sizeof(r), ACL_MEMCPY_DEVICE_TO_HOST);
    if (ret != BM_OK) { // 读回失败
        BM_LOG_ERROR("read sdma orch result failed, ret: " << ret << " sqId: " << prep.GetSqId()); // 根因日志: ret/sqId
        return ret;                                                                              // 透传失败(根因已记录)
    }
    if (mainRet != nullptr) { // 调用方关心主返回码
        *mainRet = static_cast<uint32_t>(r.mainRet); // 填 kernel 主返回码(0=成功)
    }
    if (stage != nullptr) { // 调用方关心阶段
        *stage = r.stage;   // 填状态机阶段(5=完成)
    }
    if (headAfter != nullptr) { // 调用方关心 SQ head
        *headAfter = r.headAfter; // 填 SQ head(TSFW 消费证据)
    }
    return BM_OK; // 读取成功
}
