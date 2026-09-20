/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */
#ifndef MEMFABRIC_HYBRID_TOPOLOGY_PREFLIGHT_H
#define MEMFABRIC_HYBRID_TOPOLOGY_PREFLIGHT_H

#include <chrono>
#include <string>
#include <vector>
#include <utility>

#include "smem_config_store.h"
#include "smem_def.h"

namespace ock {
namespace smem {

/* Shared topology preflight helper for BM and TRANS modes.
 * Publishes rankId:devId to the store, gathers all ranks' records,
 * and checks the per-card P2P slot budget.
 *
 * @param store      config store for cross-rank rendezvous
 * @param rankId     local rank id
 * @param devId      local device id
 * @param rankSize   total rank count
 * @param timeoutMs  gather timeout in milliseconds
 * @param logTag     prefix for log messages (e.g. "BM" or "TRANS")
 * @return SM_OK on success, SM_NO_RESOURCES if P2P slots exceeded, SM_ERROR on failure
 */
Result CheckTopologyP2PSlots(StorePtr &store, uint32_t rankId, uint32_t devId, uint32_t rankSize,
                             int64_t timeoutMs, const std::string &logTag);

} // namespace smem
} // namespace ock

#endif // MEMFABRIC_HYBRID_TOPOLOGY_PREFLIGHT_H
