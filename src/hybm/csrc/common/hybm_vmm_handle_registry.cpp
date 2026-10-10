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
