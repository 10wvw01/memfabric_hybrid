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

/* 310P(Ascend310P3) HBM 内存段: 基于 ACL Runtime VMM 接口(aclrtMallocPhysical 等)
 * 与 HybmVmmBasedSegment(910C/950, 走 HAL halMem* V4 接口) 不同, 本段全部通过
 * DlAclApi 的 aclrt* 符号实现, 适用于驱动无 HalGva* 但 libascendcl 导出 VMM 符号的环境。
 * 跨进程共享: aclrtMemExportToShareableHandle(DISABLE_PID_VALIDATION) + 对端 import;
 * die 对(同卡双 die) import 需显式 SetContext 流程绕过 runtime P2P 前置检查。
 * 对称 GVA: 每进程 aclrtReserveMemAddress(rankCnt*maxSize) 槽位窗口, slot_p = base + p*maxSize。
 */
#ifndef MF_HYBM_HYBM_ACL_VMM_SEGMENT_H
#define MF_HYBM_HYBM_ACL_VMM_SEGMENT_H

#include "hybm_mem_common.h"
#include "hybm_mem_segment.h"
#include "dl_acl_api.h"

#include <unordered_set>

namespace ock {
namespace mf {
struct AclVmmExportInfo {
    uint64_t magic{ACL_VMM_HBM_SLICE_EXPORT_INFO_MAGIC};
    uint32_t segmentType{SEGMENT_TYPE_DEFAULT};
    uint32_t version{EXPORT_INFO_VERSION};
    uint64_t gva{0};       // 导出方窗口内 GVA(与本地窗口对称, 同 slot 布局)
    uint64_t deviceVa{0};  // 同 gva(对称窗口 GVA==DVA)
    uint32_t sliceIndex{0};
    uint32_t devId{0}; // 导出方 user device id(die 对判定: devId/2 相同即同卡)
    uint32_t pid{0};
    uint32_t rankId{0};
    uint32_t sdid{0};
    uint32_t serverId{0};
    uint32_t superPodId{0};
    uint64_t size{0};
    uint64_t shareableHandle{0};
    uint8_t padding_[UNIFIED_EXCHANGE_SEG_INFO_SIZE - 80]{}; // 头部 80B(60~64 间 4B 对齐空洞)
}; // 192B
static_assert(sizeof(AclVmmExportInfo) == UNIFIED_EXCHANGE_SEG_INFO_SIZE,
              "AclVmmExportInfo must match UNIFIED_EXCHANGE_SEG_INFO_SIZE");
static_assert(offsetof(AclVmmExportInfo, segmentType) == SEGMENT_TYPE_OFFSET, "segmentType offset mismatch!");

class HybmAclVmmSegment : public MemSegment {
public:
    explicit HybmAclVmmSegment(const MemSegmentOptions &options, int eid) : MemSegment{options, eid} {}

    ~HybmAclVmmSegment() override;

    Result ValidateOptions() noexcept override;
    Result ReserveMemorySpace(void **address) noexcept override;
    Result UnReserveMemorySpace() noexcept override;
    Result AllocLocalMemory(uint64_t size, MemSlicePtr &slice) noexcept override;
    Result RegisterMemory(const void *addr, uint64_t size, MemSlicePtr &slice) noexcept override;
    Result ReleaseSliceMemory(const MemSlicePtr &slice) noexcept override;
    Result Export(std::string &exInfo) noexcept override;
    Result Export(const MemSlicePtr &slice, std::string &exInfo) noexcept override;
    Result GetExportSliceSize(size_t &size) noexcept override;
    Result Import(const std::vector<std::string> &allExInfo, void *addresses[]) noexcept override;
    Result RemoveImported(const std::vector<uint32_t> &ranks) noexcept override;
    Result Mmap() noexcept override;
    Result Unmap() noexcept override;
    MemSlicePtr GetMemSlice(hybm_mem_slice_t slice, bool quiet) const noexcept override;
    bool MemoryInRange(const void *begin, uint64_t size) const noexcept override;
    bool IsLocalRange(const void *begin, uint64_t size) const noexcept override;
    bool CheckSdmaReaches(uint32_t rankId) const noexcept override;

    hybm_mem_type GetMemoryType() const noexcept override
    {
        return HYBM_MEM_TYPE_DEVICE;
    }

private:
    Result ExportInner(const MemSlicePtr &slice) noexcept;
    Result MapPhysicalToOwnSlot(aclrtDrvMemHandle handle, uint64_t size, uint64_t allocAddr) noexcept;
    Result RegisterAndExportSlice(const MemSlicePtr &slice, aclrtDrvMemHandle handle, uint64_t allocAddr,
                                  uint64_t size) noexcept;
    Result ImportOne(const AclVmmExportInfo &im) noexcept;
    Result ImportHandleForDiePair(const AclVmmExportInfo &im, aclrtDrvMemHandle &handle) noexcept;
    Result MapImportedToSlot(const AclVmmExportInfo &im, aclrtDrvMemHandle handle, uint64_t slotAddr) noexcept;
    Result UnmapOne(uint64_t slotAddr, aclrtDrvMemHandle handle) noexcept;
    void ReleaseCrossCardRef(uint64_t slotAddr) noexcept;      /* 跨卡句柄释放: 引用归零即拆 P2P */
    void TeardownResidualCrossCardP2p() noexcept;              /* uninit 兜底: 残留跨卡 P2P 全量拆除 */
    bool IsImportInfoValid(const AclVmmExportInfo &info) const noexcept;
    bool IsDiePair(uint32_t peerDevId) const noexcept;
    uint64_t OwnSlotBase() const noexcept;
    uint64_t SlotBase(uint32_t rankId) const noexcept;
    void FillMemProp(aclrtPhysicalMemProp &prop) const noexcept;

    std::vector<AclVmmExportInfo> imports_;
    std::unordered_set<uint64_t> importKeys_; // (rankId<<32)|sliceIndex, Import 去重
    uint8_t *globalVirtualAddress_{nullptr};  // 对称窗口基址, GVA==DVA==HVA
    uint64_t totalVirtualSize_{0UL};          // rankCnt * maxSize
    uint64_t allocatedSize_{0UL};

    std::map<uint32_t, MemSliceStatus> slices_;              // index -> (slice, own handle)
    std::map<uint32_t, std::string> exportMap_;              // index -> serialized export info
    std::map<uint64_t, aclrtDrvMemHandle> mappedGvaMem_;     // slot addr -> imported handle
    std::map<uint64_t, uint32_t> slotPeerDev_;               // 跨卡导入 slot addr -> 对端 user devId
    std::map<uint32_t, uint32_t> crossCardPeerCnt_;          // 对端 user devId -> 活跃跨卡句柄计数
    std::vector<aclrtDrvMemHandle> orphanHandles_;           // unmap 成功但 free 失败的遗留句柄
};
} // namespace mf
} // namespace ock

#endif // MF_HYBM_HYBM_ACL_VMM_SEGMENT_H
