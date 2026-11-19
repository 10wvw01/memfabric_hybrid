#include <gtest/gtest.h>
#include "smem_shm_signal_define.h"

TEST(SmemSignalCompare, AllComparisonConditions)
{
    struct Case {
        smem_shm_cmp_t cmp;
        bool less;
        bool equal;
        bool greater;
    };
    const Case cases[] = {
        {SMEM_SHM_CMP_EQ, false, true, false}, {SMEM_SHM_CMP_NE, true, false, true},
        {SMEM_SHM_CMP_GT, false, false, true}, {SMEM_SHM_CMP_GE, false, true, true},
        {SMEM_SHM_CMP_LT, true, false, false}, {SMEM_SHM_CMP_LE, true, true, false},
    };
    for (const auto &item : cases) {
        SCOPED_TRACE(static_cast<int>(item.cmp));
        EXPECT_EQ(SmemShmSignalCompare(6, item.cmp, 7), item.less);
        EXPECT_EQ(SmemShmSignalCompare(7, item.cmp, 7), item.equal);
        EXPECT_EQ(SmemShmSignalCompare(8, item.cmp, 7), item.greater);
    }
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
