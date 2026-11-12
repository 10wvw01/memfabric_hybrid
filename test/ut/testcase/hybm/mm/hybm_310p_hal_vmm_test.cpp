#include <gtest/gtest.h>
#include <mockcpp/mockcpp.hpp>

#define private public
#define protected public
#include "hybm_vmm_based_segment.h"
#undef protected
#undef private

#include "dl_acl_api.h"
#include "hybm_va_manager.h"
#include <cstring>

#define MOCKER_CPP(api, TT) MOCKCPP_NS::mockAPI(#api, reinterpret_cast<TT>(api))
using namespace ock::mf;

namespace
{
constexpr uint64_t SHARED_HANDLE = 0x123456789abcdef0ULL;
constexpr uint64_t IMPORT_ADDRESS = 0x280040000000ULL;
int attributeResult = BM_OK;
int mapResult = BM_OK;
int importResult = BM_OK;
int translateResult = BM_OK;
int lastContext = -1;
uint32_t releaseCount = 0;
uint32_t unmapCount = 0;

int ExportHandle(drv_mem_handle_t *, uint64_t *shareable)
{
    *shareable = SHARED_HANDLE;
    return BM_OK;
}

int SetAttribute(uint64_t shareable, ShareHandleAttrType type, ShareHandleAttr attr)
{
    EXPECT_EQ(shareable, SHARED_HANDLE);
    EXPECT_EQ(type, SHR_HANDLE_ATTR_NO_WLIST_IN_SERVER);
    EXPECT_EQ(attr.enableFlag, SHR_HANDLE_NO_WLIST_ENABLE);
    return attributeResult;
}

int SetContext(int32_t device, bool)
{
    lastContext = device;
    return BM_OK;
}

int ImportHandle(uint64_t shareable, uint32_t device, drv_mem_handle_t **handle)
{
    EXPECT_EQ(shareable, SHARED_HANDLE);
    EXPECT_EQ(device, 7U);
    if (importResult == BM_OK) *handle = reinterpret_cast<drv_mem_handle_t *>(0x1234);
    return importResult;
}

int TranslateDevice(int32_t device, int32_t *logic)
{
    EXPECT_EQ(device, 0);
    *logic = 7;
    return translateResult;
}

int MapImported(void *address, size_t size, size_t offset, drv_mem_handle_t *, uint64_t flags)
{
    EXPECT_EQ(address, reinterpret_cast<void *>(IMPORT_ADDRESS));
    EXPECT_EQ(size, HYBM_LARGE_PAGE_SIZE);
    EXPECT_EQ(offset, 0U);
    EXPECT_EQ(flags, 0U);
    return mapResult;
}

int SetImportedAccess(void *address, size_t, drv_mem_access_desc *desc, size_t count)
{
    EXPECT_EQ(address, reinterpret_cast<void *>(IMPORT_ADDRESS));
    EXPECT_EQ(count, 1U);
    EXPECT_EQ(desc->location.id, 7U);
    EXPECT_EQ(desc->location.side, MEM_DEV_SIDE);
    EXPECT_EQ(desc->type, MEM_ACCESS_TYPE_READWRITE);
    return BM_OK;
}

int ReleaseImported(drv_mem_handle_t *)
{
    if (translateResult != BM_OK) EXPECT_EQ(unmapCount, 1U);
    ++releaseCount;
    return BM_OK;
}

int UnmapImported(void *)
{
    ++unmapCount;
    return BM_OK;
}
}

class Hybm310PHalVmmTest : public testing::Test
{
protected:
    int savedDevice = 0, savedLogic = 0, savedPhysical = 0;

