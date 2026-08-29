/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

/* P2 UT：HybmAclVmmSegment(310P ACL VMM HBM 段) — stub so 集成模式, 不依赖 mockcpp */
#include <gtest/gtest.h>

#if __has_include(<mockcpp/mockcpp.hpp>)
#define MF_ACL_VMM_UT_HAS_MOCKCPP 1
#include <mockcpp/mockcpp.hpp>
#define MOCKER_CPP(api, TT) MOCKCPP_NS::mockAPI(#api, reinterpret_cast<TT>(api))
#endif

#define private public
#define protected public
#include "hybm_acl_vmm_segment.h"
#undef private
#undef protected

#include "hybm.h"
#include "hybm_va_manager.h"
#include "hybm_mem_slice.h"
#include "hybm_ex_info_transfer.h"

namespace ock {
namespace mf {

class HybmAclVmmSegmentTest : public testing::Test {
protected:
    void SetUp() override
    {
        Result ret = hybm_init(0, 0);
        ASSERT_EQ(ret, BM_OK) << "hybm_init failed, ret=" << ret;
        MemSegment::socType_ = AscendSocType::ASCEND_310P;
        HybmVaManager::GetInstance().ClearAll();
    }
    void TearDown() override
    {
        HybmVaManager::GetInstance().ClearAll();
        MemSegment::socType_ = AscendSocType::ASCEND_UNKNOWN;
        hybm_uninit();
    }

