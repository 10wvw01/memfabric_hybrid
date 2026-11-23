#include <gtest/gtest.h>
#include <array>
#include "../../../../src/hybm/ops/hybm_kernel/hybm_sdma_contract.h"

TEST(SdmaScanTest, MapsOnlyExistingPeersAndAllEightBlocks)
{
    for (uint32_t ranks = 1; ranks <= SMEM_SHM_SDMA_MAX_RANKS; ++ranks) {
        std::array<bool, SMEM_SHM_SDMA_QUEUE_COUNT> seen{};
        std::array<uint32_t, SMEM_SHM_SDMA_QUEUE_COUNT> queues{};
        const auto count = ock::mf::HybmSdmaBuildScanQueues(ranks, 0xFF, queues.data());
        EXPECT_EQ(count, (ranks - 1) * 8);
        for (uint32_t i = 0; i < count; ++i) {
            const auto q = queues[i];
            ASSERT_LT(q, seen.size());
            EXPECT_FALSE(seen[q]);
            seen[q] = true;
            EXPECT_LT(SmemShmSdmaQueuePeerRing(q), ranks - 1);
            EXPECT_LT(SmemShmSdmaQueueBlock(q), 8U);
        }
    }
}

TEST(SdmaScanTest, FullBatchesContinueAcrossPeerBoundaries)
{
    constexpr uint32_t count = 24;
    std::array<uint32_t, SMEM_SHM_SDMA_QUEUE_COUNT> queues{};
    ASSERT_EQ(ock::mf::HybmSdmaBuildScanQueues(4, 0xFF, queues.data()), count);
    uint32_t cursor = 0;
    std::array<uint32_t, SMEM_SHM_SDMA_QUEUE_COUNT> served{};
    for (uint32_t batch = 0; batch < 12; ++batch) {
        EXPECT_EQ(queues[cursor], (batch % 3) * 16);
        for (uint32_t item = 0; item < 8; ++item) {
            ++served[queues[cursor]];
            cursor = ock::mf::HybmSdmaNextScan(cursor, count);
        }
    }
    for (uint32_t q = 0; q < served.size(); ++q) {
        EXPECT_EQ(served[q], q % 16 < 8 ? 4U : 0U);
    }
}

TEST(SdmaScanTest, EveryStartingQueueCompletesOneCycleAcrossActivePeers)
{
    for (const uint32_t count : {8U, 16U, 24U}) {
        std::array<uint32_t, SMEM_SHM_SDMA_QUEUE_COUNT> queues{};
        ASSERT_EQ(ock::mf::HybmSdmaBuildScanQueues(count / 8 + 1, 0xFF, queues.data()), count);
        for (uint32_t start = 0; start < count; ++start) {
            uint32_t cursor = start;
            std::array<bool, SMEM_SHM_SDMA_QUEUE_COUNT> seen{};
            for (uint32_t visit = 0; visit < count; ++visit) {
                const uint32_t queue = queues[cursor];
                ASSERT_LT(queue, seen.size());
                EXPECT_FALSE(seen[queue]);
                seen[queue] = true;
                cursor = ock::mf::HybmSdmaNextScan(cursor, count);
            }
            EXPECT_EQ(cursor, start);
            for (uint32_t peer = 0; peer < count / 8; ++peer) {
                for (uint32_t block = 0; block < 8; ++block) {
                    EXPECT_TRUE(seen[peer * 16 + block]);
                }
            }
        }
    }
}

TEST(SdmaScanTest, ConfiguredMasksSelectExactQueuesAcrossAllPeers)
{
    for (uint32_t ranks = 1; ranks <= 4; ++ranks) {
        for (const uint32_t mask : {0xFFU, 0x7F00U, 0x7FFFU, 0x4001U, 0x4000U, 1U}) {
            std::array<uint32_t, SMEM_SHM_SDMA_QUEUE_COUNT> queues{};
            const auto count = ock::mf::HybmSdmaBuildScanQueues(ranks, mask, queues.data());
            std::array<bool, SMEM_SHM_SDMA_QUEUE_COUNT> seen{};
            for (uint32_t i = 0; i < count; ++i) {
                ASSERT_LT(queues[i], seen.size());
                EXPECT_FALSE(seen[queues[i]]);
                seen[queues[i]] = true;
            }
            for (uint32_t peer = 0; peer < 3; ++peer) {
                for (uint32_t block = 0; block < 16; ++block) {
                    EXPECT_EQ(seen[peer * 16 + block], peer < ranks - 1 && ((mask >> block) & 1U));
                }
            }
            if (count != 0) {
                uint32_t next = 0;
                for (uint32_t i = 0; i < count; ++i) {
                    next = ock::mf::HybmSdmaNextScan(next, count);
                }
                EXPECT_EQ(next, 0U);
            }
        }
    }
}

