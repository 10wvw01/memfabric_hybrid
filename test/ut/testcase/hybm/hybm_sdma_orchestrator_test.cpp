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
 *   6. 协议握手(ConfigureOrch 轮询结果区): 成功/旧魔数/错误退出/轮询耗尽/首轮下发失败
 */
#include <gtest/gtest.h> // googletest 框架
#include <cstddef>      // offsetof
#include <vector>

#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <mockcpp/mockcpp.hpp>
#include "hybm_common_include.h"
#define private public
#include "dl_acl_api.h"             // 指针直换式注入需要 DlAclApi 私有函数指针可写(同 urma 用例手法)
#include "hybm_sdma_orchestrator.h" // 被测单例
#undef private

#include "hybm_big_mem.h" // 被测 C API 声明(hybm_sdma_*)
#include "hybm_define.h"  // BM_OK/BM_INVALID_PARAM 等错误码
#include "smem.h"         // smem 层公共定义(SMEM_OK)
#include "smem_shm.h"     // 被测 smem 层 sdma 协同 API 声明
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
    }
    void TearDown() override
    {
        GlobalMockObject::verify();
        unlink(input.c_str());
        unlink(runtime.c_str());
    }
    static int32_t Capture(SdmaOrchestrator *, const std::string &cmd, bool)
    {
        commands.push_back(cmd);
        return BM_OK;
    }
    void MockChannel()
    {
        MOCKER_CPP(&SdmaOrchestrator::KfcExec, int32_t(*)(SdmaOrchestrator *, const std::string &, bool))
            .stubs()
            .will(invoke(Capture));
    }
    SdmaOrchestrator prep;
    std::string input;
    std::string runtime;
    static std::vector<std::string> commands;
};
std::vector<std::string> SdmaDeployTest::commands;

TEST_F(SdmaDeployTest, GeneratesSeparateNamesWithoutManualVersion)
{
    ASSERT_EQ(prep.PrepareRuntimeJson(input, runtime), BM_OK);
    const auto firstName = prep.deviceSoName_;
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
    for (const auto &value : {"{}", "{\"kernelSo\":\"../bad.so\"}", "{\"kernelSo\":\"a;bad.so\"}"}) {
        std::ofstream(input) << value;
        EXPECT_EQ(prep.PrepareRuntimeJson(input, runtime), BM_INVALID_PARAM);
        EXPECT_TRUE(runtime.empty());
    }
}

TEST_F(SdmaDeployTest, ChannelFailureDoesNotAssumePredeployment)
{
    ASSERT_EQ(prep.PrepareRuntimeJson(input, runtime), BM_OK);
    MOCKER_CPP(&SdmaOrchestrator::KfcExec, int32_t(*)(SdmaOrchestrator *, const std::string &, bool))
        .stubs()
        .will(returnValue(BM_ERROR));
    EXPECT_EQ(prep.DeployOrchestratorKernel("/missing", runtime), BM_ERROR);
    EXPECT_FALSE(prep.deviceSoOwned_);
}

TEST_F(SdmaDeployTest, MissingHostArtifactFailsBeforeCopy)
{
    ASSERT_EQ(prep.PrepareRuntimeJson(input, runtime), BM_OK);
    MockChannel();
    EXPECT_EQ(prep.DeployOrchestratorKernel("/missing", runtime), BM_FILE_NOT_ACCESS);
    ASSERT_EQ(commands.size(), 1U);
    EXPECT_EQ(commands[0].find("test ! -e "), 0U);
    EXPECT_FALSE(prep.deviceSoOwned_);
}

TEST_F(SdmaDeployTest, CleanupRemovesOnlyOwnedDeviceFile)
{
    MockChannel();
    prep.launchStream_ = reinterpret_cast<void *>(1);
    prep.deviceSoName_ = "libmf_sdma_orch_test.so";
    prep.deviceSoOwned_ = true;
    MOCKER(DlAclApi::AclrtDestroyStream).stubs().will(returnValue(BM_OK));
    prep.Cleanup();
    ASSERT_EQ(commands.size(), 1U);
    EXPECT_EQ(commands[0], "rm -f /usr/lib64/aicpu_kernels/0/aicpu_kernels_device/libmf_sdma_orch_test.so");
    EXPECT_TRUE(prep.deviceSoName_.empty());
    EXPECT_FALSE(prep.deviceSoOwned_);
    prep.Cleanup();
    EXPECT_EQ(commands.size(), 1U);
}

