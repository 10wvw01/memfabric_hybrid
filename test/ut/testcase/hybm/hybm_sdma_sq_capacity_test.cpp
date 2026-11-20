#if defined(__aarch64__)
#include <gtest/gtest.h>
#include <array>
#include <cstring>
#include <vector>
#include <unistd.h>
#include <cstdarg>
#include <ctime>

extern "C" int SdmaCapacitySleep(useconds_t);
extern "C" long SdmaCapacityClock(long, ...);
#define halSqCqQuery  SdmaCapacityQuery
#define halSqCqConfig SdmaCapacityConfig
#define usleep        SdmaCapacitySleep
#define syscall       SdmaCapacityClock
#include "../../../../src/hybm/ops/hybm_kernel/hybm_sdma_orch_kernel.cc"
#undef syscall
#undef usleep
#undef halSqCqConfig
#undef halSqCqQuery

namespace {
uint32_t queryCount, configCount, sleepCount, nextHead, publishedTail;
uint64_t clockNowNs;
uint32_t clockCalls;
bool clockFailure;
int configResult;
bool replayHardware;
SqContext *hardwareContext;
uint32_t hardwareHead;
std::vector<uint16_t> executedTasks;
} // namespace
extern "C" int SdmaCapacitySleep(useconds_t)
{
    ++sleepCount;
    return 0;
}
extern "C" int32_t SdmaCapacityQuery(uint32_t, void *info)
{
    ++queryCount;
    static_cast<HalQueryInfo *>(info)->value[0] = nextHead;
    return 0;
}
extern "C" int32_t SdmaCapacityConfig(uint32_t, void *info)
{
    ++configCount;
    publishedTail = static_cast<HalConfigInfo *>(info)->value[0];
    if (replayHardware && configResult == 0 && publishedTail != 0) {
        while (hardwareHead != publishedTail) {
            uint16_t task = 0;
            std::memcpy(&task, reinterpret_cast<void *>(hardwareContext->sqBase + hardwareHead * kSqeSize + 6), 2);
            executedTasks.push_back(task);
            hardwareHead = (hardwareHead + 1) % hardwareContext->sqDepth;
        }
        nextHead = hardwareHead;
    }
    return configResult;
}

extern "C" long SdmaCapacityClock(long number, ...)
{
    ++clockCalls;
    if (clockFailure)
        return -1;
    va_list args;
    va_start(args, number);
    (void)va_arg(args, int);
    auto *ts = va_arg(args, timespec *);
    ts->tv_sec = clockNowNs / 1000000000ULL;
    ts->tv_nsec = clockNowNs % 1000000000ULL;
    va_end(args);
    return 0;
}

class SdmaSqCapacityTest : public testing::Test {
protected:
    alignas(64) std::array<uint8_t, 8 * kSqeSize> ring{};
    alignas(64) std::array<uint8_t, 8192> workspace{};
    ock::mf::HybmSdmaOrchParam param{};
    SqContext context{};
    alignas(8) uint8_t sqe[kSqeSize]{};
    void SetUp() override
    {
        queryCount = configCount = sleepCount = 0;
        clockNowNs = 1000000000ULL;
        clockCalls = 0;
        clockFailure = false;
        nextHead = 2;
        configResult = 0;
        replayHardware = false;
        hardwareContext = &context;
        hardwareHead = 0;
        executedTasks.clear();
        param.resultAddr = reinterpret_cast<uint64_t>(workspace.data());
        context.sqBase = reinterpret_cast<uint64_t>(ring.data());
        context.sqDepth = 8;
        context.sqHead = 0;
        context.sqTail = 7;
        std::memset(ring.data(), 0xA5, ring.size());
        std::memset(sqe, 0x3C, sizeof(sqe));
    }
};

TEST_F(SdmaSqCapacityTest, PollingBudgetUsesElapsedTimeInsteadOfScanCount)
{
    EpochBudget budget{};
    param.epochBudgetS = 2;
    ASSERT_EQ(initEpochBudget(&param, budget), 0);
    auto *result = reinterpret_cast<ock::mf::HybmSdmaOrchResult *>(workspace.data());
    EXPECT_EQ(checkEpochBudget(result, budget, 220032), 0);
    clockNowNs = 3000000000ULL;
    EXPECT_EQ(checkEpochBudget(result, budget, 220096), 1);
    EXPECT_EQ(sleepCount, 0U);
}

