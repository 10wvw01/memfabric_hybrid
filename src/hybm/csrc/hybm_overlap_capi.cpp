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

/* hybm_overlap_capi.cpp — 310P 协同模式 C API 实现(smem_shm 层消费, 声明见 hybm_big_mem.h)
 * 资源与提交全部经进程级单例 HybmOverlapPrep(初始化在 InitTransManager 挂载点完成)。
 */
#include "hybm_overlap_prep.h"
#include "hybm_big_mem.h"

using namespace ock::mf;

namespace {
/* SQE 对的哨兵值(每次 submit 幂等预填; 对端 AICore 判非零, 值本身无语义) */
constexpr uint64_t HYBM_OVERLAP_SUBMIT_SENTINEL = 0xE0ECA001ULL;

/* mode13/mode14 公共提交: 装配 param(经 prep 单例) + 哨兵预填 + launch */
int32_t HybmOverlapSubmitCommon(uint64_t dstAddr, uint64_t flagDoneAddr, uint32_t chunks, uint32_t submitMode)
{
    auto &prep = HybmOverlapPrep::GetInstance();
    if (!prep.Ready()) {
        BM_LOG_ERROR("overlap not ready, dstAddr: 0x" << std::hex << dstAddr << " flagDoneAddr: 0x"
                                                      << flagDoneAddr << " mode: " << std::dec << submitMode);
        return BM_NOT_INITIALIZED;
    }
    /* 哨兵幂等预填(8B H2D, SQE 对标志源) */
    const uint64_t sent = HYBM_OVERLAP_SUBMIT_SENTINEL;
    auto ret = DlAclApi::AclrtMemcpy(reinterpret_cast<void *>(prep.GetWorkspace() + HYBM_OVERLAP_WS_SENTINEL_OFF),
                                     8, &sent, 8, ACL_MEMCPY_HOST_TO_DEVICE);
    if (ret != BM_OK) {
        BM_LOG_ERROR("fill sentinel failed, ret: " << ret << " sqId: " << prep.GetSqId());
        return ret;
    }
    HybmOverlapParam p = {};
    p.devId = prep.GetDieId();
    p.sqId = prep.GetSqId();
    p.streamId = prep.GetStreamId();
    p.submitMode = submitMode;            /* 13=单 chunk 闭环 / 14=多 chunk 流水 */
    p.dstAddr = dstAddr;                  /* 数据目的基址(kernel 内按 chunk 递增) */
    p.overflowAddr = prep.GetWorkspace() + HYBM_OVERLAP_WS_OVF_OFF;
    p.flagSentinelAddr = prep.GetWorkspace() + HYBM_OVERLAP_WS_SENTINEL_OFF;
    p.flagDoneAddr = flagDoneAddr;        /* 标志目的基址(kernel 内每 chunk +8) */
    p.resultAddr = prep.GetWorkspace() + HYBM_OVERLAP_WS_RESULT_OFF;
    p.reserved[1] = 0x8U;                 /* doorbell 强制走 HAL(A.4) */
    p.mailboxAddr = prep.GetWorkspace() + HYBM_OVERLAP_WS_MAILBOX_OFF; /* 本端 AICore 写(ring) */
    p.role = 0;
    p.roleRsv[1] = (submitMode == SUBMIT_MODE_PIPELINE) ? chunks : 0; /* mode14: chunk 数 */
    return prep.LaunchOrchestrator(p);
}
} // namespace

HYBM_API int32_t hybm_overlap_get_info(hybm_overlap_info_t *info)
{
    if (info == nullptr) {
        BM_LOG_ERROR("overlap get info failed: info is null");
        return BM_INVALID_PARAM;
    }
    auto &prep = HybmOverlapPrep::GetInstance();
    if (!prep.Ready()) {
        BM_LOG_ERROR("overlap not ready (switch off or init failed)");
        return BM_NOT_INITIALIZED;
    }
    info->sqId = prep.GetSqId();
    info->streamId = prep.GetStreamId();
    info->dieId = prep.GetDieId();
    info->workspace = prep.GetWorkspace();
    return BM_OK;
}

HYBM_API int32_t hybm_overlap_submit(uint64_t dstAddr, uint64_t flagDoneAddr)
{
    return HybmOverlapSubmitCommon(dstAddr, flagDoneAddr, 1, SUBMIT_MODE_CHAIN);
}

HYBM_API int32_t hybm_overlap_submit_pipelined(uint64_t dstAddr, uint64_t flagDoneAddr, uint32_t chunks)
{
    if (chunks == 0 || chunks > HYBM_OVERLAP_WS_MAILBOX_SLOTS) {
        BM_LOG_ERROR("invalid chunks: " << chunks << " (1~" << HYBM_OVERLAP_WS_MAILBOX_SLOTS
                                        << ") dstAddr: 0x" << std::hex << dstAddr);
        return BM_INVALID_PARAM;
    }
    return HybmOverlapSubmitCommon(dstAddr, flagDoneAddr, chunks, SUBMIT_MODE_PIPELINE);
}

HYBM_API int32_t hybm_overlap_wait(void)
{
    return HybmOverlapPrep::GetInstance().WaitOrchestrator();
}

HYBM_API int32_t hybm_overlap_get_result(uint32_t *mainRet, uint32_t *stage, uint32_t *headAfter)
{
    auto &prep = HybmOverlapPrep::GetInstance();
    if (!prep.Ready()) {
        BM_LOG_ERROR("overlap not ready, result unavailable");
        return BM_NOT_INITIALIZED;
    }
    HybmOverlapResult r = {};
    auto ret = DlAclApi::AclrtMemcpy(&r, sizeof(r),
                                     reinterpret_cast<void *>(prep.GetWorkspace() + HYBM_OVERLAP_WS_RESULT_OFF),
                                     sizeof(r), ACL_MEMCPY_DEVICE_TO_HOST);
    if (ret != BM_OK) {
        BM_LOG_ERROR("read overlap result failed, ret: " << ret << " sqId: " << prep.GetSqId());
        return ret;
    }
    if (mainRet != nullptr) {
        *mainRet = static_cast<uint32_t>(r.mainRet);
    }
    if (stage != nullptr) {
        *stage = r.stage;
    }
    if (headAfter != nullptr) {
        *headAfter = r.headAfter;
    }
    return BM_OK;
}
