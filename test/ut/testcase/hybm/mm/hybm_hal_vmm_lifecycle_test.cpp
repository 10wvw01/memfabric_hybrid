#include <gtest/gtest.h>
#include <mockcpp/mockcpp.hpp>
#include <limits>
#include <cstring>
#include <vector>
#include "hybm_gva.h"
#include "hybm.h"
#include "hybm_common_include.h"
#include "hybm_ptracer.h"
#include "hybm_stream_manager.h"
#include "dl_api.h"

#define private   public
#define protected public
#include "hybm_vmm_handle_registry.h"
#include "hybm_dev_user_legacy_segment.h"
#undef private
#undef protected

#define MOCK_API(api) MOCKCPP_NS::mockAPI(#api, reinterpret_cast<decltype(&api)>(&api))

namespace ock {
namespace mf {
int32_t HybmModernInitMetaGva(uint32_t, void **, size_t, void **);
} // namespace mf
} // namespace ock

using namespace ock::mf;

namespace {
constexpr uintptr_t ADDRESS = 0x280000000000;
constexpr uint64_t SHARED = 0x123456789ABCDEULL;
drv_mem_handle_t *const HANDLE = reinterpret_cast<drv_mem_handle_t *>(0x1234);

AscendSocType testSoc = ASCEND_310P;
AscendSocType TestSoc()
{
    return testSoc;
}

struct HalState {
    int translate{0}, reserve{0}, create{0}, fallback{0}, map{0}, access{0};
    int import{0}, exportHandle{0}, attribute{0}, unmap{0}, release{0}, free{0};
    int creates{0}, reserves{0}, imports{0}, ipcCalls{0};
    int32_t driver{7};
    bool nullHandle{false}, wrongAddress{false};
    uint64_t ipcFlag{0};
    std::vector<std::string> order;
    std::vector<uint32_t> memoryTypes;
    std::vector<int32_t> contexts;
} hal;

ock::mf::Result Translate(int32_t user, int32_t *driver)
{
    EXPECT_EQ(user, 0);
    *driver = hal.driver;
    return hal.translate;
}

ock::mf::Result Context(int32_t user, bool)
{
    hal.contexts.push_back(user);
    return BM_OK;
}

int Reserve(void **address, size_t size, size_t, void *expected, uint64_t)
{
    hal.order.push_back("reserve");
    EXPECT_EQ(size % HYBM_LARGE_PAGE_SIZE, 0U);
    *address = expected && !hal.wrongAddress ? expected
                                             : reinterpret_cast<void *>(ADDRESS + hal.reserves * HYBM_LARGE_PAGE_SIZE);
    ++hal.reserves;
    return hal.reserve;
}

int Create(drv_mem_handle_t **handle, size_t, drv_mem_prop *prop, uint64_t)
{
    hal.order.push_back("create");
    EXPECT_EQ(prop->devid, static_cast<uint32_t>(hal.driver));
    EXPECT_EQ(prop->side, MEM_DEV_SIDE);
    hal.memoryTypes.push_back(prop->mem_type);
    int ret = ++hal.creates == 1 ? hal.create : hal.fallback;
    *handle = ret == 0 && !hal.nullHandle ? HANDLE : nullptr;
    return ret;
}

int Map(void *, size_t, size_t, drv_mem_handle_t *handle, uint64_t)
{
    hal.order.push_back("map");
    EXPECT_EQ(handle, HANDLE);
    return hal.map;
}

int Access(void *, size_t, drv_mem_access_desc *desc, size_t count)
{
    hal.order.push_back("access");
    EXPECT_EQ(count, 1U);
    EXPECT_EQ(desc->location.id, static_cast<uint32_t>(hal.driver));
    EXPECT_EQ(desc->location.side, MEM_DEV_SIDE);
    return hal.access;
}

int Import(uint64_t shared, uint32_t driver, drv_mem_handle_t **handle)
{
    hal.order.push_back("import");
    EXPECT_EQ(shared, SHARED);
    EXPECT_EQ(driver, static_cast<uint32_t>(hal.driver));
    ++hal.imports;
    *handle = hal.import == 0 && !hal.nullHandle ? HANDLE : nullptr;
    return hal.import;
}

int Export(drv_mem_handle_t *handle, uint64_t *shared)
{
    hal.order.push_back("export");
    EXPECT_EQ(handle, HANDLE);
    *shared = SHARED;
    return hal.exportHandle;
}

int Attribute(uint64_t shared, ShareHandleAttrType type, ShareHandleAttr attr)
{
    hal.order.push_back("attribute");
    EXPECT_EQ(shared, SHARED);
    EXPECT_EQ(type, SHR_HANDLE_ATTR_NO_WLIST_IN_SERVER);
    EXPECT_EQ(attr.enableFlag, SHR_HANDLE_NO_WLIST_ENABLE);
    return hal.attribute;
}

int Unmap(void *)
{
    hal.order.push_back("unmap");
    return hal.unmap;
}

int Release(drv_mem_handle_t *)
{
    hal.order.push_back("release");
    return hal.release;
}

int FreeAddress(void *)
{
    hal.order.push_back("free");
    return hal.free;
}

int32_t FakeMetaInit(uint16_t, uint64_t, uint64_t &base, AscendSocType, void **handle)
{
    base = HYBM_DEVICE_META_ADDR;
    *handle = HANDLE;
    return BM_OK;
}

ock::mf::Result Ipc(void **, const char *, uint64_t flag)
{
    ++hal.ipcCalls;
    hal.ipcFlag = flag;
    return BM_ERROR;
}
} // namespace

