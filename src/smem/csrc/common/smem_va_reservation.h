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
#ifndef SMEM_VA_RESERVATION_H
#define SMEM_VA_RESERVATION_H

#include <algorithm>
#include <cstring>
#include "hybm_big_mem.h"
#include "smem_net_group_engine.h"

namespace ock {
namespace smem {
struct SmemVaReservationContext {
    SmemGroupEnginePtr group;
    uint32_t rankCount;
};

inline int32_t CoordinateVaReservation(hybm_va_reservation *proposal, void *opaque)
{
    auto &context = *static_cast<SmemVaReservationContext *>(opaque);
    std::vector<hybm_va_reservation> all(context.rankCount);
    auto ret = context.group->GroupAllGather(reinterpret_cast<const char *>(proposal), sizeof(*proposal),
                                            reinterpret_cast<char *>(all.data()), all.size() * sizeof(*proposal));
    if (ret != SM_OK) {
        SM_LOG_ERROR("VA reservation all-gather failed, phase:" << proposal->phase << " ret:" << ret);
        return ret;
    }
    const auto localAddress = proposal->address;
    for (const auto &peer : all) {
        if (peer.size != proposal->size || peer.phase != proposal->phase ||
            (proposal->phase == 1 && peer.address != localAddress)) {
            SM_LOG_ERROR("Inconsistent VA reservation, phase:" << proposal->phase << " size:" << proposal->size);
            return SM_ERROR;
        }
        proposal->address = std::max(proposal->address, peer.address);
        if (peer.result != SM_OK) {
            proposal->result = SM_ERROR;
        }
    }
    return SM_OK;
}

struct SmemDynamicVaContext {
    StorePtr store;
    uint32_t windowIndex{0};
};

inline int32_t CoordinateDynamicVa(hybm_va_reservation *proposal, void *opaque)
{
    auto &context = *static_cast<SmemDynamicVaContext *>(opaque);
    if (proposal->result != SM_OK) {
        SM_LOG_ERROR("Dynamic pool VA reservation failed, phase:" << proposal->phase
                                                                 << " address:" << proposal->address);
        return SM_ERROR;
    }
    if (proposal->phase != 0) {
        context.windowIndex += proposal->phase == 1;
        return SM_OK;
    }
    const std::string key = "va_window_" + std::to_string(context.windowIndex);
    const auto *begin = reinterpret_cast<const uint8_t *>(proposal);
    std::vector<uint8_t> local(begin, begin + sizeof(*proposal)), previous;
    auto ret = context.store->Cas(key, std::vector<uint8_t>{}, local, previous);
    if (ret == SUCCESS) {
        return SM_OK;
    }
    if (ret != RESTORE || previous.size() != sizeof(*proposal)) {
        SM_LOG_ERROR("Publish dynamic pool VA failed, key:" << key << " ret:" << ret);
        return SM_ERROR;
    }
    hybm_va_reservation existing{};
    std::memcpy(&existing, previous.data(), sizeof(existing));
    if (existing.size != proposal->size || existing.phase != 0) {
        SM_LOG_ERROR("Dynamic pool VA configuration mismatch, key:" << key);
        return SM_ERROR;
    }
    proposal->address = existing.address;
    return SM_OK;
}

inline int32_t ReserveDynamicMemory(hybm_entity_t entity, StorePtr store, uint32_t rankCount, uint32_t flags)
{
    if (rankCount == 1) {
        return hybm_reserve_mem_space(entity, flags);
    }
    SmemDynamicVaContext context{store};
    return hybm_reserve_mem_space_coordinated(entity, CoordinateDynamicVa, &context, flags);
}

inline int32_t ReserveCoordinatedMemory(hybm_entity_t entity, SmemGroupEnginePtr group,
                                        uint32_t rankCount, uint32_t flags)
{
    SmemVaReservationContext context{group, rankCount};
    return hybm_reserve_mem_space_coordinated(entity, CoordinateVaReservation, &context, flags);
}
} // namespace smem
} // namespace ock
#endif
