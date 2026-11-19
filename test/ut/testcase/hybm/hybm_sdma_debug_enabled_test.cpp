#if defined(__aarch64__)
#include <gtest/gtest.h>
#include <array>
#include <vector>
#include <unistd.h>

#define MF_SDMA_DBG_MAIL
#define halSqCqQuery                      SdmaDebugQuery
#define halSqCqConfig                     SdmaDebugConfig
#define HybmSdmaOrchEpoch                 SdmaDebugEpoch
#define StaticTileFwkBackendKernelServer  SdmaDebugStaticServer
#define DynTileFwkBackendKernelServerInit SdmaDebugServerInit
#define DynTileFwkBackendKernelServer     SdmaDebugServer
#include "../../../../src/hybm/ops/hybm_kernel/hybm_sdma_orch_kernel.cc"
#undef DynTileFwkBackendKernelServer
#undef DynTileFwkBackendKernelServerInit
#undef StaticTileFwkBackendKernelServer
#undef HybmSdmaOrchEpoch
#undef halSqCqConfig
#undef halSqCqQuery
#undef MF_SDMA_DBG_MAIL

namespace {
int debugConfigResult;
}

extern "C" int32_t SdmaDebugQuery(uint32_t, void *)
{
    return -1;
}

extern "C" int32_t SdmaDebugConfig(uint32_t, void *)
{
    return debugConfigResult;
}

class SdmaDebugEnabledTest : public testing::Test {
protected:
    alignas(64) std::array<uint8_t, 8 * kSqeSize> ring{};
    alignas(64) std::array<uint8_t, 8192> workspace{};
    std::vector<uint64_t> reserved = std::vector<uint64_t>(SMEM_SHM_SDMA_RESERVED / 8, 0);
    ock::mf::HybmSdmaOrchParam param{};
    SqContext context{};
    void SetUp() override
    {
        debugConfigResult = 0;
        param.resultAddr = reinterpret_cast<uint64_t>(workspace.data());
        param.resBase = reinterpret_cast<uint64_t>(reserved.data());
        param.poolBaseGva = 0x100000000ULL;
        param.symSize = param.physSize = 64UL * 1024 * 1024;
        context.sqBase = reinterpret_cast<uint64_t>(ring.data());
        context.sqDepth = 8;
        auto *mail = reinterpret_cast<ock::mf::HybmSdmaReqMail *>(param.resBase);
        *mail = {param.poolBaseGva + 4096, param.poolBaseGva + param.symSize + 8192, (1ULL << 63) | 4, 0};
    }
    uint64_t Read(uint64_t offset)
    {
        return *reinterpret_cast<uint64_t *>(workspace.data() + offset);
    }
};

TEST_F(SdmaDebugEnabledTest, RecordsRequestAndDoorbell)
{
    using namespace ock::mf;
    ASSERT_EQ(consumeOneMail(&param, &context, 0, 0, 0), 0);
    EXPECT_EQ(Read(HYBM_SDMA_ORCH_WS_DBG_RING_CNT_OFF), 1U);
    auto *record = reinterpret_cast<HybmSdmaOrchDbgMail *>(workspace.data() + HYBM_SDMA_ORCH_WS_DBG_RING_OFF);
    EXPECT_EQ(record->seq, 0U);
    EXPECT_EQ(record->reqF0, param.poolBaseGva + 4096);
    EXPECT_EQ(record->reqF1, param.poolBaseGva + param.symSize + 8192);
    EXPECT_EQ(record->reqF2, 4U);
    EXPECT_EQ(record->sqTailBefore, 0U);
    EXPECT_EQ(record->sqTailAfter, 2U);
    ASSERT_EQ(flushBatchDoorbell(&param, &context), 0);
    EXPECT_EQ(Read(HYBM_SDMA_ORCH_WS_DBG_DOORBELL_OFF), 2U);
    EXPECT_EQ(Read(HYBM_SDMA_ORCH_WS_DBG_DBRET_OFF), 0U);
}

TEST_F(SdmaDebugEnabledTest, PreservesDoorbellFailureAndDiagnosticCode)
{
    using namespace ock::mf;
    context.sqTail = 3;
    debugConfigResult = -19;
    EXPECT_EQ(flushBatchDoorbell(&param, &context), 1390);
    EXPECT_EQ(Read(HYBM_SDMA_ORCH_WS_DBG_DOORBELL_OFF), 3U);
    EXPECT_EQ(Read(HYBM_SDMA_ORCH_WS_DBG_DBRET_OFF), static_cast<uint32_t>(-19));
}

#endif
