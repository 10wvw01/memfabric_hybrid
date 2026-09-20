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
#include "hybm_vmm_handle_registry.h"

#include "hybm_logger.h"

namespace ock {
namespace mf {

std::mutex VmmHandleRegistry::mutex_;
std::unordered_map<void *, VmmHandleRegistry::Entry> VmmHandleRegistry::map_;

void VmmHandleRegistry::Register(void *addr, aclrtDrvMemHandle handle, uint32_t deviceId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    map_[addr] = {handle, deviceId};
}

bool VmmHandleRegistry::Unregister(void *addr, aclrtDrvMemHandle &handle, uint32_t &deviceId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = map_.find(addr);
    if (it == map_.end()) {
        return false;
    }
    handle = it->second.handle;
    deviceId = it->second.deviceId;
    map_.erase(it);
    return true;
}

int32_t VmmHandleRegistry::ExportShareableHandle(void *addr, uint64_t *shareableHandle)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = map_.find(addr);
    if (it == map_.end()) {
        return -1;
    }
    uint64_t sh = 0;
    auto ret = DlAclApi::AclrtMemExportToShareableHandle(it->second.handle,
                                                         aclrtMemHandleType::ACL_MEM_HANDLE_TYPE_NONE,
                                                         ACL_RT_VMM_EXPORT_FLAG_DISABLE_PID_VALIDATION, &sh);
    if (ret != 0) {
        BM_LOG_ERROR("VmmHandleRegistry export failed, ret:" << ret << " addr:0x" << std::hex << addr);
        return ret;
    }
    *shareableHandle = sh;
    return 0;
}

} // namespace mf
} // namespace ock
