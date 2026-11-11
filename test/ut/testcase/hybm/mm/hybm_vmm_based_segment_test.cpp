/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

#include <gtest/gtest.h>
#include <mockcpp/mockcpp.hpp>

#define private   public
#define protected public
#include "hybm_vmm_based_segment.h"
#undef private
#undef protected

#include "hybm_va_manager.h"
#include "dl_acl_api.h"

#define MOCKER_CPP(api, TT) MOCKCPP_NS::mockAPI(#api, reinterpret_cast<TT>(api))

using namespace ock::mf;

namespace {
void *reserveExpected = nullptr;
void *reserveResult = nullptr;
void *releasedAddress = nullptr;
size_t reservedWindowSize = 0;
uint64_t reservedWindowFlags = 0;
int reserveReturnCode = BM_OK;

ock::mf::Result ReserveAddressForTest(void **base, size_t size, size_t, void *expected, uint64_t flags)
{
    reserveExpected = expected;
    reservedWindowSize = size;
    reservedWindowFlags = flags;
    *base = reserveResult == nullptr ? expected : reserveResult;
    return reserveReturnCode;
}

ock::mf::Result ReleaseAddressForTest(void *address)
{
    releasedAddress = address;
    return BM_OK;
}

int HalMemImportOk(drv_mem_handle_type, MemShareHandle *, uint32_t, drv_mem_handle_t **handle)
{
    *handle = reinterpret_cast<drv_mem_handle_t *>(0x1234);
    return BM_OK;
}

int HalMemMapOk(void *, size_t, size_t, drv_mem_handle_t *, uint64_t)
{
    return BM_OK;
}

int HalMemMapFail(void *, size_t, size_t, drv_mem_handle_t *, uint64_t)
{
    return BM_ERROR;
}

int HalMemReleaseOk(drv_mem_handle_t *)
{
    return BM_OK;
}

int HalMemUnmapOk(void *)
{
    return BM_OK;
}
} // namespace

class HybmVmmBasedSegmentTest : public testing::Test {
protected:
    void SetUp() override
    {
        GlobalMockObject::reset();
        HybmVaManager::GetInstance().ClearAll();
        reserveResult = nullptr;
        reserveExpected = nullptr;
        releasedAddress = nullptr;
        reserveReturnCode = BM_OK;
    }

    void TearDown() override
    {
        GlobalMockObject::verify();
        GlobalMockObject::reset();
        HybmVaManager::GetInstance().ClearAll();
    }
};

/**
* Mmap_RemoteSlice_SucceedsAndSkipsLocal
*  - 远端可达时完成映射；本 rank 记录跳过。
*/
TEST_F(HybmVmmBasedSegmentTest, Mmap_RemoteSlice_SucceedsAndSkipsLocal)
{
    MemSegmentOptions opt{};
    opt.segType = HYBM_MST_DRAM;
    opt.maxSize = HYBM_LARGE_PAGE_SIZE;
    opt.rankCnt = 2UL;
    opt.rankId = 0;
    opt.shared = true;

    HybmVmmBasedSegment seg(opt, 0);
    HybmVaManager::GetInstance().AllocReserveGva(opt.rankId, opt.maxSize * opt.rankCnt, opt.maxSize * opt.rankCnt,
                                                 HYBM_MEM_TYPE_HOST);

    HostSdmaExportInfo local{};
    local.gva = 0x1000;
    local.rankId = 0;
    local.size = HYBM_LARGE_PAGE_SIZE;
    local.segmentType = SEGMENT_TYPE_VMM;

    HostSdmaExportInfo remote{};
    remote.gva = opt.maxSize;
    remote.deviceVa = opt.maxSize;
    remote.rankId = 1;
    remote.size = HYBM_LARGE_PAGE_SIZE;
    remote.segmentType = SEGMENT_TYPE_VMM;
    remote.magic = DRAM_SLICE_EXPORT_INFO_MAGIC;
    remote.superPodId = 1;
    remote.serverId = 1;

    seg.imports_ = {local, remote};

    MOCKER_CPP(&MemSegment::CanSdmaReaches, bool (*)(MemSegment *, uint32_t, uint32_t, uint32_t))
        .stubs()
        .will(returnValue(true));
    MOCKER_CPP(&DlHalApi::HalMemImport, int (*)(drv_mem_handle_type, MemShareHandle *, uint32_t, drv_mem_handle_t **))
        .stubs()
        .will(invoke(HalMemImportOk));
    MOCKER_CPP(&DlHalApi::HalMemMap, int (*)(void *, size_t, size_t, drv_mem_handle_t *, uint64_t))
        .stubs()
        .will(invoke(HalMemMapOk));
    MOCKER_CPP(&DlHalApi::HalMemRelease, int (*)(drv_mem_handle_t *)).stubs().will(invoke(HalMemReleaseOk));
    MOCKER_CPP(&DlHalApi::HalMemUnmap, int (*)(void *)).stubs().will(invoke(HalMemUnmapOk));

    auto ret = seg.Mmap();
    EXPECT_EQ(ret, BM_OK);
    EXPECT_TRUE(seg.imports_.empty());
    EXPECT_EQ(seg.mappedGvaMem_.size(), 1U);
    EXPECT_TRUE(seg.mappedGvaMem_.count(remote.gva) == 1U);
    EXPECT_EQ(HybmVaManager::GetInstance().GetAllocCount(), 1U);
}

