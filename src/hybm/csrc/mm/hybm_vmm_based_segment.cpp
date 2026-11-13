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
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <bitset>

#include "hybm_ex_info_transfer.h"
#include "hybm_vmm_based_segment.h"
#include "dl_acl_api.h"
#include "hybm_types.h"
#include "hybm_va_manager.h"
#include "hybm_numa_util.h"

using namespace ock::mf;

HybmVmmBasedSegment::~HybmVmmBasedSegment()
{
    if (globalVirtualAddress_ == nullptr && localVirtualAddress_ == nullptr && slices_.empty()) {
        return;
    }

    auto ret = UnReserveMemorySpace();
    if (ret != BM_OK) {
        BM_LOG_WARN("Destructor unable to cleanup, ret:" << ret
                                                         << " gva:" << reinterpret_cast<void *>(globalVirtualAddress_)
                                                         << " va:" << reinterpret_cast<void *>(localVirtualAddress_));
    }
}

Result HybmVmmBasedSegment::ValidateOptions() noexcept
{
    auto checkAlignment = [&](uint64_t size, uint64_t align) -> bool {
        if (size == 0 || (align != 0 && size % align) != 0) {
            return false;
        }
        return true;
    };
    uint64_t align = (options_.segType == HYBM_MST_DRAM) ? GB : HYBM_LARGE_PAGE_SIZE;
    if (options_.size != 0 && !checkAlignment(options_.size, align)) {
        BM_LOG_ERROR("Invalid options segType:" << options_.segType << " size:" << options_.size
                                                << " must algin:" << align);
        return BM_INVALID_PARAM;
    }
    /* 310P ACL 路径不走 A5 fabric GVA 的 GB 粒度路由, 物理分配粒度即 2MB 大页 */
    const uint64_t maxAlign = Is310PPath() ? HYBM_LARGE_PAGE_SIZE : GB;
    if (!checkAlignment(options_.maxSize, maxAlign)) {
        BM_LOG_ERROR("Invalid options segType:" << options_.segType << " max size:" << options_.maxSize
                                                << " must align:" << maxAlign);
        return BM_INVALID_PARAM;
    }

    if (UINT64_MAX / options_.maxSize < options_.rankCnt) {
        BM_LOG_ERROR("Validate options error rankCnt(" << options_.rankCnt << ") size(" << options_.maxSize);
        return BM_INVALID_PARAM;
    }

    // check memory pool size upper limit 128TB for 910C
    if (options_.maxSize * options_.rankCnt > HYBM_GVM_MAX_POOL_SIZE && !options_.enable56BitsGva) {
        BM_LOG_ERROR("Memory pool size > 128T. maxSize:" << options_.maxSize << ", rankCnt:" << options_.rankCnt);
        return BM_INVALID_PARAM;
    }
    return BM_OK;
}

Result HybmVmmBasedSegment::ReserveMemorySpace(void **address) noexcept
{
    BM_ASSERT_LOG_AND_RETURN(ValidateOptions() == BM_OK, "Failed to validate options.", BM_INVALID_PARAM);
    BM_ASSERT_LOG_AND_RETURN(globalVirtualAddress_ == nullptr, "Already prepare virtual memory.", BM_NOT_INITIALIZED);
    BM_ASSERT_LOG_AND_RETURN(address != nullptr, "address is nullptr", BM_INVALID_PARAM);
    BM_ASSERT_LOG_AND_RETURN(options_.rankId < options_.rankCnt,
                             "rank(" << options_.rankId << ") but total " << options_.rankCnt, BM_INVALID_PARAM);

    void *base = nullptr;
    totalVirtualSize_ = options_.rankCnt * options_.maxSize;
    auto totalLvaSize = options_.enable56BitsGva ? options_.maxSize : totalVirtualSize_;
    auto mem_type = options_.segType == HYBM_MST_HBM ? HYBM_MEM_TYPE_DEVICE : HYBM_MEM_TYPE_HOST;
    if (Is310PPath()) {
        // Keep small logical pools usable with the driver's GiB remote reservation granularity.
        totalLvaSize = ((totalLvaSize + GB - 1) / GB) * GB;
    }
    auto reserveGvaSize = options_.enable56BitsGva ? totalVirtualSize_ : totalLvaSize;
    auto &manager = HybmVaManager::GetInstance();
    auto gvaInfo = manager.AllocReserveGva(options_.rankId, reserveGvaSize, totalLvaSize,
                                          mem_type, options_.enable56BitsGva);
    BM_ASSERT_LOG_AND_RETURN(gvaInfo.va[HVM_GVA] > 0, "No free VA window, rank:" << options_.rankId, BM_ERROR);
    auto expected = reinterpret_cast<void *>(gvaInfo.va[HVM_DVA]);
    auto ret = DlHalApi::HalMemAddressReserve(&base, totalLvaSize, 0, expected, MEM_RSV_TYPE_REMOTE_MAP);
    if (ret != BM_OK || base != expected) {
        BM_LOG_ERROR("Reserve VA failed, rank:" << options_.rankId << " expected:" << expected
                                               << " returned:" << base << " size:" << totalLvaSize << " ret:" << ret);
        if (ret == BM_OK && base != nullptr) {
            (void)DlHalApi::HalMemAddressFree(base);
        }
        manager.FreeReserveGva(gvaInfo.va[HVM_GVA]);
        return BM_MALLOC_FAILED;
    }
    BM_LOG_INFO("VMM remote window reserved, rank:" << options_.rankId << " gva:" << VaToStr(gvaInfo.va[HVM_GVA])
                                                    << " dva:" << VaToStr(base) << " size:" << totalLvaSize);
    globalVirtualAddress_ = reinterpret_cast<uint8_t *>(gvaInfo.va[HVM_GVA]);
    localVirtualAddress_ = (uint8_t *)base;
    allocatedSize_ = 0UL;
    sliceCount_ = 0;
    *address = globalVirtualAddress_;
    reservedLva_.push_back(localVirtualAddress_);
    return BM_OK;
}

Result HybmVmmBasedSegment::UnReserveMemorySpace() noexcept
{
    BM_LOG_INFO("UnReserveMemorySpace gva:" << reinterpret_cast<void *>(globalVirtualAddress_)
                                            << ", va:" << reinterpret_cast<void *>(localVirtualAddress_));
    Unmap(); // do unmap, release all imported memory

    while (!slices_.empty()) {
        auto slice = slices_.begin()->second.slice;
        auto ret = ReleaseSliceMemory(slice);
        if (ret != BM_OK) {
            BM_LOG_WARN("Unable to ReleaseSliceMemory during unreserve, ret:" << ret << " slice:" << slice);
        }
    }
    while (!registerSlices_.empty()) {
        auto slice = registerSlices_.begin()->second.first.slice;
        ReleaseSliceMemory(slice);
    }

    for (auto lva : reservedLva_) {
        HybmVaManager::GetInstance().FreeReserveLva((uintptr_t)lva, HVM_DVA);
        auto freeRet = DlHalApi::HalMemAddressFree(lva);
        BM_LOG_INFO("free reserved address va:" << lva << " return:" << freeRet);
        if (freeRet != BM_OK) {
            BM_LOG_WARN("Unable to free reserved VA, ret:" << freeRet << " gva:" << globalVirtualAddress_
                                                           << " va:" << lva);
        }
    }
    if (globalVirtualAddress_ != nullptr) {
        HybmVaManager::GetInstance().FreeReserveGva((uintptr_t)globalVirtualAddress_);
    }
    localVirtualAddress_ = nullptr;
    globalVirtualAddress_ = nullptr;
    totalVirtualSize_ = 0UL;
    allocatedSize_ = 0UL;
    reservedLva_.clear();
    return BM_OK;
}