class HybmHalVmmLifecycleTest : public testing::Test {
protected:
    int oldUser_{-1}, oldPhysical_{-1};

    void SetUp() override
    {
        GlobalMockObject::reset();
        hal = {};
        testSoc = ASCEND_310P;
        MOCK_API(DlAclApi::GetAscendSocType).stubs().will(invoke(TestSoc));
        oldUser_ = MemSegment::deviceId_;
        oldPhysical_ = MemSegment::devicePhyId_;
        MemSegment::deviceId_ = 0;
        MemSegment::devicePhyId_ = 2;
        VmmHandleRegistry::map_.clear();
        MOCK_API(DlAclApi::RtGetLogicDevIdByUserDevId).stubs().will(invoke(Translate));
        MOCK_API(DlAclApi::AclrtSetDevice).stubs().will(invoke(Context));
        MOCK_API(DlHalApi::HalMemAddressReserve).stubs().will(invoke(Reserve));
        MOCK_API(DlHalApi::HalMemCreate).stubs().will(invoke(Create));
        MOCK_API(DlHalApi::HalMemMap).stubs().will(invoke(Map));
        MOCK_API(DlHalApi::HalMemSetAccess).stubs().will(invoke(Access));
        MOCK_API(DlHalApi::HalMemImportFromShareableHandle).stubs().will(invoke(Import));
        MOCK_API(DlHalApi::HalMemExportToShareableHandle).stubs().will(invoke(Export));
        MOCK_API(DlHalApi::HalMemShareHandleSetAttribute).stubs().will(invoke(Attribute));
        MOCK_API(DlHalApi::HalMemUnmap).stubs().will(invoke(Unmap));
        MOCK_API(DlHalApi::HalMemRelease).stubs().will(invoke(Release));
        MOCK_API(DlHalApi::HalMemAddressFree).stubs().will(invoke(FreeAddress));
        MOCK_API(DlAclApi::AclrtMallocPhysical).expects(never());
        MOCK_API(DlAclApi::AclrtFreePhysical).expects(never());
        MOCK_API(DlAclApi::AclrtReserveMemAddress).expects(never());
        MOCK_API(DlAclApi::AclrtReleaseMemAddress).expects(never());
        MOCK_API(DlAclApi::AclrtMapMem).expects(never());
        MOCK_API(DlAclApi::AclrtUnmapMem).expects(never());
        MOCK_API(DlAclApi::AclrtMemSetAccess).expects(never());
        MOCK_API(DlAclApi::AclrtMemImportFromShareableHandle).expects(never());
        MOCK_API(DlAclApi::AclrtMemExportToShareableHandle).expects(never());
    }