/**
* Mmap_MapFailure_RollsBackVaInfo
*  - 后续 HalMemMap 失败时，已登记的外部 VA 应回滚。
*/
TEST_F(HybmVmmBasedSegmentTest, Mmap_MapFailure_RollsBackVaInfo)
{
    MemSegmentOptions opt{};
    opt.segType = HYBM_MST_DRAM;
    opt.maxSize = HYBM_LARGE_PAGE_SIZE;
    opt.rankCnt = 2UL;
    opt.rankId = 0;
    opt.shared = true;

    HybmVmmBasedSegment seg(opt, 0);
    HybmVaManager::GetInstance().AllocReserveGva(opt.rankId, opt.maxSize * opt.rankCnt, opt.maxSize * opt.rankCnt,
                                                 HYBM_MEM_TYPE_HOST);

    HostSdmaExportInfo remote{};
    remote.gva = opt.maxSize;
    remote.deviceVa = opt.maxSize;
    remote.rankId = 1;
    remote.size = HYBM_LARGE_PAGE_SIZE;
    remote.segmentType = SEGMENT_TYPE_VMM;
    remote.magic = DRAM_SLICE_EXPORT_INFO_MAGIC;
    remote.superPodId = 1;
    remote.serverId = 1;

    seg.imports_.push_back(remote);

    MOCKER_CPP(&MemSegment::CanSdmaReaches, bool (*)(MemSegment *, uint32_t, uint32_t, uint32_t))
        .stubs()
        .will(returnValue(true));
    MOCKER_CPP(&DlHalApi::HalMemImport, int (*)(drv_mem_handle_type, MemShareHandle *, uint32_t, drv_mem_handle_t **))
        .stubs()
        .will(invoke(HalMemImportOk));
    MOCKER_CPP(&DlHalApi::HalMemMap, int (*)(void *, size_t, size_t, drv_mem_handle_t *, uint64_t))
        .stubs()
        .will(invoke(HalMemMapFail));
    MOCKER_CPP(&DlHalApi::HalMemRelease, int (*)(drv_mem_handle_t *)).stubs().will(invoke(HalMemReleaseOk));

    auto ret = seg.Mmap();
    EXPECT_EQ(ret, BM_ERROR);
    EXPECT_TRUE(seg.mappedGvaMem_.empty());
    EXPECT_EQ(HybmVaManager::GetInstance().GetAllocCount(), 0U);
}

