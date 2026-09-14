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

/* P2 UT：HybmAclVmmSegment(310P ACL VMM HBM 段) — stub so 集成模式, 不依赖 mockcpp
 * stub 支持 g_stubSetCurrentContextFail/g_stubSetDeviceFail 等失败注入(见 acl_stub),
 * 经 dlsym 获取。 */
#include <gtest/gtest.h>
#include <dlfcn.h>

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
namespace {
/* stub so 经 DlAclApi dlopen 加载, 测试经 dlsym 取注入标志/计数器 */
int32_t *StubSym(const char *name)
{
    void *h = dlopen("libascendcl.so", RTLD_LAZY);
    return reinterpret_cast<int32_t *>(dlsym(h, name));
}
} // namespace

class HybmAclVmmSegmentTest : public testing::Test {
protected:
    void SetUp() override
    {
        Result ret = hybm_init(0, 0);
        ASSERT_EQ(ret, BM_OK) << "hybm_init failed, ret=" << ret;
        MemSegment::socType_ = AscendSocType::ASCEND_310P;
        HybmVaManager::GetInstance().ClearAll();
        ResetStubInjections();
    }
    void TearDown() override
    {
        ResetStubInjections();
        HybmVaManager::GetInstance().ClearAll();
        MemSegment::socType_ = AscendSocType::ASCEND_UNKNOWN;
        hybm_uninit();
    }
    static void ResetStubInjections()
    {
        for (const char *sym : {"g_stubSetCurrentContextFail", "g_stubSetDeviceFail", "g_stubSyncStreamFail"}) {
            auto *flag = StubSym(sym);
            if (flag != nullptr) {
                *flag = 0;
            }
        }
        auto *calls = StubSym("g_stubDisablePeerAccessCalls");
        if (calls != nullptr) {
            *calls = 0; // 拆除计数清零, 每用例独立断言
        }
        auto *lastPeer = StubSym("g_stubDisablePeerLastPeer");
        if (lastPeer != nullptr) {
            *lastPeer = -1; // 最近拆除对端复位
        }
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

TEST_F(HybmAclVmmSegmentTest, ImportValidationAndDedup)
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

    LiteralExInfoTranslater<AclVmmExportInfo> translator;
    auto tamper = [&exInfo, &translator](uint32_t sliceIndex, uint32_t rankId, uint64_t size) {
        AclVmmExportInfo info{};
        (void)translator.Deserialize(exInfo, info);
        info.sliceIndex = sliceIndex;
        info.rankId = rankId;
        info.size = size;
        std::string out;
        (void)translator.Serialize(info, out);
        return out;
    };

    // sliceIndex 越出对端 slot 窗口(maxSize/2M-1=1): 拒绝
    std::vector<std::string> oob{tamper(2U, 0U, HYBM_LARGE_PAGE_SIZE)};
    ASSERT_EQ(seg1.Import(oob, nullptr), BM_OK);
    ASSERT_TRUE(seg1.imports_.empty());

    // size 非 2M 对齐: 拒绝
    std::vector<std::string> unaligned{tamper(0U, 0U, HYBM_LARGE_PAGE_SIZE + 1)};
    ASSERT_EQ(seg1.Import(unaligned, nullptr), BM_OK);
    ASSERT_TRUE(seg1.imports_.empty());

    // rankId 越界: 拒绝
    std::vector<std::string> badRank{tamper(0U, 5U, HYBM_LARGE_PAGE_SIZE)};
    ASSERT_EQ(seg1.Import(badRank, nullptr), BM_OK);
    ASSERT_TRUE(seg1.imports_.empty());

    // 同一(rankId, sliceIndex) 重复到达: 去重后仅 1 条
    std::vector<std::string> dup{exInfo, exInfo};
    ASSERT_EQ(seg1.Import(dup, nullptr), BM_OK);
    ASSERT_EQ(seg1.imports_.size(), 1UL);

    (void)seg0.UnReserveMemorySpace();
    (void)seg1.UnReserveMemorySpace();
}

TEST_F(HybmAclVmmSegmentTest, DiePairImportContextFailurePaths)
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
    ASSERT_EQ(seg1.Import({exInfo}, nullptr), BM_OK);