TEST_F(SdmaDeployTest, RetryPreservesCustFilesAndCopiesThroughCurrentProcessLink)
{
    char path[] = "/tmp/mf_cust_retry_XXXXXX";
    ASSERT_NE(mkdtemp(path), nullptr);
    const std::string dir = path;
    std::ofstream(dir + "/libmf_sdma_orch.so") << "kernel bytes";
    std::ofstream(dir + "/libmf_sdma_orch.json") << "{}";
    MockChannel();
    MOCKER(DlAclApi::AclrtBinaryLoadFromFile).stubs().will(returnValue(BM_OK));
    MOCKER(DlAclApi::AclrtBinaryGetFunction).stubs().will(returnValue(BM_OK));
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        uint64_t size = 0;
        ASSERT_EQ(prep.MigrateViaCustChannel(dir, size), BM_OK);
        EXPECT_EQ(size, 12U);
    }
    EXPECT_TRUE(commands.empty());
    prep.dieId_ = 1;
    ASSERT_EQ(prep.CopyToSearchPath("libmf_sdma_orch_retry.so", 12), BM_OK);
    ASSERT_EQ(commands.size(), 2U);
    EXPECT_NE(commands[0].find("cust_aicpu_1_*_" + std::to_string(getpid()) + "/libmf_sdma_orch.so"),
              std::string::npos);
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

/* ---- 协议握手(ConfigureOrch 轮询结果区) ----
 * 注入方式: 直接替换 DlAclApi 函数指针(同 urma 用例手法), memcpy 按 host 内存真实拷贝,
 * 用例只需在工作区结果区预置 kernel 视角的字节即可驱动握手各分支。
 */
namespace {
uint32_t gHandshakeReadCount = 0; // memcpy 注入计数(轮询预算回归锚点)

int32_t HostPathMemcpy(void *dst, size_t destMax, const void *src, size_t count, uint32_t kind)
{
    (void)kind;
    if (dst == nullptr || src == nullptr || count > destMax) {
        return BM_INVALID_PARAM;
    }
    ++gHandshakeReadCount;
    std::memcpy(dst, src, count);
    return BM_OK;
}

int32_t AlwaysOkMemset(void *dst, size_t destMax, int32_t value, size_t count)
{
    (void)dst;
    (void)destMax;
    (void)value;
    (void)count;
    return BM_OK; // 保留区基址是设备侧地址, UT 无设备内存, 清零不可真实执行
}

int AlwaysOkCreateStream(void **stream)
{
    (void)stream; // 不写 *stream: epochStream_ 保持空, 销毁路径按空流早退
    return BM_OK;
}
} // namespace

class SdmaHandshakeTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        oldMemcpy_ = DlAclApi::pAclrtMemcpy;
        oldMemset_ = DlAclApi::pAclrtMemset;
        oldCreateStream_ = DlAclApi::pAclrtCreateStream;
        DlAclApi::pAclrtMemcpy = HostPathMemcpy;
        DlAclApi::pAclrtMemset = AlwaysOkMemset;
        DlAclApi::pAclrtCreateStream = AlwaysOkCreateStream;
        gHandshakeReadCount = 0;
        ws.fill(0);
        prep.workspace_ = reinterpret_cast<uint64_t>(ws.data()); // 工作区指向 host 内存
    }
    void TearDown() override
    {
        GlobalMockObject::verify();
        DlAclApi::pAclrtMemcpy = oldMemcpy_;
        DlAclApi::pAclrtMemset = oldMemset_;
        DlAclApi::pAclrtCreateStream = oldCreateStream_;
    }
    void MockLaunchOk()
    {
        MOCKER_CPP(&SdmaOrchestrator::LaunchEpoch, int32_t (*)(SdmaOrchestrator *, void *))
            .stubs()
            .will(returnValue(BM_OK));
    }
    /* 在工作区结果区预置 kernel 视角的字节(magic + 退出原因) */
    void PublishResult(uint32_t magic, uint32_t exitReason)
    {
        auto *r = reinterpret_cast<HybmSdmaOrchResult *>(ws.data() + HYBM_SDMA_ORCH_WS_RESULT_OFF);
        r->magic = magic;
        r->exitReason = exitReason;
    }
    auto Configure() { return prep.ConfigureOrch(kPoolBaseGva, kSegSize, kSegSize, 0, kPoolBaseGva); }

    static constexpr uint64_t kPoolBaseGva = 0x100000000ULL;
    static constexpr uint64_t kSegSize = 64ULL * 1024 * 1024;
    SdmaOrchestrator prep;
    std::array<uint8_t, HYBM_SDMA_ORCH_WS_SIZE> ws;
    aclrtMemcpyFunc oldMemcpy_{nullptr};
    aclrtMemsetFunc oldMemset_{nullptr};
    aclrtCreateStreamFunc oldCreateStream_{nullptr};
};

