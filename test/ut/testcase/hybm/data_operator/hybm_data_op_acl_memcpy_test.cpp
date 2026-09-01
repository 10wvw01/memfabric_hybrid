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

/* P3 UT：HostDataOpAclMemcpy(310P aclrtMemcpy 算子) — stub so 集成模式, 不依赖 mockcpp
 * stub 的 aclrtMemcpy/aclrtMemcpyAsync 为直通实现; aclrtSynchronizeStream 支持
 * g_stubSyncStreamFail 失败注入与调用计数(见 acl_stub), 经 dlsym 获取。
 */
#include <gtest/gtest.h>
#include <dlfcn.h>

#define private public
#define protected public
#include "hybm_data_op_acl_memcpy.h"
#undef private
#undef protected

#include "hybm.h"

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

class HybmDataOpAclMemcpyTest : public testing::Test {
protected:
    void SetUp() override
    {
        Result ret = hybm_init(0, 0);
        ASSERT_EQ(ret, BM_OK) << "hybm_init failed, ret=" << ret;
        auto *fail = StubSym("g_stubSyncStreamFail");
        ASSERT_NE(fail, nullptr) << "stub g_stubSyncStreamFail not found";
        *fail = 0;
    }
    void TearDown() override
    {
        for (const char *sym : {"g_stubSyncStreamFail", "g_stubSetCurrentContextFail", "g_stubSetDeviceFail"}) {
            auto *flag = StubSym(sym);
            if (flag != nullptr) {
                *flag = 0;
            }
        }
        hybm_uninit();
    }

    hybm_copy_params MakeParams(void *dst, void *src, uint64_t size)
    {
        hybm_copy_params p{};
        p.dest = dst;
        p.src = src;
        p.dataSize = size;
        return p;
    }
};

TEST_F(HybmDataOpAclMemcpyTest, InitializeAndUninitialize)
{
    HostDataOpAclMemcpy op(0);
    ASSERT_EQ(op.Initialize(), BM_OK);
    ASSERT_EQ(op.Initialize(), BM_OK); // 幂等
    op.UnInitialize();
    ASSERT_FALSE(op.inited_);
    op.UnInitialize(); // 重复 UnInit 安全
}

TEST_F(HybmDataOpAclMemcpyTest, DataCopyBeforeInitRejected)
{
    HostDataOpAclMemcpy op(0);
    auto params = MakeParams(nullptr, nullptr, 16);
    ASSERT_EQ(op.DataCopy(params, HYBM_GLOBAL_DEVICE_TO_GLOBAL_DEVICE, {}), BM_NOT_INITIALIZED);
}

TEST_F(HybmDataOpAclMemcpyTest, SupportedDirections)
{
    /* stub 的 aclrtMemcpy 为直通 no-op(不搬运数据), 本用例校验方向分派与返回码 */
    HostDataOpAclMemcpy op(0);
    ASSERT_EQ(op.Initialize(), BM_OK);

    uint64_t hostBuf[4] = {1, 2, 3, 4};
    uint64_t devBuf[4] = {0};
    ExtOptions options{};

    // H2D(LH2GD) / D2H(GD2LH): 同步 aclrtMemcpy 路径
    auto p1 = MakeParams(devBuf, hostBuf, sizeof(hostBuf));
    ASSERT_EQ(op.DataCopy(p1, HYBM_LOCAL_HOST_TO_GLOBAL_DEVICE, options), BM_OK);
    auto p2 = MakeParams(hostBuf, reinterpret_cast<void *>(devBuf), sizeof(devBuf));
    ASSERT_EQ(op.DataCopy(p2, HYBM_GLOBAL_DEVICE_TO_LOCAL_HOST, options), BM_OK);

    // D2D(GD2GD/LD2GD/GD2LD): MemcpyAsync + SynchronizeStream 路径
    uint64_t devBuf2[4] = {0};
    auto p3 = MakeParams(devBuf2, reinterpret_cast<void *>(devBuf), sizeof(devBuf));
    ASSERT_EQ(op.DataCopy(p3, HYBM_GLOBAL_DEVICE_TO_GLOBAL_DEVICE, options), BM_OK);
    auto p4 = MakeParams(devBuf2, reinterpret_cast<void *>(devBuf), sizeof(devBuf));
    ASSERT_EQ(op.DataCopy(p4, HYBM_LOCAL_DEVICE_TO_GLOBAL_DEVICE, options), BM_OK);
    auto p5 = MakeParams(devBuf, reinterpret_cast<void *>(devBuf2), sizeof(devBuf));
    ASSERT_EQ(op.DataCopy(p5, HYBM_GLOBAL_DEVICE_TO_LOCAL_DEVICE, options), BM_OK);

    // 不支持方向拒绝
    auto p6 = MakeParams(devBuf, hostBuf, sizeof(hostBuf));
    ASSERT_EQ(op.DataCopy(p6, HYBM_LOCAL_HOST_TO_GLOBAL_HOST, options), BM_INVALID_PARAM);
}

