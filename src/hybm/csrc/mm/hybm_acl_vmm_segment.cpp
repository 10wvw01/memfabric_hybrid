/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 * You may use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

#include "hybm_acl_vmm_segment.h"

#include <algorithm>

#include "hybm_ex_info_transfer.h"
#include "hybm_va_manager.h"

namespace ock {
namespace mf {
HybmAclVmmSegment::~HybmAclVmmSegment()
{
    if (globalVirtualAddress_ != nullptr) {
        auto ret = UnReserveMemorySpace();
        if (ret != BM_OK) {
            BM_LOG_WARN("~HybmAclVmmSegment UnReserveMemorySpace failed, ret:" << ret);
        }
    }
}

Result HybmAclVmmSegment::ValidateOptions() noexcept
{
    if (options_.segType != HYBM_MST_HBM) {
        BM_LOG_ERROR("acl vmm segment only support HBM segType, segType:" << options_.segType);
        return BM_INVALID_PARAM;
    }
    if (options_.maxSize == 0 || options_.maxSize % HYBM_LARGE_PAGE_SIZE != 0 ||
        options_.size % HYBM_LARGE_PAGE_SIZE != 0 || options_.size > options_.maxSize) {
        BM_LOG_ERROR("acl vmm segment invalid size, size:" << options_.size << " maxSize:" << options_.maxSize);
        return BM_INVALID_PARAM;
    }
    if (options_.rankCnt == 0 || options_.rankId >= options_.rankCnt) {
        BM_LOG_ERROR("acl vmm segment invalid rank, rankId:" << options_.rankId
                                                             << " rankCnt:" << options_.rankCnt);
        return BM_INVALID_PARAM;
    }
    if (options_.maxSize > UINT64_MAX / options_.rankCnt) {
        BM_LOG_ERROR("acl vmm segment size overflow, maxSize:" << options_.maxSize
                                                               << " rankCnt:" << options_.rankCnt);
        return BM_INVALID_PARAM;
    }
    return BM_OK;
}

Result HybmAclVmmSegment::ReserveMemorySpace(void **address) noexcept
{
    auto ret = ValidateOptions();
    if (ret != BM_OK) {
        return ret;
    }

    totalVirtualSize_ = options_.maxSize * options_.rankCnt;
    void *base = nullptr;
    ret = DlAclApi::AclrtReserveMemAddress(&base, totalVirtualSize_, 0, nullptr, 0);
    if (ret != BM_OK) {
        BM_LOG_ERROR("AclrtReserveMemAddress failed, ret:" << ret << " rankId:" << options_.rankId
                                                           << " size:" << totalVirtualSize_);
        return ret;
    }
    globalVirtualAddress_ = static_cast<uint8_t *>(base);
    BM_LOG_INFO("acl vmm reserve window ok, rankId:" << options_.rankId
                                                     << " base:" << VaToStr(reinterpret_cast<uint64_t>(base))
                                                     << " totalSize:" << totalVirtualSize_);
    if (address != nullptr) {
        *address = base;
    }
    return BM_OK;
}

Result HybmAclVmmSegment::UnReserveMemorySpace() noexcept
{
    auto ret = Unmap();
    if (ret != BM_OK) {
        BM_LOG_ERROR("UnReserveMemorySpace Unmap failed, ret:" << ret << " rankId:" << options_.rankId);
        return ret;
    }

    while (!slices_.empty()) {
        auto sliceRet = ReleaseSliceMemory(slices_.begin()->second.slice);
        if (sliceRet != BM_OK) {
            BM_LOG_ERROR("UnReserveMemorySpace ReleaseSliceMemory failed, ret:" << sliceRet
                       << " rankId:" << options_.rankId);
            return sliceRet;
        }
    }
    exportMap_.clear();

    if (globalVirtualAddress_ != nullptr) {
        ret = DlAclApi::AclrtReleaseMemAddress(globalVirtualAddress_);
        if (ret != BM_OK) {
            BM_LOG_ERROR("AclrtReleaseMemAddress failed, ret:" << ret << " rankId:" << options_.rankId);
            return ret;
        }
        globalVirtualAddress_ = nullptr;
        totalVirtualSize_ = 0;
        allocatedSize_ = 0;
    }
    return BM_OK;
}

void HybmAclVmmSegment::FillMemProp(aclrtPhysicalMemProp &prop) const noexcept
{
    prop.handleType = aclrtMemHandleType::ACL_MEM_HANDLE_TYPE_NONE;
    prop.allocationType = aclrtMemAllocationType::ACL_MEM_ALLOCATION_TYPE_PINNED;
    prop.memAttr = aclrtMemAttr::ACL_MEM_P2P_HUGE;
    prop.location.id = static_cast<uint32_t>(options_.devId);
    prop.location.type = aclrtMemLocationType::ACL_MEM_LOCATION_TYPE_DEVICE;
}

Result HybmAclVmmSegment::AllocLocalMemory(uint64_t size, MemSlicePtr &slice) noexcept
{
    if (globalVirtualAddress_ == nullptr) {
        BM_LOG_ERROR("AllocLocalMemory before reserve, rankId:" << options_.rankId);
        return BM_NOT_INITIALIZED;
    }
    if (size == 0 || size % HYBM_LARGE_PAGE_SIZE != 0) {
        BM_LOG_ERROR("AllocLocalMemory invalid size:" << size << " rankId:" << options_.rankId);
        return BM_INVALID_PARAM;
    }
    if (allocatedSize_ + size > options_.maxSize) {
        BM_LOG_ERROR("AllocLocalMemory exceed maxSize, rankId:" << options_.rankId << " allocatedSize:"
                   << allocatedSize_ << " reqSize:" << size << " maxSize:" << options_.maxSize);
        return BM_INVALID_PARAM;
    }

    aclrtPhysicalMemProp prop{};
    FillMemProp(prop);
    aclrtDrvMemHandle handle = nullptr;
    auto ret = DlAclApi::AclrtMallocPhysical(&handle, size, &prop, 0);
    if (ret != BM_OK) {
        BM_LOG_ERROR("AclrtMallocPhysical failed, ret:" << ret << " rankId:" << options_.rankId
                                                        << " size:" << size);
        return ret;
    }

    auto allocAddr = OwnSlotBase() + allocatedSize_;
    ret = DlAclApi::AclrtMapMem(reinterpret_cast<void *>(allocAddr), size, 0, handle, 0);
    if (ret != BM_OK) {
        BM_LOG_ERROR("AclrtMapMem failed, ret:" << ret << " rankId:" << options_.rankId
                                                << " addr:" << VaToStr(allocAddr) << " size:" << size);
        (void)DlAclApi::AclrtFreePhysical(handle);
        return ret;
    }

    aclrtMemAccessDesc acc{};
    acc.flags = aclrtMemAccessFlags::ACL_RT_MEM_ACCESS_FLAGS_READWRITE;
    acc.location.id = static_cast<uint32_t>(options_.devId);
    acc.location.type = aclrtMemLocationType::ACL_MEM_LOCATION_TYPE_DEVICE;
    ret = DlAclApi::AclrtMemSetAccess(reinterpret_cast<void *>(allocAddr), size, &acc, 1);
    if (ret != BM_OK) {
        BM_LOG_ERROR("AclrtMemSetAccess failed, ret:" << ret << " rankId:" << options_.rankId
                                                      << " addr:" << VaToStr(allocAddr));
        (void)DlAclApi::AclrtUnmapMem(reinterpret_cast<void *>(allocAddr));
        (void)DlAclApi::AclrtFreePhysical(handle);
        return ret;
    }

    try {
        slice = std::make_shared<MemSlice>(sliceCount_, HYBM_MEM_TYPE_DEVICE, MEM_PT_TYPE_GVM, allocAddr, allocAddr,
                                           size);
    } catch (...) {
        BM_LOG_ERROR("make MemSlice failed, rankId:" << options_.rankId << " sliceIndex:" << sliceCount_);
        (void)DlAclApi::AclrtUnmapMem(reinterpret_cast<void *>(allocAddr));
        (void)DlAclApi::AclrtFreePhysical(handle);
        return BM_MALLOC_FAILED;
    }

    auto vaRet = HybmVaManager::GetInstance().AddVaInfo({{allocAddr, allocAddr, allocAddr}, size,
                                                         HYBM_MEM_TYPE_DEVICE},
                                                        options_.rankId);
    if (vaRet != BM_OK) {
        BM_LOG_ERROR("AddVaInfo failed, ret:" << vaRet << " rankId:" << options_.rankId
                                              << " addr:" << VaToStr(allocAddr));
        (void)DlAclApi::AclrtUnmapMem(reinterpret_cast<void *>(allocAddr));
        (void)DlAclApi::AclrtFreePhysical(handle);
        return vaRet;
    }

    slices_.emplace(sliceCount_, MemSliceStatus(slice, handle));
    auto expRet = ExportInner(slice);
    if (expRet != BM_OK) {
        BM_LOG_ERROR("ExportInner failed, ret:" << expRet << " rankId:" << options_.rankId
                                                << " sliceIndex:" << sliceCount_);
        slices_.erase(sliceCount_);
        (void)DlAclApi::AclrtUnmapMem(reinterpret_cast<void *>(allocAddr));
        (void)DlAclApi::AclrtFreePhysical(handle);
        return expRet;
    }
    allocatedSize_ += size;
    sliceCount_++;
    BM_LOG_INFO("acl vmm alloc ok, rankId:" << options_.rankId << " sliceIndex:" << sliceCount_ - 1
                                            << " addr:" << VaToStr(allocAddr) << " size:" << size);
    return BM_OK;
}

Result HybmAclVmmSegment::RegisterMemory(const void *addr, uint64_t size, MemSlicePtr &slice) noexcept
{
    (void)addr;
    (void)size;
    (void)slice;
    BM_LOG_ERROR("acl vmm segment not support RegisterMemory, rankId:" << options_.rankId);
    return BM_NOT_SUPPORTED;
}

Result HybmAclVmmSegment::ExportInner(const MemSlicePtr &slice) noexcept
{
    auto it = slices_.find(static_cast<uint32_t>(slice->index_));
    if (it == slices_.end()) {
        BM_LOG_ERROR("ExportInner slice not found, sliceIndex:" << slice->index_
                                                                << " rankId:" << options_.rankId);
        return BM_INVALID_PARAM;
    }

    uint64_t sh = 0;
    auto ret = DlAclApi::AclrtMemExportToShareableHandle(it->second.handle,
                                                         aclrtMemHandleType::ACL_MEM_HANDLE_TYPE_NONE,
                                                         ACL_RT_VMM_EXPORT_FLAG_DISABLE_PID_VALIDATION, &sh);
    if (ret != BM_OK) {
        BM_LOG_ERROR("AclrtMemExportToShareableHandle failed, ret:" << ret << " rankId:" << options_.rankId
                                                                    << " sliceIndex:" << slice->index_);
        return ret;
    }

    AclVmmExportInfo info{};
    info.magic = ACL_VMM_HBM_SLICE_EXPORT_INFO_MAGIC;
    info.segmentType = SEGMENT_TYPE_DEFAULT;
    info.version = EXPORT_INFO_VERSION;
    info.gva = slice->gva_;
    info.deviceVa = slice->vAddress_;
    info.sliceIndex = slice->index_;
    info.devId = static_cast<uint32_t>(options_.devId);
    info.pid = pid_;
    info.rankId = options_.rankId;
    info.sdid = sdid_;
    info.serverId = serverId_;
    info.superPodId = superPodId_;
    info.size = slice->size_;
    info.shareableHandle = sh;

    LiteralExInfoTranslater<AclVmmExportInfo> translator;
    std::string exInfo;
    if (translator.Serialize(info, exInfo) != 0) {
        BM_LOG_ERROR("serialize export info failed, rankId:" << options_.rankId
                                                             << " sliceIndex:" << slice->index_);
        return BM_ERROR;
    }
    try {
        exportMap_.emplace(static_cast<uint32_t>(slice->index_), exInfo);
    } catch (...) {
        return BM_MALLOC_FAILED;
    }
    return BM_OK;
}

Result HybmAclVmmSegment::Export(const MemSlicePtr &slice, std::string &exInfo) noexcept
{
    auto it = exportMap_.find(static_cast<uint32_t>(slice->index_));
    if (it == exportMap_.end()) {
        BM_LOG_ERROR("export slice not found, sliceIndex:" << slice->index_ << " rankId:" << options_.rankId);
        return BM_INVALID_PARAM;
    }
    exInfo = it->second;
    return BM_OK;
}

Result HybmAclVmmSegment::Export(std::string &exInfo) noexcept
{
    // BM 场景 entity 级导出为空(对齐 DevLegacy/VmmBased); TRANS 场景暂不支持
    (void)exInfo;
    return BM_OK;
}

Result HybmAclVmmSegment::GetExportSliceSize(size_t &size) noexcept
{
    size = sizeof(AclVmmExportInfo);
    return BM_OK;
}

bool HybmAclVmmSegment::IsDiePair(uint32_t peerDevId) const noexcept
{
    // 310P Duo: 同卡双 die 共享 PCIe 设备, user devId/2 相同即同卡 die 对
    return peerDevId / 2U == static_cast<uint32_t>(options_.devId) / 2U;
}

uint64_t HybmAclVmmSegment::OwnSlotBase() const noexcept
{
    return reinterpret_cast<uint64_t>(globalVirtualAddress_) + options_.maxSize * options_.rankId;
}

uint64_t HybmAclVmmSegment::SlotBase(uint32_t rankId) const noexcept
{
    return reinterpret_cast<uint64_t>(globalVirtualAddress_) + options_.maxSize * rankId;
}

Result HybmAclVmmSegment::Import(const std::vector<std::string> &allExInfo, void *addresses[]) noexcept
{
    LiteralExInfoTranslater<AclVmmExportInfo> translator;
    for (auto i = 0U; i < allExInfo.size(); i++) {
        AclVmmExportInfo info{};
        auto ret = translator.Deserialize(allExInfo[i], info);
        if (ret != 0) {
            BM_LOG_ERROR("deserialize import info failed, index:" << i << " rankId:" << options_.rankId);
            return BM_INVALID_PARAM;
        }
        if (info.magic != ACL_VMM_HBM_SLICE_EXPORT_INFO_MAGIC || info.magic == ENTITY_EXPORT_INFO_MAGIC) {
            BM_LOG_WARN("import index(" << i << ") rank(" << info.rankId << ") magic(" << std::hex << info.magic
                                        << std::dec << ") mismatch, skip it.");
            continue;
        }
        if (addresses != nullptr) {
            addresses[i] = reinterpret_cast<void *>(info.gva);
        }
        if (info.size == 0 || info.rankId >= options_.rankCnt) {
            continue;
        }
        // 幂等: join 重试场景下同一(rankId, sliceIndex)可能重复到达, 去重防止二次 import 报错
        auto dup = std::find_if(imports_.begin(), imports_.end(), [&info](const AclVmmExportInfo &it) {
            return it.rankId == info.rankId && it.sliceIndex == info.sliceIndex;
        });
        if (dup != imports_.end()) {
            BM_LOG_INFO("import info duplicated, localRank:" << options_.rankId << " remoteRank:" << info.rankId
                        << " sliceIndex:" << info.sliceIndex << ", skip");
            continue;
        }
        try {
            imports_.emplace_back(info);
        } catch (...) {
            BM_LOG_ERROR("imports_ emplace failed, rankId:" << options_.rankId);
            return BM_MALLOC_FAILED;
        }
        BM_LOG_INFO("acl vmm import info ok, localRank:" << options_.rankId << " remoteRank:" << info.rankId
                   << " devId:" << info.devId << " sliceIndex:" << info.sliceIndex << " size:" << info.size);
    }
    return BM_OK;
}

Result HybmAclVmmSegment::ImportOne(const AclVmmExportInfo &im) noexcept
{
    aclrtDrvMemHandle handle = nullptr;
    Result ret;
    if (IsDiePair(im.devId)) {
        // die 对: 借导出方 ctx 跳过 runtime P2P 前置检查(同芯片 P2P 被内核拒绝)
        aclrtContext expCtx = nullptr;
        ret = DlAclApi::AclrtCreateContext(&expCtx, static_cast<int32_t>(im.devId));
        if (ret != BM_OK) {
            BM_LOG_ERROR("CreateContext(peerDie) failed, ret:" << ret << " peerDevId:" << im.devId
                                                               << " rankId:" << options_.rankId);
            return ret;
        }
        (void)DlAclApi::AclrtSetCurrentContext(expCtx);
        ret = DlAclApi::AclrtMemImportFromShareableHandle(im.shareableHandle, options_.devId, &handle);
        if (ret != BM_OK) {
            BM_LOG_ERROR("die pair import failed, ret:" << ret << " localRank:" << options_.rankId
                       << " remoteRank:" << im.rankId << " sh:" << im.shareableHandle);
            (void)DlAclApi::AclrtSetDevice(options_.devId);
            (void)DlAclApi::AclrtDestroyContext(expCtx);
            return ret;
        }
        (void)DlAclApi::AclrtSetDevice(options_.devId);
        (void)DlAclApi::AclrtDestroyContext(expCtx);
    } else {
        // 跨卡: 本进程 ctx 直接 import(隐式双向 P2P, 消耗槽位)
        ret = DlAclApi::AclrtMemImportFromShareableHandle(im.shareableHandle, options_.devId, &handle);
        if (ret != BM_OK) {
            BM_LOG_ERROR("cross card import failed, ret:" << ret << " localRank:" << options_.rankId
                       << " remoteRank:" << im.rankId << " peerDevId:" << im.devId);
            return ret;
        }
    }

    auto slotAddr = SlotBase(im.rankId) + im.sliceIndex * HYBM_LARGE_PAGE_SIZE;
    ret = DlAclApi::AclrtMapMem(reinterpret_cast<void *>(slotAddr), im.size, 0, handle, 0);
    if (ret != BM_OK) {
        BM_LOG_ERROR("AclrtMapMem(imported) failed, ret:" << ret << " rankId:" << options_.rankId
                   << " remoteRank:" << im.rankId << " slotAddr:" << VaToStr(slotAddr) << " size:" << im.size);
        (void)DlAclApi::AclrtFreePhysical(handle);
        return ret;
    }

    aclrtMemAccessDesc acc{};
    acc.flags = aclrtMemAccessFlags::ACL_RT_MEM_ACCESS_FLAGS_READWRITE;
    acc.location.id = static_cast<uint32_t>(options_.devId);
    acc.location.type = aclrtMemLocationType::ACL_MEM_LOCATION_TYPE_DEVICE;
    ret = DlAclApi::AclrtMemSetAccess(reinterpret_cast<void *>(slotAddr), im.size, &acc, 1);
    if (ret != BM_OK) {
        BM_LOG_ERROR("AclrtMemSetAccess(imported) failed, ret:" << ret << " rankId:" << options_.rankId
                   << " remoteRank:" << im.rankId << " slotAddr:" << VaToStr(slotAddr));
        (void)DlAclApi::AclrtUnmapMem(reinterpret_cast<void *>(slotAddr));
        (void)DlAclApi::AclrtFreePhysical(handle);
        return ret;
    }

    // 对称窗口: gva(导出方) 与本地 slot 同值; dva=hva=slotAddr 保证 TransformVa 直通
    auto vaRet = HybmVaManager::GetInstance().AddVaInfoFromExternal({im.gva, slotAddr, slotAddr, im.size,
                                                                     HYBM_MEM_TYPE_DEVICE},
                                                                    options_.rankId, im.rankId);
    if (vaRet != BM_OK) {
        BM_LOG_ERROR("AddVaInfoFromExternal failed, ret:" << vaRet << " rankId:" << options_.rankId
                                                          << " remoteRank:" << im.rankId);
        (void)DlAclApi::AclrtUnmapMem(reinterpret_cast<void *>(slotAddr));
        (void)DlAclApi::AclrtFreePhysical(handle);
        return vaRet;
    }
    mappedGvaMem_.emplace(slotAddr, handle);
    return BM_OK;
}

Result HybmAclVmmSegment::Mmap() noexcept
{
    if (globalVirtualAddress_ == nullptr) {
        BM_LOG_ERROR("Mmap before reserve, rankId:" << options_.rankId);
        return BM_NOT_INITIALIZED;
    }
    for (auto &im : imports_) {
        if (im.rankId == options_.rankId) {
            continue;
        }
        auto slotAddr = SlotBase(im.rankId) + im.sliceIndex * HYBM_LARGE_PAGE_SIZE;
        if (mappedGvaMem_.find(slotAddr) != mappedGvaMem_.end()) {
            BM_LOG_INFO("remote slice already mapped, localRank:" << options_.rankId
                        << " remoteRank:" << im.rankId << " slotAddr:" << VaToStr(slotAddr));
            continue;
        }
        auto ret = ImportOne(im);
        if (ret != BM_OK) {
            BM_LOG_ERROR("ImportOne failed, ret:" << ret << " localRank:" << options_.rankId
                        << " remoteRank:" << im.rankId << " sliceIndex:" << im.sliceIndex);
            return ret;
        }
    }
    imports_.clear();
    return BM_OK;
}

Result HybmAclVmmSegment::UnmapOne(uint64_t slotAddr, aclrtDrvMemHandle handle) noexcept
{
    auto ret = DlAclApi::AclrtUnmapMem(reinterpret_cast<void *>(slotAddr));
    if (ret != BM_OK) {
        BM_LOG_ERROR("AclrtUnmapMem(imported) failed, ret:" << ret << " rankId:" << options_.rankId
                                                            << " slotAddr:" << VaToStr(slotAddr));
        return ret;
    }
    ret = DlAclApi::AclrtFreePhysical(handle);
    if (ret != BM_OK) {
        BM_LOG_ERROR("AclrtFreePhysical(imported) failed, ret:" << ret << " rankId:" << options_.rankId
                                                                << " slotAddr:" << VaToStr(slotAddr));
        return ret;
    }
    HybmVaManager::GetInstance().RemoveOneVaInfo(slotAddr);
    return BM_OK;
}

Result HybmAclVmmSegment::Unmap() noexcept
{
    for (auto &it : mappedGvaMem_) {
        auto ret = UnmapOne(it.first, it.second);
        if (ret != BM_OK) {
            BM_LOG_ERROR("Unmap one failed, ret:" << ret << " rankId:" << options_.rankId
                                                  << " slotAddr:" << VaToStr(it.first));
            return ret;
        }
    }
    mappedGvaMem_.clear();
    imports_.clear();
    return BM_OK;
}

Result HybmAclVmmSegment::RemoveImported(const std::vector<uint32_t> &ranks) noexcept
{
    for (auto &rank : ranks) {
        if (rank >= options_.rankCnt) {
            BM_LOG_ERROR("invalid remove rank:" << rank << " rankCnt:" << options_.rankCnt
                                                << " rankId:" << options_.rankId);
            return BM_INVALID_PARAM;
        }
    }

    for (auto &rank : ranks) {
        auto slotBegin = SlotBase(rank);
        auto slotEnd = slotBegin + options_.maxSize;
        for (auto it = mappedGvaMem_.begin(); it != mappedGvaMem_.end();) {
            if (it->first >= slotBegin && it->first < slotEnd) {
                auto ret = UnmapOne(it->first, it->second);
                if (ret != BM_OK) {
                    BM_LOG_ERROR("RemoveImported UnmapOne failed, ret:" << ret << " rankId:" << options_.rankId
                               << " removeRank:" << rank);
                    return ret;
                }
                it = mappedGvaMem_.erase(it);
            } else {
                ++it;
            }
        }
    }
    return BM_OK;
}

Result HybmAclVmmSegment::ReleaseSliceMemory(const MemSlicePtr &slice) noexcept
{
    /* 入参可能是 map 内 shared_ptr 的引用: 先拷贝提升引用计数,
     * 防止 slices_.erase 后(析构路径下 map 为最后所有者)再访问 slice 成员导致 use-after-free */
    MemSlicePtr holder = slice;
    auto it = slices_.find(static_cast<uint32_t>(holder->index_));
    if (it == slices_.end()) {
        BM_LOG_ERROR("ReleaseSliceMemory slice not found, sliceIndex:" << holder->index_
                    << " rankId:" << options_.rankId);
        return BM_INVALID_PARAM;
    }

    auto ret = DlAclApi::AclrtUnmapMem(reinterpret_cast<void *>(holder->vAddress_));
    if (ret != BM_OK) {
        BM_LOG_ERROR("AclrtUnmapMem(own) failed, ret:" << ret << " rankId:" << options_.rankId
                   << " sliceIndex:" << holder->index_ << " addr:" << VaToStr(holder->vAddress_));
        return ret;
    }
    ret = DlAclApi::AclrtFreePhysical(it->second.handle);
    if (ret != BM_OK) {
        BM_LOG_ERROR("AclrtFreePhysical(own) failed, ret:" << ret << " rankId:" << options_.rankId
                   << " sliceIndex:" << holder->index_);
        return ret;
    }
    HybmVaManager::GetInstance().RemoveOneVaInfo(holder->vAddress_);
    slices_.erase(it);
    exportMap_.erase(static_cast<uint32_t>(holder->index_));
    return BM_OK;
}

MemSlicePtr HybmAclVmmSegment::GetMemSlice(hybm_mem_slice_t slice, bool quiet) const noexcept
{
    (void)quiet;
    auto index = MemSlice::GetIndexFrom(slice);
    auto it = slices_.find(index);
    if (it == slices_.end() || it->second.slice == nullptr || !it->second.slice->ValidateId(slice)) {
        return nullptr;
    }
    return it->second.slice;
}

bool HybmAclVmmSegment::MemoryInRange(const void *begin, uint64_t size) const noexcept
{
    auto addr = reinterpret_cast<const uint8_t *>(begin);
    return addr >= globalVirtualAddress_ && addr + size <= globalVirtualAddress_ + totalVirtualSize_ &&
           size != 0;
}

bool HybmAclVmmSegment::IsLocalRange(const void *begin, uint64_t size) const noexcept
{
    auto addr = reinterpret_cast<const uint8_t *>(begin);
    auto ownBase = reinterpret_cast<const uint8_t *>(OwnSlotBase());
    return addr >= ownBase && addr + size <= ownBase + allocatedSize_ && size != 0;
}

bool HybmAclVmmSegment::CheckSdmaReaches(uint32_t rankId) const noexcept
{
    // 2-rank die 对/槽位预算内已建立映射即视为 AclMemcpy 可达; 池内地址均已 AddVaInfo 登记
    if (rankId == options_.rankId) {
        return true;
    }
    auto slotBegin = SlotBase(rankId);
    auto slotEnd = slotBegin + options_.maxSize;
    for (auto &it : mappedGvaMem_) {
        if (it.first >= slotBegin && it.first < slotEnd) {
            return true;
        }
    }
    return false;
}
} // namespace mf
} // namespace ock