Result HybmVmmBasedSegment::HalMemCreateAdapterFromHost(size_t size, drv_mem_handle_t **handle, drv_mem_prop prop)
{
    Result ret = BM_ERROR;
    if (DlAclApi::GetAscendSocType() == AscendSocType::ASCEND_950 || (options_.flags & HYBM_FLAG_UNRESTRICTED_MEM)) {
        prop = {MEM_HOST_SIDE, 0, 0, MEM_HUGE_PAGE_TYPE, MEM_DDR_TYPE, 0};
        auto start = std::chrono::high_resolution_clock::now();
        ret = DlHalApi::HalMemCreate(handle, size, &prop, 0);
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
        BM_LOG_INFO("Try HalMemCreate ret:" << ret << " dev:" << prop.devid << " spend time:" << duration.count()
                                            << " size:" << size);
    } else {
        const auto bindInfo = HybmNumaUtil::GetNumaBindPolicyInfo(options_.flags);
        if (!bindInfo.valid) {
            BM_LOG_ERROR("Failed to get numa from flag:" << (std::bitset<UINT32_WIDTH>(options_.flags))
                                                         << " start index:" << HYBM_BIND_NUMA_FLAG_INDEX
                                                         << " flag len:" << HYBM_BIND_NUMA_FLAG_LEN);
            return BM_INVALID_PARAM;
        }
        if (bindInfo.policy == NumaBindPolicy::AUTO) {
            prop.side = MEM_HOST_SIDE;
            prop.devid = 0;
        } else if (bindInfo.policy == NumaBindPolicy::MANUAL) {
            prop.devid = bindInfo.numaIndex;
        } else {
            prop.devid = -1;
        }
        auto start = std::chrono::high_resolution_clock::now();
        ret = DlHalApi::HalMemCreate(handle, size, &prop, 0);
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
        BM_LOG_INFO("Try HalMemCreate ret:" << ret << " numa:" << prop.devid << " spend time:" << duration.count()
                                            << " size:" << size);
    }
    return ret;
}

Result HybmVmmBasedSegment::MallocFromHost(size_t size, uint32_t devId, drv_mem_handle_t **handle) noexcept
{
    drv_mem_prop prop{};
    prop = {MEM_HOST_NUMA_SIDE, devId, 0, MEM_GIANT_PAGE_TYPE, MEM_P2P_DDR_TYPE, 0};
    size_t granularity = 0;
    if (DlAclApi::GetAscendSocType() == AscendSocType::ASCEND_950 || // A5当前仅支持huge page
        DlHalApi::HalMemGetAllocationGranularity(&prop, MEM_ALLOC_GRANULARITY_RECOMMENDED, &granularity) != 0) {
        prop.pg_type = MEM_HUGE_PAGE_TYPE;
        BM_LOG_INFO("Not support giant page size change use huge page, memType:" << prop.mem_type);
    }
    int32_t ret = HalMemCreateAdapterFromHost(size, handle, prop);
    if (ret == HAL_OUT_OF_MEMORY_ERROR && prop.pg_type == MEM_GIANT_PAGE_TYPE) {
        BM_LOG_WARN("Try HalMemCreate use 1GB page not successful, ret:" << ret << ", than try 2MB page");
        prop.pg_type = MEM_HUGE_PAGE_TYPE;
        ret = HalMemCreateAdapterFromHost(size, handle, prop);
    }
    if (ret != BM_OK) {
        BM_LOG_ERROR("Try HalMemCreate failed ret:" << ret << " device:" << prop.devid << " spend time:"
                                                    << " size:" << size);
    }
    return ret;
}

Result HybmVmmBasedSegment::MallocFromDevice(size_t size, uint32_t devId, drv_mem_handle_t **handle) noexcept
{
    const bool is310P = Is310PPath();
    drv_mem_prop prop{MEM_DEV_SIDE, devId, 0, MEM_HUGE_PAGE_TYPE,
                      is310P ? MEM_P2P_DDR_TYPE : MEM_HBM_TYPE, 0};
    auto ret = DlHalApi::HalMemCreate(handle, size, &prop, 0);
    if (is310P && ret == HAL_OUT_OF_MEMORY_ERROR) {
        BM_LOG_WARN("HalMemCreate(P2P_DDR) exhausted, retry DDR, devId:"
                    << devId << " size:" << size << " ret:" << ret);
        prop.mem_type = MEM_DDR_TYPE;
        ret = DlHalApi::HalMemCreate(handle, size, &prop, 0);
    }
    if (ret != BM_OK) {
        BM_LOG_ERROR("HalMemCreate failed, devId:" << devId << " size:" << size << " ret:" << ret);
    }
    return ret;
}

Result HybmVmmBasedSegment::MallocEmptySlice(MemSlicePtr &slice) noexcept
{
    auto localVirtualBase =
        localVirtualAddress_ + (options_.enable56BitsGva ? 0 : options_.maxSize * options_.rankId);
    auto globalVirtualBase = globalVirtualAddress_ + options_.maxSize * options_.rankId;
    auto allocAddr = reinterpret_cast<uint64_t>(localVirtualBase + allocatedSize_);
    auto gva = reinterpret_cast<uint64_t>(globalVirtualBase + allocatedSize_);
    auto memType = options_.segType == HYBM_MST_HBM ? HYBM_MEM_TYPE_DEVICE : HYBM_MEM_TYPE_HOST;
    slice = std::make_shared<MemSlice>(sliceCount_++, memType, MEM_PT_TYPE_GVM, gva, allocAddr, 0);
    slices_.emplace(slice->index_, MemSliceStatus(slice, nullptr));

    HostSdmaExportInfo info;
    std::string exInfo;
    info.devicePhyId = devicePhyId_;
    info.deviceUserId = static_cast<uint32_t>(deviceId_);
    info.magic = (options_.segType == HYBM_MST_DRAM) ? VMM_BASE_DRAM_SLICE_EXPORT_INFO_MAGIC
                                                     : VMM_BASE_HBM_SLICE_EXPORT_INFO_MAGIC;
    info.version = EXPORT_INFO_VERSION;
    info.gva = slice->vAddress_;
    info.deviceVa = slice->vAddress_;
    info.sliceIndex = static_cast<uint32_t>(slice->index_);
    info.rankId = options_.rankId;
    info.size = slice->size_;
    info.sdid = sdid_;
    info.serverId = serverId_;
    info.superPodId = superPodId_;
    info.physicalChipId = physicalChipId_;
    auto ret = LiteralExInfoTranslater<HostSdmaExportInfo>{}.Serialize(info, exInfo);
    if (ret != BM_OK) {
        BM_LOG_ERROR("export info failed: " << ret);
        return BM_ERROR;
    }
    BM_LOG_INFO("Success to export vmm segment info rank:"
                << info.rankId << " superPodId:" << info.superPodId << " serverId:" << info.serverId
                << " devId:" << info.devicePhyId << " segType:" << options_.segType << " size:" << info.size);
    exportMap_[slice->index_] = exInfo;
    return BM_OK;
}