    MemSegmentOptions MakeOptions(uint32_t rankId, uint32_t rankCnt, uint64_t size, uint64_t maxSize)
    {
        MemSegmentOptions opt{};
        opt.devId = static_cast<int32_t>(rankId);
        opt.segType = HYBM_MST_HBM;
        opt.dataOpType = HYBM_DOP_TYPE_SDMA;
        opt.shared = true;
        opt.size = size;
        opt.maxSize = maxSize;
        opt.rankId = rankId;
        opt.rankCnt = rankCnt;
        return opt;
    }
};

TEST_F(HybmAclVmmSegmentTest, ValidateOptions_RejectsInvalid)
{
    MemSegmentOptions opt = MakeOptions(0, 2, HYBM_LARGE_PAGE_SIZE, 2UL * HYBM_LARGE_PAGE_SIZE);
    HybmAclVmmSegment seg(opt, 0);
    ASSERT_EQ(seg.ValidateOptions(), BM_OK);

    opt.segType = HYBM_MST_DRAM; // 仅支持 HBM
    HybmAclVmmSegment segDram(opt, 0);
    ASSERT_EQ(segDram.ValidateOptions(), BM_INVALID_PARAM);

    opt = MakeOptions(0, 2, HYBM_LARGE_PAGE_SIZE, 2UL * HYBM_LARGE_PAGE_SIZE);
    opt.size = HYBM_LARGE_PAGE_SIZE + 1; // 非 2M 对齐
    HybmAclVmmSegment segUnaligned(opt, 0);
    ASSERT_EQ(segUnaligned.ValidateOptions(), BM_INVALID_PARAM);

    opt = MakeOptions(0, 2, HYBM_LARGE_PAGE_SIZE, 2UL * HYBM_LARGE_PAGE_SIZE);
    opt.rankId = 2; // 越界
    HybmAclVmmSegment segRank(opt, 0);
    ASSERT_EQ(segRank.ValidateOptions(), BM_INVALID_PARAM);
}

TEST_F(HybmAclVmmSegmentTest, ReserveAndAlloc)
{
    const uint64_t maxSize = 2UL * HYBM_LARGE_PAGE_SIZE;
    MemSegmentOptions opt = MakeOptions(0, 2, maxSize, maxSize);
    HybmAclVmmSegment seg(opt, 0);
    void *base = nullptr;
    ASSERT_EQ(seg.ReserveMemorySpace(&base), BM_OK);
    ASSERT_NE(base, nullptr);
    ASSERT_EQ(seg.totalVirtualSize_, 2UL * maxSize);

    MemSlicePtr slice = nullptr;
    ASSERT_EQ(seg.AllocLocalMemory(HYBM_LARGE_PAGE_SIZE, slice), BM_OK);
    ASSERT_NE(slice, nullptr);
    ASSERT_EQ(slice->gva_, reinterpret_cast<uint64_t>(base));          // own slot 基址
    ASSERT_EQ(slice->vAddress_, slice->gva_);                          // 对称窗口 GVA==DVA
    ASSERT_EQ(slice->size_, HYBM_LARGE_PAGE_SIZE);
    ASSERT_EQ(seg.slices_.size(), 1UL);
    ASSERT_EQ(seg.exportMap_.size(), 1UL);

    // 超出 maxSize 拒绝
    MemSlicePtr over = nullptr;
    ASSERT_EQ(seg.AllocLocalMemory(maxSize, over), BM_INVALID_PARAM);

    size_t expSize = 0;
    ASSERT_EQ(seg.GetExportSliceSize(expSize), BM_OK);
    ASSERT_EQ(expSize, UNIFIED_EXCHANGE_SEG_INFO_SIZE);
}

TEST_F(HybmAclVmmSegmentTest, ExportImportMmapFullChain)
{
    const uint64_t maxSize = 2UL * HYBM_LARGE_PAGE_SIZE;
    // rank0 导出
    MemSegmentOptions opt0 = MakeOptions(0, 2, maxSize, maxSize);
    HybmAclVmmSegment seg0(opt0, 0);
    void *base0 = nullptr;
    ASSERT_EQ(seg0.ReserveMemorySpace(&base0), BM_OK);
    MemSlicePtr slice0 = nullptr;
    ASSERT_EQ(seg0.AllocLocalMemory(HYBM_LARGE_PAGE_SIZE, slice0), BM_OK);
    std::string exInfo;
    ASSERT_EQ(seg0.Export(slice0, exInfo), BM_OK);
    ASSERT_EQ(exInfo.size(), UNIFIED_EXCHANGE_SEG_INFO_SIZE);

    // rank1 导入(同 die 对 devId 0 vs 1)
    MemSegmentOptions opt1 = MakeOptions(1, 2, maxSize, maxSize);
    opt1.devId = 1;
    HybmAclVmmSegment seg1(opt1, 1);
    void *base1 = nullptr;
    ASSERT_EQ(seg1.ReserveMemorySpace(&base1), BM_OK);
    std::vector<std::string> all{exInfo};
    void *addresses[1] = {nullptr};
    ASSERT_EQ(seg1.Import(all, addresses), BM_OK);
    ASSERT_EQ(addresses[0], reinterpret_cast<void *>(slice0->gva_)); // addresses 回填导出方 gva
    ASSERT_EQ(seg1.Mmap(), BM_OK);
    ASSERT_EQ(seg1.mappedGvaMem_.size(), 1UL);
    // 映射登记在 rank0 的 slot: base1 + 0
    auto slotAddr = reinterpret_cast<uint64_t>(base1);
    ASSERT_EQ(seg1.mappedGvaMem_.count(slotAddr), 1UL);
    // 可达性: 已映射 rank0 true; own rank 本地可达 true
    ASSERT_TRUE(seg1.CheckSdmaReaches(0));
    ASSERT_TRUE(seg1.CheckSdmaReaches(1));

    // 未知 magic 跳过
    std::string bad = exInfo;
    AclVmmExportInfo badInfo{};
    LiteralExInfoTranslater<AclVmmExportInfo> translator;
    (void)translator.Deserialize(bad, badInfo);
    badInfo.magic = 0xDEADBEEF;
    std::string badSerialized;
    (void)translator.Serialize(badInfo, badSerialized);
    std::vector<std::string> mixed{badSerialized};
    void *addr2[1] = {nullptr};
    ASSERT_EQ(seg1.Import(mixed, addr2), BM_OK);
    ASSERT_EQ(addr2[0], nullptr); // 跳过项不回填
}

TEST_F(HybmAclVmmSegmentTest, RemoveImportedAndRelease)
{
    const uint64_t maxSize = 2UL * HYBM_LARGE_PAGE_SIZE;
    MemSegmentOptions opt0 = MakeOptions(0, 2, maxSize, maxSize);
    HybmAclVmmSegment seg0(opt0, 0);
    void *base0 = nullptr;
    ASSERT_EQ(seg0.ReserveMemorySpace(&base0), BM_OK);
    MemSlicePtr slice0 = nullptr;
    ASSERT_EQ(seg0.AllocLocalMemory(HYBM_LARGE_PAGE_SIZE, slice0), BM_OK);
    std::string exInfo;
    ASSERT_EQ(seg0.Export(slice0, exInfo), BM_OK);

    MemSegmentOptions opt1 = MakeOptions(1, 2, maxSize, maxSize);
    opt1.devId = 1;
    HybmAclVmmSegment seg1(opt1, 1);
    void *base1 = nullptr;
    ASSERT_EQ(seg1.ReserveMemorySpace(&base1), BM_OK);
    MemSlicePtr slice1 = nullptr;
    ASSERT_EQ(seg1.AllocLocalMemory(HYBM_LARGE_PAGE_SIZE, slice1), BM_OK);
    std::vector<std::string> all{exInfo};
    ASSERT_EQ(seg1.Import(all, nullptr), BM_OK);
    ASSERT_EQ(seg1.Mmap(), BM_OK);
    ASSERT_EQ(seg1.mappedGvaMem_.size(), 1UL);

    // 移除 rank0 的导入
    ASSERT_EQ(seg1.RemoveImported({0}), BM_OK);
    ASSERT_EQ(seg1.mappedGvaMem_.size(), 0UL);
    // 非法 rank 拒绝
    ASSERT_EQ(seg1.RemoveImported({2}), BM_INVALID_PARAM);

    // 释放本地 slice + 窗口
    ASSERT_EQ(seg1.ReleaseSliceMemory(slice1), BM_OK);
    ASSERT_EQ(seg1.slices_.size(), 0UL);
    ASSERT_EQ(seg1.UnReserveMemorySpace(), BM_OK);
    ASSERT_EQ(seg1.globalVirtualAddress_, nullptr);
    ASSERT_EQ(seg0.ReleaseSliceMemory(slice0), BM_OK);
    ASSERT_EQ(seg0.UnReserveMemorySpace(), BM_OK);
}

#if MF_ACL_VMM_UT_HAS_MOCKCPP
/* Create 分派用例需 mock InitDeviceInfo(否则 stub soc name 覆盖 socType_), 依赖 mockcpp 环境 */
TEST_F(HybmAclVmmSegmentTest, CreateDispatchesToAclVmmFor310P)
{
    MOCKER_CPP(&ock::mf::MemSegment::InitDeviceInfo, Result (*)(int32_t)).stubs().will(returnValue(BM_OK));
    MemSegment::socType_ = AscendSocType::ASCEND_310P;
    MemSegmentOptions opt = MakeOptions(0, 2, HYBM_LARGE_PAGE_SIZE, HYBM_LARGE_PAGE_SIZE);
    auto seg = MemSegment::Create(opt, 0);
    GlobalMockObject::verify();
    ASSERT_NE(seg, nullptr);
    ASSERT_NE(std::dynamic_pointer_cast<HybmAclVmmSegment>(seg), nullptr);
}
#endif

TEST_F(HybmAclVmmSegmentTest, MemoryInRangeAndLocalRange)
{
    const uint64_t maxSize = 2UL * HYBM_LARGE_PAGE_SIZE;
    MemSegmentOptions opt = MakeOptions(0, 2, maxSize, maxSize);
    HybmAclVmmSegment seg(opt, 0);
    void *base = nullptr;
    ASSERT_EQ(seg.ReserveMemorySpace(&base), BM_OK);
    MemSlicePtr slice = nullptr;
    ASSERT_EQ(seg.AllocLocalMemory(HYBM_LARGE_PAGE_SIZE, slice), BM_OK);

    // 窗口内(含 peer slot) true
    ASSERT_TRUE(seg.MemoryInRange(base, HYBM_LARGE_PAGE_SIZE));
    auto peerSlot = static_cast<uint8_t *>(base) + maxSize;
    ASSERT_TRUE(seg.MemoryInRange(peerSlot, HYBM_LARGE_PAGE_SIZE));
    // 窗口外 false
    ASSERT_FALSE(seg.MemoryInRange(static_cast<uint8_t *>(base) + 2UL * maxSize, 1));
    // 本地范围: own slot 已分配区 true, peer slot false
    ASSERT_TRUE(seg.IsLocalRange(base, HYBM_LARGE_PAGE_SIZE));
    ASSERT_FALSE(seg.IsLocalRange(peerSlot, HYBM_LARGE_PAGE_SIZE));
    // 越界 rank
    ASSERT_EQ(seg.RemoveImported({5}), BM_INVALID_PARAM);
    (void)seg.UnReserveMemorySpace();
}
} // namespace mf
} // namespace ock
