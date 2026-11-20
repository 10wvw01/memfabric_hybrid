#ifndef MEMFABRIC_SMEM_SHM_SIGNAL_DEFINE_H
#define MEMFABRIC_SMEM_SHM_SIGNAL_DEFINE_H

#include <cstdint>
#include "smem_shm_sdma_layout.h"

typedef enum smem_shm_cmp {
    SMEM_SHM_CMP_EQ = 0,
    SMEM_SHM_CMP_NE,
    SMEM_SHM_CMP_GT,
    SMEM_SHM_CMP_GE,
    SMEM_SHM_CMP_LT,
    SMEM_SHM_CMP_LE,
} smem_shm_cmp_t;

SMEM_SHM_SDMA_LAYOUT_FN bool SmemShmSignalCompare(uint64_t actual, smem_shm_cmp_t cmp, uint64_t expected)
{
    switch (cmp) {
        case SMEM_SHM_CMP_EQ:
            return actual == expected;
        case SMEM_SHM_CMP_NE:
            return actual != expected;
        case SMEM_SHM_CMP_GT:
            return actual > expected;
        case SMEM_SHM_CMP_GE:
            return actual >= expected;
        case SMEM_SHM_CMP_LT:
            return actual < expected;
        case SMEM_SHM_CMP_LE:
            return actual <= expected;
        default:
            return false;
    }
}

#endif