// HAL uses driver logical device ids, including when ACL visible devices are renumbered.
static Result FillHalAccessDesc(drv_mem_access_desc &desc, uint32_t userDevId, uint32_t rankId)
{
    int32_t logicDevId = -1;
    auto ret = DlAclApi::RtGetLogicDevIdByUserDevId(static_cast<int32_t>(userDevId), &logicDevId);
    if (ret != BM_OK || logicDevId < 0) {
        BM_LOG_ERROR("Translate HAL access device failed, userDevId:" << userDevId << " rank:" << rankId
                                                                 << " ret:" << ret << " logicDevId:" << logicDevId);
        return BM_DL_FUNCTION_FAILED;
    }
    desc.type = MEM_ACCESS_TYPE_READWRITE;
    desc.location.side = MEM_DEV_SIDE;
    desc.location.id = static_cast<uint32_t>(logicDevId);
    return BM_OK;
}

Result HybmVmmBasedSegment::AllocLocalMemory(uint64_t size, MemSlicePtr &slice) noexcept
{
    if ((size % HYBM_LARGE_PAGE_SIZE) != 0UL || size + allocatedSize_ > options_.maxSize) {
        BM_LOG_ERROR("invalid allocate memory size : " << size << ", now used " << allocatedSize_ << " of "
                                                       << options_.maxSize);
        return BM_INVALID_PARAM;
    }

    BM_ASSERT_LOG_AND_RETURN(localVirtualAddress_ != nullptr, "localVirtualAddress_ is nullptr", BM_NOT_INITIALIZED);
    if (size == 0) {
        return MallocEmptySlice(slice);
    }
    auto localVirtualBase =
        localVirtualAddress_ + (options_.enable56BitsGva ? 0 : options_.maxSize * options_.rankId);
    auto globalVirtualBase = globalVirtualAddress_ + options_.maxSize * options_.rankId;
    auto allocAddr = reinterpret_cast<uint64_t>(localVirtualBase + allocatedSize_);
    auto gva = reinterpret_cast<uint64_t>(globalVirtualBase + allocatedSize_);
    drv_mem_handle_t *handle = nullptr;
    Result ret = BM_OK;
    switch (options_.segType) {
        case HYBM_MST_HBM:
            ret = MallocFromDevice(size, logicDeviceId_, &handle);
            break;
        case HYBM_MST_DRAM:
            ret = MallocFromHost(size, logicDeviceId_, &handle);
            break;
        default:
            BM_LOG_ERROR("invalid seg type: " << options_.segType);
            return BM_INVALID_PARAM;
    }
    BM_VALIDATE_RETURN(ret == BM_OK,
                       "MallocFromDevice failed: " << ret << " segType:" << options_.segType
                                                   << " devId:" << logicDeviceId_ << " size:" << size,
                       BM_DL_FUNCTION_FAILED);

    allocatedSize_ += size;
    auto memType = options_.segType == HYBM_MST_HBM ? HYBM_MEM_TYPE_DEVICE : HYBM_MEM_TYPE_HOST;
    slice = std::make_shared<MemSlice>(sliceCount_++, memType, MEM_PT_TYPE_GVM, gva, allocAddr, size);
    BM_ASSERT_LOG_AND_RETURN(slice != nullptr, "slice is nullptr", BM_MALLOC_FAILED);
    slices_.emplace(slice->index_, MemSliceStatus(slice, reinterpret_cast<void *>(handle)));
    auto type = options_.segType == HYBM_MST_HBM ? HYBM_MEM_TYPE_DEVICE : HYBM_MEM_TYPE_HOST;
    ret =
        HybmVaManager::GetInstance().AddVaInfo({gva, slice->vAddress_, slice->vAddress_, size, type}, options_.rankId);
    if (ret != 0) {
        BM_LOG_ERROR("AddVaInfo failed, size: " << size << " ret: " << ret);
        ReleaseHandle(handle);
        slices_.erase(slice->index_);
        return ret;
    }
    MemShareHandle sHandle = {};
    ret = ExportInner(slice, sHandle);
    if (ret != BM_OK) {
        BM_LOG_ERROR("export failed: " << ret);
        ReleaseHandle(handle);
        slices_.erase(slice->index_);
        return BM_ERROR;
    }

    void *mapHandle = reinterpret_cast<void *>(handle);
    if (options_.segType == HYBM_MST_DRAM && options_.shared && (options_.flags & HYBM_FLAG_DRAM_MAP_HOST_VA) == 0) {
        if (!Is310PPath()) {
            drv_mem_handle_t *dHandle = handle;
            ret = DlHalApi::HalMemImport(MEM_HANDLE_TYPE_FABRIC, &sHandle, logicDeviceId_, &dHandle);
            BM_VALIDATE_RETURN(ret == BM_OK, "HalMemImport memory failed:" << ret, BM_ERROR);
            mapHandle = reinterpret_cast<void *>(dHandle);
        }
    }

    ret = DlHalApi::HalMemMap(reinterpret_cast<void *>(allocAddr), size, 0,
                              reinterpret_cast<drv_mem_handle_t *>(mapHandle), 0);
    if (ret != BM_OK) {
        BM_LOG_ERROR("MapMem failed, ret: " << ret << " addr: 0x" << std::hex << allocAddr << " size: " << size
                                            << " segType: " << options_.segType);
        ReleaseHandle(reinterpret_cast<drv_mem_handle_t *>(mapHandle));
        slices_.erase(slice->index_);
        return BM_DL_FUNCTION_FAILED;
    }
    if (Is310PPath() && options_.segType == HYBM_MST_HBM) {
        /* 310P3: SetAccess(RW) on local HBM for all peer devices (cross-card).
         * Same-card peers share the P2P domain and don't need SetAccess.
         * deviceId equals rankId on 310P3 Duo, so rankCnt is the total device count. */
        for (uint32_t dev = 0; dev < options_.rankCnt; dev++) {
            if (dev == static_cast<uint32_t>(deviceId_)) {
                continue;
            }
            if (IsSamePhysicalCard(dev, static_cast<uint32_t>(deviceId_))) {
                continue;
            }
            drv_mem_access_desc desc{};
            /* 换算失败即终止: 回退/跳过=把显性报错变成无声的错误授权 */
            auto faRet = FillHalAccessDesc(desc, dev, options_.rankId);
            if (faRet != BM_OK) {
                (void)DlHalApi::HalMemUnmap(reinterpret_cast<void *>(allocAddr));
                ReleaseHandle(handle);
                slices_.erase(slice->index_);
                return faRet;
            }
            auto saRet = DlHalApi::HalMemSetAccess(reinterpret_cast<void *>(allocAddr), size, &desc, 1);
            if (saRet != BM_OK) {
                BM_LOG_WARN("HalMemSetAccess(RW) dev:" << dev << " failed:" << saRet << " addr:0x" << std::hex
                                                         << allocAddr << std::dec << " segType:" << options_.segType
                                                         << " rankId:" << options_.rankId);
            }
        }
        /* Also set for local device */
        drv_mem_access_desc localDesc{};
        auto faRet = FillHalAccessDesc(localDesc, static_cast<uint32_t>(deviceId_), options_.rankId);
        if (faRet != BM_OK) {
            (void)DlHalApi::HalMemUnmap(reinterpret_cast<void *>(allocAddr));
            ReleaseHandle(handle);
            slices_.erase(slice->index_);
            return faRet;
        }
        auto saRet = DlHalApi::HalMemSetAccess(reinterpret_cast<void *>(allocAddr), size, &localDesc, 1);
        if (saRet != BM_OK) {
            BM_LOG_WARN("HalMemSetAccess(RW) local dev failed:" << saRet << " addr:0x" << std::hex << allocAddr
                                                                  << std::dec << " segType:" << options_.segType
                                                                  << " rankId:" << options_.rankId);
        } else {
            BM_LOG_DEBUG("310P3 SetAccess(RW) for all devices OK, addr:0x" << std::hex << allocAddr);
        }
    }
    if (options_.segType == HYBM_MST_DRAM && options_.shared && (options_.flags & HYBM_FLAG_DRAM_MAP_HOST_VA) != 0) {
        struct drv_mem_access_desc desc[1];
        desc[0].location.side = MEM_DEV_SIDE;
        desc[0].location.id = logicDeviceId_;
        desc[0].type = MEM_ACCESS_TYPE_READWRITE;
        ret = DlHalApi::HalMemSetAccess(reinterpret_cast<void *>(allocAddr), size, desc, 1);
        if (ret != BM_OK) {
            BM_LOG_ERROR("HalMemSetAccess failed: " << ret << " addr:0x" << std::hex << allocAddr
                                                    << " size:" << size << " segType:" << options_.segType);
            /* unmap before releasing handle to avoid dangling mapping */
            DlHalApi::HalMemUnmap(reinterpret_cast<void *>(allocAddr));
            ReleaseHandle(reinterpret_cast<drv_mem_handle_t *>(mapHandle));
            slices_.erase(slice->index_);
            return BM_DL_FUNCTION_FAILED;
        }
    }
    BM_LOG_INFO("alloc mem success, type:" << memType << " addr:" << VaToInfo(allocAddr) << " size:" << size);
    return BM_OK;
}

