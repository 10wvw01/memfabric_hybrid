#include <gtest/gtest.h>
#include "smem_shm_signal_define.h"

TEST(SmemSignalCompare, AllComparisonConditions)
{
    EXPECT_TRUE(SmemShmSignalCompare(7, SMEM_SHM_CMP_EQ, 7));
    EXPECT_FALSE(SmemShmSignalCompare(7, SMEM_SHM_CMP_EQ, 8));
    EXPECT_TRUE(SmemShmSignalCompare(7, SMEM_SHM_CMP_NE, 8));
    EXPECT_TRUE(SmemShmSignalCompare(8, SMEM_SHM_CMP_GT, 7));
    EXPECT_FALSE(SmemShmSignalCompare(7, SMEM_SHM_CMP_GT, 7));
    EXPECT_TRUE(SmemShmSignalCompare(7, SMEM_SHM_CMP_GE, 7));
    EXPECT_TRUE(SmemShmSignalCompare(6, SMEM_SHM_CMP_LT, 7));
    EXPECT_FALSE(SmemShmSignalCompare(7, SMEM_SHM_CMP_LT, 7));
    EXPECT_TRUE(SmemShmSignalCompare(7, SMEM_SHM_CMP_LE, 7));
    EXPECT_FALSE(SmemShmSignalCompare(7, static_cast<smem_shm_cmp_t>(99), 7));
}

TEST(SmemSignalCompare, UnsignedValuesKeepFullWidth)
{
    EXPECT_TRUE(SmemShmSignalCompare(UINT64_MAX, SMEM_SHM_CMP_GT, 0));
    EXPECT_TRUE(SmemShmSignalCompare(UINT64_MAX, SMEM_SHM_CMP_EQ, UINT64_MAX));
    EXPECT_FALSE(SmemShmSignalCompare(0, SMEM_SHM_CMP_GT, UINT64_MAX));
}

TEST(SmemSignalCompare, NotificationStorageFitsReservedRegion)
{
    EXPECT_GE(SMEM_SHM_SDMA_RS_SIGNAL,
              SMEM_SHM_SDMA_RS_STAGE + SMEM_SHM_SDMA_QUEUE_COUNT * SMEM_SHM_SDMA_RS_STAGE_STRIDE);
    EXPECT_LE(SMEM_SHM_SDMA_RS_SIGNAL + SMEM_SHM_SDMA_QUEUE_COUNT * SMEM_SHM_SDMA_RS_SIGNAL_STRIDE,
              SMEM_SHM_SDMA_RESERVED);
}