TEST_F(SdmaSqCapacityTest, PollingClockIsSampledAndHeartbeatStillExpires)
{
    EpochBudget budget{};
    param.epochBudgetS = 2;
    ASSERT_EQ(initEpochBudget(&param, budget), 0);
    auto *result = reinterpret_cast<ock::mf::HybmSdmaOrchResult *>(workspace.data());
    EXPECT_EQ(checkEpochBudget(result, budget, 1), 0);
    EXPECT_EQ(clockCalls, 1U);
    auto *heartbeat = reinterpret_cast<uint64_t *>(workspace.data() + ock::mf::HYBM_SDMA_ORCH_WS_HEARTBEAT_OFF);
    *heartbeat = 20;
    EXPECT_EQ(checkEpochBudget(result, budget, 2), 1);
    EXPECT_EQ(clockCalls, 1U);
}

TEST_F(SdmaSqCapacityTest, PollingClockErrorsAreReported)
{
    EpochBudget budget{};
    clockFailure = true;
    EXPECT_EQ(initEpochBudget(&param, budget), 1370);
    clockFailure = false;
    ASSERT_EQ(initEpochBudget(&param, budget), 0);
    clockFailure = true;
    auto *result = reinterpret_cast<ock::mf::HybmSdmaOrchResult *>(workspace.data());
    EXPECT_EQ(checkEpochBudget(result, budget, 0), -1370);
}

TEST_F(SdmaSqCapacityTest, ResidentPollingDoesNotRequireClockOrHeartbeat)
{
    EpochBudget budget{};
    param.epochBudgetS = ock::mf::HYBM_SDMA_ORCH_EPOCH_BUDGET_RESIDENT;
    clockFailure = true;
    ASSERT_EQ(initEpochBudget(&param, budget), 0);
    auto *result = reinterpret_cast<ock::mf::HybmSdmaOrchResult *>(workspace.data());
    EXPECT_EQ(checkEpochBudget(result, budget, 0), 0);
    EXPECT_EQ(clockCalls, 0U);
}

TEST_F(SdmaSqCapacityTest, FullQueuePublishesPendingTasksAndRefreshesHead)
{
    EXPECT_EQ(submitSqe(&param, &context, sqe), 0);
    EXPECT_EQ(configCount, 1U);
    EXPECT_EQ(queryCount, 1U);
    EXPECT_EQ(context.sqHead, 2U);
    EXPECT_EQ(context.sqTail, 0U);
    EXPECT_EQ(ring[0], 0xA5);
}

TEST_F(SdmaSqCapacityTest, WrappedFullQueueAlsoWaitsForConsumption)
{
    context.sqTail = 1;
    context.sqHead = 2;
    nextHead = 4;
    EXPECT_EQ(submitSqe(&param, &context, sqe), 0);
    EXPECT_EQ(configCount, 1U);
    EXPECT_EQ(queryCount, 1U);
    EXPECT_EQ(context.sqHead, 4U);
}

TEST_F(SdmaSqCapacityTest, StalledConsumerDoesNotChangeQueueOrTail)
{
    nextHead = 0;
    EXPECT_NE(submitSqe(&param, &context, sqe), 0);
    EXPECT_EQ(context.sqTail, 7U);
    EXPECT_EQ(ring[7 * kSqeSize], 0xA5);
    EXPECT_GT(queryCount, 0U);
}

TEST_F(SdmaSqCapacityTest, DoorbellFailureDoesNotWriteAnotherDescriptor)
{
    configResult = 1;
    EXPECT_NE(submitSqe(&param, &context, sqe), 0);
    EXPECT_EQ(context.sqTail, 7U);
    EXPECT_EQ(ring[7 * kSqeSize], 0xA5);
}

TEST_F(SdmaSqCapacityTest, QueueWithSpaceDoesNotPublishEarly)
{
    context.sqTail = 6;
    EXPECT_EQ(submitSqe(&param, &context, sqe), 0);
    EXPECT_EQ(configCount, 0U);
    EXPECT_EQ(queryCount, 0U);
    EXPECT_EQ(context.sqTail, 7U);
}

