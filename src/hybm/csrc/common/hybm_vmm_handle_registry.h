#ifndef MEMFABRIC_HYBRID_VMM_HANDLE_REGISTRY_H
#define MEMFABRIC_HYBRID_VMM_HANDLE_REGISTRY_H

#include <cstdint>
#include <mutex>
#include <unordered_map>

#include "dl_acl_api.h"

namespace ock {
namespace mf {

/* Registry for VMM-backed device buffer handles.
 * Encapsulates the handle map + mutex so that both the entry layer
 * (hybm_alloc/free_vmm_buffer) and the mm layer (dev_user_legacy_segment)
 * share the same registry without cross-layer extern declarations. */
class VmmHandleRegistry {
public:
    struct Entry {
        aclrtDrvMemHandle handle{nullptr};
        uint32_t deviceId{0};
    };

    /* Register a VMM buffer address → handle mapping. */
    static void Register(void *addr, aclrtDrvMemHandle handle, uint32_t deviceId);

    /* Unregister and retrieve the handle + deviceId for cleanup.
     * Returns false if addr is not found. */
    static bool Unregister(void *addr, aclrtDrvMemHandle &handle, uint32_t &deviceId);

    /* Atomically look up and export a shareable handle for the VMM buffer
     * at addr, under the registry lock. Prevents UAF by ensuring the handle
     * is not freed by another thread during export.
     * Returns 0 on success, -1 if not found, or ACL error code on export failure. */
    static int32_t ExportShareableHandle(void *addr, uint64_t *shareableHandle);

private:
    static std::mutex mutex_;
    static std::unordered_map<void *, Entry> map_;
};

} // namespace mf
} // namespace ock

#endif // MEMFABRIC_HYBRID_VMM_HANDLE_REGISTRY_H