    void TearDown() override
    {
        VmmHandleRegistry::map_.clear();
        MemSegment::deviceId_ = oldUser_;
        MemSegment::devicePhyId_ = oldPhysical_;
        GlobalMockObject::verify();
        GlobalMockObject::reset();
    }

    UserHbmExportSliceInfo Slice(uint32_t peer = 3)
    {
        UserHbmExportSliceInfo info{};
        info.rankId = 1;
        info.devicePhyId = peer;
        info.shareableHandle = SHARED;
        info.size = HYBM_LARGE_PAGE_SIZE;
        strcpy(info.name, "hal-trans-test");
        return info;
    }

    HalVmmBuffer Registered()
    {
        HalVmmBuffer buffer{reinterpret_cast<void *>(ADDRESS), HANDLE, HYBM_LARGE_PAGE_SIZE, true};
        VmmHandleRegistry::RegisterHal(buffer, 0);
        return buffer;
    }
};

TEST_F(HybmHalVmmLifecycleTest, AllocateAndFreeUseOnlyHalAndDriverDevice)
{
    HalVmmBuffer buffer;
    ASSERT_EQ(AllocateHalVmm(7, 1, nullptr, buffer), BM_OK);
    EXPECT_EQ(buffer.size, HYBM_LARGE_PAGE_SIZE);
    EXPECT_EQ(ReleaseHalVmm(buffer), BM_OK);
    EXPECT_EQ(hal.order, (std::vector<std::string>{"reserve", "create", "map", "access", "unmap", "release", "free"}));
    EXPECT_TRUE(hal.contexts.empty());
    EXPECT_EQ(buffer.address, nullptr);
}

TEST_F(HybmHalVmmLifecycleTest, OnlyOutOfMemoryFallsBackToDdr)
{
    hal.create = HAL_OUT_OF_MEMORY_ERROR;
    HalVmmBuffer buffer;
    ASSERT_EQ(AllocateHalVmm(7, HYBM_LARGE_PAGE_SIZE, nullptr, buffer), BM_OK);
    EXPECT_EQ(hal.memoryTypes, (std::vector<uint32_t>{MEM_P2P_DDR_TYPE, MEM_DDR_TYPE}));
    EXPECT_EQ(ReleaseHalVmm(buffer), BM_OK);
}

TEST_F(HybmHalVmmLifecycleTest, OtherCreateErrorsDoNotRetry)
{
    hal.create = 8;
    HalVmmBuffer buffer;
    EXPECT_EQ(AllocateHalVmm(7, HYBM_LARGE_PAGE_SIZE, nullptr, buffer), 8);
    EXPECT_EQ(hal.creates, 1);
    EXPECT_EQ(hal.order.back(), "free");
}

TEST_F(HybmHalVmmLifecycleTest, BothAllocationTypesFailAndFreeReservedAddress)
{
    hal.create = hal.fallback = HAL_OUT_OF_MEMORY_ERROR;
    HalVmmBuffer buffer;
    EXPECT_EQ(AllocateHalVmm(7, HYBM_LARGE_PAGE_SIZE, nullptr, buffer), HAL_OUT_OF_MEMORY_ERROR);
    EXPECT_EQ(hal.creates, 2);
    EXPECT_EQ(hal.order.back(), "free");
    EXPECT_EQ(buffer.address, nullptr);
}

TEST_F(HybmHalVmmLifecycleTest, InvalidSizeStopsBeforeReserve)
{
    HalVmmBuffer buffer;
    EXPECT_EQ(AllocateHalVmm(7, 0, nullptr, buffer), BM_INVALID_PARAM);
    EXPECT_EQ(AllocateHalVmm(7, std::numeric_limits<size_t>::max(), nullptr, buffer), BM_INVALID_PARAM);
    EXPECT_TRUE(hal.order.empty());
}

