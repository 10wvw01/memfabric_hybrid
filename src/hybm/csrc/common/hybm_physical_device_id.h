/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 * You may use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at http://license.coscl.org.cn/MulanPSL2
 */
#ifndef MF_HYBM_PHYSICAL_DEVICE_ID_H
#define MF_HYBM_PHYSICAL_DEVICE_ID_H

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <vector>

namespace ock {
namespace mf {

// 310P3/CANN9.1 现场实测: MallocPhysical需要逻辑ID; SetAccess需要物理ID。
// 只对AclrtMemSetAccess的access desc做转换，不改变分配、逻辑奇偶或SDMA协议。
// 环境变量缺失使用identity；已设置但格式非法时返回UINT32_MAX促使ACL拒绝。
inline uint32_t HybmPhysicalDeviceIdOf(uint32_t logicalDeviceId) noexcept
{
    const char *env = std::getenv("ASCEND_RT_VISIBLE_DEVICES");
    if (env == nullptr) {
        return logicalDeviceId;
    }
    constexpr uint32_t invalid = std::numeric_limits<uint32_t>::max();
    if (*env == '\0' || logicalDeviceId == invalid) {
        return invalid;
    }
    std::vector<uint32_t> devices;
    const char *p = env;
    while (*p != '\0') {
        if (devices.size() >= 64 || *p < '0' || *p > '9') {
            return invalid;
        }
        uint32_t value = 0;
        while (*p >= '0' && *p <= '9') {
            const uint32_t digit = static_cast<uint32_t>(*p - '0');
            if (value > (invalid - 1U - digit) / 10U) {
                return invalid;
            }
            value = value * 10U + digit;
            ++p;
        }
        if (std::find(devices.begin(), devices.end(), value) != devices.end()) {
            return invalid;
        }
        devices.push_back(value);
        if (*p == '\0') {
            break;
        }
        if (*p != ',' || *(p + 1) == '\0') {
            return invalid;
        }
        ++p;
    }
    return logicalDeviceId < devices.size() ? devices[logicalDeviceId] : invalid;
}

} // namespace mf
} // namespace ock
#endif // MF_HYBM_PHYSICAL_DEVICE_ID_H