Result HybmVmmBasedSegment::RegisterMemory(const void *addr, uint64_t size, MemSlicePtr &slice) noexcept
{
    auto ret = RegisterMemCommon(addr, size, slice);
    BM_ASSERT_LOG_AND_RETURN(ret == BM_OK, "ret = " << ret, ret);

    registerSlices_.emplace(slice->index_, std::make_pair(slice, reinterpret_cast<uint64_t>(addr)));
    BM_LOG_INFO("HybmVmmBasedSegment: RegisterMemory success, size: " << size << " addr: " << VaToInfo(addr));
    return BM_OK;
}

Result HybmVmmBasedSegment::ReleaseSliceMemory(const MemSlicePtr &slice) noexcept
{
    BM_VALIDATE_RETURN(slice != nullptr, "input slice is nullptr", BM_INVALID_PARAM);

    auto pos = slices_.find(slice->index_);
    if (pos != slices_.end()) {
        if (pos->second.slice != slice) {
            BM_LOG_ERROR("input slice(magic:" << std::hex << slice->magic_ << ") not match.");
            return BM_INVALID_PARAM;
        }
        auto res = DlHalApi::HalMemUnmap(reinterpret_cast<void *>(slice->vAddress_));
        BM_LOG_INFO("unmap slice(idx:" << slice->index_ << "), size: " << slice->size_ << " return:" << res);

        ReleaseHandle(reinterpret_cast<drv_mem_handle_t *>(pos->second.handle));
        BM_LOG_INFO("release slice(idx:" << slice->index_ << "), size: " << slice->size_);
        HybmVaManager::GetInstance().RemoveOneVaInfo(slice->vAddress_, HVM_DVA);

        slices_.erase(pos);
        return BM_OK;
    }
    auto registerPos = registerSlices_.find(slice->index_);
    if (registerPos != registerSlices_.end()) {
        if (registerPos->second.first.slice != slice) {
            BM_LOG_ERROR("input slice(magic:" << std::hex << slice->magic_ << ") not match.");
            return BM_INVALID_PARAM;
        }
        uint64_t realAddr = registerPos->second.second;
        bool isDevice = slice->GetMemoryType() == HYBM_MEM_TYPE_DEVICE;
        if (!isDevice && ((options_.dataOpType & HYBM_DOP_TYPE_DEVICE_RDMA) != 0U)) {
            auto ret =
                DlHalApi::HalHostUnregisterEx(reinterpret_cast<void *>(realAddr), logicDeviceId_, HOST_MEM_MAP_DEV);
            BM_LOG_INFO("unregister slice(idx:" << slice->index_ << "), size: " << slice->size_ << " return:" << ret);
        }
        HybmVaManager::GetInstance().RemoveOneVaInfo(slice->vAddress_, HVM_HVA);
        registerSlices_.erase(registerPos);
        return BM_OK;
    }

    BM_LOG_ERROR("input slice(idx:" << slice->index_ << ") not exist.");
    return BM_INVALID_PARAM;
}

Result HybmVmmBasedSegment::Export(std::string &exInfo) noexcept
{
    return BM_OK;
}