    void SetUp() override
    {
        GlobalMockObject::reset();
        HybmVaManager::GetInstance().ClearAll();
        savedDevice = MemSegment::deviceId_;
        savedLogic = MemSegment::logicDeviceId_;
        savedPhysical = MemSegment::devicePhyId_;
        MemSegment::deviceId_ = 0;
        MemSegment::logicDeviceId_ = 7;
        MemSegment::devicePhyId_ = 4;
        attributeResult = mapResult = importResult = translateResult = BM_OK;
        releaseCount = unmapCount = 0;
        lastContext = -1;
        MOCKER_CPP(&DlAclApi::GetAscendSocType, AscendSocType(*)()).stubs().will(returnValue(ASCEND_310P));
        MOCKER_CPP(&DlHalApi::HalMemRelease, int(*)(drv_mem_handle_t *)).stubs().will(invoke(ReleaseImported));
        MOCKER_CPP(&DlHalApi::HalMemUnmap, int(*)(void *)).stubs().will(invoke(UnmapImported));
    }

    void TearDown() override
    {
        GlobalMockObject::verify();
        GlobalMockObject::reset();
        MemSegment::deviceId_ = savedDevice;
        MemSegment::logicDeviceId_ = savedLogic;
        MemSegment::devicePhyId_ = savedPhysical;
        HybmVaManager::GetInstance().ClearAll();
    }

    MemSlicePtr PrepareExport(HybmVmmBasedSegment &segment)
    {
        auto slice = std::make_shared<MemSlice>(0, HYBM_MEM_TYPE_DEVICE, MEM_PT_TYPE_GVM,
                                               IMPORT_ADDRESS, IMPORT_ADDRESS, HYBM_LARGE_PAGE_SIZE);
        EXPECT_EQ(HybmVaManager::GetInstance().AddVaInfo(
            {IMPORT_ADDRESS, IMPORT_ADDRESS, IMPORT_ADDRESS, HYBM_LARGE_PAGE_SIZE, HYBM_MEM_TYPE_DEVICE}, 0), BM_OK);
        segment.slices_.emplace(0, MemSliceStatus(slice, reinterpret_cast<void *>(0x1234)));
        MOCKER_CPP(&DlHalApi::HalMemExportToShareableHandle, int(*)(drv_mem_handle_t *, uint64_t *))
            .stubs().will(invoke(ExportHandle));
        MOCKER_CPP(&DlHalApi::HalMemShareHandleSetAttribute, int(*)(uint64_t, ShareHandleAttrType, ShareHandleAttr))
            .stubs().will(invoke(SetAttribute));
        return slice;
    }

    HostSdmaExportInfo PrepareImport()
    {
        HostSdmaExportInfo info{};
        info.devicePhyId = 5;
        info.deviceUserId = 1;
        info.rankId = 1;
        info.size = HYBM_LARGE_PAGE_SIZE;
        std::memcpy(info.shareHandle.share_info, &SHARED_HANDLE, sizeof(SHARED_HANDLE));
        MOCKER_CPP(&DlAclApi::AclrtSetDevice, ock::mf::Result(*)(int32_t, bool)).stubs().will(invoke(SetContext));
        MOCKER_CPP(&DlAclApi::AclrtDeviceCanAccessPeer, ock::mf::Result(*)(int32_t *, int32_t, int32_t))
            .stubs().will(returnValue(BM_OK));
        MOCKER_CPP(&DlAclApi::RtGetLogicDevIdByUserDevId, ock::mf::Result(*)(int32_t, int32_t *))
            .stubs().will(invoke(TranslateDevice));
        MOCKER_CPP(&DlHalApi::HalMemImportFromShareableHandle, int(*)(uint64_t, uint32_t, drv_mem_handle_t **))
            .stubs().will(invoke(ImportHandle));
        MOCKER_CPP(&DlHalApi::HalMemMap, int(*)(void *, size_t, size_t, drv_mem_handle_t *, uint64_t))
            .stubs().will(invoke(MapImported));
        MOCKER_CPP(&DlHalApi::HalMemSetAccess, int(*)(void *, size_t, drv_mem_access_desc *, size_t))
            .stubs().will(invoke(SetImportedAccess));
        return info;
    }
};