TEST_F(SdmaSqCapacityTest, WholeTransferReservesSpaceBeforeWritingDescriptors)
{
    context.sqTail = 6;
    nextHead = 6;
    ock::mf::HybmSdmaReqMail mail{0, 0, 4, 0};
    EXPECT_EQ(issueMailSqes(&param, &context, 0, 0, mail, 0, 0, 0), 0);
    EXPECT_EQ(configCount, 1U);
    EXPECT_EQ(publishedTail, 6U);
    EXPECT_EQ(queryCount, 1U);
    EXPECT_EQ(context.sqTail, 1U);
}

TEST_F(SdmaSqCapacityTest, PendingTransfersSurviveQueueWrap)
{
    replayHardware = true;
    context.sqTail = 6;
    for (uint16_t task = 0; task < 6; ++task)
        std::memcpy(ring.data() + task * kSqeSize + 6, &task, 2);
    ock::mf::HybmSdmaReqMail mail{0, 0, 4, 0};
    ASSERT_EQ(issueMailSqes(&param, &context, 0, 10, mail, 0, 0, 0), 0);
    ASSERT_EQ(flushBatchDoorbell(&param, &context), 0);
    EXPECT_EQ(executedTasks, (std::vector<uint16_t>{0, 1, 2, 3, 4, 5, 12, 10, 11}));
}

class SdmaDirectionTest : public SdmaSqCapacityTest {
protected:
    std::vector<uint64_t> reserved = std::vector<uint64_t>(SMEM_SHM_SDMA_RESERVED / sizeof(uint64_t), 0);
    void SetUp() override
    {
        SdmaSqCapacityTest::SetUp();
        param.rank = 0;
        param.symSize = 64UL * 1024UL * 1024UL;
        param.physSize = param.symSize;
        param.poolBaseGva = 0x100000000ULL;
        param.resBase = reinterpret_cast<uint64_t>(reserved.data());
        context.sqTail = 0;
        context.sqHead = 0;
    }

    void SetMail(bool isGet, uint64_t imm = 0)
    {
        auto *mail = reinterpret_cast<ock::mf::HybmSdmaReqMail *>(
            param.resBase + SMEM_SHM_SDMA_RS_REQ_RING);
        const uint64_t local = param.poolBaseGva + 4096;
        const uint64_t remote = param.poolBaseGva + param.symSize + 8192;
        *mail = {isGet ? remote : local, isGet ? local : remote, (80 * 1024) | (1ULL << 63), imm};
    }

    uint64_t DescriptorAddress(uint32_t descriptor, uint32_t offset)
    {
        uint64_t value = 0;
        std::memcpy(&value, ring.data() + descriptor * kSqeSize + offset, sizeof(value));
        return value;
    }
};

TEST_F(SdmaDirectionTest, GetUsesRemoteSourceAndLocalCompletion)
{
    SetMail(true);
    ASSERT_EQ(consumeOneMail(&param, &context, 0, 0, 0), 0);
    EXPECT_EQ(context.sqTail, 2U);
    EXPECT_EQ(DescriptorAddress(0, 32), param.poolBaseGva + param.symSize + 8192);
    EXPECT_EQ(DescriptorAddress(0, 40), param.poolBaseGva + 4096);
    EXPECT_EQ(DescriptorAddress(1, 40), param.resBase + SMEM_SHM_SDMA_RS_QUIET);
    EXPECT_EQ(loadCell(param.resBase + SMEM_SHM_SDMA_RS_REQ_HEAD), 1U);
}

TEST_F(SdmaDirectionTest, PutRetainsRemoteDestinationAndAddOpcode)
{
    SetMail(false, (0x5244ULL << 48) | (0x02ULL << 40));
    ASSERT_EQ(consumeOneMail(&param, &context, 0, 0, 0), 0);
    EXPECT_EQ(DescriptorAddress(0, 32), param.poolBaseGva + 4096);
    EXPECT_EQ(DescriptorAddress(0, 40), param.poolBaseGva + param.symSize + 8192);
    EXPECT_EQ(ring[16], 0x02);
    EXPECT_EQ(context.sqTail, 2U);
}

TEST_F(SdmaDirectionTest, GetSupportsOrdinaryLocalDeviceMemory)
{
    SetMail(true);
    auto *mail = reinterpret_cast<ock::mf::HybmSdmaReqMail *>(param.resBase + SMEM_SHM_SDMA_RS_REQ_RING);
    mail->dst = 0x200000000ULL;
    ASSERT_EQ(consumeOneMail(&param, &context, 0, 0, 0), 0);
    EXPECT_EQ(DescriptorAddress(0, 40), 0x200000000ULL);
}

