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
 * 310P3 SHM Functional Verification Demo
 *
 * SHM 模式：对称 HBM 池，device kernel（MTE）通过 GVA 直接访问远端 rank 内存。
 *
 * 310P3 限制：GVA ≠ device VA（AclrtReserveMemAddress 不支持指定地址），
 * device kernel 的 DataCopy 无法通过 GVA 跨 rank 访问。
 * 因此本测试只验证 pool 生命周期和控制面操作，不验证数据传输。
 *
 * Tests:
 *   1. SHM create: smem_shm_init + smem_shm_create (VMM symmetric GVA pool)
 *   2. Control ops: smem_shm_control_barrier, smem_shm_topology_can_reach
 *   3. Extra context: smem_shm_set_extra_context
 *   4. Lifecycle: 10x create -> barrier -> destroy
 *
 * Usage:
 *   mpirun -np <rankSize> ./310p3_shm_test <store_url> <rankSize>
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

namespace {

constexpr uint64_t SHM_SIZE = 256ULL * 1024ULL * 1024ULL; /* 256MB per rank */
constexpr int32_t LOOPS = 10;

void MpBarrier()
{
    MPI_Barrier(MPI_COMM_WORLD);
}

int32_t ShmInit(int rank, int worldSize, int devId, const char *storeUrl)
{
    smem_set_log_level(1); /* INFO */
    smem_shm_config_t config;
    smem_shm_config_init(&config);
    config.startConfigStoreServer = (rank == 0);
    config.shmInitTimeout = 300;
    config.shmCreateTimeout = 300;
    config.controlOperationTimeout = 300;

    int32_t ret = smem_shm_init(storeUrl, static_cast<uint32_t>(worldSize),
                                static_cast<uint32_t>(rank), static_cast<uint16_t>(devId), &config);
    if (ret != 0) {
        fprintf(stderr, "[rank %d] smem_shm_init failed:%d storeUrl:%s devId:%d\n", rank, ret, storeUrl, devId);
        return ret;
    }
    int32_t socType = hybm_get_soc_type();
    printf("[rank %d] smem_shm_init OK socType=%d devId=%d worldSize=%d\n", rank, socType, devId, worldSize);
    return 0;
}

/* Test SHM create + control ops (barrier, topology, extra_context) + destroy.
 * 310P3 上 SHM create 走 ACL VMM 路径：AclrtMallocPhysical + AclrtMapMem +
 * AclrtMemExportToShareableHandle + ImportFromShareableHandle(ctx 技巧)。
 * topology_can_reach 需要 bmDataOpType 同时设置 MTE | SDMA 标志才能返回可达。 */
int32_t TestShmCreate(int rank, int worldSize)
{
    printf("\n========== SHM Create + Control Ops Test (rankSize=%d) ==========\n", worldSize);

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

    int32_t ret = smem_shm_control_barrier(handle);
    printf("[rank %d] barrier after create ret=%d\n", rank, ret);
    fflush(stdout);

    int nfail = 0;
    for (int peer = 0; peer < worldSize; peer++) {
        if (peer == rank) {
            continue;
        }
        uint32_t reachInfo = 0;
        ret = smem_shm_topology_can_reach(handle, static_cast<uint32_t>(peer), &reachInfo);
        if (ret != 0) {
            fprintf(stderr, "[rank %d] topology_can_reach(peer %d) failed:%d\n", rank, peer, ret);
            nfail++;
        } else {
            printf("[rank %d] topology_can_reach(peer %d) reachInfo=0x%x (MTE=%s)\n",
                   rank, peer, reachInfo, (reachInfo & SMEMS_DATA_OP_MTE) ? "yes" : "no");
        }
    }
    fflush(stdout);

    smem_shm_control_barrier(handle);

    uint8_t ctxData[32] = {};
    for (int i = 0; i < 32; i++) {
        ctxData[i] = static_cast<uint8_t>((rank << 4) | i);
    }
    ret = smem_shm_set_extra_context(handle, ctxData, sizeof(ctxData));
    if (ret != 0) {
        fprintf(stderr, "[rank %d] set_extra_context failed:%d\n", rank, ret);
        nfail++;
    } else {
        printf("[rank %d] set_extra_context OK (32 bytes)\n", rank);
    }
    fflush(stdout);

    smem_shm_control_barrier(handle);

    printf("[rank %d] before smem_shm_destroy\n", rank);
    fflush(stdout);
    smem_shm_destroy(handle, 0);
    printf("[rank %d] after smem_shm_destroy\n", rank);
    fflush(stdout);

    printf("[rank %d] SHM create test %s (rankSize:%d)\n", rank, nfail > 0 ? "FAIL" : "PASS", worldSize);
    return nfail;
}

/* 10x lifecycle: create -> barrier -> destroy */
int32_t TestShmLifecycle(int rank, int worldSize)
{
    printf("\n========== 10x Lifecycle Test (rankSize=%d) ==========\n", worldSize);

    int nfail = 0;
    for (int loop = 0; loop < LOOPS; loop++) {
        void *gva = nullptr;
        smem_shm_t handle = smem_shm_create(static_cast<uint32_t>(loop % 63) + 2,
                                            static_cast<uint32_t>(worldSize),
                                            static_cast<uint32_t>(rank), SHM_SIZE,
                                            SMEMS_DATA_OP_MTE, 0, &gva);
        if (handle == nullptr || gva == nullptr) {
            fprintf(stderr, "[rank %d] loop %d create failed\n", rank, loop);
            nfail++;
            MpBarrier();
            continue;
        }

        printf("[rank %d] loop %d create OK gva=%p\n", rank, loop, gva);
        fflush(stdout);

        smem_shm_control_barrier(handle);
        printf("[rank %d] loop %d barrier OK\n", rank, loop);
        fflush(stdout);

        smem_shm_destroy(handle, 0);
        printf("[rank %d] loop %d destroy OK\n", rank, loop);
        fflush(stdout);

        if ((loop + 1) % 5 == 0) {
            printf("[rank %d] lifecycle loop %d/%d OK\n", rank, loop + 1, LOOPS);
        }
        MpBarrier();
    }

    printf("[rank %d] Lifecycle test %s (%d/%d loops failed)\n", rank, nfail > 0 ? "FAIL" : "PASS", nfail, LOOPS);
    return nfail;
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
    const char *devStr = getenv("DEVICE_ID");
    if (devStr != nullptr) {
        devId = atoi(devStr);
    }

    printf("[rank %d] start devId:%d rankSize:%d\n", rank, devId, worldSize);
    fflush(stdout);

    aclInit(nullptr);
    aclrtSetDevice(devId);

    int32_t ret = ShmInit(rank, worldSize, devId, storeUrl);
    if (ret != 0) {
        MPI_Finalize();
        return ret;
    }

    int32_t testRet = 0;
    testRet += TestShmCreate(rank, worldSize);
    MpBarrier();
    testRet += TestShmLifecycle(rank, worldSize);

    MpBarrier();
    smem_shm_uninit(0);
    aclrtResetDevice(devId);
    aclFinalize();

    MpBarrier();
    MPI_Finalize();

    printf("[rank %d] %s\n", rank, testRet != 0 ? "TEST FAILED" : "ALL TESTS PASSED");
    return testRet != 0 ? 1 : 0;
}
