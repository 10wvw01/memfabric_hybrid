#include <gtest/gtest.h>
#include <array>
#include "../../../../src/hybm/ops/hybm_kernel/hybm_sdma_contract.h"

TEST(SdmaScanTest, MapsOnlyExistingPeersAndAllEightBlocks)
{
    for (uint32_t ranks = 1; ranks <= SMEM_SHM_SDMA_MAX_RANKS; ++ranks) {
        std::array<bool, SMEM_SHM_SDMA_QUEUE_COUNT> seen{};
        for (uint32_t i = 0; i < (ranks - 1) * SMEM_SHM_SDMA_MAX_BLOCKS; ++i) {
            const auto q = ock::mf::HybmSdmaScanQueue(i);
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
    uint32_t cursor = 0;
    std::array<uint32_t, SMEM_SHM_SDMA_QUEUE_COUNT> served{};
    for (uint32_t batch = 0; batch < 12; ++batch) {
        EXPECT_EQ(ock::mf::HybmSdmaScanQueue(cursor), (batch % 3) * 16);
        for (uint32_t item = 0; item < 8; ++item) {
            ++served[ock::mf::HybmSdmaScanQueue(cursor)];
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
        for (uint32_t start = 0; start < count; ++start) {
            uint32_t cursor = start;
            std::array<bool, SMEM_SHM_SDMA_QUEUE_COUNT> seen{};
            for (uint32_t visit = 0; visit < count; ++visit) {
                const uint32_t queue = ock::mf::HybmSdmaScanQueue(cursor);
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
