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
    auto &prep = HybmOverlapPrep::GetInstance();
    if (!prep.Ready()) {
        BM_LOG_ERROR("overlap not ready, dstAddr: 0x" << std::hex << dstAddr << " flagDoneAddr: 0x"
                                                      << flagDoneAddr);
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
    p.submitMode = SUBMIT_MODE_CHAIN; /* mode13: 单 chunk 八步闭环(P1 范围) */
    p.dstAddr = dstAddr;
    p.overflowAddr = prep.GetWorkspace() + HYBM_OVERLAP_WS_OVF_OFF;
    p.flagSentinelAddr = prep.GetWorkspace() + HYBM_OVERLAP_WS_SENTINEL_OFF;
    p.flagDoneAddr = flagDoneAddr;
    p.resultAddr = prep.GetWorkspace() + HYBM_OVERLAP_WS_RESULT_OFF;
    p.reserved[1] = 0x8U; /* doorbell 强制走 HAL(A.4) */
    p.mailboxAddr = prep.GetWorkspace() + HYBM_OVERLAP_WS_MAILBOX_OFF; /* 本端 AICore 写 */
    p.role = 0;
    return prep.LaunchOrchestrator(p);
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