TEST_F(HybmHalVmmLifecycleTest, ReserveFailureDoesNotReleaseUnownedAddress)
{
    hal.reserve = BM_ERROR;
    HalVmmBuffer buffer;
    EXPECT_NE(AllocateHalVmm(7, 1, nullptr, buffer), BM_OK);
    EXPECT_EQ(hal.order, (std::vector<std::string>{"reserve"}));
}

TEST_F(HybmHalVmmLifecycleTest, ExpectedMetadataAddressMustMatch)
{
    hal.wrongAddress = true;
    HalVmmBuffer buffer;
    EXPECT_NE(AllocateHalVmm(7, 1, reinterpret_cast<void *>(HYBM_DEVICE_META_ADDR), buffer), BM_OK);
    EXPECT_EQ(hal.order, (std::vector<std::string>{"reserve", "free"}));
}

TEST_F(HybmHalVmmLifecycleTest, MapFailureReleasesHandleThenAddress)
{
    hal.map = BM_ERROR;
    HalVmmBuffer buffer;
    EXPECT_NE(AllocateHalVmm(7, 1, nullptr, buffer), BM_OK);
    EXPECT_EQ(hal.order, (std::vector<std::string>{"reserve", "create", "map", "release", "free"}));
}

TEST_F(HybmHalVmmLifecycleTest, AccessFailureUnmapsBeforeReleasingHandle)
{
    hal.access = BM_ERROR;
    HalVmmBuffer buffer;
    EXPECT_NE(AllocateHalVmm(7, 1, nullptr, buffer), BM_OK);
    EXPECT_EQ(hal.order, (std::vector<std::string>{"reserve", "create", "map", "access", "unmap", "release", "free"}));
}

TEST_F(HybmHalVmmLifecycleTest, ReleaseFailureRetriesFromOwnedStage)
{
    auto buffer = Registered();
    hal.release = BM_ERROR;
    EXPECT_EQ(VmmHandleRegistry::Release(buffer.address), BM_ERROR);
    EXPECT_EQ(VmmHandleRegistry::map_.size(), 1U);
    hal.release = BM_OK;
    EXPECT_EQ(VmmHandleRegistry::Release(buffer.address), BM_OK);
    EXPECT_EQ(hal.order, (std::vector<std::string>{"unmap", "release", "release", "free"}));
    EXPECT_TRUE(VmmHandleRegistry::map_.empty());
}

TEST_F(HybmHalVmmLifecycleTest, UnmapFailureKeepsPhysicalHandle)
{
    auto buffer = Registered();
    hal.unmap = BM_ERROR;
    EXPECT_EQ(VmmHandleRegistry::Release(buffer.address), BM_ERROR);
    EXPECT_EQ(hal.order, (std::vector<std::string>{"unmap"}));
    EXPECT_EQ(VmmHandleRegistry::map_.size(), 1U);
    hal.unmap = BM_OK;
    EXPECT_EQ(VmmHandleRegistry::Release(buffer.address), BM_OK);
}

TEST_F(HybmHalVmmLifecycleTest, AddressFreeFailureDoesNotReleaseHandleTwice)
{
    auto buffer = Registered();
    hal.free = BM_ERROR;
    EXPECT_EQ(VmmHandleRegistry::Release(buffer.address), BM_ERROR);
    hal.free = BM_OK;
    EXPECT_EQ(VmmHandleRegistry::Release(buffer.address), BM_OK);
    EXPECT_EQ(hal.order, (std::vector<std::string>{"unmap", "release", "free", "free"}));
}