TEST_F(HybmDataOpAclMemcpyTest, BatchCopyAndAsyncAndWait)
{
    HostDataOpAclMemcpy op(0);
    ASSERT_EQ(op.Initialize(), BM_OK);

    uint64_t src[2][4] = {{1, 1, 1, 1}, {2, 2, 2, 2}};
    uint64_t dst[2][4] = {{0}, {0}};
    void *srcs[2] = {src[0], src[1]};
    void *dsts[2] = {dst[0], dst[1]};
    uint64_t sizes[2] = {sizeof(src[0]), sizeof(src[1])};
    hybm_batch_copy_params bp{};
    bp.sources = srcs;
    bp.destinations = dsts;
    bp.dataSizes = sizes;
    bp.batchSize = 2;
    ExtOptions options{};
    ASSERT_EQ(op.BatchDataCopy(bp, HYBM_GLOBAL_DEVICE_TO_GLOBAL_DEVICE, options), BM_OK);

    // async 降级 sync
    auto p = MakeParams(dst[0], reinterpret_cast<void *>(src[0]), sizeof(src[0]));
    ASSERT_EQ(op.DataCopyAsync(p, HYBM_GLOBAL_DEVICE_TO_GLOBAL_DEVICE, options), BM_OK);
    // Wait no-op
    ASSERT_EQ(op.Wait(0), BM_OK);

    // batch 非法方向拒绝
    ASSERT_EQ(op.BatchDataCopy(bp, HYBM_LOCAL_HOST_TO_GLOBAL_HOST, options), BM_INVALID_PARAM);
}

TEST_F(HybmDataOpAclMemcpyTest, TransformVaIsNoop)
{
    HostDataOpAclMemcpy op(0);
    ASSERT_EQ(op.Initialize(), BM_OK);
    void *src = reinterpret_cast<void *>(0x1234);
    void *dst = reinterpret_cast<void *>(0x5678);
    op.TransformVa(src, dst, HYBM_GLOBAL_DEVICE_TO_GLOBAL_DEVICE);
    ASSERT_EQ(src, reinterpret_cast<void *>(0x1234));
    ASSERT_EQ(dst, reinterpret_cast<void *>(0x5678));
}

TEST_F(HybmDataOpAclMemcpyTest, StreamRecoveryAfterSyncFailure)
{
    constexpr int64_t kStubNo = 0;
    constexpr int64_t kStubFail = 1;
    auto *fail = StubSym("g_stubSyncStreamFail");
    auto *creates = StubSym("g_stubCreateStreamCalls");
    auto *destroys = StubSym("g_stubDestroyStreamCalls");
    ASSERT_NE(creates, nullptr);
    ASSERT_NE(destroys, nullptr);

    HostDataOpAclMemcpy op(0);
    ASSERT_EQ(op.Initialize(), BM_OK);
    uint64_t src[2] = {1, 2};
    uint64_t dst[2] = {0};

    // 注入同步失败: D2D 返回错误且 stream 被销毁重建(各一次), 算子不残留持久错误态
    *fail = static_cast<int32_t>(kStubFail);
    const int32_t createBefore = *creates;
    const int32_t destroyBefore = *destroys;
    auto p1 = MakeParams(dst, src, sizeof(src));
    ASSERT_NE(op.DataCopy(p1, HYBM_GLOBAL_DEVICE_TO_GLOBAL_DEVICE, {}), BM_OK);
    ASSERT_EQ(*destroys - destroyBefore, 1);
    ASSERT_EQ(*creates - createBefore, 1);

    // 恢复注入后同一算子仍可完成 D2D
    *fail = static_cast<int32_t>(kStubNo);
    auto p2 = MakeParams(dst, src, sizeof(src));
    ASSERT_EQ(op.DataCopy(p2, HYBM_GLOBAL_DEVICE_TO_GLOBAL_DEVICE, {}), BM_OK);
    op.UnInitialize();
}

TEST_F(HybmDataOpAclMemcpyTest, BatchD2DUsesSingleSync)
{
    auto *syncs = StubSym("g_stubSyncStreamCalls");
    ASSERT_NE(syncs, nullptr);
    HostDataOpAclMemcpy op(0);
    ASSERT_EQ(op.Initialize(), BM_OK);

    constexpr uint32_t kBatchSize = 4;
    uint64_t src[kBatchSize][2] = {{1, 1}, {2, 2}, {3, 3}, {4, 4}};
    uint64_t dst[kBatchSize][2] = {{0}, {0}, {0}, {0}};
    void *srcs[kBatchSize] = {src[0], src[1], src[2], src[3]};
    void *dsts[kBatchSize] = {dst[0], dst[1], dst[2], dst[3]};
    uint64_t sizes[kBatchSize] = {sizeof(src[0]), sizeof(src[1]), sizeof(src[2]), sizeof(src[3])};
    hybm_batch_copy_params bp{};
    bp.sources = srcs;
    bp.destinations = dsts;
    bp.dataSizes = sizes;
    bp.batchSize = kBatchSize;

    // 两阶段: N 次提交仅 1 次流同步(同一 stream 保序)
    const int32_t syncBefore = *syncs;
    ASSERT_EQ(op.BatchDataCopy(bp, HYBM_GLOBAL_DEVICE_TO_GLOBAL_DEVICE, {}), BM_OK);
    ASSERT_EQ(*syncs - syncBefore, 1);
    op.UnInitialize();
}
} // namespace mf
} // namespace ock
