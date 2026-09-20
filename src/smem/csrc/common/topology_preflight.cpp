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
#include "topology_preflight.h"

#include <chrono>
#include <cstring>
#include <map>
#include <unordered_map>

#include "smem_logger.h"

namespace ock {
namespace smem {

static constexpr uint32_t P2P_SLOTS_PER_CARD = 8U;
static const std::string TOPO_PREFIX = "TOPO_";

Result CheckTopologyP2PSlots(StorePtr &store, uint32_t rankId, uint32_t devId, uint32_t rankSize,
                             int64_t timeoutMs, const std::string &logTag)
{
    if (rankSize <= 1U) {
        return SM_OK;
    }

    /* publish local topology record */
    std::string topoKey = TOPO_PREFIX + std::to_string(rankId);
    std::string topoVal = std::to_string(rankId) + ":" + std::to_string(devId);
    auto ret = store->Set(topoKey, std::vector<uint8_t>(topoVal.begin(), topoVal.end()));
    if (ret != 0) {
        SM_LOG_ERROR("[" << logTag << "] Set topology key failed, ret:" << ret << " rank:" << rankId
                         << " key:" << topoKey);
        return SM_ERROR;
    }

    /* gather all ranks' topology records */
    std::unordered_map<std::string, std::string> topoMap;
    auto gatherStart = std::chrono::steady_clock::now();
    while (true) {
        topoMap.clear();
        ret = store->PrefixGet(TOPO_PREFIX, topoMap);
        if (ret != 0) {
            SM_LOG_ERROR("[" << logTag << "] PrefixGet topology failed, ret:" << ret << " rank:" << rankId);
            return SM_ERROR;
        }
        if (topoMap.size() >= rankSize) {
            break;
        }
        auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - gatherStart)
                .count();
        if (elapsed >= timeoutMs) {
            SM_LOG_ERROR("[" << logTag << "] topology gather timeout:" << elapsed << "ms rank:" << rankId
                             << " got:" << topoMap.size() << "/" << rankSize);
            return SM_ERROR;
        }
        usleep(100 * 1000);
    }

    /* parse records */
    std::vector<std::pair<uint32_t, uint32_t>> rankTopos;
    for (const auto &kv : topoMap) {
        uint32_t r = 0;
        uint32_t d = 0;
        if (sscanf(kv.second.c_str(), "%u:%u", &r, &d) != 2) {
            SM_LOG_ERROR("[" << logTag << "] parse topology record failed:" << kv.first << " val:" << kv.second);
            continue;
        }
        rankTopos.emplace_back(r, d);
    }
    if (rankTopos.size() < rankSize) {
        SM_LOG_ERROR("[" << logTag << "] topology records incomplete:" << rankTopos.size() << "/" << rankSize
                         << " rank:" << rankId);
        return SM_ERROR;
    }

    /* check P2P slot budget: count cross-card inbound slots per card.
     * For card C with r ranks, each rank imports from (totalRanks - r) ranks on
     * other cards, so inbound slots = r * (totalRanks - r). O(n) via bucketing. */
    std::map<uint32_t, uint32_t> ranksPerCard;
    for (const auto &topo : rankTopos) {
        ranksPerCard[topo.second / 2U]++;
    }

    bool feasible = true;
    for (const auto &card : ranksPerCard) {
        uint32_t required = card.second * (static_cast<uint32_t>(rankTopos.size()) - card.second);
        if (required > P2P_SLOTS_PER_CARD) {
            SM_LOG_ERROR("[" << logTag << "] P2P slot budget exceeded: cardId:" << card.first
                             << " ranks:" << card.second << " required:" << required
                             << " limit:" << P2P_SLOTS_PER_CARD
                             << " rank:" << rankId << " worldSize:" << rankSize);
            feasible = false;
        }
    }
    if (!feasible) {
        SM_LOG_ERROR("[" << logTag << "] topology preflight FAILED, rank:" << rankId
                         << " worldSize:" << rankSize);
        return SM_NO_RESOURCES;
    }

    SM_LOG_INFO("[" << logTag << "] topology preflight PASS: rank:" << rankId << " worldSize:" << rankSize
                    << " cross-card slots per card all <= " << P2P_SLOTS_PER_CARD);
    return SM_OK;
}

} // namespace smem
} // namespace ock