TEST_F(HybmHalVmmLifecycleTest, ShareHandleUses64BitNoneAndSharingAttribute)
{
    auto buffer = Registered();
    uint64_t shared = 0;
    EXPECT_EQ(VmmHandleRegistry::ExportShareableHandle(buffer.address, &shared, buffer.size), BM_OK);
    EXPECT_EQ(shared, SHARED);
    EXPECT_EQ(hal.order, (std::vector<std::string>{"export", "attribute"}));
}

TEST_F(HybmHalVmmLifecycleTest, ExportFailureIsDifferentFromUnregisteredAddress)
{
    auto buffer = Registered();
    uint64_t shared = 0;
    hal.exportHandle = BM_ERROR;
    EXPECT_EQ(VmmHandleRegistry::ExportShareableHandle(buffer.address, &shared), BM_DL_FUNCTION_FAILED);
    EXPECT_EQ(VmmHandleRegistry::ExportShareableHandle(reinterpret_cast<void *>(ADDRESS + 1), &shared), -1);
}

TEST_F(HybmHalVmmLifecycleTest, SharingAttributeFailureDoesNotPublishHandle)
{
    auto buffer = Registered();
    uint64_t shared = 0;
    hal.attribute = BM_ERROR;
    EXPECT_EQ(VmmHandleRegistry::ExportShareableHandle(buffer.address, &shared), BM_DL_FUNCTION_FAILED);
    EXPECT_EQ(shared, 0U);
}

TEST_F(HybmHalVmmLifecycleTest, OversizedRegistrationStopsBeforeExport)
{
    auto buffer = Registered();
    uint64_t shared = 0;
    EXPECT_EQ(VmmHandleRegistry::ExportShareableHandle(buffer.address, &shared, buffer.size + 1),
              BM_DL_FUNCTION_FAILED);
    EXPECT_TRUE(hal.order.empty());
}

TEST_F(HybmHalVmmLifecycleTest, SameCardImportKeepsLocalContext)
{
    HybmDevUserLegacySegment segment(MemSegmentOptions{}, 0);
    void *address = nullptr;
    ASSERT_EQ(segment.ImportVmmSlice(Slice(), address), BM_OK);
    EXPECT_TRUE(hal.contexts.empty());
    EXPECT_EQ(hal.order, (std::vector<std::string>{"reserve", "import", "map", "access"}));
    segment.CleanupImportedVmm("hal-trans-test");
    EXPECT_TRUE(segment.importedVmmInfo_.empty());
}

TEST_F(HybmHalVmmLifecycleTest, CrossCardImportAlsoKeepsLocalContext)
{
    HybmDevUserLegacySegment segment(MemSegmentOptions{}, 0);
    void *address = nullptr;
    ASSERT_EQ(segment.ImportVmmSlice(Slice(6), address), BM_OK);
    EXPECT_TRUE(hal.contexts.empty());
    EXPECT_EQ(hal.imports, 1);
}

TEST_F(HybmHalVmmLifecycleTest, TranslationFailureStopsBeforeReserve)
{
    hal.translate = BM_ERROR;
    HybmDevUserLegacySegment segment(MemSegmentOptions{}, 0);
    void *address = nullptr;
    EXPECT_NE(segment.ImportVmmSlice(Slice(), address), BM_OK);
    EXPECT_TRUE(hal.order.empty());
}

TEST_F(HybmHalVmmLifecycleTest, ImportFailureFreesOnlyReservedAddress)
{
    hal.import = BM_ERROR;
    HybmDevUserLegacySegment segment(MemSegmentOptions{}, 0);
    void *address = nullptr;
    EXPECT_NE(segment.ImportVmmSlice(Slice(), address), BM_OK);
    EXPECT_EQ(hal.order, (std::vector<std::string>{"reserve", "import", "free"}));
    EXPECT_TRUE(hal.contexts.empty());
}

TEST_F(HybmHalVmmLifecycleTest, ImportedMapFailureReleasesOwnedHandle)
{
    hal.map = BM_ERROR;
    HybmDevUserLegacySegment segment(MemSegmentOptions{}, 0);
    void *address = nullptr;
    EXPECT_NE(segment.ImportVmmSlice(Slice(), address), BM_OK);
    EXPECT_EQ(hal.order, (std::vector<std::string>{"reserve", "import", "map", "release", "free"}));
}

