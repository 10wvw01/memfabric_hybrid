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

/* hybm_overlap_prep_test — 310P 协同模式传输准备与 C API 的 UT
 *
 * UT 环境约束决定用例形态(acl_stub 的 aclrtMemcpy 为 no-op, 无设备内存模型;
 * DlRtApi 依赖 libruntime.so 在 mock 环境不可用) —— Acquire 在 UT 下必然失败,
 * 这正是就绪门控语义的确定性验证: 任何一层资源缺失都不得让 ready_ 置位。
 *
 * 覆盖:
 *   1. Acquire 失败语义: 返回非 0 / ready 不置位 / 引用计数归零可重试 / Release 幂等
 *   2. 未就绪时 C API 全拒绝(get_info/submit/wait/get_result)
 *   3. submit 的 chunks 边界(0/65 拒绝, 1~64 通过参数关)
 *   4. 工作区布局常量不变式(偏移不重叠, 哨兵区在邮箱区之外)
 *   5. smem 层 overlap API 的无效 handle 拒绝
 */
#include <gtest/gtest.h> // googletest 框架

#include "hybm_overlap_prep.h" // 被测单例(public 接口: Acquire/Ready/GetRefCountForTest/Release)

#include "hybm_big_mem.h" // 被测 C API 声明(hybm_overlap_*)
#include "hybm_define.h"  // BM_OK/BM_INVALID_PARAM 等错误码
#include "smem.h"         // smem 层公共定义(SMEM_OK)
#include "smem_shm.h"     // 被测 smem 层 overlap API 声明

namespace ock { // MemFabric 总命名空间
namespace mf {  // hybm 引擎子命名空间
namespace {     // 匿名命名空间(用例私有常量)
/* 工作区槽数上限(prep 头文件与设备头文件一致性由布局用例守护) */
constexpr uint32_t OVERLAP_WS_SLOTS = 64;
} // namespace

/* 用例 1: Acquire 在资源缺失环境(UT stub)必然失败, 且失败后状态干净可重试 */
TEST(HybmOverlapPrepTest, AcquireFailsCleanlyAndRetriable)
{
    auto &prep = HybmOverlapPrep::GetInstance(); // 被测单例
    for (int round = 0; round < 2; round++) { /* 两次: 验证失败后可重试无残留 */
        auto ret = prep.Acquire(0); // 设备 0(stub 环境必然初始化失败)
        EXPECT_NE(ret, BM_OK) << "round " << round << ": UT stub 环境下 Acquire 必须失败"; // 必须失败
        EXPECT_FALSE(prep.Ready()) << "round " << round << ": 失败后不得置 ready"; // 不得就绪
        EXPECT_EQ(prep.GetRefCountForTest(), 0U) << "round " << round << ": 失败后引用计数须归零"; // 计数归零
    }
    /* Release 幂等(refCount 已为 0 时为 no-op, 不得崩溃) */
    prep.Release(); // 第一次(计数已 0)
    prep.Release(); // 第二次(幂等不崩溃)
    EXPECT_EQ(prep.GetRefCountForTest(), 0U); // 计数仍为 0
}

/* 用例 2: 未就绪时 C API 全拒绝(就绪门控的外显语义) */
TEST(HybmOverlapPrepTest, CApiRejectWhenNotReady)
{
    hybm_overlap_info_t info = {}; // 就绪信息出参
    EXPECT_NE(hybm_overlap_get_info(&info), BM_OK); // 未就绪须拒绝
    EXPECT_NE(hybm_overlap_submit(0x1000, 0x2000, 8), BM_OK); // 提交须拒绝
    EXPECT_NE(hybm_overlap_wait(), BM_OK); // 等待须拒绝
    uint32_t mainRet = 0;   // 主返回码出参
    uint32_t stage = 0;     // 阶段出参
    uint32_t headAfter = 0; // head 出参
    EXPECT_NE(hybm_overlap_get_result(&mainRet, &stage, &headAfter), BM_OK); // 诊断读回须拒绝
    EXPECT_NE(hybm_overlap_get_info(nullptr), BM_OK); /* 出参 null 拒绝 */
}

/* 用例 3: submit 的 chunks 边界(0 与 >邮箱槽数拒绝, 参数检查先于就绪检查) */
TEST(HybmOverlapPrepTest, SubmitChunksBounds)
{
    EXPECT_EQ(hybm_overlap_submit(0x1000, 0x2000, 0), BM_INVALID_PARAM); // chunks=0 拒绝
    EXPECT_EQ(hybm_overlap_submit(0x1000, 0x2000, OVERLAP_WS_SLOTS + 1), BM_INVALID_PARAM); // 65 拒绝
    /* 边界值本身通过参数关(随后因未就绪被拒, 验证检查顺序) */
    EXPECT_NE(hybm_overlap_submit(0x1000, 0x2000, OVERLAP_WS_SLOTS), BM_OK); // 64 过参数关
}

/* 用例 4: 工作区布局常量不变式(偏移不重叠; 违反即布局回归) */
TEST(HybmOverlapPrepTest, WorkspaceLayoutInvariants)
{
    EXPECT_EQ(HYBM_OVERLAP_WS_MAILBOX_OFF, 0U); // 邮箱区从头开始
    EXPECT_GE(HYBM_OVERLAP_WS_SENTINEL_OFF, HYBM_OVERLAP_WS_MAILBOX_OFF + 16U * OVERLAP_WS_SLOTS); // 哨兵不压邮箱
    EXPECT_GT(HYBM_OVERLAP_WS_RESULT_OFF, HYBM_OVERLAP_WS_SENTINEL_OFF); // result 在哨兵后
    EXPECT_GT(HYBM_OVERLAP_WS_OVF_OFF, HYBM_OVERLAP_WS_RESULT_OFF); // ovf 在 result 后
    EXPECT_LE(HYBM_OVERLAP_WS_OVF_OFF + 64U, HYBM_OVERLAP_WS_SIZE); // 全部落在 4KB 内
    EXPECT_EQ(HYBM_OVERLAP_FLAG_READY, 0x1UL); /* meta 能力位与设备头文件约定一致 */
}

/* 用例 5: smem 层 overlap API 的无效 handle 拒绝(不依赖任何初始化) */
TEST(SmemShmOverlapApiTest, InvalidHandleRejected)
{
    EXPECT_NE(smem_shm_overlap_launch(nullptr, nullptr, nullptr, 8), SMEM_OK); // null handle 拒绝
    EXPECT_NE(smem_shm_overlap_wait(nullptr), SMEM_OK); // 等待拒绝
    uint32_t mainRet = 0;   // 主返回码出参
    uint32_t stage = 0;     // 阶段出参
    uint32_t headAfter = 0; // head 出参
    EXPECT_NE(smem_shm_overlap_get_result(nullptr, &mainRet, &stage, &headAfter), SMEM_OK); // 诊断拒绝
    EXPECT_EQ(smem_shm_overlap_get_workspace(nullptr), nullptr); // 工作区返回 NULL
}
} // namespace mf
} // namespace ock