Result HybmVmmBasedSegment::ExportInner(const MemSlicePtr &slice, MemShareHandle &sHandle) noexcept
{
    auto [gvaInfo, stat] = HybmVaManager::GetInstance().FindAllocByVa(slice->vAddress_, HVM_DVA);
    if (!stat) {
        BM_LOG_ERROR("input device va(" << slice->vAddress_ << ") not match.");
        return BM_INVALID_PARAM;
    }

    HostSdmaExportInfo info;
    std::string exInfo;
    auto pos = slices_.find(slice->index_);
    Result ret = BM_OK;

    if (Is310PPath()) {
        uint64_t shareable = 0U;
        ret = DlHalApi::HalMemExportToShareableHandle(
            reinterpret_cast<drv_mem_handle_t *>(pos->second.handle), &shareable);
        BM_VALIDATE_RETURN(ret == BM_OK, "HAL export failed, rank:" << options_.rankId << " ret:" << ret,
                           BM_DL_FUNCTION_FAILED);
        // Preserve the existing cross-process sharing policy and uint64_t wire handle.
        ShareHandleAttr attr{SHR_HANDLE_NO_WLIST_ENABLE, {0}};
        ret = DlHalApi::HalMemShareHandleSetAttribute(shareable, SHR_HANDLE_ATTR_NO_WLIST_IN_SERVER, attr);
        BM_VALIDATE_RETURN(ret == BM_OK, "HAL handle attribute failed, rank:" << options_.rankId << " ret:" << ret,
                           BM_DL_FUNCTION_FAILED);
        /* Store shareable handle (uint64_t) in the first 8 bytes of MemShareHandle */
        memcpy(info.shareHandle.share_info, &shareable, sizeof(shareable));
    } else {
        ret = DlHalApi::HalMemExport(reinterpret_cast<drv_mem_handle_t *>(pos->second.handle), MEM_HANDLE_TYPE_FABRIC,
                                     0, &info.shareHandle);
        if (ret != 0) {
            BM_LOG_ERROR("HalMemExport(create shm key) failed, ret: " << ret << " sliceIdx: " << slice->index_
                                                                      << " va: 0x" << std::hex << slice->vAddress_
                                                                      << " size: " << slice->size_);
            return BM_DL_FUNCTION_FAILED;
        }

        uint64_t shareable = 0U;
        uint32_t sId;
        ret = DlHalApi::HalMemTransShareableHandle(MEM_HANDLE_TYPE_FABRIC, &info.shareHandle, &sId, &shareable);
        BM_VALIDATE_RETURN(ret == BM_OK, "HalMemTransShareableHandle failed:" << ret, BM_ERROR);
        struct ShareHandleAttr attr = {.enableFlag = SHR_HANDLE_NO_WLIST_ENABLE, .rsv = {0}};
        ret = DlHalApi::HalMemShareHandleSetAttribute(shareable, SHR_HANDLE_ATTR_NO_WLIST_IN_SERVER, attr);
        BM_VALIDATE_RETURN(ret == BM_OK, "HalMemShareHandleSetAttribute failed:" << ret, BM_ERROR);
    }

    info.devicePhyId = devicePhyId_;
    info.deviceUserId = static_cast<uint32_t>(deviceId_);
    info.magic = (options_.segType == HYBM_MST_DRAM) ? VMM_BASE_DRAM_SLICE_EXPORT_INFO_MAGIC
                                                     : VMM_BASE_HBM_SLICE_EXPORT_INFO_MAGIC;
    info.version = EXPORT_INFO_VERSION;
    info.gva = gvaInfo.base.va[HVM_GVA];
    info.deviceVa = gvaInfo.base.va[HVM_DVA];
    info.sliceIndex = static_cast<uint32_t>(slice->index_);
    info.rankId = options_.rankId;
    info.size = slice->size_;
    info.sdid = sdid_;
    info.serverId = serverId_;
    info.superPodId = superPodId_;
    info.physicalChipId = physicalChipId_;
    ret = LiteralExInfoTranslater<HostSdmaExportInfo>{}.Serialize(info, exInfo);
    if (ret != BM_OK) {
        BM_LOG_ERROR("export info failed: " << ret);
        return BM_ERROR;
    }

    BM_LOG_INFO("Success to export vmm segment info rank:"
                << info.rankId << " superPodId:" << info.superPodId << " serverId:" << info.serverId
                << " devId:" << info.devicePhyId << " segType:" << options_.segType << " size:" << info.size);
    exportMap_[slice->index_] = exInfo;
    sHandle = info.shareHandle;
    return BM_OK;
}

Result HybmVmmBasedSegment::Export(const MemSlicePtr &slice, std::string &exInfo) noexcept
{
    BM_ASSERT_LOG_AND_RETURN(slice != nullptr, "slice is nullptr", BM_INVALID_PARAM);
    auto pos = slices_.find(slice->index_);
    BM_VALIDATE_RETURN(pos != slices_.end(), "input slice(idx:" << slice->index_ << ") not exist.", BM_INVALID_PARAM);
    BM_VALIDATE_RETURN(pos->second.slice == slice, "input slice(magic:" << std::hex << slice->magic_ << ") not match.",
                       BM_INVALID_PARAM);

    auto exp = exportMap_.find(slice->index_);
    if (exp != exportMap_.end()) {
        exInfo = exp->second;
        return BM_OK;
    } else {
        return BM_ERROR;
    }
}

Result HybmVmmBasedSegment::GetExportSliceSize(size_t &size) noexcept
{
    size = sizeof(HostSdmaExportInfo);
    return BM_OK;
}

