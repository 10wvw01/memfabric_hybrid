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

#include "hybm_common_include.h"
#include "devmm_svm_gva.h"
#include "dl_api.h"
#include "dl_acl_api.h"
#include "dl_hal_api.h"
#include "hybm_cmd.h"
#include "hybm_gva.h"
#include <limits>

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

Result CreateHalMemory(uint32_t driverDeviceId, size_t size, drv_mem_handle_t **handle)
{
    const bool is310P = DlAclApi::GetAscendSocType() == AscendSocType::ASCEND_310P;
    drv_mem_prop prop{MEM_DEV_SIDE, driverDeviceId, 0, MEM_HUGE_PAGE_TYPE, is310P ? MEM_P2P_DDR_TYPE : MEM_HBM_TYPE, 0};
    auto ret = DlHalApi::HalMemCreate(handle, size, &prop, 0);
    if (is310P && ret == HAL_OUT_OF_MEMORY_ERROR) {
        BM_LOG_INFO("310P P2P_DDR分配不足，改用DDR driver:" << driverDeviceId << " size:" << size);
        prop.mem_type = MEM_DDR_TYPE;
        ret = DlHalApi::HalMemCreate(handle, size, &prop, 0);
    }
    if (ret != BM_OK || (is310P && *handle == nullptr)) {
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

Result AllocateHalVmm(uint32_t driverDeviceId, size_t size, void *expected, HalVmmBuffer &buffer)
{
    auto ret = ReserveHalVmm(size, expected, buffer);
    if (ret != BM_OK) {
        return ret;
    }
    ret = CreateHalMemory(driverDeviceId, buffer.size, &buffer.handle);
    if (ret == BM_OK) {
        ret = MapHalVmm(buffer, driverDeviceId);
    }
    if (ret != BM_OK) {
        (void)ReleaseHalVmm(buffer);
    }
    return ret;
}

namespace {
int32_t initedLogicDeviceId = -1;
} // namespace

int32_t HybmGetInitedLogicDeviceId()
{
    return initedLogicDeviceId; // logicDeviceId
}

int32_t HybmModernInitMetaGva(uint32_t driverDeviceId, void **globalMemoryBase, size_t allocSize, void **allocHandle)
{
    const bool is310P = DlAclApi::GetAscendSocType() == AscendSocType::ASCEND_310P;
    const uint64_t reserveBase = is310P ? HYBM_DEVICE_META_ADDR : SVM_END_ADDR - GB;
    const size_t reserveSize = is310P ? allocSize : GB;
    void *address = nullptr;
    auto ret = DlHalApi::HalMemAddressReserve(&address, reserveSize, 0, reinterpret_cast<void *>(reserveBase), 0);
    if (ret != BM_OK || address != reinterpret_cast<void *>(reserveBase)) {
        BM_LOG_ERROR("元数据地址预留失败 driver:" << driverDeviceId << " expected:" << reserveBase
                                                  << " actual:" << address << " ret:" << ret);
        if (ret == BM_OK && address != nullptr) {
            (void)DlHalApi::HalMemAddressFree(address);
        }
        return BM_ERROR;
    }
    drv_mem_handle_t *handle = nullptr;
    ret = CreateHalMemory(driverDeviceId, allocSize, &handle);
    bool mapped = false;
    if (ret == BM_OK) {
        ret = DlHalApi::HalMemMap(reinterpret_cast<void *>(HYBM_DEVICE_META_ADDR), allocSize, 0, handle, 0);
        mapped = ret == BM_OK;
    }
    if (mapped && is310P) {
        ret = SetHalVmmAccess(reinterpret_cast<void *>(HYBM_DEVICE_META_ADDR), allocSize, driverDeviceId);
    }
    if (ret != BM_OK) {
        BM_LOG_ERROR("元数据内存初始化失败 driver:" << driverDeviceId << " size:" << allocSize << " ret:" << ret);
        if (mapped) {
            (void)DlHalApi::HalMemUnmap(reinterpret_cast<void *>(HYBM_DEVICE_META_ADDR));
        }
        if (handle != nullptr) {
            (void)DlHalApi::HalMemRelease(handle);
        }
        (void)DlHalApi::HalMemAddressFree(address);
        return ret;
    }
    *globalMemoryBase = address;
    *allocHandle = handle;
    return BM_OK;
}

int32_t HybmLegacyInitMetaGva(void **globalMemoryBase, size_t allocSize, uint64_t flags){
    auto ret = drv::HalGvaReserveMemory((uint64_t *)globalMemoryBase, allocSize, initedLogicDeviceId, flags);
    if (ret != 0 || reinterpret_cast<uint64_t>(*globalMemoryBase) != (SVM_END_ADDR - GB)) {
        if (ret == 0 && *globalMemoryBase != nullptr) {
            (void)drv::HalGvaUnreserveMemory((uint64_t)(*globalMemoryBase));
        }
        BM_LOG_ERROR("initialize meta memory failed: " << ret << " size:0x" << std::hex << allocSize << " flag:0x"
                                                       << flags << " ret_addr:" << *globalMemoryBase);
        return BM_ERROR;
    }

    ret = drv::HalGvaAlloc(HYBM_DEVICE_META_ADDR, HYBM_DEVICE_INFO_SIZE, 0);
    if (ret != BM_OK) {
        (void)drv::HalGvaUnreserveMemory((uint64_t)(*globalMemoryBase));
        BM_LOG_ERROR("HalGvaAlloc hybm meta memory failed: " << ret);
        return BM_MALLOC_FAILED;
    }
    return BM_OK;
}

int32_t hybm_init_hbm_gva(uint16_t deviceId, uint64_t flags, uint64_t &baseAddress, AscendSocType socType,
                          void **allocHandle)
{
#if !defined(ASCEND_NPU)
    return BM_OK;
#else
    initedLogicDeviceId = -1;
    DlAclApi::RtGetLogicDevIdByUserDevId(deviceId, &initedLogicDeviceId);
    if (initedLogicDeviceId < 0) {
        BM_LOG_ERROR("RtGetLogicDevIdByUserDevId failed, deviceId: " << deviceId
                                                                     << " logicDeviceId: " << initedLogicDeviceId);
        return BM_ERROR;
    }
    BM_LOG_INFO("Success get deviceId: " << deviceId << ", logicDeviceId: " << initedLogicDeviceId
                                         << " sco:" << (int)socType << " ver:" << HybmGvaVersion());
    auto ret = DlAclApi::AclrtSetDevice(deviceId);
    if (ret != BM_OK) {
        BM_LOG_ERROR("set device id to be " << deviceId << " failed: " << ret);
        return BM_ERROR;
    }
    drv::HybmInitialize(initedLogicDeviceId, DlHalApi::GetFd());

    if ((flags & HYBM_FLAG_INIT_SHMEM_META) == 0) {
        BM_LOG_DEBUG("skip init shm meta space:" << flags);
        baseAddress = 0;
        return BM_OK;
    } else {
        BM_LOG_DEBUG("restore init flag");
        flags &= ~HYBM_FLAG_INIT_SHMEM_META;
    }

    void *globalMemoryBase = nullptr;
    size_t allocSize = HYBM_DEVICE_INFO_SIZE; // 申请meta空间
    if (socType == AscendSocType::ASCEND_310P || socType == AscendSocType::ASCEND_950 ||
        HybmGetGvaVersion() == HYBM_GVA_V4) {
        ret = HybmModernInitMetaGva(initedLogicDeviceId, &globalMemoryBase, allocSize, allocHandle);
    } else {
        ret = HybmLegacyInitMetaGva(&globalMemoryBase, allocSize, flags);
    }
    if (ret != BM_OK) {
        BM_LOG_ERROR("hybm init meta gva failed: " << ret);
        return BM_ERROR;
    }
    baseAddress = reinterpret_cast<uint64_t>(globalMemoryBase);
    return BM_OK;
#endif
}

} // namespace mf
} // namespace ock
