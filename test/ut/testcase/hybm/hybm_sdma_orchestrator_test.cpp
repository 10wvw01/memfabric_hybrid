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
#include <vector>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <mockcpp/mockcpp.hpp>
#include "dl_acl_api.h"
#include "hybm_common_include.h"
#define private public
#include "hybm_sdma_orchestrator.h" // 被测单例
#undef private

#include "hybm_big_mem.h"         // 被测 C API 声明(hybm_sdma_*)
#include "hybm_define.h"          // BM_OK/BM_INVALID_PARAM 等错误码
#include "smem.h"                 // smem 层公共定义(SMEM_OK)
#include "smem_shm.h"             // 被测 smem 层 sdma 协同 API 声明
#include "smem_shm_sdma_layout.h" // 共享布局头(单一事实源, 无 AscendC 可进 host UT)

namespace ock { // MemFabric 总命名空间
namespace mf {  // hybm 引擎子命名空间

#define MOCKER_CPP(api, TT) MOCKCPP_NS::mockAPI(#api, reinterpret_cast<TT>(api))

class SdmaDeployTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        char path[] = "/tmp/mf_deploy_test_XXXXXX";
        int fd = mkstemp(path);
        ASSERT_GE(fd, 0);
        close(fd);
        input = path;
        std::ofstream(input) << R"({"HybmSdmaOrchEpoch":{"opInfo":{"kernelSo":"libmf_sdma_orch.so"}}})";
        commands.clear();
        checkCalls = 0;
        barePid = static_cast<uint32_t>(getpid()) + 1000000U;
        bareResult = BM_OK;
        MOCKER(DlAclApi::RtDeviceGetBareTgid).stubs().will(invoke(QueryBarePid));
    }
    void TearDown() override
    {
        GlobalMockObject::verify();
        unlink(input.c_str());
        unlink(runtime.c_str());
    }
    static int32_t QueryBarePid(uint32_t *pid)
    {
        if (bareResult == BM_OK) {
            *pid = barePid;
        }
        return bareResult;
    }
    static int32_t Capture(SdmaOrchestrator *, const std::string &cmd)
    {
        commands.push_back(cmd);
        return BM_OK;
    }
    static int32_t CheckAbsent(SdmaOrchestrator *, const void *bytes, size_t size, bool allowPresent)
    {
        EXPECT_EQ(bytes, nullptr);
        EXPECT_EQ(size, 0U);
        EXPECT_EQ(allowPresent, checkCalls++ == 0);
        return BM_OK;
    }
    void MockChannel()
    {
        MOCKER_CPP(&SdmaOrchestrator::KfcExec, int32_t(*)(SdmaOrchestrator *, const std::string &))
            .stubs()
            .will(invoke(Capture));
    }
    SdmaOrchestrator prep;
    std::string input;
    std::string runtime;
    static std::vector<std::string> commands;
    static unsigned checkCalls;
    static uint32_t barePid;
    static int32_t bareResult;
};
std::vector<std::string> SdmaDeployTest::commands;
unsigned SdmaDeployTest::checkCalls = 0;
uint32_t SdmaDeployTest::barePid = 0;
int32_t SdmaDeployTest::bareResult = BM_OK;

