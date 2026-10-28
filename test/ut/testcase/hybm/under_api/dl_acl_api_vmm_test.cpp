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

/* P1 UT：310P SOC 识别 + ACL VMM 符号层 */
#include <gtest/gtest.h>
#include <string>
#include "hybm.h"
#include "dl_acl_api.h"
#include "hybm_define.h"

namespace ock {
namespace mf {

/* ---- SOC 类型获取(stub 环境, GetAscendSocType 内部缓存) ---- */
TEST(DlAclApiVmmTest, GetAscendSocType_StubEnv)
{
    auto socType = DlAclApi::GetAscendSocType();
    ASSERT_TRUE(socType == AscendSocType::ASCEND_310P || socType == AscendSocType::ASCEND_UNKNOWN ||
                socType == AscendSocType::ASCEND_910B || socType == AscendSocType::ASCEND_910C ||
                socType == AscendSocType::ASCEND_950);
}

/* ---- ACL VMM 符号加载: stub 环境下 hybm_init 后 wrapper 可调用(非 UNLOAD) ---- */
class DlAclApiVmmInitTest : public testing::Test {
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
};

TEST_F(DlAclApiVmmInitTest, VmmSymbolsLoaded)
{
    aclrtContext ctx = nullptr;
    ASSERT_NE(DlAclApi::AclrtCreateContext(&ctx, 0), BM_UNDER_API_UNLOAD);
    ASSERT_NE(DlAclApi::AclrtSetCurrentContext(ctx), BM_UNDER_API_UNLOAD);
    ASSERT_NE(DlAclApi::AclrtDestroyContext(ctx), BM_UNDER_API_UNLOAD);

    aclrtPhysicalMemProp prop{};
    prop.handleType = aclrtMemHandleType::ACL_MEM_HANDLE_TYPE_NONE;
    prop.allocationType = ACL_MEM_ALLOCATION_TYPE_PINNED;
    prop.memAttr = static_cast<uint32_t>(aclrtMemAttr::ACL_MEM_P2P_HUGE);
    prop.location.id = 0;
    prop.location.type = aclrtMemLocationType::ACL_MEM_LOCATION_TYPE_DEVICE;

    aclrtDrvMemHandle handle = nullptr;
    ASSERT_EQ(DlAclApi::AclrtMallocPhysical(&handle, 2UL * 1024UL * 1024UL, &prop, 0), 0);
    ASSERT_NE(handle, nullptr);
    ASSERT_NE(DlAclApi::AclrtFreePhysical(handle), BM_UNDER_API_UNLOAD);

    void *base = nullptr;
    ASSERT_EQ(DlAclApi::AclrtReserveMemAddress(&base, 4UL * 1024UL * 1024UL, 0, nullptr, 0), 0);
    ASSERT_NE(base, nullptr);
    ASSERT_NE(DlAclApi::AclrtReleaseMemAddress(base), BM_UNDER_API_UNLOAD);
    ASSERT_NE(DlAclApi::AclrtMapMem(base, 2UL * 1024UL * 1024UL, 0, handle, 0), BM_UNDER_API_UNLOAD);
    ASSERT_NE(DlAclApi::AclrtUnmapMem(base), BM_UNDER_API_UNLOAD);

    aclrtMemAccessDesc acc{};
    acc.flags = ACL_RT_MEM_ACCESS_FLAGS_READWRITE;
    acc.location.id = 0;
    acc.location.type = aclrtMemLocationType::ACL_MEM_LOCATION_TYPE_DEVICE;
    ASSERT_NE(DlAclApi::AclrtMemSetAccess(base, 2UL * 1024UL * 1024UL, &acc, 1), BM_UNDER_API_UNLOAD);

    uint64_t sh = 0;
    ASSERT_EQ(DlAclApi::AclrtMemExportToShareableHandle(handle, aclrtMemHandleType::ACL_MEM_HANDLE_TYPE_NONE,
                                                        ACL_RT_VMM_EXPORT_FLAG_DISABLE_PID_VALIDATION, &sh),
              0);
    ASSERT_NE(sh, 0UL);
    aclrtDrvMemHandle imported = nullptr;
    ASSERT_EQ(DlAclApi::AclrtMemImportFromShareableHandle(sh, 0, &imported), 0);
    ASSERT_NE(imported, nullptr);

    size_t gran = 0;
    ASSERT_EQ(DlAclApi::AclrtMemGetAllocationGranularity(&prop, 1U, &gran), 0);
    ASSERT_EQ(gran, 2UL * 1024UL * 1024UL);
}

} // namespace mf
} // namespace ock