Result HybmVmmBasedSegment::Import(const std::vector<std::string> &allExInfo, void *addresses[]) noexcept
{
    LiteralExInfoTranslater<HostSdmaExportInfo> translator;
    uint64_t exportMagic = (options_.segType == HYBM_MST_DRAM) ? VMM_BASE_DRAM_SLICE_EXPORT_INFO_MAGIC
                                                               : VMM_BASE_HBM_SLICE_EXPORT_INFO_MAGIC;
    std::vector<HostSdmaExportInfo> deserializedInfos;
    Result ret = BM_ERROR;
    for (auto i = 0U; i < allExInfo.size(); i++) {
        HostSdmaExportInfo info{};
        ret = translator.Deserialize(allExInfo[i], info);
        if (ret != 0) {
            BM_LOG_ERROR("deserialize imported info(" << i << ") failed.");
            return BM_INVALID_PARAM;
        }
        if (info.magic != exportMagic || info.magic == ENTITY_EXPORT_INFO_MAGIC) {
            BM_LOG_WARN("import i(" << i << ") rank(" << info.rankId << ") magic(" << info.magic
                                    << ") invalid skip it.");
            continue;
        }

        if (info.magic != exportMagic) {
            BM_LOG_ERROR("import info(" << i << ") magic(" << info.magic << ") invalid.");
            return BM_INVALID_PARAM;
        }
        if (options_.shared && options_.segType == HYBM_MST_HBM && info.rankId != options_.rankId &&
            devicePhyId_ != static_cast<int>(info.devicePhyId) &&
            CanLocalHostReaches(info.superPodId, info.serverId, info.devicePhyId)) {
            if (Is310PPath()) {
                /* 310P3: same-card ctx trick is applied in Mmap; cross-card P2P must be explicitly enabled.
                 * 15_peer_needed measured cross-card must be bidirectionally EnablePeerAccess (one direction fails
                 * with 507899); p3_store_repro/04_vmm_cross only verified host-copy paths can succeed without EPA
                 * — device-side access (AICPU/STARS SQE) without EPA gets devmm page fault
                 * (EX09 4-rank real-machine test: cross-card epoch mail write faults at peer reserved area).
                 * Each rank enables toward all cross-card peers of its imports, naturally forming bidirectionality. */
                if (IsSameCard(info.devicePhyId)) {
                    BM_LOG_INFO("[rank " << options_.rankId << "] 310P3 Import: rank:" << info.rankId
                                         << " sameCard:true (deviceId:" << deviceId_ << "/2 == peer:"
                                         << info.devicePhyId << "/2) — ctx trick will be applied in Mmap");
                } else if (p2pLeases_.find(static_cast<int32_t>(info.devicePhyId)) == p2pLeases_.end()) {
                    auto epRet = EnableRemotePeerAccess(info.devicePhyId);
                    if (epRet != BM_OK) {
                        BM_LOG_ERROR("310P3 cross-card enable peer access failed, ret:" << epRet
                                    << " local_user:" << deviceId_ << " remote_phy:" << info.devicePhyId
                                    << " remoteRank:" << info.rankId); // Root cause: ret/local device/peer rank
                        return epRet;
                    }
                    p2pLeases_.insert(static_cast<int32_t>(info.devicePhyId));
                }
            } else {
                if (IsSameCard(info.physicalChipId)) {
                    BM_LOG_DEBUG("Same card (same physicalChipId:" << info.physicalChipId
                                                                   << "), skip P2P for rank:" << info.rankId);
                } else {
                    if (p2pLeases_.find(static_cast<int32_t>(info.devicePhyId)) == p2pLeases_.end()) {
                        auto ret = EnableRemotePeerAccess(info.devicePhyId);
                        if (ret != BM_OK) {
                            return ret;
                        }
                        p2pLeases_.insert(static_cast<int32_t>(info.devicePhyId));
                    }
                }
            }
        }
        if (addresses != nullptr) {
            addresses[i] = reinterpret_cast<void *>(info.gva);
        }
        if (info.size == 0) {
            continue;
        }
        deserializedInfos.emplace_back(info);
        BM_LOG_INFO("Success to import rank:" << info.rankId << " superPodId:" << info.superPodId
                                              << " serverId:" << info.serverId << " devId:" << info.devicePhyId
                                              << " segType:" << options_.segType << " size:" << info.size);
    }

    try {
        std::copy(deserializedInfos.begin(), deserializedInfos.end(), std::back_inserter(imports_));
    } catch (...) {
        BM_LOG_ERROR("copy failed.");
        return BM_MALLOC_FAILED;
    }
    return BM_OK;
}

uint64_t HybmVmmBasedSegment::ReserveLva(const HostSdmaExportInfo &im)
{
    if (Is310PPath()) {
        /* Device-side access uses GVA directly. All ranks of a pool must reserve the same
         * window and map a peer slice at its exported address. Fixed-range allocation
         * therefore requires the same pool creation order on all ranks. */
        const uint64_t windowBase = reinterpret_cast<uint64_t>(localVirtualAddress_);
        const uint64_t windowEnd = windowBase + totalVirtualSize_;
        if (im.gva < windowBase || im.gva + im.size > windowEnd) {
            BM_LOG_ERROR("310P import gva out of window, rank:" << im.rankId << " gva:" << VaToStr(im.gva)
                                                                << " size:" << im.size
                                                                << " windowBase:" << VaToStr(windowBase)
                                                                << " windowEnd:" << VaToStr(windowEnd));
            return 0;
        }
        return im.gva;
    }
    if (!options_.enable56BitsGva) {
        return im.deviceVa;
    }
    void *lva = reinterpret_cast<void *>(im.deviceVa);
    auto memType = im.magic == HBM_SLICE_EXPORT_INFO_MAGIC ? HYBM_MEM_TYPE_DEVICE : HYBM_MEM_TYPE_HOST;
    auto info = HybmVaManager::GetInstance().AllocReserveLva(options_.rankId, im.size, HVM_DVA, memType);
    auto reservedLva = info.va[HVM_DVA];
    if (reservedLva == 0) {
        BM_LOG_ERROR("AllocReserveLva failed");
        return 0;
    }
    uint64_t flag = MEM_RSV_TYPE_REMOTE_MAP;
    auto ret = DlHalApi::HalMemAddressReserve(&lva, im.size, 0, reinterpret_cast<void *>(reservedLva), flag);
    if (ret != 0 || lva != reinterpret_cast<void *>(reservedLva)) {
        BM_LOG_ERROR("Failed to reserve va local:" << options_.rankId << " remoteRank:" << im.rankId
                                                   << " size:" << im.size << " reservedVa:" << VaToStr(reservedLva)
                                                   << " va:" << lva << " ret:" << ret);
        HybmVaManager::GetInstance().FreeReserveLva(reservedLva, HVM_DVA);
        return 0;
    }
    reservedLva_.push_back(lva);
    return reinterpret_cast<uint64_t>(lva);
}

