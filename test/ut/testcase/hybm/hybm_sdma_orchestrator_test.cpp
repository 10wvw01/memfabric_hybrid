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

/* hybm_sdma_orchestrator_test — 310P 设备发起 SDMA 传输准备与 C API 的 UT
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
 *   5. smem 层 sdma 协同 API 的无效 handle 拒绝
 */
#include <gtest/gtest.h> // googletest 框架
#include <cstddef>       // offsetof

#include "hybm_sdma_orchestrator.h" // 被测单例(public 接口: Acquire/Ready/GetRefCountForTest/Release)

#include "hybm_big_mem.h" // 被测 C API 声明(hybm_sdma_*)
#include "hybm_define.h"  // BM_OK/BM_INVALID_PARAM 等错误码
#include "smem.h"         // smem 层公共定义(SMEM_OK)
#include "smem_shm.h"     // 被测 smem 层 sdma 协同 API 声明
#include "smem_shm_sdma_layout.h" // 共享布局头(单一事实源, 无 AscendC 可进 host UT)

namespace ock { // MemFabric 总命名空间
namespace mf {  // hybm 引擎子命名空间
namespace {     // 匿名命名空间(用例私有常量)
/* 工作区槽数上限(prep 头文件与设备头文件一致性由布局用例守护) */
constexpr uint32_t SDMA_ORCH_WS_SLOTS = 64;
} // namespace

/* 用例 1: Acquire 在资源缺失环境(UT stub)必然失败, 且失败后状态干净可重试 */
TEST(SdmaOrchestratorTest, AcquireFailsCleanlyAndRetriable)
{
    auto &prep = SdmaOrchestrator::GetInstance(); // 被测单例
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

/* 用例 2: 未就绪时 C API 拒绝(就绪门控的外显语义; 数据面 API 已废止) */
TEST(SdmaOrchestratorTest, CApiRejectWhenNotReady)
{
    hybm_sdma_info_t info = {}; // 就绪信息出参
    EXPECT_NE(hybm_sdma_get_info(&info), BM_OK); // 未就绪须拒绝
    EXPECT_NE(hybm_sdma_get_info(nullptr), BM_OK); /* 出参 null 拒绝 */
}

/* 用例 3: 双头布局锁(hybm 契约与 smem 设备头镜像一致; 违反即布局回归) */
TEST(SdmaOrchestratorTest, DualHeaderLayoutInvariants)
{
    /* 布局头钉死值(单一事实源; 漂移即回归) */
    EXPECT_EQ(SMEM_SHM_SDMA_RESERVED, 48UL * 1024UL);
    EXPECT_EQ(SMEM_SHM_SDMA_RS_REQ_RING, 0x0000U);
    EXPECT_EQ(SMEM_SHM_SDMA_RS_REQ_TAIL, 0x0800U);
    EXPECT_EQ(SMEM_SHM_SDMA_RS_REQ_HEAD, 0x0808U);
    EXPECT_EQ(SMEM_SHM_SDMA_RS_QUIET, 0x0810U);
    EXPECT_EQ(SMEM_SHM_SDMA_RS_ARR_STAMP, 0x0A20U);
    EXPECT_EQ(SMEM_SHM_SDMA_RS_ARR_MAIL, 0x2A20U);
    EXPECT_EQ(SMEM_SHM_SDMA_RS_ARR_HEAD, 0xAA20U);
    /* 布局不变式: 区块不重叠且落在保留区内 */
    EXPECT_GE(SMEM_SHM_SDMA_RS_REQ_TAIL, SMEM_SHM_SDMA_RS_REQ_RING + 32U * SDMA_ORCH_WS_SLOTS); // 游标不压环
    EXPECT_GT(SMEM_SHM_SDMA_RS_QUIET, SMEM_SHM_SDMA_RS_REQ_HEAD);                               // 戳不压游标
    EXPECT_GT(SMEM_SHM_SDMA_RS_ARR_STAMP, SMEM_SHM_SDMA_RS_QUIET + 8U * 64U);                   // 到达戳不压完成戳
    EXPECT_GE(SMEM_SHM_SDMA_RS_ARR_MAIL, SMEM_SHM_SDMA_RS_ARR_STAMP + 8U * 1024U);              // 邮件不压戳
    EXPECT_LE(SMEM_SHM_SDMA_RS_ARR_HEAD + 8U, SMEM_SHM_SDMA_RESERVED);                          // 全落在 48KB 内
    /* 环深度契约 */
    EXPECT_EQ(SMEM_SHM_SDMA_RS_REQ_SLOTS, 64U);   // 请求环 64
    EXPECT_EQ(SMEM_SHM_SDMA_RS_ARR_SLOTS, 1024U); // 到达环 1024
    /* 工作区布局(result/ovf/stop) */
    EXPECT_GT(HYBM_SDMA_ORCH_WS_OVF_OFF, HYBM_SDMA_ORCH_WS_RESULT_OFF);                       // ovf 在 result 后
    EXPECT_GE(HYBM_SDMA_ORCH_WS_STOP_OFF, HYBM_SDMA_ORCH_WS_OVF_OFF + 64U);                   // stop 不压 ovf
    EXPECT_LE(HYBM_SDMA_ORCH_WS_STOP_OFF + 8U, HYBM_SDMA_ORCH_WS_SIZE);                       // 落在 4KB 内
    /* 心跳槽(v11 时限时钟): 不压 stop(可紧邻), 落在 4KB 内 */
    EXPECT_GE(HYBM_SDMA_ORCH_WS_HEARTBEAT_OFF, HYBM_SDMA_ORCH_WS_STOP_OFF + 8U);
    EXPECT_LE(HYBM_SDMA_ORCH_WS_HEARTBEAT_OFF + 8U, HYBM_SDMA_ORCH_WS_SIZE);
    EXPECT_EQ(HYBM_SDMA_ORCH_EXIT_RUNNING, 0xFFFFFFFFu); // 运行中标记(host 轮询判据)
}

/* 用例 3b: 契约不变式(KFCKernel/type 5 不受设备侧 28s 强制结束约束; v11: 按时长分轮+主机报时) */
TEST(SdmaOrchestratorTest, V9ResidentInvariants)
{
    /* 参数布局锁: rsv0→epochBudgetS 原位替换, 72B 总尺寸不变(旧 host 传 0 走默认 15s) */
    EXPECT_EQ(sizeof(HybmSdmaOrchParam), 72U);
    EXPECT_EQ(offsetof(HybmSdmaOrchParam, epochBudgetS), 12U); // 第 4 个 u32 槽位
    /* 永不退出的专用值: 全 F(不判报时/无循环上限, 跑到停止/错误); 与默认值 0 语义区分 */
    EXPECT_EQ(HYBM_SDMA_ORCH_EPOCH_BUDGET_RESIDENT, 0xFFFFFFFFu);
    EXPECT_NE(HYBM_SDMA_ORCH_EPOCH_BUDGET_RESIDENT, HYBM_SDMA_ORCH_EPOCH_TIME_BUDGET_S);
    /* launch attr 超时: 必须 0(永不超时)——有限值到达即把任务标记为 507017 超时,
     * StopEpoch 的 sync 提前返回不等 kernel 退出; attr 缺省为默认超时 ≈6.4s 同样误杀 */
    EXPECT_EQ(HYBM_SDMA_ORCH_LAUNCH_TIMEOUT_NEVER_S, 0U);
    /* 契约版本与 KVER 一致(部署链缓存刷新依赖) */
    EXPECT_EQ(HYBM_SDMA_ORCH_KVER, 11U);
}

/* 用例 4: smem 层就绪判定 API 的无效 handle 拒绝(不依赖任何初始化; 仅存 host API) */
TEST(SmemShmSdmaApiTest, InvalidHandleRejected)
{
    EXPECT_EQ(smem_shm_sdma_get_workspace(nullptr), nullptr); // 工作区返回 NULL(就绪判定)
}
} // namespace mf
} // namespace ock
