#ifndef HYBM_HAL_VMM_H
#define HYBM_HAL_VMM_H

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
Result Create310PHalMemory(uint32_t driverDeviceId, size_t size, drv_mem_handle_t **handle);
Result SetHalVmmAccess(void *address, size_t size, uint32_t driverDeviceId);
Result ReserveHalVmm(size_t size, void *expected, HalVmmBuffer &buffer);
Result MapHalVmm(HalVmmBuffer &buffer, uint32_t driverDeviceId);
Result Allocate310PHalVmm(uint32_t driverDeviceId, size_t size, void *expected, HalVmmBuffer &buffer);
Result ReleaseHalVmm(HalVmmBuffer &buffer);

} // namespace mf
} // namespace ock
#endif
