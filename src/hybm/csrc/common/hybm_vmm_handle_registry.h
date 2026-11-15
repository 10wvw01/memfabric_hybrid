#ifndef MEMFABRIC_HYBRID_VMM_HANDLE_REGISTRY_H
#define MEMFABRIC_HYBRID_VMM_HANDLE_REGISTRY_H

#include <mutex>
#include <unordered_map>
#include "dl_acl_api.h"
#include "hybm_hal_vmm.h"

namespace ock {
namespace mf {

class VmmHandleRegistry {
public:
    struct Entry {
        HalVmmBuffer buffer;
        uint32_t deviceId{0};
        bool useHal{false};
    };

    static void Register(void *addr, aclrtDrvMemHandle handle, uint32_t deviceId);
    static void RegisterHal(const HalVmmBuffer &buffer, uint32_t deviceId);
    static Result Release(void *addr);

    // 查询、导出和释放共用同一把锁，导出过程中物理句柄保持有效。
    // -1仅表示地址未注册；HAL已注册地址的导出失败返回BM_DL_FUNCTION_FAILED。
    static int32_t ExportShareableHandle(void *addr, uint64_t *shareableHandle, size_t size = 0);

private:
    static std::mutex mutex_;
    static std::unordered_map<void *, Entry> map_;
};

} // namespace mf
} // namespace ock
#endif