TEST_F(SdmaDeployTest, GeneratesSeparateNamesWithoutManualVersion)
{
    ASSERT_EQ(prep.PrepareRuntimeJson(input, runtime), BM_OK);
    const auto firstName = prep.deviceSoName_;
    EXPECT_EQ(firstName.find("libmf_sdma_orch_" + std::to_string(barePid) + "_"), 0U);
    EXPECT_EQ(prep.hostPid_, barePid);
    std::ifstream file(runtime);
    const std::string json((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    EXPECT_NE(json.find(firstName), std::string::npos);
    EXPECT_EQ(json.find("libmf_sdma_orch.so"), std::string::npos);
    unlink(runtime.c_str());
    ASSERT_EQ(prep.PrepareRuntimeJson(input, runtime), BM_OK);
    EXPECT_NE(firstName, prep.deviceSoName_);
    EXPECT_FALSE(prep.deviceSoOwned_);
}

TEST_F(SdmaDeployTest, RejectsMissingAndUnsafeKernelName)
{
    for (const auto &value : {"{}", "{\"kernelSo\":\"../bad.so\"}", "{\"kernelSo\":\"a;bad.so\"}",
                              "{\"kernelSo\":\"libmf_sdma_orch.so\""}) {
        std::ofstream(input) << value;
        EXPECT_EQ(prep.PrepareRuntimeJson(input, runtime), BM_INVALID_PARAM);
        EXPECT_TRUE(runtime.empty());
    }
}

TEST_F(SdmaDeployTest, HostPidQueryFailureStopsBeforeCreatingRuntimeJson)
{
    bareResult = BM_ERROR;
    EXPECT_EQ(prep.PrepareRuntimeJson(input, runtime), BM_DL_FUNCTION_FAILED);
    EXPECT_TRUE(runtime.empty());
    EXPECT_TRUE(prep.deviceSoName_.empty());
    EXPECT_EQ(prep.hostPid_, 0U);
}

TEST_F(SdmaDeployTest, ZeroHostPidStopsBeforeCreatingRuntimeJson)
{
    barePid = 0;
    EXPECT_EQ(prep.PrepareRuntimeJson(input, runtime), BM_DL_FUNCTION_FAILED);
    EXPECT_TRUE(runtime.empty());
    EXPECT_TRUE(prep.deviceSoName_.empty());
    EXPECT_EQ(prep.hostPid_, 0U);
}

TEST_F(SdmaDeployTest, CopyRejectsUnavailableHostPid)
{
    MOCKER_CPP(&SdmaOrchestrator::KfcExec, int32_t(*)(SdmaOrchestrator *, const std::string &)).expects(never());
    EXPECT_EQ(prep.CopyToSearchPath("libmf_sdma_orch_test.so", 123), BM_INVALID_PARAM);
}

TEST_F(SdmaDeployTest, ChannelFailureDoesNotAssumePredeployment)
{
    ASSERT_EQ(prep.PrepareRuntimeJson(input, runtime), BM_OK);
    MOCKER_CPP(&SdmaOrchestrator::MigrateViaCustChannel,
               int32_t(*)(SdmaOrchestrator *, const std::string &, uint64_t &))
        .stubs()
        .will(returnValue(BM_OK));
    MOCKER_CPP(&SdmaOrchestrator::KfcExec, int32_t(*)(SdmaOrchestrator *, const std::string &))
        .stubs()
        .will(returnValue(BM_ERROR));
    MOCKER_CPP(&SdmaOrchestrator::TryLoadKernel, int32_t(*)(SdmaOrchestrator *, const std::string &)).expects(never());
    EXPECT_EQ(prep.DeployOrchestratorKernel("/unused", runtime), BM_ERROR);
    EXPECT_FALSE(prep.ready_.load());
}

TEST_F(SdmaDeployTest, MissingHostArtifactFailsBeforeCopy)
{
    ASSERT_EQ(prep.PrepareRuntimeJson(input, runtime), BM_OK);
    MockChannel();
    EXPECT_EQ(prep.DeployOrchestratorKernel("/missing", runtime), BM_FILE_NOT_ACCESS);
    EXPECT_TRUE(commands.empty());
    EXPECT_FALSE(prep.deviceSoOwned_);
}

TEST_F(SdmaDeployTest, CleanupRemovesOnlyOwnedDeviceFile)
{
    MockChannel();
    prep.launchStream_ = reinterpret_cast<void *>(1);
    prep.deviceSoName_ = "libmf_sdma_orch_test.so";
    prep.deviceSoOwned_ = true;
    prep.deployNonce_ = 2468;
    prep.hostPid_ = barePid;
    MOCKER_CPP(&SdmaOrchestrator::CheckDeploymentResult, int32_t(*)(SdmaOrchestrator *, const void *, size_t, bool))
        .expects(exactly(2))
        .will(invoke(CheckAbsent));
    MOCKER(DlAclApi::AclrtDestroyStream).stubs().will(returnValue(BM_OK));
    prep.Cleanup();
    ASSERT_EQ(commands.size(), 1U);
    EXPECT_EQ(commands[0], "rm -f /usr/lib64/aicpu_kernels/0/aicpu_kernels_device/libmf_sdma_orch_test.so");
    EXPECT_TRUE(prep.deviceSoName_.empty());
    EXPECT_EQ(prep.hostPid_, 0U);
    EXPECT_FALSE(prep.deviceSoOwned_);
    prep.Cleanup();
    EXPECT_EQ(commands.size(), 1U);
}

TEST_F(SdmaDeployTest, CopiesDirectlyThroughCurrentProcessLink)
{
    char path[] = "/tmp/mf_cust_copy_XXXXXX";
    ASSERT_NE(mkdtemp(path), nullptr);
    const std::string dir = path;
    std::ofstream(dir + "/libmf_sdma_orch.so") << "kernel bytes";
    std::ofstream(dir + "/libmf_sdma_orch.json") << "{}";
    MockChannel();
    MOCKER(DlAclApi::AclrtBinaryLoadFromFile).stubs().will(returnValue(BM_OK));
    MOCKER(DlAclApi::AclrtBinaryGetFunction).stubs().will(returnValue(BM_OK));
    uint64_t size = 0;
    ASSERT_EQ(prep.MigrateViaCustChannel(dir, size), BM_OK);
    EXPECT_EQ(size, 12U);
    EXPECT_TRUE(commands.empty());
    prep.dieId_ = 1;
    prep.hostPid_ = barePid;
    ASSERT_EQ(prep.CopyToSearchPath("libmf_sdma_orch_copy.so", 12), BM_OK);
    ASSERT_EQ(commands.size(), 1U);
    EXPECT_NE(commands[0].find("cust_aicpu_1_*_" + std::to_string(barePid) + "/libmf_sdma_orch.so"), std::string::npos);
    EXPECT_EQ(commands[0].find("/home/CustAiCpuUser/lib/libmf_sdma_orch.so.*"), std::string::npos);
    unlink((dir + "/libmf_sdma_orch.so").c_str());
    unlink((dir + "/libmf_sdma_orch.json").c_str());
    rmdir(dir.c_str());
}

TEST_F(SdmaDeployTest, CleanupPreservesUnownedFiles)
{
    MockChannel();
    prep.deviceSoName_ = "libmf_sdma_orch_preexisting.so";
    prep.Cleanup();
    EXPECT_TRUE(commands.empty());
}

TEST_F(SdmaDeployTest, UnconfirmedShellResultRequiresSuccessfulDeploymentVerification)
{
    ASSERT_EQ(prep.PrepareRuntimeJson(input, runtime), BM_OK);
    MOCKER_CPP(&SdmaOrchestrator::MigrateViaCustChannel,
               int32_t(*)(SdmaOrchestrator *, const std::string &, uint64_t &))
        .expects(once())
        .will(returnValue(BM_OK));
    MOCKER_CPP(&SdmaOrchestrator::KfcExec, int32_t(*)(SdmaOrchestrator *, const std::string &))
        .expects(once())
        .will(returnValue(507018));
    MOCKER_CPP(&SdmaOrchestrator::TryLoadKernel, int32_t(*)(SdmaOrchestrator *, const std::string &))
        .expects(once())
        .will(returnValue(BM_OK));
    MOCKER_CPP(&SdmaOrchestrator::VerifyDeployment, int32_t(*)(SdmaOrchestrator *, const std::string &))
        .expects(once())
        .will(returnValue(BM_OK));
    EXPECT_EQ(prep.DeployOrchestratorKernel("/unused", runtime), BM_OK);
}

TEST_F(SdmaDeployTest, UnconfirmedShellResultStillRejectsFailedVerification)
{
    ASSERT_EQ(prep.PrepareRuntimeJson(input, runtime), BM_OK);
    MOCKER_CPP(&SdmaOrchestrator::MigrateViaCustChannel,
               int32_t(*)(SdmaOrchestrator *, const std::string &, uint64_t &))
        .expects(once())
        .will(returnValue(BM_OK));
    MOCKER_CPP(&SdmaOrchestrator::KfcExec, int32_t(*)(SdmaOrchestrator *, const std::string &))
        .expects(once())
        .will(returnValue(507018));
    MOCKER_CPP(&SdmaOrchestrator::TryLoadKernel, int32_t(*)(SdmaOrchestrator *, const std::string &))
        .expects(once())
        .will(returnValue(BM_OK));
    MOCKER_CPP(&SdmaOrchestrator::VerifyDeployment, int32_t(*)(SdmaOrchestrator *, const std::string &))
        .expects(once())
        .will(returnValue(BM_ERROR));
    EXPECT_EQ(prep.DeployOrchestratorKernel("/unused", runtime), BM_ERROR);
}

TEST_F(SdmaDeployTest, SuccessfulCopyLoadsOnceWithoutExtraVerification)
{
    ASSERT_EQ(prep.PrepareRuntimeJson(input, runtime), BM_OK);
    MOCKER_CPP(&SdmaOrchestrator::MigrateViaCustChannel,
               int32_t(*)(SdmaOrchestrator *, const std::string &, uint64_t &))
        .expects(once())
        .will(returnValue(BM_OK));
    MOCKER_CPP(&SdmaOrchestrator::KfcExec, int32_t(*)(SdmaOrchestrator *, const std::string &))
        .expects(once())
        .will(returnValue(BM_OK));
    MOCKER_CPP(&SdmaOrchestrator::TryLoadKernel, int32_t(*)(SdmaOrchestrator *, const std::string &))
        .expects(once())
        .will(returnValue(BM_OK));
    MOCKER_CPP(&SdmaOrchestrator::VerifyDeployment, int32_t(*)(SdmaOrchestrator *, const std::string &))
        .expects(never());
    MOCKER(::sleep).expects(never());
    EXPECT_EQ(prep.DeployOrchestratorKernel("/unused", runtime), BM_OK);
}

TEST_F(SdmaDeployTest, CopyCommandAlwaysCopiesAndFitsOriginalBuffer)
{
    MockChannel();
    prep.dieId_ = 1;
    prep.hostPid_ = barePid;
    const std::string name = "libmf_sdma_orch_2147483647_XXXXXX.so";
    ASSERT_EQ(prep.CopyToSearchPath(name, 19056), BM_OK);
    ASSERT_EQ(prep.CopyToSearchPath(name, 19056), BM_OK);
    ASSERT_EQ(commands.size(), 2U);
    EXPECT_EQ(commands[0], commands[1]);
    EXPECT_EQ(commands[0].find("cat "), 0U);
    EXPECT_NE(commands[0].find(" > "), std::string::npos);
    EXPECT_EQ(commands[0].find("if "), std::string::npos);
    EXPECT_EQ(commands[0].find(".owner"), std::string::npos);
    EXPECT_EQ(commands[0].find("set -C"), std::string::npos);
    EXPECT_LT(commands[0].size(), 512U);
}

TEST(SdmaServiceOptionsTest, DefaultAndLegacyModes)
{
    SdmaServiceOptions options;
    ASSERT_TRUE(ResolveSdmaOptions("", "", "", "", options));
    EXPECT_EQ(options.epochSeconds, HYBM_SDMA_ORCH_EPOCH_BUDGET_RESIDENT);
    EXPECT_EQ(options.leaseMs, 10000U);
    EXPECT_EQ(options.stopMs, 5000U);
    ASSERT_TRUE(ResolveSdmaOptions("", "0", "", "", options));
    EXPECT_EQ(options.epochSeconds, 15U);
    ASSERT_TRUE(ResolveSdmaOptions("", "2", "", "", options));
    EXPECT_EQ(options.epochSeconds, 2U);
    ASSERT_TRUE(ResolveSdmaOptions("", "4294967295", "", "", options));
    EXPECT_EQ(options.epochSeconds, HYBM_SDMA_ORCH_EPOCH_BUDGET_RESIDENT);
    ASSERT_TRUE(ResolveSdmaOptions("resident", "2", "", "", options));
    EXPECT_EQ(options.epochSeconds, HYBM_SDMA_ORCH_EPOCH_BUDGET_RESIDENT);
}

TEST(SdmaServiceOptionsTest, InvalidModesAndTimeoutsAreRejected)
{
    SdmaServiceOptions options;
    EXPECT_FALSE(ResolveSdmaOptions("other", "", "", "", options));
    EXPECT_FALSE(ResolveSdmaOptions("epoch", "4294967295", "", "", options));
    EXPECT_FALSE(ResolveSdmaOptions("", "", "0", "", options));
    EXPECT_FALSE(ResolveSdmaOptions("", "", "2999", "", options));
    EXPECT_FALSE(ResolveSdmaOptions("", "", "-1", "", options));
    EXPECT_FALSE(ResolveSdmaOptions("", "", "10000", "999", options));
    EXPECT_FALSE(ResolveSdmaOptions("", "", "10000", "4294967295", options));
    ASSERT_TRUE(ResolveSdmaOptions("epoch", "bad", "", "", options));
    EXPECT_EQ(options.epochSeconds, 15U);
}

TEST_F(SdmaDeployTest, UnconfirmedDeviceReferencesPreventCleanup)
{
    prep.epochLaunched_ = true;
    prep.releaseConfirmed_ = false;
    prep.workspace_ = 4096;
    prep.workerResult_ = BM_TIMEOUT;
    MOCKER(DlAclApi::AclrtFree).expects(never());
    EXPECT_EQ(prep.StopEpoch(), BM_TIMEOUT);
    EXPECT_EQ(prep.workspace_.load(), 4096U);
    prep.workspace_ = 0;
    prep.epochLaunched_ = false;
}

TEST_F(SdmaDeployTest, ConfigureFailureRestoresCallerContext)
{
    prep.serviceContext_ = reinterpret_cast<void *>(1);
    MOCKER(DlAclApi::AclrtGetCurrentContext).expects(once()).will(invoke(+[](void **ctx) {
        *ctx = reinterpret_cast<void *>(2);
        return BM_OK;
    }));
    MOCKER(DlAclApi::AclrtSetCurrentContext).expects(exactly(2)).will(returnValue(BM_OK));
    EXPECT_EQ(prep.ConfigureOrch(0, 0, 0, 0, 0), BM_INVALID_PARAM);
    prep.serviceContext_ = nullptr;
}

TEST_F(SdmaDeployTest, WorkerTimeoutReturnsWithoutJoiningActiveThread)
{
    prep.serviceOptions_.stopMs = 1;
    prep.workerDone_ = false;
    std::atomic<bool> finish{false};
    prep.supervisor_ = std::thread([&finish] {
        while (!finish.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    });
    EXPECT_EQ(prep.StopEpoch(), BM_TIMEOUT);
    EXPECT_TRUE(prep.supervisor_.joinable());
    finish = true;
    prep.supervisor_.join();
    prep.workerDone_ = true;
}

TEST_F(SdmaDeployTest, OldServiceInstanceIsRejected)
{
    prep.epochParam_.instance = 2;
    HybmSdmaOrchResult result{};
    result.magic = HYBM_SDMA_ORCH_RESULT_MAGIC;
    result.instance = 1;
    bool finished = false;
    MOCKER(DlAclApi::AclrtSynchronizeStreamWithTimeout).expects(never());
    EXPECT_EQ(prep.HandleServiceResult(result, finished), BM_ERROR);
    EXPECT_FALSE(finished);
}

TEST_F(SdmaDeployTest, FailedServiceIsReportedWithoutReplayingRequests)
{
    prep.epochParam_.instance = 2;
    HybmSdmaOrchResult result{};
    result.magic = HYBM_SDMA_ORCH_RESULT_MAGIC;
    result.instance = 2;
    result.serviceState = SMEM_SHM_SDMA_FAILED;
    result.exitReason = HYBM_SDMA_ORCH_EXIT_ERROR;
    result.mainRet = 1380;
    result.releaseConfirmed = 1;
    bool finished = false;
    MOCKER(DlAclApi::AclrtSynchronizeStreamWithTimeout).expects(once()).will(returnValue(BM_OK));
    MOCKER_CPP(&SdmaOrchestrator::ReadServiceResult, int32_t(*)(SdmaOrchestrator *, HybmSdmaOrchResult &))
        .stubs()
        .with(any(), outBound(result))
        .will(returnValue(BM_OK));
    MOCKER_CPP(&SdmaOrchestrator::LaunchEpoch, int32_t(*)(SdmaOrchestrator *, void *)).expects(never());
    EXPECT_EQ(prep.HandleServiceResult(result, finished), BM_ERROR);
    EXPECT_TRUE(finished);
    EXPECT_TRUE(prep.releaseConfirmed_);
    HybmSdmaOrchResult snapshot{};
    EXPECT_EQ(prep.GetServiceStatus(snapshot), BM_OK);
    EXPECT_EQ(snapshot.mainRet, 1380U);
    EXPECT_EQ(snapshot.instance, 2U);
}

TEST_F(SdmaDeployTest, TerminalStatusIsReadAgainAfterStreamCompletion)
{
    prep.epochParam_.instance = 2;
    HybmSdmaOrchResult observed{};
    observed.magic = HYBM_SDMA_ORCH_RESULT_MAGIC;
    observed.instance = 2;
    observed.exitReason = HYBM_SDMA_ORCH_EXIT_STOP;
    observed.serviceState = SMEM_SHM_SDMA_DRAINING;
    HybmSdmaOrchResult completed = observed;
    completed.serviceState = SMEM_SHM_SDMA_STOPPED;
    completed.releaseConfirmed = 1;
    bool finished = false;
    MOCKER(DlAclApi::AclrtSynchronizeStreamWithTimeout).expects(once()).will(returnValue(BM_OK));
    MOCKER_CPP(&SdmaOrchestrator::ReadServiceResult, int32_t(*)(SdmaOrchestrator *, HybmSdmaOrchResult &))
        .expects(once())
        .with(any(), outBound(completed))
        .will(returnValue(BM_OK));
    EXPECT_EQ(prep.HandleServiceResult(observed, finished), BM_OK);
    EXPECT_TRUE(finished);
    EXPECT_TRUE(prep.releaseConfirmed_);
    EXPECT_EQ(prep.cachedResult_.serviceState, SMEM_SHM_SDMA_STOPPED);
}

TEST_F(SdmaDeployTest, HostTimeoutIsVisibleInStatusSnapshot)
{
    prep.cachedResult_.magic = HYBM_SDMA_ORCH_RESULT_MAGIC;
    prep.cachedResult_.serviceState = SMEM_SHM_SDMA_RUNNING;
    prep.RecordHostFailure(1383);
    HybmSdmaOrchResult snapshot{};
    EXPECT_EQ(prep.GetServiceStatus(snapshot), BM_OK);
    EXPECT_EQ(snapshot.serviceState, SMEM_SHM_SDMA_FAILED);
    EXPECT_EQ(snapshot.mainRet, 1383U);
    EXPECT_EQ(snapshot.releaseConfirmed, 0U);
}

TEST_F(SdmaDeployTest, ServiceBindingDistinguishesRejectedPool)
{
    prep.boundReservation_ = 0x1800;
    EXPECT_TRUE(prep.IsBoundTo(0x1000, 0x1000));
    EXPECT_FALSE(prep.IsBoundTo(0x2000, 0x1000));
    EXPECT_FALSE(prep.IsBoundTo(0x1000, 0x800));
}

TEST_F(SdmaDeployTest, CompletionTimeoutRetainsEpochStream)
{
    prep.epochStream_ = reinterpret_cast<void *>(1);
    MOCKER(DlAclApi::AclrtSynchronizeStreamWithTimeout).expects(once()).will(returnValue(BM_TIMEOUT));
    MOCKER(DlAclApi::AclrtDestroyStream).expects(never());
    EXPECT_EQ(prep.DestroyEpochResources(), BM_TIMEOUT);
    EXPECT_NE(prep.epochStream_, nullptr);
    prep.epochStream_ = nullptr;
}

TEST_F(SdmaDeployTest, CompletionConfirmedBeforeDestroyingEpochStream)
{
    prep.epochStream_ = reinterpret_cast<void *>(1);
    MOCKER(DlAclApi::AclrtSynchronizeStreamWithTimeout).expects(once()).will(returnValue(BM_OK));
    MOCKER(DlAclApi::AclrtDestroyStream).expects(once()).will(returnValue(BM_OK));
    EXPECT_EQ(prep.DestroyEpochResources(), BM_OK);
    EXPECT_EQ(prep.epochStream_, nullptr);
}

TEST_F(SdmaDeployTest, CleanupRetainsWorkspaceWhenCompletionIsUnconfirmed)
{
    prep.epochStream_ = reinterpret_cast<void *>(1);
    prep.workspace_ = 4096;
    MOCKER(DlAclApi::AclrtSynchronizeStreamWithTimeout).expects(once()).will(returnValue(BM_TIMEOUT));
    MOCKER(DlAclApi::AclrtFree).expects(never());
    prep.Cleanup();
    EXPECT_EQ(prep.workspace_.load(), 4096U);
    prep.workspace_ = 0;
    prep.epochStream_ = nullptr;
}

TEST_F(SdmaDeployTest, CleanupConfirmsAbsenceAfterUnconfirmedShellResult)
{
    prep.launchStream_ = reinterpret_cast<void *>(1);
    prep.deviceSoName_ = "libmf_sdma_orch_owned.so";
    prep.deployNonce_ = 2468;
    prep.deviceSoOwned_ = true;
    MOCKER_CPP(&SdmaOrchestrator::KfcExec, int32_t(*)(SdmaOrchestrator *, const std::string &))
        .stubs()
        .will(returnValue(507018));
    MOCKER_CPP(&SdmaOrchestrator::CheckDeploymentResult, int32_t(*)(SdmaOrchestrator *, const void *, size_t, bool))
        .expects(exactly(2))
        .will(invoke(CheckAbsent));
    MOCKER(DlAclApi::AclrtDestroyStream).stubs().will(returnValue(BM_OK));
    prep.Cleanup();
    EXPECT_FALSE(prep.deviceSoOwned_);
}

TEST_F(SdmaDeployTest, CleanupKeepsFileWhenVerifierCannotExecute)
{
    prep.launchStream_ = reinterpret_cast<void *>(1);
    prep.deviceSoName_ = "libmf_sdma_orch_owned.so";
    prep.deviceSoOwned_ = true;
    MOCKER_CPP(&SdmaOrchestrator::CheckDeploymentResult, int32_t(*)(SdmaOrchestrator *, const void *, size_t, bool))
        .expects(once())
        .will(returnValue(BM_ERROR));
    MOCKER_CPP(&SdmaOrchestrator::KfcExec, int32_t(*)(SdmaOrchestrator *, const std::string &)).expects(never());
    MOCKER(DlAclApi::AclrtDestroyStream).stubs().will(returnValue(BM_OK));
    prep.Cleanup();
}

TEST_F(SdmaDeployTest, LaunchFailureIsNotAnUnconfirmedCommand)
{
    MOCKER_CPP(&SdmaOrchestrator::LoadKfcChannel, int32_t(*)(SdmaOrchestrator *, aclrtFuncHandle &))
        .stubs()
        .will(returnValue(BM_OK));
    MOCKER(DlAclApi::AclrtCreateStream).stubs().will(returnValue(BM_OK));
    MOCKER(DlAclApi::AclrtLaunchKernelWithHostArgs).stubs().will(returnValue(507018));
    MOCKER(DlAclApi::AclrtDestroyStream).stubs().will(returnValue(BM_OK));
    EXPECT_EQ(prep.KfcExec("true"), BM_ERROR);
}

TEST_F(SdmaDeployTest, SyncExceptionKeepsUnconfirmedCommandStatus)
{
    MOCKER_CPP(&SdmaOrchestrator::LoadKfcChannel, int32_t(*)(SdmaOrchestrator *, aclrtFuncHandle &))
        .stubs()
        .will(returnValue(BM_OK));
    MOCKER(DlAclApi::AclrtCreateStream).stubs().will(returnValue(BM_OK));
    MOCKER(DlAclApi::AclrtLaunchKernelWithHostArgs).stubs().will(returnValue(BM_OK));
    MOCKER(DlAclApi::AclrtSynchronizeStream).stubs().will(returnValue(507018));
    MOCKER(DlAclApi::AclrtDestroyStream).stubs().will(returnValue(BM_OK));
    EXPECT_EQ(prep.KfcExec("true"), 507018);
}

class SdmaArtifactDirectoryTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        char path[] = "/tmp/mf_artifact_dir_XXXXXX";
        ASSERT_NE(mkdtemp(path), nullptr);
        root = path;
        originalDirectory = std::filesystem::current_path();
    }
    void TearDown() override
    {
        std::error_code error;
        std::filesystem::current_path(originalDirectory, error);
        std::filesystem::remove_all(root, error);
    }
    std::filesystem::path CreateArtifacts(const std::string &relative)
    {
        const auto directory = root / relative;
        std::filesystem::create_directories(directory);
        std::ofstream(directory / "libmf_sdma_orch.so") << "kernel bytes";
        std::ofstream(directory / "libmf_sdma_orch.json") << "{}";
        std::ofstream(directory / "libmf_sdma_orch_launch.json")
            << R"({"HybmSdmaOrchEpoch":{"opInfo":{"kernelSo":"libmf_sdma_orch.so"}}})";
        return directory;
    }
    SdmaOrchestrator prep;
    std::filesystem::path root;
    std::filesystem::path originalDirectory;
};

TEST_F(SdmaArtifactDirectoryTest, FindsArtifactsInNestedDirectory)
{
    const auto directory = CreateArtifacts("install tree/aarch64-linux/hybm/aicpu_kernel");
    std::string json;
    ASSERT_EQ(prep.ResolveOrchestratorJson(root.string(), json), BM_OK);
    EXPECT_EQ(json, (directory / "libmf_sdma_orch_launch.json").string());
}

TEST_F(SdmaArtifactDirectoryTest, AcceptsArtifactDirectoryAndRootSymlink)
{
    const auto directory = CreateArtifacts("version/kernel");
    const auto alias = root / "latest";
    std::filesystem::create_directory_symlink(directory, alias);
    std::string json;
    ASSERT_EQ(prep.ResolveOrchestratorJson(directory.string(), json), BM_OK);
    EXPECT_EQ(json, (directory / "libmf_sdma_orch_launch.json").string());
    ASSERT_EQ(prep.ResolveOrchestratorJson(alias.string(), json), BM_OK);
    EXPECT_EQ(json, (directory / "libmf_sdma_orch_launch.json").string());
    ASSERT_EQ(prep.ResolveOrchestratorJson(root.string(), json), BM_OK);
    EXPECT_EQ(json, (directory / "libmf_sdma_orch_launch.json").string());
}

TEST_F(SdmaArtifactDirectoryTest, MissingFilesRejectInitializationPath)
{
    const char *names[] = {"libmf_sdma_orch.so", "libmf_sdma_orch.json", "libmf_sdma_orch_launch.json"};
    for (const auto *name : names) {
        const auto directory = CreateArtifacts(name);
        std::filesystem::remove(directory / name);
        std::string json = "stale result";
        EXPECT_EQ(prep.ResolveOrchestratorJson(directory.string(), json), BM_FILE_NOT_ACCESS) << name;
        EXPECT_TRUE(json.empty());
    }
}

TEST_F(SdmaArtifactDirectoryTest, RejectsEmptyMissingAndFileDirectories)
{
    const auto file = root / "file";
    std::ofstream(file) << "not a directory";
    std::string json;
    EXPECT_EQ(prep.ResolveOrchestratorJson(root.string(), json), BM_FILE_NOT_ACCESS);
    EXPECT_EQ(prep.ResolveOrchestratorJson((root / "missing").string(), json), BM_FILE_NOT_ACCESS);
    EXPECT_EQ(prep.ResolveOrchestratorJson(file.string(), json), BM_FILE_NOT_ACCESS);
}

TEST_F(SdmaArtifactDirectoryTest, RejectsMultipleCompleteArtifactDirectories)
{
    CreateArtifacts("version1/kernel");
    CreateArtifacts("version2/kernel");
    std::string json;
    EXPECT_EQ(prep.ResolveOrchestratorJson(root.string(), json), BM_FILE_NOT_ACCESS);
    EXPECT_TRUE(json.empty());
}

TEST_F(SdmaArtifactDirectoryTest, ExplicitDirectoryDoesNotFallbackToCurrentDirectory)
{
    const auto directory = CreateArtifacts("kernel");
    std::filesystem::current_path(directory);
    std::string json;
    EXPECT_EQ(prep.ResolveOrchestratorJson((root / "missing").string(), json), BM_FILE_NOT_ACCESS);
    EXPECT_TRUE(json.empty());
}

TEST_F(SdmaArtifactDirectoryTest, UnconfiguredDirectoryFindsCurrentDirectoryArtifacts)
{
    const auto directory = CreateArtifacts("kernel");
    std::filesystem::current_path(directory);
    std::string json;
    ASSERT_EQ(prep.ResolveOrchestratorJson("", json), BM_OK);
    EXPECT_EQ(json, (directory / "libmf_sdma_orch_launch.json").string());
}

TEST_F(SdmaArtifactDirectoryTest, RejectsDirectoryInPlaceOfKernelFile)
{
    const auto directory = CreateArtifacts("kernel");
    const auto kernel = directory / "libmf_sdma_orch.so";
    std::filesystem::remove(kernel);
    std::filesystem::create_directory(kernel);
    std::string json;
    EXPECT_EQ(prep.ResolveOrchestratorJson(root.string(), json), BM_FILE_NOT_ACCESS);
}

/* 用例 1: Acquire 在资源缺失环境(UT stub)必然失败, 且失败后状态干净可重试 */
TEST(SdmaOrchestratorTest, AcquireFailsCleanlyAndRetriable)
{
    auto &prep = SdmaOrchestrator::GetInstance(); // 被测单例
    for (int round = 0; round < 2; round++) {     /* 两次: 验证失败后可重试无残留 */
        auto ret = prep.Acquire(0);               // 设备 0(stub 环境必然初始化失败)
        EXPECT_NE(ret, BM_OK) << "round " << round << ": UT stub 环境下 Acquire 必须失败";         // 必须失败
        EXPECT_FALSE(prep.Ready()) << "round " << round << ": 失败后不得置 ready";                 // 不得就绪
        EXPECT_EQ(prep.GetRefCountForTest(), 0U) << "round " << round << ": 失败后引用计数须归零"; // 计数归零
    }
    /* Release 幂等(refCount 已为 0 时为 no-op, 不得崩溃) */
    prep.Release();                           // 第一次(计数已 0)
    prep.Release();                           // 第二次(幂等不崩溃)
    EXPECT_EQ(prep.GetRefCountForTest(), 0U); // 计数仍为 0
}

/* 用例 2: 未就绪时 C API 拒绝(就绪门控的外显语义; 数据面 API 已废止) */
TEST(SdmaOrchestratorTest, CApiRejectWhenNotReady)
{
    hybm_sdma_info_t info = {};                    // 就绪信息出参
    EXPECT_NE(hybm_sdma_get_info(&info), BM_OK);   // 未就绪须拒绝
    EXPECT_NE(hybm_sdma_get_info(nullptr), BM_OK); /* 出参 null 拒绝 */
}

/* 用例 3: 双头布局锁(hybm 契约与 smem 设备头镜像一致; 违反即布局回归)
 * v29 4-rank 化: 请求/到达环按对端划分; v30 邮件像经 SQE ⓪ 投递;
 * v31 队列按 AICore block 扩展; v32 队列数对齐 hcomm 48(每对端 16 槽位, block 0..7 在用) */
TEST(SdmaOrchestratorTest, DualHeaderLayoutInvariants)
{
    /* 布局头钉死值(单一事实源; 漂移即回归) */
    EXPECT_EQ(SMEM_SHM_SDMA_RESERVED, 2048UL * 1024UL);
    EXPECT_EQ(SMEM_SHM_SDMA_RS_REQ_RING, 0x0000U);
    EXPECT_EQ(SMEM_SHM_SDMA_RS_REQ_SLOTS, 64U);
    EXPECT_EQ(SMEM_SHM_SDMA_RS_REQ_RING_STRIDE, 0x800U);
    EXPECT_EQ(SMEM_SHM_SDMA_RS_REQ_TAIL, 0x18000U);
    EXPECT_EQ(SMEM_SHM_SDMA_RS_REQ_HEAD, 0x18C00U);
    EXPECT_EQ(SMEM_SHM_SDMA_RS_CURSOR_STRIDE, 0x40U);
    EXPECT_EQ(SMEM_SHM_SDMA_RS_QUIET, 0x19800U);
    EXPECT_EQ(SMEM_SHM_SDMA_RS_QUIET_SLOTS, 64U);
    EXPECT_EQ(SMEM_SHM_SDMA_RS_QUIET_STRIDE, 0x200U);
    EXPECT_EQ(SMEM_SHM_SDMA_RS_STAGE, 0x1F800U);
    EXPECT_EQ(SMEM_SHM_SDMA_RS_STAGE_SLOTS, 512U);
    EXPECT_EQ(SMEM_SHM_SDMA_RS_STAGE_STRIDE, 0x4000U);
    EXPECT_EQ(SMEM_SHM_SDMA_RS_PEER_RINGS, 0xDF800U);
    EXPECT_EQ(SMEM_SHM_SDMA_RS_PEER_STAMP, 0x0000U);
    EXPECT_EQ(SMEM_SHM_SDMA_RS_PEER_MAIL, 0x1000U);
    EXPECT_EQ(SMEM_SHM_SDMA_RS_PEER_HEAD, 0x5000U);
    EXPECT_EQ(SMEM_SHM_SDMA_RS_PEER_SLOTS, 512U);
    EXPECT_EQ(SMEM_SHM_SDMA_RS_PEER_STRIDE, 0x5040U);
    EXPECT_EQ(SMEM_SHM_SDMA_MAX_RANKS, 4U);
    EXPECT_EQ(SMEM_SHM_SDMA_MAX_PEERS, 3U);
    EXPECT_EQ(SMEM_SHM_SDMA_MAX_BLOCKS, 8U);
    EXPECT_EQ(SMEM_SHM_SDMA_QUEUES_PER_PEER, 16U);
    EXPECT_EQ(SMEM_SHM_SDMA_QUEUE_COUNT, 48U);
    /* 布局不变式: 本端四组数组各 48 队列完整覆盖且相互不压(数组首尾相接恒等式) */
    EXPECT_EQ(SMEM_SHM_SDMA_RS_REQ_RING + SMEM_SHM_SDMA_QUEUE_COUNT * SMEM_SHM_SDMA_RS_REQ_RING_STRIDE,
              SMEM_SHM_SDMA_RS_REQ_TAIL);
    EXPECT_EQ(SMEM_SHM_SDMA_RS_REQ_TAIL + SMEM_SHM_SDMA_QUEUE_COUNT * SMEM_SHM_SDMA_RS_CURSOR_STRIDE,
              SMEM_SHM_SDMA_RS_REQ_HEAD);
    EXPECT_EQ(SMEM_SHM_SDMA_RS_REQ_HEAD + SMEM_SHM_SDMA_QUEUE_COUNT * SMEM_SHM_SDMA_RS_CURSOR_STRIDE,
              SMEM_SHM_SDMA_RS_QUIET);
    EXPECT_EQ(SMEM_SHM_SDMA_RS_QUIET + SMEM_SHM_SDMA_QUEUE_COUNT * SMEM_SHM_SDMA_RS_QUIET_STRIDE,
              SMEM_SHM_SDMA_RS_STAGE);
    EXPECT_EQ(SMEM_SHM_SDMA_RS_STAGE + SMEM_SHM_SDMA_QUEUE_COUNT * SMEM_SHM_SDMA_RS_STAGE_STRIDE,
              SMEM_SHM_SDMA_RS_PEER_RINGS);
    /* 到达子环: 队列内区块不重叠 + 全部落保留区内(含最深一队列的游标) */
    EXPECT_GE(SMEM_SHM_SDMA_RS_PEER_MAIL, SMEM_SHM_SDMA_RS_PEER_STAMP + 8U * 512U); // 邮件不压戳
    EXPECT_GE(SMEM_SHM_SDMA_RS_PEER_HEAD, SMEM_SHM_SDMA_RS_PEER_MAIL + 32U * 512U); // 游标不压邮件
    EXPECT_LE(SMEM_SHM_SDMA_RS_PEER_RINGS + SMEM_SHM_SDMA_QUEUE_COUNT * SMEM_SHM_SDMA_RS_PEER_STRIDE,
              SMEM_SHM_SDMA_RESERVED); // 全落在 1.25MB 内
    /* v23 缓存行分隔纪律(共行回写竞态=并发邮件丢失挂死类根因; 多 block 并发写同区各槽独占行) */
    for (uint64_t v : {SMEM_SHM_SDMA_RS_REQ_TAIL, SMEM_SHM_SDMA_RS_REQ_HEAD, SMEM_SHM_SDMA_RS_QUIET,
                       SMEM_SHM_SDMA_RS_STAGE, SMEM_SHM_SDMA_RS_PEER_RINGS}) {
        EXPECT_EQ(v & 0x3FU, 0U) << "base 0x" << std::hex << v;
    }
    for (uint64_t v : {SMEM_SHM_SDMA_RS_REQ_RING_STRIDE, SMEM_SHM_SDMA_RS_CURSOR_STRIDE, SMEM_SHM_SDMA_RS_QUIET_STRIDE,
                       SMEM_SHM_SDMA_RS_STAGE_STRIDE, SMEM_SHM_SDMA_RS_PEER_STRIDE, SMEM_SHM_SDMA_RS_PEER_MAIL,
                       SMEM_SHM_SDMA_RS_PEER_HEAD}) {
        EXPECT_EQ(v & 0x3FU, 0U) << "stride 0x" << std::hex << v;
    }
    /* 暂存槽数 > 单队列在途上界(SQ 环深/4——每封 4 支 SQE ①⓪②③) */
    EXPECT_GT(SMEM_SHM_SDMA_RS_STAGE_SLOTS, 1024U / 4U);
    /* 到达深度 > 单子环在途上界(请求环未消费 64 + SQ 在途 256) */
    EXPECT_GT(SMEM_SHM_SDMA_RS_PEER_SLOTS, 64U + 1024U / 4U);
    /* 环深度契约 */
    EXPECT_EQ(SMEM_SHM_SDMA_RS_REQ_SLOTS, 64U);   // 每队列请求环 64
    EXPECT_EQ(SMEM_SHM_SDMA_RS_PEER_SLOTS, 512U); // 每队列到达子环 512
    /* 队列下标编码不变式: (对端, block) 双射 [0, 队列数) */
    for (uint32_t own = 0; own < SMEM_SHM_SDMA_MAX_RANKS; own++) {
        std::vector<uint32_t> seen(SMEM_SHM_SDMA_QUEUE_COUNT, 0);
        for (uint32_t peer = 0; peer < SMEM_SHM_SDMA_MAX_RANKS; peer++) {
            if (peer == own) {
                continue;
            }
            for (uint32_t b = 0; b < SMEM_SHM_SDMA_MAX_BLOCKS; b++) {
                const uint32_t q = SmemShmSdmaQueueIdx(SmemShmSdmaPeerRingIdx(own, peer), b);
                EXPECT_LT(q, SMEM_SHM_SDMA_QUEUE_COUNT) << "own:" << own << " peer:" << peer << " b:" << b;
                seen[q]++;
            }
        }
        for (uint32_t q = 0; q < SMEM_SHM_SDMA_QUEUE_COUNT; q++) {
            /* 在用槽位(block < MAX_BLOCKS)恰好一次; 预留槽位(8..15)无映射恒零 */
            EXPECT_EQ(seen[q], (q % SMEM_SHM_SDMA_QUEUES_PER_PEER < SMEM_SHM_SDMA_MAX_BLOCKS) ? 1U : 0U)
                << "own:" << own << " q:" << q;
        }
    }
    /* 工作区布局(result/ovf/stop) */
    EXPECT_GT(HYBM_SDMA_ORCH_WS_OVF_OFF, HYBM_SDMA_ORCH_WS_RESULT_OFF);     // ovf 在 result 后
    EXPECT_GE(HYBM_SDMA_ORCH_WS_STOP_OFF, HYBM_SDMA_ORCH_WS_OVF_OFF + 64U); // stop 不压 ovf
    EXPECT_LE(HYBM_SDMA_ORCH_WS_STOP_OFF + 8U, HYBM_SDMA_ORCH_WS_SIZE);     // 落在 4KB 内
    /* 心跳槽(v11 时限时钟): 不压 stop(可紧邻), 落在 4KB 内 */
    EXPECT_GE(HYBM_SDMA_ORCH_WS_HEARTBEAT_OFF, HYBM_SDMA_ORCH_WS_STOP_OFF + 8U);
    EXPECT_LE(HYBM_SDMA_ORCH_WS_HEARTBEAT_OFF + 8U, HYBM_SDMA_ORCH_WS_SIZE);
    EXPECT_EQ(HYBM_SDMA_ORCH_EXIT_RUNNING, 0xFFFFFFFFu); // 运行中标记(host 轮询判据)
}

/* 用例 3b: 契约不变式(KFCKernel/type 5 不受设备侧 28s 强制结束约束; v11: 按时长分轮+主机报时) */
TEST(SdmaOrchestratorTest, V9ResidentInvariants)
{
    /* 参数布局锁: v29 重构(reqBase/quietBase→resBase, 尾部追加 rank/rsv/poolBaseGva), 80B */
    EXPECT_EQ(sizeof(HybmSdmaOrchParam), 96U);
    EXPECT_EQ(offsetof(HybmSdmaOrchParam, epochBudgetS), 12U);     // 第 4 个 u32 槽位
    EXPECT_EQ(offsetof(HybmSdmaOrchParam, resBase), 16U);          // 保留区基址(定位原点)
    EXPECT_EQ(offsetof(HybmSdmaOrchParam, rank), 64U);             // v29 追加: 本端 rank
    EXPECT_EQ(offsetof(HybmSdmaOrchParam, poolBaseGva), 80U - 8U); // v29 追加: 池窗口基址
    /* 永不退出的专用值: 全 F(不判报时/无循环上限, 跑到停止/错误); 与默认值 0 语义区分 */
    EXPECT_EQ(HYBM_SDMA_ORCH_EPOCH_BUDGET_RESIDENT, 0xFFFFFFFFu);
    EXPECT_NE(HYBM_SDMA_ORCH_EPOCH_BUDGET_RESIDENT, HYBM_SDMA_ORCH_EPOCH_TIME_BUDGET_S);
    /* launch attr 超时: 必须 0(永不超时)——有限值到达即把任务标记为 507017 超时,
     * StopEpoch 的 sync 提前返回不等 kernel 退出; attr 缺省为默认超时 ≈6.4s 同样误杀 */
    EXPECT_EQ(HYBM_SDMA_ORCH_LAUNCH_TIMEOUT_NEVER_S, 0U);
}

/* 用例 4: smem 层就绪判定 API 的无效 handle 拒绝(不依赖任何初始化; 仅存 host API) */
TEST(SdmaServiceLayoutTest, HostControlsDoNotShareDiagnosticCacheLines)
{
    EXPECT_NE(HYBM_SDMA_ORCH_WS_STOP_OFF / 64, HYBM_SDMA_ORCH_WS_DBG_DOORBELL_OFF / 64);
    EXPECT_NE(HYBM_SDMA_ORCH_WS_HEARTBEAT_OFF / 64, HYBM_SDMA_ORCH_WS_DBG_RING_CNT_OFF / 64);
    EXPECT_EQ(HYBM_SDMA_ORCH_WS_DBG_RING_OFF % 64, 0U);
    EXPECT_LE(HYBM_SDMA_ORCH_WS_DBG_RING_OFF + HYBM_SDMA_ORCH_DBG_RING_SLOTS * sizeof(HybmSdmaOrchDbgMail),
              HYBM_SDMA_ORCH_WS_PAD_SCRATCH_OFF & ~uint64_t(63));
    EXPECT_LE(SMEM_SHM_SDMA_RS_PUBLISHING + SMEM_SHM_SDMA_QUEUE_COUNT * SMEM_SHM_SDMA_RS_CURSOR_STRIDE,
              SMEM_SHM_SDMA_RESERVED);
}

TEST(SmemShmSdmaApiTest, InvalidHandleRejected)
{
    EXPECT_EQ(smem_shm_sdma_get_workspace(nullptr), nullptr); // 工作区返回 NULL(就绪判定)
}
} // namespace mf
} // namespace ock