    auto *ctxFail = StubSym("g_stubSetCurrentContextFail");
    auto *devFail = StubSym("g_stubSetDeviceFail");
    auto *destroys = StubSym("g_stubDestroyContextCalls");
    ASSERT_NE(ctxFail, nullptr);
    ASSERT_NE(devFail, nullptr);
    ASSERT_NE(destroys, nullptr);

    // ① SetCurrentContext 失败: 不执行 import, Mmap 报错且无映射建立
    *ctxFail = 1;
    ASSERT_NE(seg1.Mmap(), BM_OK);
    ASSERT_EQ(seg1.mappedGvaMem_.size(), 0UL);
    *ctxFail = 0;

    // ② SetDevice 恢复失败: import 已成功但恢复现场失败——跳过 DestroyContext 防线程上下文悬空
    *devFail = 1;
    const int32_t destroyBefore = *destroys;
    ASSERT_EQ(seg1.Mmap(), BM_OK); // import 本身成功, 映射建立
    ASSERT_EQ(seg1.mappedGvaMem_.size(), 1UL);
    ASSERT_EQ(*destroys - destroyBefore, 0); // 关键: 未销毁(宁泄漏不悬空)
    *devFail = 0;

    ASSERT_EQ(seg1.Unmap(), BM_OK);
    (void)seg0.UnReserveMemorySpace();
    (void)seg1.UnReserveMemorySpace();
}

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

/* ===================== D 态拆除纪律(跨卡 P2P teardown)用例 ===================== */

/* 辅助: 搭建跨卡导入形态——导出方 devId 与导入方不同卡(devId/2 不同), 返回导出串 */
static std::string ExportOneSlice(uint32_t exporterRank, int32_t exporterDevId, uint64_t maxSize)
{
    MemSegmentOptions opt = MemSegmentOptions{};
    opt.devId = exporterDevId;
    opt.segType = HYBM_MST_HBM;
    opt.dataOpType = HYBM_DOP_TYPE_SDMA;
    opt.shared = true;
    opt.size = HYBM_LARGE_PAGE_SIZE;
    opt.maxSize = maxSize;
    opt.rankId = exporterRank;
    opt.rankCnt = 3;
    HybmAclVmmSegment exporter(opt, 0);
    void *base = nullptr;
    (void)exporter.ReserveMemorySpace(&base);
    MemSlicePtr slice = nullptr;
    (void)exporter.AllocLocalMemory(HYBM_LARGE_PAGE_SIZE, slice);
    std::string exInfo;
    (void)exporter.Export(slice, exInfo);
    (void)exporter.UnReserveMemorySpace();
    return exInfo;
}

