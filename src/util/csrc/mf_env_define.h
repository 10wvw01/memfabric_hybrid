/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */
#ifndef MEMFABRIC_HYBRID_MF_ENV_CONFIG_H
#define MEMFABRIC_HYBRID_MF_ENV_CONFIG_H

#include <cstdlib>
#include <string>

#include "mf_out_logger.h"

namespace ock {
namespace mf {
namespace env {

static std::string GetEnvStr(const char *name, const char *deprecatedName = nullptr) noexcept
{
    if (const char *val = std::getenv(name); val != nullptr) {
        return std::string(val);
    }
    if (deprecatedName != nullptr) {
        if (const char *depVal = std::getenv(deprecatedName); depVal != nullptr) {
            MF_OUT_LOG("[MF ", WARN_LEVEL,
                       "Environment variable '" << deprecatedName << "' is deprecated, please use '" << name
                                                << "' instead.");
            return std::string(depVal);
        }
    }
    return std::string();
}

inline const std::string CUDA_HOME = GetEnvStr("CUDA_HOME");

inline const std::string ASCEND_HOME_PATH = GetEnvStr("ASCEND_HOME_PATH");
inline const std::string ASCEND_RT_VISIBLE_DEVICES = GetEnvStr("ASCEND_RT_VISIBLE_DEVICES");

inline const std::string HCOM_MAX_SLICE_SIZE = GetEnvStr("HCOM_MAX_SLICE_SIZE");
inline const std::string HCOM_RECV_DATA_SIZE = GetEnvStr("HCOM_RECV_DATA_SIZE");

inline const std::string MF_HCOM_CQ_DEPTH = GetEnvStr("MF_HCOM_CQ_DEPTH");
inline const std::string MF_HCOM_SQ_SIZE = GetEnvStr("MF_HCOM_SQ_SIZE");
inline const std::string MF_HCOM_RQ_SIZE = GetEnvStr("MF_HCOM_RQ_SIZE");
inline const std::string MF_HCOM_PREPOST_SIZE = GetEnvStr("MF_HCOM_PREPOST_SIZE");
inline const std::string MF_HCOM_MAX_SEND_RECV_DATA_CNT = GetEnvStr("MF_HCOM_MAX_SEND_RECV_DATA_CNT");
inline const std::string MF_HYBM_RDMA_SWAP_SPACE_SIZE =
    GetEnvStr("MF_HYBM_RDMA_SWAP_SPACE_SIZE", "HYBM_RDMA_SWAP_SPACE_SIZE");
inline const std::string MF_HYBM_URMA_SWAP_SPACE_SIZE =
    GetEnvStr("MF_HYBM_URMA_SWAP_SPACE_SIZE", "HYBM_URMA_SWAP_SPACE_SIZE");
inline const std::string MF_HYBM_RDMA_FORCE_UNREGISTERED =
    GetEnvStr("MF_HYBM_RDMA_FORCE_UNREGISTERED", "HYBM_RDMA_FORCE_UNREGISTERED");
inline const std::string MF_LOG_LEVEL = GetEnvStr("MF_LOG_LEVEL", "ASCEND_MF_LOG_LEVEL");
inline const std::string MF_SOCKET_URL = GetEnvStr("MF_SOCKET_URL");
inline const std::string MF_TRANSPORT_MANAGER = GetEnvStr("MF_TRANSPORT_MANAGER", "TRANSPORT_MANAGER");
inline const std::string MF_CONFIG_STORE_URL = GetEnvStr("MF_CONFIG_STORE_URL", "ASCEND_MF_STORE_URL");
inline const std::string MF_CONFIG_STORE_PORT_START =
    GetEnvStr("MF_CONFIG_STORE_PORT_START", "MEMFABRIC_HYBRID_CONFIG_STORE_PORT_START");
inline const std::string MF_CONFIG_STORE_PORT_END =
    GetEnvStr("MF_CONFIG_STORE_PORT_END", "MEMFABRIC_HYBRID_CONFIG_STORE_PORT_END");

// 310P 设备发起 SDMA 协同模式显式禁用开关(默认启用: 310P 上 A3 SDMA 配方必败,
// 协同为该平台唯一可用 SDMA 链路, 故零配置自动启用; 该变量仅供诊断场景显式关断)
inline const std::string MF_SDMA_ORCH_DISABLE = GetEnvStr("MF_SDMA_ORCH_DISABLE"); // SdmaOrchestrator::Disabled 读取
// 协同编排 kernel 的 launch json 路径(缺省依次: 安装前缀自动发现 → CWD 的 libmf_sdma_orch_v5.json)
inline const std::string MF_SDMA_ORCH_JSON = GetEnvStr("MF_SDMA_ORCH_JSON"); // LoadOrchestratorKernel 读取
// 搬运 kernel 每轮时长秒数(缺省 15): 到点自退后由监督线程重新拉起(环+游标零丢失)。
// 4294967295=永不退出(诊断用; 会阻塞任何全设备同步, 见平台事实 2026-10-12 定案条目)
inline const std::string MF_SDMA_ORCH_EPOCH_BUDGET_S = GetEnvStr("MF_SDMA_ORCH_EPOCH_BUDGET_S"); // ConfigureOrch 读取
inline const std::string MF_GROUP_JOIN_MAX_TIMEOUT = GetEnvStr("MF_GROUP_JOIN_MAX_TIMEOUT");
inline const std::string MF_GROUP_RETRY_TIME = GetEnvStr("MF_GROUP_RETRY_TIME", "MF_SMEM_GROUP_RETRY_TIME");
inline const std::string MF_ACC_CHECK_PERIOD_HOURS =
    GetEnvStr("MF_ACC_CHECK_PERIOD_HOURS", "ACCLINK_CHECK_PERIOD_HOURS");
inline const std::string MF_ACC_CERT_CHECK_AHEAD_DAYS =
    GetEnvStr("MF_ACC_CERT_CHECK_AHEAD_DAYS", "ACCLINK_CERT_CHECK_AHEAD_DAYS");

} // namespace env
} // namespace mf
} // namespace ock

#endif // MEMFABRIC_HYBRID_MF_ENV_CONFIG_H