TEST_F(Hybm310PHalVmmTest, ExportPreservesUint64WireHandleAndSharingPolicy)
{
    HybmVmmBasedSegment segment(MemSegmentOptions{}, 0);
    auto slice = PrepareExport(segment);
    MemShareHandle output{};
    EXPECT_EQ(segment.ExportInner(slice, output), BM_OK);
    uint64_t handle = 0;
    std::memcpy(&handle, output.share_info, sizeof(handle));
    EXPECT_EQ(handle, SHARED_HANDLE);
    EXPECT_EQ(segment.exportMap_.size(), 1U);
    segment.slices_.clear();
}

TEST_F(Hybm310PHalVmmTest, ExportAttributeFailureDoesNotPublishHandle)
{
    HybmVmmBasedSegment segment(MemSegmentOptions{}, 0);
    auto slice = PrepareExport(segment);
    attributeResult = BM_ERROR;
    MemShareHandle output{};
    EXPECT_EQ(segment.ExportInner(slice, output), BM_DL_FUNCTION_FAILED);
    EXPECT_TRUE(segment.exportMap_.empty());
    segment.slices_.clear();
}

TEST_F(Hybm310PHalVmmTest, ImportUsesDriverIdAndRestoresLocalContext)
{
    auto info = PrepareImport();
    HybmVmmBasedSegment segment(MemSegmentOptions{}, 0);
    void *handle = nullptr;
    EXPECT_EQ(segment.MmapImport310P(info, IMPORT_ADDRESS, handle), BM_OK);
    EXPECT_EQ(lastContext, 0);
    EXPECT_EQ(handle, reinterpret_cast<void *>(0x1234));
    EXPECT_EQ(releaseCount, 0U);
}

TEST_F(Hybm310PHalVmmTest, ImportFailureRestoresContextWithoutReleasingUnownedHandle)
{
    auto info = PrepareImport();
    importResult = BM_ERROR;
    HybmVmmBasedSegment segment(MemSegmentOptions{}, 0);
    void *handle = nullptr;
    EXPECT_EQ(segment.MmapImport310P(info, IMPORT_ADDRESS, handle), BM_ERROR);
    EXPECT_EQ(lastContext, 0);
    EXPECT_EQ(handle, nullptr);
    EXPECT_EQ(releaseCount, 0U);
}

TEST_F(Hybm310PHalVmmTest, MapFailureReleasesImportedHandle)
{
    auto info = PrepareImport();
    mapResult = BM_ERROR;
    HybmVmmBasedSegment segment(MemSegmentOptions{}, 0);
    void *handle = nullptr;
    EXPECT_EQ(segment.MmapImport310P(info, IMPORT_ADDRESS, handle), BM_ERROR);
    EXPECT_EQ(releaseCount, 1U);
    EXPECT_EQ(unmapCount, 0U);
}

TEST_F(Hybm310PHalVmmTest, DeviceTranslationFailureUnmapsBeforeReleasingHandle)
{
    auto info = PrepareImport();
    translateResult = BM_ERROR;
    HybmVmmBasedSegment segment(MemSegmentOptions{}, 0);
    void *handle = nullptr;
    EXPECT_EQ(segment.MmapImport310P(info, IMPORT_ADDRESS, handle), BM_ERROR);
    EXPECT_EQ(unmapCount, 1U);
    EXPECT_EQ(releaseCount, 1U);
}

TEST_F(Hybm310PHalVmmTest, MissingUint64SymbolsReturnUnloaded)
{
    auto exportFn = DlHalApi::pHalMemExportToShareableHandle;
    auto importFn = DlHalApi::pHalMemImportFromShareableHandle;
    DlHalApi::pHalMemExportToShareableHandle = nullptr;
    DlHalApi::pHalMemImportFromShareableHandle = nullptr;
    uint64_t shareable = 0;
    drv_mem_handle_t *handle = nullptr;
    EXPECT_EQ(DlHalApi::HalMemExportToShareableHandle(handle, &shareable), BM_UNDER_API_UNLOAD);
    EXPECT_EQ(DlHalApi::HalMemImportFromShareableHandle(shareable, 7, &handle), BM_UNDER_API_UNLOAD);
    DlHalApi::pHalMemExportToShareableHandle = exportFn;
    DlHalApi::pHalMemImportFromShareableHandle = importFn;
}