/* 用例 6a: magic 正确且运行中 → 建池成功; 失败侧状态翻转由显式 StopEpoch 收尾 */
TEST_F(SdmaHandshakeTest, AcceptsRunningKernelAndConfiguresPool)
{
    MockLaunchOk();
    PublishResult(HYBM_SDMA_ORCH_RESULT_MAGIC, HYBM_SDMA_ORCH_EXIT_RUNNING);
    EXPECT_EQ(Configure(), BM_OK);
    EXPECT_TRUE(prep.configured_);
    EXPECT_TRUE(prep.epochLaunched_);
    prep.StopEpoch(); // 默认预算会启动监督线程, 先结束再离场, 避免可 join 线程随用例析构
    EXPECT_FALSE(prep.epochLaunched_);
}

/* 用例 6b: 旧协议版本的魔数必须被拒绝(混布旧 kernel 时建池失败) */
TEST_F(SdmaHandshakeTest, RejectsStaleKernelMagic)
{
    MockLaunchOk();
    PublishResult(0x5D0A0005u, HYBM_SDMA_ORCH_EXIT_RUNNING); // 旧协议版本的魔数
    EXPECT_EQ(Configure(), BM_ERROR);
    EXPECT_FALSE(prep.configured_);
    EXPECT_FALSE(prep.epochLaunched_); // 失败路径经 StopEpoch 复位, 不得残留已下发状态
}

/* 用例 6c: 魔数正确但 kernel 已错误退出 → 建池失败 */
TEST_F(SdmaHandshakeTest, RejectsKernelExitedWithError)
{
    MockLaunchOk();
    PublishResult(HYBM_SDMA_ORCH_RESULT_MAGIC, HYBM_SDMA_ORCH_EXIT_ERROR);
    EXPECT_EQ(Configure(), BM_ERROR);
    EXPECT_FALSE(prep.configured_);
    EXPECT_FALSE(prep.epochLaunched_);
}

/* 用例 6d: kernel 未启动(结果区恒 0) → 轮询预算耗尽后失败返回, 不得死循环 */
TEST_F(SdmaHandshakeTest, ExhaustsPollBudgetWhenKernelNeverStarts)
{
    MockLaunchOk();
    EXPECT_EQ(Configure(), BM_ERROR);
    EXPECT_FALSE(prep.configured_);
    EXPECT_FALSE(prep.epochLaunched_);
    EXPECT_EQ(gHandshakeReadCount, 201U); // 200 次轮询 + StopEpoch 诊断读 1 次
}