TEST_F(HybmAclVmmSegmentTest, CrossCardTeardownOnFullRelease)
{
    // rank2(dev0) 从 rank0(dev2)、rank1(dev4) 两个跨卡对端各导入 1 分片
    const uint64_t maxSize = 2UL * HYBM_LARGE_PAGE_SIZE;
    auto ex0 = ExportOneSlice(0, 2, maxSize);
    auto ex1 = ExportOneSlice(1, 4, maxSize);
    ASSERT_EQ(ex0.size(), UNIFIED_EXCHANGE_SEG_INFO_SIZE);
    ASSERT_EQ(ex1.size(), UNIFIED_EXCHANGE_SEG_INFO_SIZE);

    MemSegmentOptions opt = MakeOptions(2, 3, HYBM_LARGE_PAGE_SIZE, maxSize);
    opt.devId = 0; // 本端 dev0, 对端 dev2/dev4 均跨卡
    HybmAclVmmSegment seg(opt, 0);
    void *base = nullptr;
    ASSERT_EQ(seg.ReserveMemorySpace(&base), BM_OK);
    std::vector<std::string> all{ex0, ex1};
    ASSERT_EQ(seg.Import(all, nullptr), BM_OK);
    ASSERT_EQ(seg.Mmap(), BM_OK);
    ASSERT_EQ(seg.mappedGvaMem_.size(), 2UL);
    ASSERT_EQ(seg.crossCardPeerCnt_.size(), 2UL); // dev2/dev4 各计 1

    // 移除 rank0(对端 dev2): 其计数归零 -> 恰好拆除一次 dev2; dev4 仍在册不拆
    ASSERT_EQ(seg.RemoveImported({0}), BM_OK);
    auto *calls = StubSym("g_stubDisablePeerAccessCalls");
    auto *lastPeer = StubSym("g_stubDisablePeerLastPeer");
    ASSERT_NE(calls, nullptr);
    ASSERT_NE(lastPeer, nullptr);
    ASSERT_EQ(*calls, 1);
    ASSERT_EQ(*lastPeer, 2);
    ASSERT_EQ(seg.crossCardPeerCnt_.size(), 1UL);

    // 移除 rank1(对端 dev4): 第二次拆除
    ASSERT_EQ(seg.RemoveImported({1}), BM_OK);
    ASSERT_EQ(*calls, 2);
    ASSERT_EQ(*lastPeer, 4);
    ASSERT_TRUE(seg.crossCardPeerCnt_.empty());

    ASSERT_EQ(seg.UnReserveMemorySpace(), BM_OK); // 无残留, 不再拆除
    ASSERT_EQ(*calls, 2);
}

TEST_F(HybmAclVmmSegmentTest, DiePairImportSkipsTeardown)
{
    // die 对导入(dev0<->dev1)全程不产生 PCIe P2P, 不得调用拆除
    const uint64_t maxSize = 2UL * HYBM_LARGE_PAGE_SIZE;
    auto ex = ExportOneSlice(0, 0, maxSize);
    MemSegmentOptions opt = MakeOptions(1, 2, HYBM_LARGE_PAGE_SIZE, maxSize);
    opt.devId = 1; // dev1 与 dev0 同卡 die 对
    HybmAclVmmSegment seg(opt, 0);
    void *base = nullptr;
    ASSERT_EQ(seg.ReserveMemorySpace(&base), BM_OK);
    std::vector<std::string> all{ex};
    ASSERT_EQ(seg.Import(all, nullptr), BM_OK);
    ASSERT_EQ(seg.Mmap(), BM_OK);
    ASSERT_EQ(seg.crossCardPeerCnt_.size(), 0UL); // die 对不入跨卡册

    ASSERT_EQ(seg.RemoveImported({0}), BM_OK);
    auto *calls = StubSym("g_stubDisablePeerAccessCalls");
    ASSERT_NE(calls, nullptr);
    ASSERT_EQ(*calls, 0); // die 对无 P2P, 全程零拆除
    ASSERT_EQ(seg.UnReserveMemorySpace(), BM_OK);
    ASSERT_EQ(*calls, 0);
}

