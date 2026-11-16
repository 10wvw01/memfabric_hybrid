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

#ifndef HYBM_GVA_H
#define HYBM_GVA_H

#include <cstdint>
#include "hybm_define.h"
#include "dl_hal_api.h"

namespace ock {
namespace mf {

struct HalVmmBuffer {
    void *address{nullptr};
    drv_mem_handle_t *handle{nullptr};
    size_t size{0};
    bool mapped{false};
};

Result GetHalDeviceId(uint32_t userDeviceId, uint32_t &driverDeviceId);
Result CreateHalMemory(uint32_t driverDeviceId, size_t size, drv_mem_handle_t **handle, bool allowDdrFallback = true);
Result SetHalVmmAccess(void *address, size_t size, uint32_t driverDeviceId);
Result ReserveHalVmm(size_t size, void *expected, HalVmmBuffer &buffer);
Result MapHalVmm(HalVmmBuffer &buffer, uint32_t driverDeviceId);
Result AllocateHalVmm(uint32_t driverDeviceId, size_t size, void *expected, HalVmmBuffer &buffer);
Result ReleaseHalVmm(HalVmmBuffer &buffer);

int32_t HybmGetInitedLogicDeviceId();
int32_t hybm_init_hbm_gva(uint16_t deviceId, uint64_t flags, uint64_t &baseAddress, AscendSocType socType,
                          void **allocHandle);

} // namespace mf
} // namespace ock

#endif