/* 用例 6e: 首轮下发失败必须立即报错, 不得进入握手(防止池看似就绪实则无消费者) */
TEST_F(SdmaHandshakeTest, LaunchFailureFailsBeforeHandshakeWithoutHalfState)
{
    MOCKER_CPP(&SdmaOrchestrator::LaunchEpoch, int32_t (*)(SdmaOrchestrator *, void *))
        .stubs()
        .will(returnValue(BM_NOT_INITIALIZED));
    EXPECT_EQ(Configure(), BM_NOT_INITIALIZED);
    EXPECT_FALSE(prep.configured_);
    EXPECT_FALSE(prep.epochLaunched_);
    EXPECT_EQ(gHandshakeReadCount, 0U); // 未进入握手轮询
}

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
    EXPECT_GE(SMEM_SHM_SDMA_RS_PEER_MAIL, SMEM_SHM_SDMA_RS_PEER_STAMP + 8U * 512U);           // 邮件不压戳
    EXPECT_GE(SMEM_SHM_SDMA_RS_PEER_HEAD, SMEM_SHM_SDMA_RS_PEER_MAIL + 32U * 512U);           // 游标不压邮件
    EXPECT_LE(SMEM_SHM_SDMA_RS_PEER_RINGS + SMEM_SHM_SDMA_QUEUE_COUNT * SMEM_SHM_SDMA_RS_PEER_STRIDE,
              SMEM_SHM_SDMA_RESERVED);                                                        // 全落在 1.25MB 内
    /* v23 缓存行分隔纪律(共行回写竞态=并发邮件丢失挂死类根因; 多 block 并发写同区各槽独占行) */
    for (uint64_t v : {SMEM_SHM_SDMA_RS_REQ_TAIL, SMEM_SHM_SDMA_RS_REQ_HEAD, SMEM_SHM_SDMA_RS_QUIET,
                       SMEM_SHM_SDMA_RS_STAGE, SMEM_SHM_SDMA_RS_PEER_RINGS}) {
        EXPECT_EQ(v & 0x3FU, 0U) << "base 0x" << std::hex << v;
    }
    for (uint64_t v : {SMEM_SHM_SDMA_RS_REQ_RING_STRIDE, SMEM_SHM_SDMA_RS_CURSOR_STRIDE,
                       SMEM_SHM_SDMA_RS_QUIET_STRIDE, SMEM_SHM_SDMA_RS_STAGE_STRIDE,
                       SMEM_SHM_SDMA_RS_PEER_STRIDE, SMEM_SHM_SDMA_RS_PEER_MAIL, SMEM_SHM_SDMA_RS_PEER_HEAD}) {
        EXPECT_EQ(v & 0x3FU, 0U) << "stride 0x" << std::hex << v;
    }
    /* 暂存槽数 > 单队列在途上界(SQ 环深/4——每封 4 支 SQE ①⓪②③) */
    EXPECT_GT(SMEM_SHM_SDMA_RS_STAGE_SLOTS, 1024U / 4U);
    /* 到达深度 > 单子环在途上界(请求环未消费 64 + SQ 在途 256) */
    EXPECT_GT(SMEM_SHM_SDMA_RS_PEER_SLOTS, 64U + 1024U / 4U);
    /* 环深度契约 */
    EXPECT_EQ(SMEM_SHM_SDMA_RS_REQ_SLOTS, 64U);   // 每队列请求环 64
    EXPECT_EQ(SMEM_SHM_SDMA_RS_PEER_SLOTS, 512U);  // 每队列到达子环 512
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
            EXPECT_EQ(seen[q],
                      (q % SMEM_SHM_SDMA_QUEUES_PER_PEER < SMEM_SHM_SDMA_MAX_BLOCKS) ? 1U : 0U)
                << "own:" << own << " q:" << q;
        }
    }
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
    /* 参数布局锁: v29 重构(reqBase/quietBase→resBase, 尾部追加 rank/rsv/poolBaseGva), 80B */
    EXPECT_EQ(sizeof(HybmSdmaOrchParam), 80U);
    EXPECT_EQ(offsetof(HybmSdmaOrchParam, epochBudgetS), 12U); // 第 4 个 u32 槽位
    EXPECT_EQ(offsetof(HybmSdmaOrchParam, resBase), 16U);       // 保留区基址(定位原点)
    EXPECT_EQ(offsetof(HybmSdmaOrchParam, rank), 64U);          // v29 追加: 本端 rank
    EXPECT_EQ(offsetof(HybmSdmaOrchParam, poolBaseGva), 80U - 8U); // v29 追加: 池窗口基址
    /* 永不退出的专用值: 全 F(不判报时/无循环上限, 跑到停止/错误); 与默认值 0 语义区分 */
    EXPECT_EQ(HYBM_SDMA_ORCH_EPOCH_BUDGET_RESIDENT, 0xFFFFFFFFu);
    EXPECT_NE(HYBM_SDMA_ORCH_EPOCH_BUDGET_RESIDENT, HYBM_SDMA_ORCH_EPOCH_TIME_BUDGET_S);
    /* launch attr 超时: 必须 0(永不超时)——有限值到达即把任务标记为 507017 超时,
     * StopEpoch 的 sync 提前返回不等 kernel 退出; attr 缺省为默认超时 ≈6.4s 同样误杀 */
    EXPECT_EQ(HYBM_SDMA_ORCH_LAUNCH_TIMEOUT_NEVER_S, 0U);
}

/* 用例 4: smem 层就绪判定 API 的无效 handle 拒绝(不依赖任何初始化; 仅存 host API) */
TEST(SmemShmSdmaApiTest, InvalidHandleRejected)
{
    EXPECT_EQ(smem_shm_sdma_get_workspace(nullptr), nullptr); // 工作区返回 NULL(就绪判定)
}
} // namespace mf
} // namespace ock