/**
* Mmap_A2_HBM_CrossMachine
*  - A2跨机 Device_rdma访问HBM内存，VaManager中只记录gva和rank_id，hva和dva值0
*/
TEST_F(HybmVmmBasedSegmentTest, Mmap_A2_HBM_CrossMachine)
{
    MemSegmentOptions opt{};
    opt.segType = HYBM_MST_HBM;
    opt.maxSize = HYBM_LARGE_PAGE_SIZE;
    opt.rankCnt = 2UL;
    opt.rankId = 0;
    opt.shared = true;

    HybmVmmBasedSegment seg(opt, 0);
    HybmVaManager::GetInstance().AllocReserveGva(opt.rankId, opt.maxSize * opt.rankCnt, opt.maxSize * opt.rankCnt,
                                                 HYBM_MEM_TYPE_DEVICE);

    HostSdmaExportInfo local{};
    local.gva = 0x1000;
    local.rankId = 0;
    local.size = HYBM_LARGE_PAGE_SIZE;
    local.segmentType = SEGMENT_TYPE_VMM;

    HostSdmaExportInfo remote{};
    remote.gva = opt.maxSize;
    remote.deviceVa = opt.maxSize;
    remote.rankId = 1;
    remote.size = HYBM_LARGE_PAGE_SIZE;
    remote.segmentType = SEGMENT_TYPE_VMM;
    remote.magic = DRAM_SLICE_EXPORT_INFO_MAGIC;
    remote.superPodId = 1;
    remote.serverId = 1;

    seg.imports_ = {local, remote};

    MOCKER_CPP(&MemSegment::CanSdmaReaches, bool (*)(MemSegment *, uint32_t, uint32_t, uint32_t))
        .stubs()
        .will(returnValue(false));
    MOCKER_CPP(&DlHalApi::HalMemImport, int (*)(drv_mem_handle_type, MemShareHandle *, uint32_t, drv_mem_handle_t **))
        .stubs()
        .will(invoke(HalMemImportOk));
    MOCKER_CPP(&DlHalApi::HalMemMap, int (*)(void *, size_t, size_t, drv_mem_handle_t *, uint64_t))
        .stubs()
        .will(invoke(HalMemMapOk));
    MOCKER_CPP(&DlHalApi::HalMemRelease, int (*)(drv_mem_handle_t *)).stubs().will(invoke(HalMemReleaseOk));
    MOCKER_CPP(&DlHalApi::HalMemUnmap, int (*)(void *)).stubs().will(invoke(HalMemUnmapOk));

    auto ret = seg.Mmap();
    EXPECT_EQ(ret, BM_OK);
    EXPECT_TRUE(seg.imports_.empty());
    EXPECT_EQ(seg.mappedGvaMem_.size(), 0U);
    EXPECT_EQ(HybmVaManager::GetInstance().GetAllocCount(), 1U);

    auto outDva = HybmVaManager::GetInstance().TransformVa(remote.gva, HVM_GVA, HVM_DVA); // dva == 0
    EXPECT_EQ(outDva, 0U);
    auto outHva = HybmVaManager::GetInstance().TransformVa(remote.gva, HVM_GVA, HVM_HVA); // hva == 0
    EXPECT_EQ(outHva, 0U);
}

// ReserveLva 非56bit返回im.deviceVa
TEST_F(HybmVmmBasedSegmentTest, ReserveLva_Non56Bit_ReturnsDeviceVa)
{
    MemSegmentOptions opts;
    opts.enable56BitsGva = false;
    HybmVmmBasedSegment seg(opts, 0);
    HostSdmaExportInfo im;
    im.magic = VMM_BASE_HBM_SLICE_EXPORT_INFO_MAGIC;
    im.deviceVa = 0x1000;
    im.size = 4096; // 4096
    uint64_t lva = seg.ReserveLva(im);
    EXPECT_EQ(lva, 0x1000U);
}

// ValidateOptions HBM通过
TEST_F(HybmVmmBasedSegmentTest, ValidateOptions_Hbm_Passes)
{
    MemSegmentOptions opts;
    opts.segType = HYBM_MST_HBM;
    opts.maxSize = GB;
    opts.rankCnt = 2; // 2
    opts.size = 0;
    HybmVmmBasedSegment seg(opts, 0);
    EXPECT_EQ(seg.ValidateOptions(), BM_OK);
}

// 空析构不崩溃
TEST_F(HybmVmmBasedSegmentTest, Destroy_NoCrash)
{
    MemSegmentOptions opts;
    opts.segType = HYBM_MST_HBM;
    HybmVmmBasedSegment seg(opts, 0);
}

/* 310P 工厂分派: 统一路由到共用类(差异由类内 Is310PAclPath 分支处理, 专用段已删除) */
TEST_F(HybmVmmBasedSegmentTest, CreateDispatchesToVmmBasedFor310P)
{
    MOCKER_CPP(&ock::mf::MemSegment::InitDeviceInfo, int32_t (*)(int32_t)).stubs().will(returnValue(BM_OK));
    MemSegment::socType_ = AscendSocType::ASCEND_310P;
    MemSegmentOptions opt{};
    opt.segType = HYBM_MST_HBM;
    opt.rankId = 0;
    opt.rankCnt = 2;
    opt.maxSize = HYBM_LARGE_PAGE_SIZE;
    opt.size = HYBM_LARGE_PAGE_SIZE;
    auto seg = MemSegment::Create(opt, 0);
    GlobalMockObject::verify();
    ASSERT_NE(seg, nullptr);
    ASSERT_NE(std::dynamic_pointer_cast<HybmVmmBasedSegment>(seg), nullptr);
}

