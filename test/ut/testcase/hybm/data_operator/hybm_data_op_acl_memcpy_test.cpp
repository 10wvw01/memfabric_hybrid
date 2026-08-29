/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
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
 * stub 的 aclrtMemcpy/aclrtMemcpyAsync/aclrtSynchronizeStream 为直通实现(见 acl_stub)。
 */
#include <gtest/gtest.h>

#define private public
#define protected public
#include "hybm_data_op_acl_memcpy.h"
#undef private
#undef protected

#include "hybm.h"

namespace ock {
namespace mf {

class HybmDataOpAclMemcpyTest : public testing::Test {
protected:
    void SetUp() override
    {
        Result ret = hybm_init(0, 0);
        ASSERT_EQ(ret, BM_OK) << "hybm_init failed, ret=" << ret;
    }
    void TearDown() override
    {
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
} // namespace mf
} // namespace ock
