#include "hybm_vmm_handle_registry.h"
#include "hybm_logger.h"

namespace ock {
namespace mf {

std::mutex VmmHandleRegistry::mutex_;
std::unordered_map<void *, VmmHandleRegistry::Entry> VmmHandleRegistry::map_;

void VmmHandleRegistry::Register(void *addr, aclrtDrvMemHandle handle, uint32_t deviceId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    map_[addr] = {{addr, reinterpret_cast<drv_mem_handle_t *>(handle), 0, true}, deviceId, false};
}

void VmmHandleRegistry::RegisterHal(const HalVmmBuffer &buffer, uint32_t deviceId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    map_[buffer.address] = {buffer, deviceId, true};
}

Result VmmHandleRegistry::Release(void *addr)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = map_.find(addr);
    if (it == map_.end()) {
        BM_LOG_ERROR("VMM地址未注册 address:" << addr);
        return BM_INVALID_PARAM;
    }
    auto &entry = it->second;
    if (entry.useHal) {
        auto ret = ReleaseHalVmm(entry.buffer);
        if (ret != BM_OK) {
            return ret;
        }
    } else {
        auto setRet = DlAclApi::AclrtSetDevice(static_cast<int32_t>(entry.deviceId), true);
        if (setRet != BM_OK) {
            BM_LOG_ERROR("ACL VMM设置本端设备失败 user:" << entry.deviceId << " ret:" << setRet);
        }
        auto unmapRet = DlAclApi::AclrtUnmapMem(addr);
        auto addressRet = DlAclApi::AclrtReleaseMemAddress(addr);
        auto freeRet = DlAclApi::AclrtFreePhysical(reinterpret_cast<aclrtDrvMemHandle>(entry.buffer.handle));
        BM_LOG_INFO("ACL VMM释放 user:" << entry.deviceId << " set:" << setRet << " unmap:" << unmapRet
                                        << " address:" << addressRet << " physical:" << freeRet);
    }
    map_.erase(it);
    return BM_OK;
}

int32_t VmmHandleRegistry::ExportShareableHandle(void *addr, uint64_t *shareableHandle, size_t size)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = map_.find(addr);
    if (it == map_.end()) {
        return -1;
    }
    auto &entry = it->second;
    if (shareableHandle == nullptr || !entry.buffer.mapped || entry.buffer.handle == nullptr ||
        (entry.useHal && size > entry.buffer.size)) {
        BM_LOG_ERROR("VMM导出状态无效 address:" << addr);
        return BM_DL_FUNCTION_FAILED;
    }
    uint64_t shared = 0;
    Result ret;
    if (entry.useHal) {
        ret = DlHalApi::HalMemExportToShareableHandle(entry.buffer.handle, &shared);
        if (ret == BM_OK) {
            ShareHandleAttr attr{SHR_HANDLE_NO_WLIST_ENABLE, {0}};
            ret = DlHalApi::HalMemShareHandleSetAttribute(shared, SHR_HANDLE_ATTR_NO_WLIST_IN_SERVER, attr);
        }
    } else {
        ret = DlAclApi::AclrtMemExportToShareableHandle(reinterpret_cast<aclrtDrvMemHandle>(entry.buffer.handle),
                                                        aclrtMemHandleType::ACL_MEM_HANDLE_TYPE_NONE,
                                                        ACL_RT_VMM_EXPORT_FLAG_DISABLE_PID_VALIDATION, &shared);
        if (ret != BM_OK) {
            BM_LOG_ERROR("ACL VMM导出失败 address:" << addr << " ret:" << ret);
            return ret;
        }
        *shareableHandle = shared;
        return BM_OK;
    }
    if (ret != BM_OK || shared == 0) {
        BM_LOG_ERROR("VMM共享句柄导出失败 address:" << addr << " HAL:" << entry.useHal << " ret:" << ret);
        return BM_DL_FUNCTION_FAILED;
    }
    *shareableHandle = shared;
    BM_LOG_INFO("VMM共享句柄导出 address:" << addr << " HAL:" << entry.useHal << " handle:" << shared);
    return BM_OK;
}

} // namespace mf
} // namespace ock