TEST_F(HybmHalVmmLifecycleTest, ImportedAccessFailureDoesNotReturnMappedAddress)
{
    hal.access = BM_ERROR;
    HybmDevUserLegacySegment segment(MemSegmentOptions{}, 0);
    void *address = nullptr;
    EXPECT_NE(segment.ImportVmmSlice(Slice(), address), BM_OK);
    EXPECT_EQ(address, nullptr);
    EXPECT_EQ(hal.order.back(), "free");
}

TEST_F(HybmHalVmmLifecycleTest, CleanupMultipleImportsDoesNotInvalidateIteration)
{
    HybmDevUserLegacySegment segment(MemSegmentOptions{}, 0);
    void *address = nullptr;
    auto first = Slice();
    ASSERT_EQ(segment.ImportVmmSlice(first, address), BM_OK);
    strcpy(first.name, "second-hal-import");
    ASSERT_EQ(segment.ImportVmmSlice(first, address), BM_OK);
    segment.CloseMemory();
    EXPECT_TRUE(segment.importedVmmInfo_.empty());
}

TEST_F(HybmHalVmmLifecycleTest, MetadataUsesFixedAddressAndDriverPermissions)
{
    void *address = nullptr, *handle = nullptr;
    EXPECT_EQ(HybmModernInitMetaGva(7, &address, HYBM_DEVICE_INFO_SIZE, &handle), BM_OK);
    EXPECT_EQ(address, reinterpret_cast<void *>(HYBM_DEVICE_META_ADDR));
    EXPECT_EQ(handle, HANDLE);
    EXPECT_TRUE(hal.contexts.empty());
}

TEST_F(HybmHalVmmLifecycleTest, PublicBufferAllocatorSelectsHalFor310P)
{
    MOCK_API(DlAclApi::GetAscendSocType).stubs().will(returnValue(ASCEND_310P));
    void *address = hybm_alloc_vmm_buffer(0, 4, 4096);
    ASSERT_NE(address, nullptr);
    EXPECT_EQ(hal.contexts, (std::vector<int32_t>{0}));
    EXPECT_EQ(VmmHandleRegistry::map_.size(), 1U);
    hybm_free_vmm_buffer(address, 4096);
    EXPECT_TRUE(VmmHandleRegistry::map_.empty());
}

TEST_F(HybmHalVmmLifecycleTest, NegativeDriverDeviceStopsBeforeReserve)
{
    hal.driver = -1;
    HybmDevUserLegacySegment segment(MemSegmentOptions{}, 0);
    void *address = nullptr;
    EXPECT_NE(segment.ImportVmmSlice(Slice(), address), BM_OK);
    EXPECT_TRUE(hal.order.empty());
}

TEST_F(HybmHalVmmLifecycleTest, NullImportedHandleFreesOnlyReservedAddress)
{
    hal.nullHandle = true;
    HybmDevUserLegacySegment segment(MemSegmentOptions{}, 0);
    void *address = nullptr;
    EXPECT_NE(segment.ImportVmmSlice(Slice(), address), BM_OK);
    EXPECT_EQ(hal.order, (std::vector<std::string>{"reserve", "import", "free"}));
}

TEST_F(HybmHalVmmLifecycleTest, NullCreatedHandleDoesNotReturnSuccess)
{
    hal.nullHandle = true;
    HalVmmBuffer buffer;
    EXPECT_NE(AllocateHalVmm(7, 1, nullptr, buffer), BM_OK);
    EXPECT_EQ(hal.order, (std::vector<std::string>{"reserve", "create", "free"}));
}

