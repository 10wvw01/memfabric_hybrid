/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

/*
 * 310P3 SHM Kernel Test — Device MTE DataCopyPad via GVA
 *
 * Each rank's kernel writes local data to next rank's GVA slot (shift put),
 * then reads from previous rank's GVA slot into local output (shift get).
 * Host verifies: output[0] == (rank-1+size)%size (prev rank's id).
 *
 * Usage: mpirun -np <rankSize> ./310p3_shm_kernel_test <store_url> <rankSize>
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <vector>
#include <unistd.h>
#include <mpi.h>

#include "acl/acl.h"
#include "smem.h"
#include "smem_shm.h"
#include "hybm.h"
#include "shm_kernel.h"

namespace {

constexpr uint64_t SHM_SIZE = 256ULL * 1024ULL * 1024ULL; /* 256MB per rank */
constexpr int64_t BLOCK_LEN = 32 / sizeof(int64_t);       /* 4 int64_t = 32B (SMEM_SHM_ALIGN_SIZE) */

void MpBarrier()
{
    MPI_Barrier(MPI_COMM_WORLD);
}

int32_t ShmInit(int rank, int worldSize, int devId, const char *storeUrl)
{
    smem_set_log_level(1);
    smem_shm_config_t config;
    smem_shm_config_init(&config);
    config.startConfigStoreServer = (rank == 0);
    config.shmInitTimeout = 300;
    config.shmCreateTimeout = 300;
    config.controlOperationTimeout = 300;

    int32_t ret = smem_shm_init(storeUrl, static_cast<uint32_t>(worldSize),
                                static_cast<uint32_t>(rank), static_cast<uint16_t>(devId), &config);
    if (ret != 0) {
        fprintf(stderr, "[rank %d] smem_shm_init failed:%d\n", rank, ret);
        return ret;
    }
    printf("[rank %d] smem_shm_init OK\n", rank);
    return 0;
}

int32_t TestKernelShift(int rank, int worldSize)
{
    printf("\n========== SHM Kernel Shift Test (rankSize=%d) ==========\n", worldSize);

    void *gva = nullptr;
    smem_shm_t handle = smem_shm_create(0, static_cast<uint32_t>(worldSize),
                                        static_cast<uint32_t>(rank), SHM_SIZE,
                                        SMEMS_DATA_OP_MTE, 0, &gva);
    if (handle == nullptr || gva == nullptr) {
        fprintf(stderr, "[rank %d] smem_shm_create failed\n", rank);
        return 1;
    }

    uint64_t symSize = smem_shm_get_symmetric_size(handle);
    printf("[rank %d] smem_shm_create OK gva=%p symSize=%lluMB\n",
           rank, gva, static_cast<unsigned long long>(symSize / (1024ULL * 1024ULL)));
    fflush(stdout);

    smem_shm_control_barrier(handle);

    /* Prepare local device buffers */
    int64_t inputHost[BLOCK_LEN] = {static_cast<int64_t>(rank), static_cast<int64_t>(worldSize), -1, -1};
    int64_t outputHost[BLOCK_LEN] = {0, 0, 0, 0};
    int64_t *inputDev = nullptr;
    int64_t *outputDev = nullptr;
    aclrtMalloc((void **)&inputDev, sizeof(inputHost), ACL_MEM_MALLOC_HUGE_FIRST);
    aclrtMalloc((void **)&outputDev, sizeof(outputHost), ACL_MEM_MALLOC_HUGE_FIRST);
    aclrtMemcpy(inputDev, sizeof(inputHost), inputHost, sizeof(inputHost), ACL_MEMCPY_HOST_TO_DEVICE);
    aclrtMemcpy(outputDev, sizeof(outputHost), outputHost, sizeof(outputHost), ACL_MEMCPY_HOST_TO_DEVICE);

    printf("[rank %d] launching kernel with GVA=%p\n", rank, gva);
    fflush(stdout);

    aclrtStream stream;
    aclrtCreateStream(&stream);
    shm_shift_kernel_do(stream, static_cast<uint8_t *>(gva), inputDev, outputDev);
    aclrtSynchronizeStream(stream);
    aclrtDestroyStream(stream);

    printf("[rank %d] kernel done\n", rank);
    fflush(stdout);

    smem_shm_control_barrier(handle);

    aclrtMemcpy(outputHost, sizeof(outputHost), outputDev, sizeof(outputHost), ACL_MEMCPY_DEVICE_TO_HOST);

    int32_t srcRank = (rank + worldSize - 1) % worldSize;
    bool ok = (outputHost[0] == srcRank);
    printf("[rank %d] output[0]=%ld expected=%d %s\n",
           rank, static_cast<long>(outputHost[0]), srcRank, ok ? "OK" : "FAIL");
    fflush(stdout);

    aclrtFree(inputDev);
    aclrtFree(outputDev);
    smem_shm_destroy(handle, 0);

    printf("[rank %d] SHM kernel test %s\n", rank, ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

} // namespace

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "usage: mpirun -np <rankSize> %s <store_url> [rankSize]\n", argv[0]);
        return 1;
    }

    const char *storeUrl = argv[1];
    int worldSize = (argc >= 3) ? atoi(argv[2]) : 2;

    MPI_Init(&argc, &argv);
    int rank;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &worldSize);

    if (worldSize < 2) {
        fprintf(stderr, "[rank %d] need at least 2 ranks\n", rank);
        MPI_Finalize();
        return 1;
    }

    int devId = rank;
    printf("[rank %d] start devId:%d rankSize:%d\n", rank, devId, worldSize);
    fflush(stdout);

    aclInit(nullptr);
    aclrtSetDevice(devId);

    int32_t ret = ShmInit(rank, worldSize, devId, storeUrl);
    if (ret != 0) {
        MPI_Finalize();
        return ret;
    }

    int32_t testRet = TestKernelShift(rank, worldSize);

    MpBarrier();
    smem_shm_uninit(0);
    aclrtResetDevice(devId);
    aclFinalize();

    MpBarrier();
    MPI_Finalize();

    printf("[rank %d] %s\n", rank, testRet != 0 ? "TEST FAILED" : "ALL TESTS PASSED");
    return testRet != 0 ? 1 : 0;
}
