#include "hybm_hal_vmm.h"

#include <limits>
#include "dl_acl_api.h"
#include "hybm_define.h"
#include "hybm_logger.h"

namespace ock {
namespace mf {

Result GetHalDeviceId(uint32_t userDeviceId, uint32_t &driverDeviceId)
{
    int32_t logical = -1;
    auto ret = DlAclApi::RtGetLogicDevIdByUserDevId(static_cast<int32_t>(userDeviceId), &logical);
    if (ret != BM_OK || logical < 0) {
        BM_LOG_ERROR("HAL设备编号转换失败 user:" << userDeviceId << " driver:" << logical << " ret:" << ret);
        return BM_DL_FUNCTION_FAILED;
    }
    driverDeviceId = static_cast<uint32_t>(logical);
    return BM_OK;
}

Result Create310PHalMemory(uint32_t driverDeviceId, size_t size, drv_mem_handle_t **handle)
{
    drv_mem_prop prop{MEM_DEV_SIDE, driverDeviceId, 0, MEM_HUGE_PAGE_TYPE, MEM_P2P_DDR_TYPE, 0};
    auto ret = DlHalApi::HalMemCreate(handle, size, &prop, 0);
    if (ret == HAL_OUT_OF_MEMORY_ERROR) {
        BM_LOG_INFO("310P P2P_DDR分配不足，改用DDR driver:" << driverDeviceId << " size:" << size);
        prop.mem_type = MEM_DDR_TYPE;
        ret = DlHalApi::HalMemCreate(handle, size, &prop, 0);
    }
    if (ret != BM_OK || *handle == nullptr) {
        BM_LOG_ERROR("HalMemCreate失败 driver:" << driverDeviceId << " size:" << size << " ret:" << ret);
        return ret != BM_OK ? ret : BM_DL_FUNCTION_FAILED;
    }
    return BM_OK;
}

Result SetHalVmmAccess(void *address, size_t size, uint32_t driverDeviceId)
{
    drv_mem_access_desc access{};
    access.type = MEM_ACCESS_TYPE_READWRITE;
    access.location.side = MEM_DEV_SIDE;
    access.location.id = driverDeviceId;
    auto ret = DlHalApi::HalMemSetAccess(address, size, &access, 1);
    if (ret != BM_OK) {
        BM_LOG_ERROR("HalMemSetAccess失败 driver:" << driverDeviceId << " address:" << address << " size:" << size
                                                   << " ret:" << ret);
    }
    return ret;
}

Result ReleaseHalVmm(HalVmmBuffer &buffer)
{
    if (buffer.mapped) {
        auto ret = DlHalApi::HalMemUnmap(buffer.address);
        if (ret != BM_OK) {
            BM_LOG_ERROR("HalMemUnmap失败 address:" << buffer.address << " ret:" << ret);
            return ret;
        }
        buffer.mapped = false;
    }
    if (buffer.handle != nullptr) {
        auto ret = DlHalApi::HalMemRelease(buffer.handle);
        if (ret != BM_OK) {
            BM_LOG_ERROR("HalMemRelease失败 handle:" << buffer.handle << " ret:" << ret);
            return ret;
        }
        buffer.handle = nullptr;
    }
    if (buffer.address != nullptr) {
        auto ret = DlHalApi::HalMemAddressFree(buffer.address);
        if (ret != BM_OK) {
            BM_LOG_ERROR("HalMemAddressFree失败 address:" << buffer.address << " ret:" << ret);
            return ret;
        }
        buffer.address = nullptr;
    }
    buffer.size = 0;
    return BM_OK;
}

Result ReserveHalVmm(size_t size, void *expected, HalVmmBuffer &buffer)
{
    if (size == 0 || size > std::numeric_limits<size_t>::max() - (HYBM_LARGE_PAGE_SIZE - 1) ||
        buffer.address != nullptr || buffer.handle != nullptr) {
        BM_LOG_ERROR("HAL VMM预留参数无效 size:" << size);
        return BM_INVALID_PARAM;
    }
    buffer.size = (size + HYBM_LARGE_PAGE_SIZE - 1) & ~(HYBM_LARGE_PAGE_SIZE - 1);
    auto ret = DlHalApi::HalMemAddressReserve(&buffer.address, buffer.size, 0, expected, 0);
    if (ret != BM_OK || buffer.address == nullptr) {
        BM_LOG_ERROR("HalMemAddressReserve失败 size:" << buffer.size << " expected:" << expected << " ret:" << ret);
        buffer.address = nullptr;
        return BM_DL_FUNCTION_FAILED;
    }
    if (expected != nullptr && buffer.address != expected) {
        BM_LOG_ERROR("HAL预留地址不匹配 expected:" << expected << " actual:" << buffer.address);
        (void)ReleaseHalVmm(buffer);
        return BM_ERROR;
    }
    return BM_OK;
}

Result MapHalVmm(HalVmmBuffer &buffer, uint32_t driverDeviceId)
{
    auto ret = DlHalApi::HalMemMap(buffer.address, buffer.size, 0, buffer.handle, 0);
    if (ret != BM_OK) {
        BM_LOG_ERROR("HalMemMap失败 address:" << buffer.address << " ret:" << ret);
        return ret;
    }
    buffer.mapped = true;
    return SetHalVmmAccess(buffer.address, buffer.size, driverDeviceId);
}

Result Allocate310PHalVmm(uint32_t driverDeviceId, size_t size, void *expected, HalVmmBuffer &buffer)
{
    auto ret = ReserveHalVmm(size, expected, buffer);
    if (ret != BM_OK) {
        return ret;
    }
    ret = Create310PHalMemory(driverDeviceId, buffer.size, &buffer.handle);
    if (ret == BM_OK) {
        ret = MapHalVmm(buffer, driverDeviceId);
    }
    if (ret != BM_OK) {
        (void)ReleaseHalVmm(buffer);
    }
    return ret;
}

} // namespace mf
} // namespace ock