TEST_F(HybmAclVmmSegmentTest, CrossCardPartialReleaseKeepsP2p)
{
    // 同一对端 2 分片: 仅释放 1 个时不得拆除(直接驱动 UnmapOne 单槽路径)
    const uint64_t maxSize = 4UL * HYBM_LARGE_PAGE_SIZE;
    MemSegmentOptions opt0 = MakeOptions(0, 2, HYBM_LARGE_PAGE_SIZE, maxSize);
    opt0.devId = 2; // 跨卡对端 dev2
    HybmAclVmmSegment seg0(opt0, 0);
    void *base0 = nullptr;
    ASSERT_EQ(seg0.ReserveMemorySpace(&base0), BM_OK);
    MemSlicePtr slice0 = nullptr;
    MemSlicePtr slice1 = nullptr;
    ASSERT_EQ(seg0.AllocLocalMemory(HYBM_LARGE_PAGE_SIZE, slice0), BM_OK);
    ASSERT_EQ(seg0.AllocLocalMemory(HYBM_LARGE_PAGE_SIZE, slice1), BM_OK);
    std::string ex0, ex1;
    ASSERT_EQ(seg0.Export(slice0, ex0), BM_OK);
    ASSERT_EQ(seg0.Export(slice1, ex1), BM_OK);

    MemSegmentOptions opt = MakeOptions(1, 2, HYBM_LARGE_PAGE_SIZE, maxSize);
    opt.devId = 0; // 本端 dev0, 对端 dev2 跨卡
    HybmAclVmmSegment seg(opt, 0);
    void *base = nullptr;
    ASSERT_EQ(seg.ReserveMemorySpace(&base), BM_OK);
    std::vector<std::string> all{ex0, ex1};
    ASSERT_EQ(seg.Import(all, nullptr), BM_OK);
    ASSERT_EQ(seg.Mmap(), BM_OK);
    ASSERT_EQ(seg.crossCardPeerCnt_.at(2), 2U); // dev2 计 2

    // 单槽 UnmapOne: 计数 2->1, 不拆除
    auto slotIt = seg.mappedGvaMem_.begin();
    ASSERT_EQ(seg.UnmapOne(slotIt->first, slotIt->second), BM_OK);
    seg.mappedGvaMem_.erase(slotIt);
    auto *calls = StubSym("g_stubDisablePeerAccessCalls");
    ASSERT_NE(calls, nullptr);
    ASSERT_EQ(*calls, 0); // 仍有活跃句柄, 不拆
    ASSERT_EQ(seg.crossCardPeerCnt_.at(2), 1U);

    // 第二槽释放: 计数归零, 恰好拆除一次
    slotIt = seg.mappedGvaMem_.begin();
    ASSERT_EQ(seg.UnmapOne(slotIt->first, slotIt->second), BM_OK);
    seg.mappedGvaMem_.erase(slotIt);
    ASSERT_EQ(*calls, 1);
    ASSERT_TRUE(seg.crossCardPeerCnt_.empty());

    ASSERT_EQ(seg.UnReserveMemorySpace(), BM_OK);
    ASSERT_EQ(seg0.UnReserveMemorySpace(), BM_OK);
}

TEST_F(HybmAclVmmSegmentTest, CrossCardUninitFallbackTeardown)
{
    // 兜底路径: 残留计数(如孤立句柄)在 UnReserve 末段被全量拆除
    const uint64_t maxSize = 2UL * HYBM_LARGE_PAGE_SIZE;
    MemSegmentOptions opt = MakeOptions(0, 2, HYBM_LARGE_PAGE_SIZE, maxSize);
    opt.devId = 0;
    HybmAclVmmSegment seg(opt, 0);
    void *base = nullptr;
    ASSERT_EQ(seg.ReserveMemorySpace(&base), BM_OK);
    // 模拟清理路径提前返回遗留的残留计数(dev4 仍计 1)
    seg.crossCardPeerCnt_[4] = 1;
    seg.slotPeerDev_[reinterpret_cast<uint64_t>(base) + maxSize] = 4;

    ASSERT_EQ(seg.UnReserveMemorySpace(), BM_OK);
    auto *calls = StubSym("g_stubDisablePeerAccessCalls");
    auto *lastPeer = StubSym("g_stubDisablePeerLastPeer");
    ASSERT_NE(calls, nullptr);
    ASSERT_NE(lastPeer, nullptr);
    ASSERT_EQ(*calls, 1); // 兜底扫描拆除 dev4 恰好一次
    ASSERT_EQ(*lastPeer, 4);
    ASSERT_TRUE(seg.crossCardPeerCnt_.empty()); // 册已清空
}
} // namespace mf
} // namespace ock