TEST(SdmaScanTest, InvalidMaskAndWorldDoNotWriteScanList)
{
    uint32_t queue = 123;
    for (const uint32_t mask : {0U, 0x8000U, 0x10000U, UINT32_MAX}) {
        EXPECT_FALSE(SmemShmSdmaProducerMaskValid(mask));
        EXPECT_EQ(ock::mf::HybmSdmaBuildScanQueues(4, mask, &queue), 0U);
        EXPECT_EQ(queue, 123U);
    }
    EXPECT_EQ(ock::mf::HybmSdmaBuildScanQueues(0, 0xFF, &queue), 0U);
    EXPECT_EQ(ock::mf::HybmSdmaBuildScanQueues(5, 0xFF, &queue), 0U);
    EXPECT_EQ(queue, 123U);
    EXPECT_FALSE(SmemShmSdmaProducerAllowed(0x7FFF, 15));
    EXPECT_FALSE(SmemShmSdmaProducerAllowed(0x7FFF, 16));
    EXPECT_FALSE(SmemShmSdmaProducerAllowed(0x7FFF, UINT32_MAX));
}

TEST(SdmaScanTest, ProducerFieldsCoverTwoBytesWithLastVectorBitReserved)
{
    EXPECT_EQ(SMEM_SHM_SDMA_AICORE_PRODUCER_MASK, 0x00FFU);
    EXPECT_EQ(SMEM_SHM_SDMA_VECTOR_PRODUCER_MASK, 0xFF00U);
    EXPECT_EQ(SMEM_SHM_SDMA_RESERVED_PRODUCER_MASK, 0x8000U);
    EXPECT_EQ(SMEM_SHM_SDMA_DEFAULT_PRODUCER_MASK, 0x00FFU);
    EXPECT_EQ(SMEM_SHM_SDMA_VALID_PRODUCER_MASK, 0x7FFFU);
    for (uint32_t block = 0; block < 16; ++block) {
        EXPECT_EQ(SmemShmSdmaProducerAllowed(SMEM_SHM_SDMA_AICORE_PRODUCER_MASK, block), block < 8);
        EXPECT_EQ(SmemShmSdmaProducerAllowed(SMEM_SHM_SDMA_VECTOR_PRODUCER_MASK, block), block >= 8 && block < 15);
        EXPECT_EQ(SmemShmSdmaProducerAllowed(0xFFFF, block), block < 15);
    }
    EXPECT_FALSE(SmemShmSdmaProducerAllowed(0xFFFF, 16));
    EXPECT_FALSE(SmemShmSdmaProducerAllowed(0xFFFF, UINT32_MAX));
}

TEST(SdmaScanTest, ReservedVectorBitRejectsConfigurationAndPreservesQueueList)
{
    for (const uint32_t mask : {0x8000U, 0xFF00U, 0xFFFFU}) {
        EXPECT_FALSE(SmemShmSdmaProducerMaskValid(mask));
        EXPECT_FALSE(SmemShmSdmaProducerAllowed(mask, 15));
        std::array<uint32_t, SMEM_SHM_SDMA_QUEUE_COUNT> queues;
        queues.fill(UINT32_MAX);
        for (uint32_t ranks = 1; ranks <= 4; ++ranks) {
            EXPECT_EQ(ock::mf::HybmSdmaBuildScanQueues(ranks, mask, queues.data()), 0U);
        }
        for (auto q : queues) {
            EXPECT_EQ(q, UINT32_MAX);
        }
    }
}
