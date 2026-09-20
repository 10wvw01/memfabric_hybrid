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
 *
 * V5 邮箱化(2026-09-18): host 侧数据面 API(submit/wait/get_result)废止——搬运由 AICore
 * kernel 内 smem_shm_sdma_signal/wait/quiet 设备侧自驱, host 不再参与提交; 本文件仅保留
 * 就绪信息查询(get_info, 工作区非空 = 协同可用, smem_shm_sdma_get_workspace 的实现基础)。
 */
#include "hybm_sdma_orchestrator.h" // 单例(Ready/GetWorkspace)与 V5 契约
#include "hybm_big_mem.h"           // 本文件实现的 C API 声明(hybm_sdma_get_info)

using namespace ock::mf; // 引入 SdmaOrchestrator/BM_LOG 所在命名空间

HYBM_API int32_t hybm_sdma_get_info(hybm_sdma_info_t *info) // 查询就绪信息
{
    if (info == nullptr) { // 出参指针空校验
        BM_LOG_ERROR("sdma orch get info failed: info is null");
        return BM_INVALID_PARAM;                               // 参数非法
    }
    auto &prep = SdmaOrchestrator::GetInstance(); // 单例引用
    if (!prep.Ready()) {                         // 未就绪
        BM_LOG_ERROR("sdma orch not ready (switch off or init failed)");
        return BM_NOT_INITIALIZED;                                     // 未初始化
    }
    info->workspace = prep.GetWorkspace(); // 填工作区设备地址(非空 = 协同就绪)
    return BM_OK;                     // 查询成功
}