namespace {
void MockRemoteWindowReservation()
{
    MOCKER_CPP(&DlAclApi::GetAscendSocType, AscendSocType(*)()).stubs()
        .will(returnValue(AscendSocType::ASCEND_310P));
    MOCKER_CPP(&DlHalApi::HalMemAddressReserve, int(*)(void **, size_t, size_t, void *, uint64_t))
        .stubs().will(invoke(ReserveAddressForTest));
    MOCKER_CPP(&DlHalApi::HalMemAddressFree, int(*)(void *))
        .stubs().will(invoke(ReleaseAddressForTest));
}
MemSegmentOptions SmallHbmPool(uint32_t ranks)
{
    MemSegmentOptions options{};
    options.segType = HYBM_MST_HBM;
    options.rankCnt = ranks;
    options.maxSize = HYBM_LARGE_PAGE_SIZE;
    options.size = HYBM_LARGE_PAGE_SIZE;
    return options;
}
}

TEST_F(HybmVmmBasedSegmentTest, ReserveUsesHalRemoteWindowAndGiBAlignmentOn310P)
{
    MockRemoteWindowReservation();
    for (uint32_t ranks : {1U, 2U}) {
        HybmVmmBasedSegment segment(SmallHbmPool(ranks), 0);
        void *address = nullptr;
        ASSERT_EQ(segment.ReserveMemorySpace(&address), BM_OK);
        EXPECT_EQ(address, reinterpret_cast<void *>(HYBM_GVM_START_ADDR));
        EXPECT_EQ(reserveExpected, address);
        EXPECT_EQ(reservedWindowFlags, MEM_RSV_TYPE_REMOTE_MAP);
        EXPECT_EQ(reservedWindowSize, GB);
        EXPECT_EQ(segment.UnReserveMemorySpace(), BM_OK);
        EXPECT_EQ(releasedAddress, address);
        EXPECT_EQ(HybmVaManager::GetInstance().GetReservedCount(), 0U);
    }
}

TEST_F(HybmVmmBasedSegmentTest, RemoteWindowFailureClearsManagerWithoutFreeingUnownedAddress)
{
    MockRemoteWindowReservation();
    reserveReturnCode = BM_MALLOC_FAILED;
    HybmVmmBasedSegment segment(SmallHbmPool(2), 0);
    void *address = nullptr;
    EXPECT_EQ(segment.ReserveMemorySpace(&address), BM_MALLOC_FAILED);
    EXPECT_EQ(address, nullptr);
    EXPECT_EQ(releasedAddress, nullptr);
    EXPECT_EQ(HybmVaManager::GetInstance().GetReservedCount(), 0U);
}

TEST_F(HybmVmmBasedSegmentTest, RemoteWindowMismatchReleasesReturnedAddressAndClearsManager)
{
    MockRemoteWindowReservation();
    reserveResult = reinterpret_cast<void *>(HYBM_GVM_START_ADDR + GB);
    HybmVmmBasedSegment segment(SmallHbmPool(2), 0);
    void *address = nullptr;
    EXPECT_EQ(segment.ReserveMemorySpace(&address), BM_MALLOC_FAILED);
    EXPECT_EQ(address, nullptr);
    EXPECT_EQ(releasedAddress, reserveResult);
    EXPECT_EQ(HybmVaManager::GetInstance().GetReservedCount(), 0U);
}

TEST_F(HybmVmmBasedSegmentTest, RemotePoolsUseDistinctWindowsAndReuseReleasedWindow)
{
    MockRemoteWindowReservation();
    HybmVmmBasedSegment first(SmallHbmPool(2), 0), second(SmallHbmPool(2), 1), reused(SmallHbmPool(2), 2);
    void *a = nullptr, *b = nullptr, *c = nullptr;
    ASSERT_EQ(first.ReserveMemorySpace(&a), BM_OK);
    ASSERT_EQ(second.ReserveMemorySpace(&b), BM_OK);
    EXPECT_EQ(reinterpret_cast<uint64_t>(b), reinterpret_cast<uint64_t>(a) + GB);
    EXPECT_EQ(first.UnReserveMemorySpace(), BM_OK);
    ASSERT_EQ(reused.ReserveMemorySpace(&c), BM_OK);
    EXPECT_EQ(c, a);
    EXPECT_EQ(second.UnReserveMemorySpace(), BM_OK);
    EXPECT_EQ(reused.UnReserveMemorySpace(), BM_OK);
    EXPECT_EQ(HybmVaManager::GetInstance().GetReservedCount(), 0U);
}