TEST_F(SdmaDirectionTest, PutSupportsOrdinaryLocalDeviceMemory)
{
    SetMail(false);
    auto *mail = reinterpret_cast<ock::mf::HybmSdmaReqMail *>(param.resBase + SMEM_SHM_SDMA_RS_REQ_RING);
    mail->src = 0x200000000ULL;
    ASSERT_EQ(consumeOneMail(&param, &context, 0, 0, 0), 0);
    EXPECT_EQ(DescriptorAddress(0, 32), 0x200000000ULL);
}

TEST_F(SdmaDirectionTest, GetRejectsAddBeforeSubmittingDescriptors)
{
    SetMail(true, (0x5244ULL << 48) | (0x02ULL << 40));
    EXPECT_EQ(consumeOneMail(&param, &context, 0, 0, 0), 1330);
    EXPECT_EQ(context.sqTail, 0U);
}

TEST_F(SdmaDirectionTest, RejectsRequestsWhoseEndpointsDoNotMatchTheQueue)
{
    SetMail(false);
    auto *mail = reinterpret_cast<ock::mf::HybmSdmaReqMail *>(param.resBase + SMEM_SHM_SDMA_RS_REQ_RING);
    mail->src += param.symSize;
    EXPECT_EQ(consumeOneMail(&param, &context, 0, 0, 0), 1330);
    EXPECT_EQ(context.sqTail, 0U);
}

TEST_F(SdmaDirectionTest, TransferPutUsesTwoDescriptorsAndNoArrival)
{
    SetMail(false);
    auto *mail = reinterpret_cast<ock::mf::HybmSdmaReqMail *>(param.resBase + SMEM_SHM_SDMA_RS_REQ_RING);
    mail->len |= 1ULL << 63;
    ASSERT_EQ(consumeOneMail(&param, &context, 0, 0, 0), 0);
    EXPECT_EQ(context.sqTail, 2U);
    EXPECT_EQ(DescriptorAddress(1, 40), param.resBase + SMEM_SHM_SDMA_RS_QUIET);
}

TEST_F(SdmaDirectionTest, RejectsUndefinedLengthBitsAndReservedRange)
{
    SetMail(true);
    auto *mail = reinterpret_cast<ock::mf::HybmSdmaReqMail *>(param.resBase + SMEM_SHM_SDMA_RS_REQ_RING);
    mail->len |= 1ULL << 40;
    EXPECT_EQ(consumeOneMail(&param, &context, 0, 0, 0), 1331);
    SetMail(true);
    mail->src = param.poolBaseGva + param.symSize + param.physSize - SMEM_SHM_SDMA_RESERVED;
    EXPECT_EQ(consumeOneMail(&param, &context, 0, 0, 0), 1330);
    EXPECT_EQ(context.sqTail, 0U);
}

TEST_F(SdmaDirectionTest, UnmarkedRequestsRejected)
{
    SetMail(true);
    auto *mail = reinterpret_cast<ock::mf::HybmSdmaReqMail *>(param.resBase + SMEM_SHM_SDMA_RS_REQ_RING);
    mail->len &= UINT32_MAX;
    EXPECT_EQ(consumeOneMail(&param, &context, 0, 0, 0), 1334);
    SetMail(false);
    mail->len &= UINT32_MAX;
    EXPECT_EQ(consumeOneMail(&param, &context, 0, 0, 0), 1334);
    EXPECT_EQ(context.sqTail, 0U);
}

TEST_F(SdmaSqCapacityTest, TransferPairPadsZeroTailAndKeepsCompletionLast)
{
    context.sqTail = 6;
    context.sqHead = 5;
    ock::mf::HybmSdmaReqMail mail{0x1000, 0x2000, 4, 0};
    ASSERT_EQ(issueMailSqes(&param, &context, 0, 7, mail, 0x3000, 0x4000, 0), 0);
    EXPECT_EQ(context.sqTail, 1U);
    uint64_t target = 0;
    std::memcpy(&target, ring.data() + 40, 8);
    EXPECT_EQ(target, 0x4000U);
}