Result HybmVmmBasedSegment::MmapImport310P(const HostSdmaExportInfo &im, uint64_t lva, void *&mapHandle) noexcept
{
    uint64_t shareable = 0;
    memcpy(&shareable, im.shareHandle.share_info, sizeof(shareable));
    bool sameCard = IsSameCard(im.devicePhyId);
    BM_LOG_INFO("[rank " << options_.rankId << "] 310P3 import step1: rank:" << im.rankId
                         << " devId:" << std::dec << deviceId_
                         << " sameCard:" << sameCard << " myDevicePhyId:" << devicePhyId_
                         << " peerDevicePhyId:" << im.devicePhyId);
    /* Check P2P accessibility before import */
    {
        int32_t canAccess = -1;
        auto canRet = DlAclApi::AclrtDeviceCanAccessPeer(&canAccess, deviceId_, static_cast<int32_t>(im.devicePhyId));
        BM_LOG_INFO("[rank " << options_.rankId << "] 310P3 CanAccessPeer: ret:" << canRet
                             << " canAccess:" << canAccess << " local:" << deviceId_
                             << " peer:" << im.devicePhyId << " sameCard:" << sameCard);
    }
    drv_mem_handle_t *halHandle = nullptr;
    auto ret = DlHalApi::HalMemImportFromShareableHandle(shareable, logicDeviceId_, &halHandle);
    BM_LOG_INFO("[rank " << options_.rankId << "] 310P3 import step2: HalMemImportFromShareableHandle"
                         << " ret:" << ret << " handle:" << halHandle << " rank:" << im.rankId
                         << " deviceId:" << deviceId_);
    if (ret != BM_OK || halHandle == nullptr) {
        BM_LOG_ERROR("[rank " << options_.rankId << "] 310P3 import failed:" << ret << " rank:" << im.rankId
                              << " sameCard:" << sameCard
                              << " deviceId:" << deviceId_ << " segType:" << options_.segType);
        /* do NOT pre-release lva here: it stays in reservedLva_ and will be
         * released by UnReserveMemorySpace. Pre-releasing causes double-free. */
        return BM_ERROR;
    }
    mapHandle = halHandle;
    ret = DlHalApi::HalMemMap(reinterpret_cast<void *>(lva), im.size, 0,
                              reinterpret_cast<drv_mem_handle_t *>(mapHandle), 0);
    BM_LOG_INFO("[rank " << options_.rankId << "] 310P3 import step3: HalMemMap ret:" << ret
                         << " rank:" << im.rankId << " lva:" << VaToStr(lva) << " size:" << im.size);
    if (ret != BM_OK) {
        BM_LOG_ERROR("[rank " << options_.rankId << "] HalMemMap failed:" << ret << " rank:" << im.rankId
                              << " va:" << VaToStr(lva) << " size:" << im.size
                              << " segType:" << options_.segType);
        ReleaseHandle(reinterpret_cast<drv_mem_handle_t *>(mapHandle));
        return BM_ERROR;
    }
    /* SetAccess on imported VA for local device */
    drv_mem_access_desc impDesc{};
    auto faRet = FillHalAccessDesc(impDesc, static_cast<uint32_t>(deviceId_), options_.rankId);
    if (faRet != BM_OK) {
        BM_LOG_ERROR("FillHalAccessDesc(imported) failed, ret:" << faRet << " rankId:" << options_.rankId
                                                                << " remoteRank:" << im.rankId
                                                                << " va:" << VaToStr(lva));
        (void)DlHalApi::HalMemUnmap(reinterpret_cast<void *>(lva));
        ReleaseHandle(reinterpret_cast<drv_mem_handle_t *>(mapHandle));
        return BM_ERROR;
    }
    auto saRet = DlHalApi::HalMemSetAccess(reinterpret_cast<void *>(lva), im.size, &impDesc, 1);
    if (saRet != BM_OK) {
        BM_LOG_WARN("[rank " << options_.rankId << "] HalMemSetAccess on imported VA failed:" << saRet
                             << " rank:" << im.rankId << " va:" << VaToStr(lva) << " size:" << im.size
                             << " segType:" << options_.segType);
    }
    return BM_OK;
}

Result HybmVmmBasedSegment::Mmap() noexcept
{
    if (imports_.empty()) {
        return BM_OK;
    }
    if (Is310PPath()) {
        // The listener thread needs its own local ACL context.
        DlAclApi::AclrtSetDevice(deviceId_, true);

        auto sameCardFirst = [this](const HostSdmaExportInfo &a, const HostSdmaExportInfo &b) {
            bool aSame = IsSameCard(a.devicePhyId);
            bool bSame = IsSameCard(b.devicePhyId);
            if (aSame != bSame) {
                return aSame; /* same-card first */
            }
            return a.rankId < b.rankId; /* then by rankId */
        };
        std::stable_sort(imports_.begin(), imports_.end(), sameCardFirst);
    }

    for (auto &im : imports_) {
        if (im.rankId == options_.rankId || im.magic == ENTITY_EXPORT_INFO_MAGIC) {
            continue;
        }

        if (im.segmentType != SEGMENT_TYPE_VMM) {
            BM_LOG_ERROR("mmap failed for invalid segment type in vmm:" << im.segmentType);
            return BM_ERROR;
        }

        if (im.size == 0) {
            BM_LOG_ERROR("mmap failed for invalid size 0");
            return BM_ERROR;
        }

        if (mappedGvaMem_.find(im.gva) != mappedGvaMem_.end()) {
            BM_LOG_INFO("remote slice on rank(" << im.rankId << ") has maped gva:" << VaToStr(im.gva));
            continue;
        }

        if (!options_.shared || !CanSdmaReaches(im.superPodId, im.serverId, im.devicePhyId)) {
            // A2 device_rdma 跨机访问适配
            // AddVaInfoFromExternal 只需要记录GVA，因为device_rdma不需要访问HVM_DVA，所以才值0，
            // 防止copy的时候地址被TransformVa转换，Transport就找不到 Lkey/Rkey
            auto memType = (options_.segType == HYBM_MST_HBM) ? HYBM_MEM_TYPE_DEVICE : HYBM_MEM_TYPE_HOST;
            auto ret = HybmVaManager::GetInstance().AddVaInfoFromExternal({im.gva, 0, 0, im.size, memType},
                                                                          options_.rankId, im.rankId);
            if (ret != BM_OK) {
                BM_LOG_ERROR("AddVaInfoFromExternal failed:" << ret << " gva:" << VaToStr(im.gva)
                                                             << " size:" << im.size);
                return ret;
            }
            continue;
        }

        uint64_t lva = ReserveLva(im);
        BM_ASSERT_LOG_AND_RETURN(lva != 0, "va = " << lva, BM_ERROR);
        BM_LOG_INFO("Try to mmap rank:" << im.rankId << " superPodId:" << im.superPodId << " serverId:" << im.serverId
                                        << " devId:" << im.devicePhyId << " segType:" << options_.segType
                                        << " size:" << im.size << " gva:" << VaToStr(im.gva)
                                        << " dva:" << VaToStr(im.deviceVa) << " va:" << VaToStr(lva));

        void *mapHandle = nullptr;
        Result ret = BM_OK;

        if (Is310PPath()) {
            ret = MmapImport310P(im, lva, mapHandle);
            if (ret != BM_OK) {
                return ret;
            }
        } else {
            drv_mem_handle_t *handle = nullptr;
            ret = DlHalApi::HalMemImport(MEM_HANDLE_TYPE_FABRIC, &im.shareHandle, logicDeviceId_, &handle);
            if (ret != BM_OK) {
                BM_LOG_ERROR("HalMemImport memory failed:" << ret << " local sdid:" << sdid_
                                                           << " remote ssid:" << im.sdid);
                if (options_.enable56BitsGva) {
                    HybmVaManager::GetInstance().FreeReserveLva(lva, HVM_DVA);
                }
                return BM_ERROR;
            }
            mapHandle = reinterpret_cast<void *>(handle);
            ret = DlHalApi::HalMemMap(reinterpret_cast<void *>(lva), im.size, 0,
                                      reinterpret_cast<drv_mem_handle_t *>(mapHandle), 0);
        }

        if (ret != BM_OK) {
            BM_LOG_ERROR("MapMem failed:" << ret << " gva:" << VaToStr(im.gva) << " va:" << VaToStr(lva)
                                          << " dva:" << VaToStr(im.deviceVa) << " size:" << im.size);
            ReleaseHandle(reinterpret_cast<drv_mem_handle_t *>(mapHandle));
            if (options_.enable56BitsGva) {
                HybmVaManager::GetInstance().FreeReserveLva(lva, HVM_DVA);
            }
            return BM_ERROR;
        }

        auto memType = IsHbmSlice(im.magic) ? HYBM_MEM_TYPE_DEVICE : HYBM_MEM_TYPE_HOST;
        ret = HybmVaManager::GetInstance().AddVaInfoFromExternal({im.gva, lva, 0, im.size, memType}, options_.rankId,
                                                                 im.rankId);
        if (ret != BM_OK) {
            DlHalApi::HalMemUnmap(reinterpret_cast<void *>(lva));
            ReleaseHandle(reinterpret_cast<drv_mem_handle_t *>(mapHandle));
            imports_.clear();
            return ret;
        }

        mappedGvaMem_.emplace(im.gva, mapHandle);
    }

    /* Pending cross-card imports will be retried by the post-join hybm_mmap call
     * from the main thread. Do NOT retry or clear here — same-card imports on
     * other ranks may still be in progress in their JoinHandle. */
    imports_.clear();
    return BM_OK;
}