TEST_F(HybmHalVmmLifecycleTest, SameCardIpcCompatibilityUsesPhysicalNumbers)
{
    MOCK_API(DlAclApi::GetAscendSocType).stubs().will(returnValue(ASCEND_310P));
    MOCKCPP_NS::mockAPI("CanSdmaReaches",
                        reinterpret_cast<bool (*)(uint32_t, uint32_t, uint32_t)>(&MemSegment::CanSdmaReaches))
        .stubs()
        .will(returnValue(true));
    MOCK_API(DlAclApi::AclrtIpcMemImportByKey).stubs().will(invoke(Ipc));
    MemSegmentOptions options{};
    options.rankCnt = 2;
    options.dataOpType = HYBM_DOP_TYPE_SDMA;
    HybmDevUserLegacySegment segment(options, 0);
    auto info = Slice();
    info.shareableHandle = 0;
    MemSlicePtr slice;
    EXPECT_EQ(segment.ImportSliceInfo(std::string(reinterpret_cast<char *>(&info), sizeof(info)), slice),
              BM_DL_FUNCTION_FAILED);
    EXPECT_EQ(hal.ipcCalls, 1);
    EXPECT_EQ(hal.ipcFlag, ACL_RT_IPC_MEM_IMPORT_FLAG_DEFAULT);
}

TEST_F(HybmHalVmmLifecycleTest, CrossCardIpcCompatibilityUsesPhysicalNumbers)
{
    MOCK_API(DlAclApi::GetAscendSocType).stubs().will(returnValue(ASCEND_310P));
    MOCKCPP_NS::mockAPI("CanSdmaReaches",
                        reinterpret_cast<bool (*)(uint32_t, uint32_t, uint32_t)>(&MemSegment::CanSdmaReaches))
        .stubs()
        .will(returnValue(true));
    MOCK_API(DlAclApi::AclrtIpcMemImportByKey).stubs().will(invoke(Ipc));
    MemSegment::deviceId_ = 1;
    MemSegmentOptions options{};
    options.rankCnt = 2;
    options.dataOpType = HYBM_DOP_TYPE_SDMA;
    HybmDevUserLegacySegment segment(options, 0);
    auto info = Slice(0);
    info.shareableHandle = 0;
    MemSlicePtr slice;
    EXPECT_EQ(segment.ImportSliceInfo(std::string(reinterpret_cast<char *>(&info), sizeof(info)), slice),
              BM_DL_FUNCTION_FAILED);
    EXPECT_EQ(hal.ipcCalls, 1);
    EXPECT_EQ(hal.ipcFlag, ACL_RT_IPC_MEM_IMPORT_FLAG_ENABLE_PEER_ACCESS);
}

TEST_F(HybmHalVmmLifecycleTest, LegacyRegisteredBufferStillUsesAclRelease)
{
    GlobalMockObject::reset();
    MOCK_API(DlAclApi::AclrtSetDevice).expects(once()).will(returnValue(BM_OK));
    MOCK_API(DlAclApi::AclrtUnmapMem).expects(once()).will(returnValue(BM_OK));
    MOCK_API(DlAclApi::AclrtReleaseMemAddress).expects(once()).will(returnValue(BM_OK));
    MOCK_API(DlAclApi::AclrtFreePhysical).expects(once()).will(returnValue(BM_OK));
    MOCK_API(DlHalApi::HalMemRelease).expects(never());
    VmmHandleRegistry::Register(reinterpret_cast<void *>(ADDRESS), reinterpret_cast<void *>(HANDLE), 0);
    EXPECT_EQ(VmmHandleRegistry::Release(reinterpret_cast<void *>(ADDRESS)), BM_OK);
    EXPECT_TRUE(VmmHandleRegistry::map_.empty());
}