TEST_F(SdmaDirectionTest, UnfinishedPreviousTransferPreventsStageOverwrite)
{
    const uint64_t seq = SMEM_SHM_SDMA_RS_STAGE_SLOTS;
    auto *mail = reinterpret_cast<ock::mf::HybmSdmaReqMail *>(param.resBase + SMEM_SHM_SDMA_RS_REQ_RING +
        (seq % SMEM_SHM_SDMA_RS_REQ_SLOTS) * sizeof(ock::mf::HybmSdmaReqMail));
    *mail = {param.poolBaseGva + 4096, param.poolBaseGva + param.symSize + 8192, (1ULL << 63) | 4, 0};
    const uint64_t stage = param.resBase + SMEM_SHM_SDMA_RS_STAGE;
    storeCell(stage, 99);
    EXPECT_EQ(consumeOneMail(&param, &context, 0, 0, seq), 1366);
    EXPECT_EQ(loadCell(stage), 99U);
    EXPECT_EQ(context.sqTail, 0U);
}

class SdmaDeployVerifyTest : public testing::Test {
protected:
    std::array<unsigned char, 8192> bytes{};
    alignas(64) ock::mf::HybmSdmaDeployResult result{};
    ock::mf::HybmSdmaDeployCheck param{};
    std::string path;
    void SetUp() override
    {
        char name[] = "/tmp/mf_deploy_verify_XXXXXX";
        int fd = mkstemp(name);
        ASSERT_GE(fd, 0);
        path = name;
        for (size_t i = 0; i < bytes.size(); ++i) {
            bytes[i] = i % 251;
        }
        ASSERT_EQ(write(fd, bytes.data(), bytes.size()), static_cast<ssize_t>(bytes.size()));
        close(fd);
        param.expectedAddr = reinterpret_cast<uint64_t>(bytes.data());
        param.expectedSize = bytes.size();
        param.resultAddr = reinterpret_cast<uint64_t>(&result);
        param.nonce = 2468;
        memcpy(param.path, path.c_str(), path.size() + 1);
    }
    void TearDown() override
    {
        unlink(path.c_str());
    }
};

TEST_F(SdmaDeployVerifyTest, ExactBytesAndRequestNonceAreReturned)
{
    ASSERT_EQ(HybmSdmaVerifyDeploy(&param), 0U);
    EXPECT_EQ(result.status, 0U);
    EXPECT_EQ(result.nonce, 2468U);
    EXPECT_EQ(result.compared, bytes.size());
}

TEST_F(SdmaDeployVerifyTest, SameSizeCorruptedFileIsRejected)
{
    FILE *file = fopen(path.c_str(), "r+b");
    ASSERT_NE(file, nullptr);
    fputc(255, file);
    fclose(file);
    ASSERT_EQ(HybmSdmaVerifyDeploy(&param), 0U);
    EXPECT_EQ(result.status, 5U);
}

TEST_F(SdmaDeployVerifyTest, DifferentRequestNonceIsReturnedWithMatchingBytes)
{
    param.nonce = 1357;
    ASSERT_EQ(HybmSdmaVerifyDeploy(&param), 0U);
    EXPECT_EQ(result.status, 0U);
    EXPECT_EQ(result.nonce, 1357U);
    EXPECT_EQ(result.compared, bytes.size());
}

TEST_F(SdmaDeployVerifyTest, ExtraBytesAreRejected)
{
    FILE *file = fopen(path.c_str(), "ab");
    ASSERT_NE(file, nullptr);
    fputc(0, file);
    fclose(file);
    ASSERT_EQ(HybmSdmaVerifyDeploy(&param), 0U);
    EXPECT_EQ(result.status, 4U);
}

TEST_F(SdmaDeployVerifyTest, TruncatedFileIsRejected)
{
    ASSERT_EQ(truncate(path.c_str(), bytes.size() - 1), 0);
    ASSERT_EQ(HybmSdmaVerifyDeploy(&param), 0U);
    EXPECT_EQ(result.status, 4U);
}

TEST_F(SdmaDeployVerifyTest, MissingTargetIsRejected)
{
    unlink(path.c_str());
    ASSERT_EQ(HybmSdmaVerifyDeploy(&param), 0U);
    EXPECT_EQ(result.status, 3U);
}

TEST_F(SdmaDeployVerifyTest, CleanupRequiresTargetToBeAbsent)
{
    param.expectedSize = 0;
    ASSERT_EQ(HybmSdmaVerifyDeploy(&param), 0U);
    EXPECT_EQ(result.status, 6U);
    unlink(path.c_str());
    ASSERT_EQ(HybmSdmaVerifyDeploy(&param), 0U);
    EXPECT_EQ(result.status, 0U);
    EXPECT_EQ(result.nonce, param.nonce);
}

