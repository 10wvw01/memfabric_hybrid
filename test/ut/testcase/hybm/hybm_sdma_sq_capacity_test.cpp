#if defined(__aarch64__)
#include <gtest/gtest.h>
#include <array>
#include <cstring>
#include <vector>
#include <unistd.h>

extern "C" int SdmaCapacitySleep(useconds_t);
#define halSqCqQuery  SdmaCapacityQuery
#define halSqCqConfig SdmaCapacityConfig
#define usleep        SdmaCapacitySleep
#include "../../../../src/hybm/ops/hybm_kernel/hybm_sdma_orch_kernel.cc"
#undef usleep
#undef halSqCqConfig
#undef halSqCqQuery

namespace {
uint32_t queryCount, configCount, sleepCount, nextHead, publishedTail;
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

TEST_F(SdmaSqCapacityTest, WholeRequestReservesSpaceBeforeWritingDescriptors)
{
    context.sqTail = 4;
    nextHead = 4;
    ock::mf::HybmSdmaReqMail mail{};
    EXPECT_EQ(issueMailSqes(&param, &context, 0, 0, mail, 0, 0, 0, 0, 0), 0);
    EXPECT_EQ(configCount, 1U);
    EXPECT_EQ(publishedTail, 4U);
    EXPECT_EQ(queryCount, 1U);
    EXPECT_EQ(context.sqTail, 1U);
}

TEST_F(SdmaSqCapacityTest, PendingReceiveNotificationSurvivesQueueWrap)
{
    replayHardware = true;
    context.sqTail = 4;
    for (uint16_t task = 0; task < 4; ++task) {
        std::memcpy(ring.data() + task * kSqeSize + 6, &task, 2);
    }
    ock::mf::HybmSdmaReqMail mail{};
    ASSERT_EQ(issueMailSqes(&param, &context, 0, 10, mail, 0, 0, 0, 0, 0), 0);
    ASSERT_EQ(flushBatchDoorbell(&param, &context), 0);
    EXPECT_EQ(executedTasks, (std::vector<uint16_t>{0, 1, 2, 3, 14, 10, 11, 12, 13}));
}

#else
#include <gtest/gtest.h>
TEST(SdmaSqCapacityTest, RequiresAarch64CacheInstructions)
{
    GTEST_SKIP() << "AICPU cache instructions require AArch64";
}
#endif