TEST_F(HybmHalVmmLifecycleTest, MetadataReleaseFailureCanRetryWithoutDoubleUnmap)
{
    MOCK_API(DlAclApi::GetAscendSocType).stubs().will(returnValue(ASCEND_310P));
    MOCK_API(HalGvaPrecheck).stubs().will(returnValue(BM_OK));
    MOCK_API(DlApi::LoadLibrary).stubs().will(returnValue(BM_OK));
    MOCK_API(ptracer_init).stubs().will(returnValue(BM_OK));
    MOCK_API(ptracer_uninit).stubs();
    MOCK_API(hybm_init_hbm_gva).stubs().will(invoke(FakeMetaInit));
    MOCK_API(HybmStreamManager::DestroyAllThreadHybmStream).stubs();
    MOCK_API(DlApi::CleanupLibrary).expects(once());
    ASSERT_EQ(hybm_init(0, 0), BM_OK);
    hal.release = BM_ERROR;
    hybm_uninit();
    EXPECT_TRUE(HybmHasInited());
    EXPECT_EQ(hybm_init(0, 0), BM_ERROR);
    hal.release = BM_OK;
    hybm_uninit();
    EXPECT_FALSE(HybmHasInited());
    EXPECT_EQ(hal.order, (std::vector<std::string>{"unmap", "release", "release", "free"}));
}

TEST_F(HybmHalVmmLifecycleTest, SharedDeviceAllocationUsesHbmOn910B)
{
    testSoc = ASCEND_910B;
    drv_mem_handle_t *handle = nullptr;
    EXPECT_EQ(CreateHalMemory(7, HYBM_LARGE_PAGE_SIZE, &handle), BM_OK);
    EXPECT_EQ(hal.memoryTypes, (std::vector<uint32_t>{MEM_HBM_TYPE}));
    EXPECT_EQ(handle, HANDLE);
}

TEST_F(HybmHalVmmLifecycleTest, SharedDeviceAllocationDoesNotFallbackOn910B)
{
    testSoc = ASCEND_910B;
    hal.create = HAL_OUT_OF_MEMORY_ERROR;
    drv_mem_handle_t *handle = nullptr;
    EXPECT_EQ(CreateHalMemory(7, HYBM_LARGE_PAGE_SIZE, &handle), HAL_OUT_OF_MEMORY_ERROR);
    EXPECT_EQ(hal.creates, 1);
    EXPECT_EQ(hal.memoryTypes, (std::vector<uint32_t>{MEM_HBM_TYPE}));
}

TEST_F(HybmHalVmmLifecycleTest, Non310PMetadataKeepsOriginalReserveAddressHandling)
{
    testSoc = ASCEND_910B;
    hal.wrongAddress = true;
    void *address = nullptr, *handle = nullptr;
    EXPECT_EQ(HybmModernInitMetaGva(7, &address, HYBM_DEVICE_INFO_SIZE, &handle), BM_OK);
    EXPECT_NE(address, reinterpret_cast<void *>(SVM_END_ADDR - GB));
    EXPECT_EQ(hal.order, (std::vector<std::string>{"reserve", "create", "map"}));
}

TEST_F(HybmHalVmmLifecycleTest, Non310PMetadataCreateFailureKeepsOriginalCleanup)
{
    testSoc = ASCEND_910B;
    hal.create = 8;
    void *address = nullptr, *handle = nullptr;
    EXPECT_EQ(HybmModernInitMetaGva(7, &address, HYBM_DEVICE_INFO_SIZE, &handle), BM_ERROR);
    EXPECT_NE(address, nullptr);
    EXPECT_EQ(hal.order, (std::vector<std::string>{"reserve", "create"}));
}

TEST_F(HybmHalVmmLifecycleTest, Non310PMetadataMapFailureReleasesOnlyPhysicalHandle)
{
    testSoc = ASCEND_910B;
    hal.map = 8;
    void *address = nullptr, *handle = nullptr;
    EXPECT_EQ(HybmModernInitMetaGva(7, &address, HYBM_DEVICE_INFO_SIZE, &handle), BM_ERROR);
    EXPECT_NE(address, nullptr);
    EXPECT_EQ(handle, HANDLE);
    EXPECT_EQ(hal.order, (std::vector<std::string>{"reserve", "create", "map", "release"}));
}