Result HybmVmmBasedSegment::Unmap() noexcept
{
    if (!options_.shared) {
        BM_LOG_INFO("no need to share, skip unmap");
        return BM_OK;
    }

    for (auto it : mappedGvaMem_) {
        auto deviceVa = HybmVaManager::GetInstance().TransformVa(it.first, HVM_GVA, HVM_DVA);
        DlHalApi::HalMemUnmap(reinterpret_cast<void *>(deviceVa));
        DlHalApi::HalMemRelease(reinterpret_cast<drv_mem_handle_t *>(it.second));
        HybmVaManager::GetInstance().RemoveOneVaInfo(it.first);
    }
    mappedGvaMem_.clear();

    ReleaseP2PLeases();
    return BM_OK;
}

Result HybmVmmBasedSegment::RemoveImported(const std::vector<uint32_t> &ranks) noexcept
{
    if (!options_.shared) {
        BM_LOG_INFO("no need to share, skip remove");
        return BM_OK;
    }
    for (auto &rank : ranks) {
        if (rank >= options_.rankCnt) {
            BM_LOG_ERROR("input rank is invalid! rank:" << rank << " rankSize:" << options_.rankCnt);
            return BM_INVALID_PARAM;
        }
    }

    for (auto &rank : ranks) {
        uint64_t gvaLocal = reinterpret_cast<uint64_t>(globalVirtualAddress_) + options_.maxSize * rank;
        auto it = mappedGvaMem_.lower_bound(gvaLocal);
        auto st = it;
        while (it != mappedGvaMem_.end() && (*it).first < gvaLocal + options_.maxSize) {
            auto deviceVa = HybmVaManager::GetInstance().TransformVa((*it).first, HVM_GVA, HVM_DVA);
            DlHalApi::HalMemUnmap(reinterpret_cast<void *>(deviceVa));
            DlHalApi::HalMemRelease(reinterpret_cast<drv_mem_handle_t *>((*it).second));
            HybmVaManager::GetInstance().RemoveOneVaInfo((*it).first);
            it++;
        }

        if (st != it) {
            mappedGvaMem_.erase(st, it);
        }
    }

    // remove imports_ infos for specified ranks
    std::set<uint32_t> removedRanks(ranks.begin(), ranks.end());
    ReleaseP2PLeases(removedRanks);
    imports_.erase(std::remove_if(imports_.begin(), imports_.end(),
                                  [&removedRanks](const HostSdmaExportInfo &info) {
                                      return removedRanks.find(info.rankId) != removedRanks.end();
                                  }),
                   imports_.end());
    return BM_OK;
}

MemSlicePtr HybmVmmBasedSegment::GetMemSlice(hybm_mem_slice_t slice, bool quiet) const noexcept
{
    auto index = MemSlice::GetIndexFrom(slice);
    MemSlicePtr target = nullptr;
    auto pos = slices_.find(index);
    auto registerPos = registerSlices_.find(index);
    if (pos != slices_.end()) {
        target = pos->second.slice;
    } else if (registerPos != registerSlices_.end()) {
        target = registerPos->second.first.slice;
    }

    if (target == nullptr || !target->ValidateId(slice)) {
        return nullptr;
    }

    return target;
}

bool HybmVmmBasedSegment::MemoryInRange(const void *begin, uint64_t size) const noexcept
{
    if (begin < globalVirtualAddress_) {
        return false;
    }

    if (reinterpret_cast<const uint8_t *>(begin) + size > globalVirtualAddress_ + totalVirtualSize_) {
        return false;
    }

    return true;
}

bool HybmVmmBasedSegment::CheckSdmaReaches(uint32_t rankId) const noexcept
{
    return true;
}

void HybmVmmBasedSegment::ReleaseP2PLeases(const std::set<uint32_t> &removedRanks) noexcept
{
    if (p2pLeases_.empty()) {
        return;
    }
    auto phyIdByRank = [this](uint32_t rank) -> int32_t {
        for (const auto &im : imports_) {
            if (im.rankId == rank) {
                return static_cast<int32_t>(im.devicePhyId);
            }
        }
        return -1;
    };
    std::set<int32_t> toRelease;
    if (removedRanks.empty()) {
        toRelease = p2pLeases_;
    } else {
        for (auto rank : removedRanks) {
            auto phyId = phyIdByRank(rank);
            if (phyId >= 0) {
                toRelease.insert(phyId);
            }
        }
        std::set<int32_t> stillUsed;
        for (const auto &im : imports_) {
            if (removedRanks.find(im.rankId) == removedRanks.end()) {
                stillUsed.insert(static_cast<int32_t>(im.devicePhyId));
            }
        }
        std::set<int32_t> actualRelease;
        for (auto phyId : toRelease) {
            if (stillUsed.find(phyId) == stillUsed.end()) {
                actualRelease.insert(phyId);
            }
        }
        toRelease = std::move(actualRelease);
    }
    for (auto phyId : toRelease) {
        Result disRet = BM_OK;
        if (DlAclApi::GetAscendSocType() == AscendSocType::ASCEND_310P) {
            disRet = DlAclApi::AclrtDeviceDisablePeerAccess(phyId, 0);
        } else {
            disRet = DlAclApi::RtDisableP2P(static_cast<uint32_t>(deviceId_), static_cast<uint32_t>(phyId));
        }
        if (disRet != BM_OK) {
            BM_LOG_WARN("DisableP2P failed:" << disRet << " local_user:" << deviceId_ << " remote_phy:" << phyId);
        } else {
            BM_LOG_INFO("DisableP2P success local_user:" << deviceId_ << " remote_phy:" << phyId);
        }
        p2pLeases_.erase(phyId);
    }
}

bool HybmVmmBasedSegment::Is310PPath() const noexcept
{
    return DlAclApi::GetAscendSocType() == AscendSocType::ASCEND_310P;
}

void HybmVmmBasedSegment::ReleaseHandle(drv_mem_handle_t *handle) noexcept
{
    if (handle == nullptr) {
        return;
    }
    DlHalApi::HalMemRelease(handle);
}