#else
#include <gtest/gtest.h>
TEST(SdmaSqCapacityTest, RequiresAarch64CacheInstructions)
{
    GTEST_SKIP() << "AICPU cache instructions require AArch64";
}

TEST_F(SdmaDirectionTest, PutSignalOrdersDataNotificationAndCompletion)
{
    SetMail(false, SMEM_SHM_SDMA_SIGNAL_FLAG);
    auto *signal = reinterpret_cast<uint64_t *>(param.resBase + SMEM_SHM_SDMA_RS_SIGNAL);
    signal[0] = param.poolBaseGva + param.symSize + 16384;
    signal[1] = 1234567;
    ASSERT_EQ(consumeOneMail(&param, &context, 0, 0, 0), 0);
    EXPECT_EQ(context.sqTail, 3U);
    EXPECT_EQ(DescriptorAddress(0, 40), param.poolBaseGva + param.symSize + 8192);
    EXPECT_EQ(DescriptorAddress(1, 40), signal[0]);
    EXPECT_EQ(DescriptorAddress(2, 40), param.resBase + SMEM_SHM_SDMA_RS_QUIET);
    const uint64_t source = DescriptorAddress(1, 32);
    EXPECT_EQ(*reinterpret_cast<uint64_t *>(source), 1234567U);
    signal[1] = 9;
    EXPECT_EQ(*reinterpret_cast<uint64_t *>(source), 1234567U);
}

TEST_F(SdmaDirectionTest, StandaloneSignalHasNoDataDescriptor)
{
    SetMail(false, SMEM_SHM_SDMA_SIGNAL_FLAG);
    auto *mail = reinterpret_cast<ock::mf::HybmSdmaReqMail *>(param.resBase);
    mail->src = 0;
    mail->len = 1ULL << 63;
    auto *signal = reinterpret_cast<uint64_t *>(param.resBase + SMEM_SHM_SDMA_RS_SIGNAL);
    signal[0] = mail->dst;
    signal[1] = 88;
    ASSERT_EQ(consumeOneMail(&param, &context, 0, 0, 0), 0);
    EXPECT_EQ(context.sqTail, 2U);
    EXPECT_EQ(DescriptorAddress(0, 40), mail->dst);
    EXPECT_EQ(DescriptorAddress(1, 40), param.resBase + SMEM_SHM_SDMA_RS_QUIET);
}

TEST_F(SdmaDirectionTest, RejectsUnalignedOrWrongPeerSignal)
{
    SetMail(false, SMEM_SHM_SDMA_SIGNAL_FLAG);
    auto *signal = reinterpret_cast<uint64_t *>(param.resBase + SMEM_SHM_SDMA_RS_SIGNAL);
    for (uint64_t addr :
         {param.poolBaseGva + param.symSize + 3, param.poolBaseGva + 64, param.poolBaseGva + 2 * param.symSize - 4}) {
        signal[0] = addr;
        EXPECT_EQ(consumeOneMail(&param, &context, 0, 0, 0), 1330);
        EXPECT_EQ(context.sqTail, 0U);
    }
}

TEST_F(SdmaDirectionTest, GetSignalIsRejected)
{
    SetMail(true, SMEM_SHM_SDMA_SIGNAL_FLAG);
    auto *signal = reinterpret_cast<uint64_t *>(param.resBase + SMEM_SHM_SDMA_RS_SIGNAL);
    signal[0] = param.poolBaseGva + param.symSize + 64;
    EXPECT_EQ(consumeOneMail(&param, &context, 0, 0, 0), 1330);
}

TEST_F(SdmaSqCapacityTest, SignalGroupPreservesNonzeroDoorbellOnWrap)
{
    context.sqTail = 5;
    context.sqHead = 4;
    ock::mf::HybmSdmaReqMail mail{0x1000, 0x2000, 4, 0};
    ASSERT_EQ(issueMailSqes(&param, &context, 0, 7, mail, 0x3000, 0x4000, 0, 0x5000), 0);
    EXPECT_EQ(context.sqTail, 1U);
    ASSERT_EQ(flushBatchDoorbell(&param, &context), 0);
    EXPECT_EQ(publishedTail, 1U);
}


#endif
