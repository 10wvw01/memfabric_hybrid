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
 * 310P3 BM Functional Verification Demo
 *
 * Based on verified test programs from commit 986eec67 (310p-p0-verify).
 *
 * Tests:
 *   1. HBM pool: L2G / G2L / G2G / G2H (all peer ranks)
 *   2. DRAM HOST_SHM pool: H2GH / GH2H (all peer ranks)
 *   3. 100x lifecycle (create -> join -> copy -> destroy)
 *
 * Usage:
 *   mpirun -np <rankSize> ./310p3_bm_test <store_url> [mode]
 *
 *   mode: hbm | dram | lifecycle (default: hbm)
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <vector>
#include <ctime>
#include <unistd.h>
#include <mpi.h>

#include "acl/acl.h"
#include "smem_bm.h"
#include "hybm.h"

namespace {

constexpr uint64_t HBM_SIZE = 1ULL * 1024ULL * 1024ULL * 1024ULL;
constexpr uint64_t DRAM_SIZE = 256ULL * 1024ULL * 1024ULL;
constexpr uint64_t COPY_SIZE = 16ULL * 1024ULL * 1024ULL;
constexpr int32_t LOOPS = 100;

uint32_t Pat(uint32_t tag, size_t idx)
{
    return (tag << 24) | (static_cast<uint32_t>(idx) & 0x00FFFFFFU);
}

void MpBarrier()
{
    MPI_Barrier(MPI_COMM_WORLD);
}

int32_t BmInit(int rank, int worldSize, int devId, const char *storeUrl)
{
    smem_bm_config_t config;
    smem_bm_config_init(&config);
    config.startConfigStoreServer = (rank == 0);
    config.autoRanking = false;
    config.rankId = static_cast<uint32_t>(rank);
    config.controlOperationTimeout = 300;
    config.initTimeout = 300;
    config.createTimeout = 300;

    int32_t ret = smem_bm_init(storeUrl, static_cast<uint32_t>(worldSize), static_cast<uint16_t>(devId), &config);
    if (ret != 0) {
        fprintf(stderr, "[rank %d] smem_bm_init failed:%d storeUrl:%s devId:%d\n", rank, ret, storeUrl, devId);
        return ret;
    }
    int32_t socType = hybm_get_soc_type();
    printf("[rank %d] smem_bm_init OK socType=%d devId=%d worldSize=%d\n", rank, socType, devId, worldSize);
    return 0;
}

/* HBM test: L2G to peer, G2H verify, G2L verify, G2G verify */
int32_t TestHbm(int rank, int worldSize)
{
    printf("\n========== HBM Pool Test (rankSize=%d) ==========\n", worldSize);

    smem_bm_create_option_t opt{};
    opt.maxHbmSize = HBM_SIZE;
    opt.localHBMSize = HBM_SIZE;
    opt.dataOpType = SMEMB_DATA_OP_SDMA;

    smem_bm_t handle = smem_bm_create2(1, &opt);
    if (handle == nullptr) {
        fprintf(stderr, "[rank %d] smem_bm_create2(HBM) failed\n", rank);
        return 1;
    }

    int32_t ret = smem_bm_join(handle, 0);
    if (ret != 0) {
        fprintf(stderr, "[rank %d] smem_bm_join(HBM) failed:%d\n", rank, ret);
        smem_bm_destroy(handle);
        return 1;
    }
    printf("[rank %d] smem_bm_join(HBM) OK\n", rank);
    MpBarrier();
    sleep(1);

    void *localGva = smem_bm_ptr_by_mem_type(handle, SMEM_MEM_TYPE_DEVICE, static_cast<uint16_t>(rank));
    if (localGva == nullptr) {
        fprintf(stderr, "[rank %d] ptr_by_mem_type(DEVICE, self) failed\n", rank);
        smem_bm_destroy(handle);
        return 1;
    }

    int nfail = 0;
    for (int peerRank = 0; peerRank < worldSize; peerRank++) {
        if (peerRank == rank) {
            continue;
        }
        void *peerGva = smem_bm_ptr_by_mem_type(handle, SMEM_MEM_TYPE_DEVICE, static_cast<uint16_t>(peerRank));
        if (peerGva == nullptr) {
            fprintf(stderr, "[rank %d] ptr_by_mem_type(DEVICE, peer:%d) failed\n", rank, peerRank);
            nfail++;
            continue;
        }
        printf("[rank %d] peerRank:%d localGva:%p peerGva:%p\n", rank, peerRank, localGva, peerGva);

        uint32_t myTag = 0xA0U | static_cast<uint32_t>(rank);
        uint32_t peerTag = 0xA0U | static_cast<uint32_t>(peerRank);

        /* L2G: local device buffer -> peer GVA */
        void *localDevBuf = nullptr;
        aclrtMalloc(&localDevBuf, COPY_SIZE, ACL_MEM_MALLOC_HUGE_FIRST);
        if (localDevBuf == nullptr) {
            fprintf(stderr, "[rank %d] aclrtMalloc failed for L2G\n", rank);
            nfail++;
            continue;
        }
        {
            std::vector<uint8_t> hbuf(COPY_SIZE);
            uint32_t *p = reinterpret_cast<uint32_t *>(hbuf.data());
            for (size_t i = 0; i < COPY_SIZE / 4; i++) {
                p[i] = Pat(myTag, i);
            }
            aclrtMemcpy(localDevBuf, COPY_SIZE, hbuf.data(), COPY_SIZE, ACL_MEMCPY_HOST_TO_DEVICE);

            smem_copy_params_t params{};
            params.src = localDevBuf;
            params.dest = peerGva;
            params.dataSize = COPY_SIZE;
            params.stream = nullptr;
            ret = smem_bm_copy(handle, &params, SMEMB_COPY_L2G, 0);
            if (ret != 0) {
                fprintf(stderr, "[rank %d] L2G(peer %d) failed:%d\n", rank, peerRank, ret);
                nfail++;
            } else {
                smem_bm_wait(handle);
                printf("[rank %d] L2G(peer %d) OK\n", rank, peerRank);
            }
        }
        aclrtFree(localDevBuf);

        MpBarrier();

        /* G2H: read own GVA (peer wrote into it via L2G) -> host buffer, verify */
        {
            std::vector<uint8_t> hbuf(COPY_SIZE, 0);
            smem_copy_params_t params{};
            params.src = localGva;
            params.dest = hbuf.data();
            params.dataSize = COPY_SIZE;
            params.stream = nullptr;
            ret = smem_bm_copy(handle, &params, SMEMB_COPY_G2H, 0);
            if (ret != 0) {
                fprintf(stderr, "[rank %d] G2H(peer %d) failed:%d\n", rank, peerRank, ret);
                nfail++;
            } else {
                smem_bm_wait(handle);
                uint32_t *p = reinterpret_cast<uint32_t *>(hbuf.data());
                bool ok = true;
                for (size_t i = 0; i < COPY_SIZE / 4; i++) {
                    if (p[i] != Pat(peerTag, i)) {
                        fprintf(stderr, "[rank %d] G2H verify FAIL at word %zu: got 0x%08x want 0x%08x\n",
                                rank, i, p[i], Pat(peerTag, i));
                        ok = false;
                        break;
                    }
                }
                if (ok) {
                    printf("[rank %d] G2H(peer %d) verify OK\n", rank, peerRank);
                } else {
                    nfail++;
                }
            }
        }

        MpBarrier();

        /* G2L: peer GVA -> local device buffer, verify own pattern (peerGva has my L2G data) */
        {
            void *localDevBuf = nullptr;
            aclrtMalloc(&localDevBuf, COPY_SIZE, ACL_MEM_MALLOC_HUGE_FIRST);
            if (localDevBuf == nullptr) {
                fprintf(stderr, "[rank %d] aclrtMalloc failed for G2L\n", rank);
                nfail++;
                continue;
            }
            smem_copy_params_t params{};
            params.src = peerGva;
            params.dest = localDevBuf;
            params.dataSize = COPY_SIZE;
            params.stream = nullptr;
            ret = smem_bm_copy(handle, &params, SMEMB_COPY_G2L, 0);
            if (ret != 0) {
                fprintf(stderr, "[rank %d] G2L(peer %d) failed:%d\n", rank, peerRank, ret);
                nfail++;
            } else {
                smem_bm_wait(handle);
                std::vector<uint8_t> hbuf(COPY_SIZE);
                aclrtMemcpy(hbuf.data(), COPY_SIZE, localDevBuf, COPY_SIZE, ACL_MEMCPY_DEVICE_TO_HOST);
                uint32_t *p = reinterpret_cast<uint32_t *>(hbuf.data());
                bool ok = true;
                for (size_t i = 0; i < COPY_SIZE / 4; i++) {
                    if (p[i] != Pat(myTag, i)) {
                        fprintf(stderr, "[rank %d] G2L verify FAIL at word %zu\n", rank, i);
                        ok = false;
                        break;
                    }
                }
                if (ok) {
                    printf("[rank %d] G2L(peer %d) verify OK\n", rank, peerRank);
                } else {
                    nfail++;
                }
            }
            aclrtFree(localDevBuf);
        }

        MpBarrier();

        /* G2G: peer GVA -> own GVA (serial to avoid read-write race), then G2H verify */
        for (int r = 0; r < worldSize; r++) {
            if (rank != r) {
                MpBarrier();
                continue;
            }
            smem_copy_params_t params{};
            params.src = peerGva;
            params.dest = localGva;
            params.dataSize = COPY_SIZE;
            params.stream = nullptr;
            ret = smem_bm_copy(handle, &params, SMEMB_COPY_G2G, 0);
            if (ret != 0) {
                fprintf(stderr, "[rank %d] G2G(peer %d) failed:%d\n", rank, peerRank, ret);
                nfail++;
            } else {
                smem_bm_wait(handle);
                std::vector<uint8_t> hbuf(COPY_SIZE, 0);
                params.src = localGva;
                params.dest = hbuf.data();
                smem_bm_copy(handle, &params, SMEMB_COPY_G2H, 0);
                smem_bm_wait(handle);
                /* Serial G2G: rank0 overwrites own GVA first, then rank1 reads rank0's GVA.
                 * After serial G2G, all ranks' localGva have rank0's original L2G pattern. */
                uint32_t g2gTag = 0xA0U;
                uint32_t *p = reinterpret_cast<uint32_t *>(hbuf.data());
                bool ok = true;
                for (size_t i = 0; i < COPY_SIZE / 4; i++) {
                    if (p[i] != Pat(g2gTag, i)) {
                        ok = false;
                        break;
                    }
                }
                if (ok) {
                    printf("[rank %d] G2G(peer %d) verify OK\n", rank, peerRank);
                } else {
                    nfail++;
                }
            }
            MpBarrier();
        }

        MpBarrier();
    }

    smem_bm_destroy(handle);
    printf("[rank %d] HBM test %s (rankSize:%d)\n", rank, nfail > 0 ? "FAIL" : "PASS", worldSize);
    return nfail;
}

/* DRAM HOST_SHM test: H2GH to peer, GH2H verify */
int32_t TestDram(int rank, int worldSize)
{
    printf("\n========== DRAM HOST_SHM Pool Test (rankSize=%d) ==========\n", worldSize);

    smem_bm_create_option_t opt{};
    opt.maxDramSize = DRAM_SIZE;
    opt.localDRAMSize = DRAM_SIZE;
    opt.maxHbmSize = 0;
    opt.localHBMSize = 0;
    opt.dataOpType = SMEMB_DATA_OP_HOST_SHM;

    smem_bm_t handle = smem_bm_create2(2, &opt);
    if (handle == nullptr) {
        fprintf(stderr, "[rank %d] smem_bm_create2(DRAM) failed\n", rank);
        return 1;
    }

    int32_t ret = smem_bm_join(handle, 0);
    if (ret != 0) {
        fprintf(stderr, "[rank %d] smem_bm_join(DRAM) failed:%d\n", rank, ret);
        smem_bm_destroy(handle);
        return 1;
    }
    printf("[rank %d] smem_bm_join(DRAM) OK\n", rank);
    MpBarrier();
    sleep(1);

    void *localGha = smem_bm_ptr_by_mem_type(handle, SMEM_MEM_TYPE_HOST, static_cast<uint16_t>(rank));
    if (localGha == nullptr) {
        fprintf(stderr, "[rank %d] ptr_by_mem_type(HOST, self) failed\n", rank);
        smem_bm_destroy(handle);
        return 1;
    }

    int nfail = 0;
    uint32_t myTag = 0x80U | static_cast<uint32_t>(rank);

    /* Round-robin: for each target rank, all other ranks write to it, then it verifies.
     * This avoids concurrent writes to different peers causing read-write races on the
     * same HOST_SHM backing store. */
    for (int targetRank = 0; targetRank < worldSize; targetRank++) {
        /* Phase 1: one designated writer writes to target's GHA.
         * Writer = (targetRank + 1) % worldSize, ensuring every rank writes once
         * and every rank is written to once. */
        int writer = (targetRank + 1) % worldSize;
        if (rank == writer) {
            void *targetGha = smem_bm_ptr_by_mem_type(handle, SMEM_MEM_TYPE_HOST,
                                                      static_cast<uint16_t>(targetRank));
            if (targetGha == nullptr) {
                fprintf(stderr, "[rank %d] ptr_by_mem_type(HOST, target:%d) failed\n", rank, targetRank);
                nfail++;
            } else {
                std::vector<uint8_t> hbuf(COPY_SIZE);
                uint32_t *p = reinterpret_cast<uint32_t *>(hbuf.data());
                for (size_t i = 0; i < COPY_SIZE / 4; i++) {
                    p[i] = Pat(myTag, i);
                }
                smem_copy_params_t params{};
                params.src = hbuf.data();
                params.dest = targetGha;
                params.dataSize = COPY_SIZE;
                params.stream = nullptr;
                ret = smem_bm_copy(handle, &params, SMEMB_COPY_H2GH, 0);
                if (ret != 0) {
                    fprintf(stderr, "[rank %d] H2GH(target %d) failed:%d\n", rank, targetRank, ret);
                    nfail++;
                } else {
                    smem_bm_wait(handle);
                    printf("[rank %d] H2GH(target %d) OK\n", rank, targetRank);
                }
            }
        }

        MpBarrier();

        /* Phase 2: target rank reads its own GHA and verifies the writer's data */
        if (targetRank == rank) {
            uint32_t expectedTag = 0x80U | static_cast<uint32_t>(writer);
            std::vector<uint8_t> hbuf(COPY_SIZE, 0);
            smem_copy_params_t params{};
            params.src = localGha;
            params.dest = hbuf.data();
            params.dataSize = COPY_SIZE;
            params.stream = nullptr;
            ret = smem_bm_copy(handle, &params, SMEMB_COPY_GH2H, 0);
            if (ret != 0) {
                fprintf(stderr, "[rank %d] GH2H(writer %d) failed:%d\n", rank, writer, ret);
                nfail++;
            } else {
                smem_bm_wait(handle);
                uint32_t *p = reinterpret_cast<uint32_t *>(hbuf.data());
                bool ok = true;
                for (size_t i = 0; i < COPY_SIZE / 4; i++) {
                    if (p[i] != Pat(expectedTag, i)) {
                        fprintf(stderr, "[rank %d] GH2H verify FAIL at word %zu: got 0x%08x want 0x%08x\n",
                                rank, i, p[i], Pat(expectedTag, i));
                        ok = false;
                        break;
                    }
                }
                if (ok) {
                    printf("[rank %d] GH2H(writer %d) verify OK\n", rank, writer);
                } else {
                    nfail++;
                }
            }
        }

        MpBarrier();
    }

    smem_bm_destroy(handle);
    printf("[rank %d] DRAM test %s (rankSize:%d)\n", rank, nfail > 0 ? "FAIL" : "PASS", worldSize);
    return nfail;
}

/* 100x lifecycle: create -> join -> L2G to peer -> G2H verify -> destroy */
int32_t TestLifecycle(int rank, int worldSize)
{
    printf("\n========== 100x Lifecycle Test (HBM, rankSize=%d) ==========\n", worldSize);

    int nfail = 0;
    int peerRank = (rank == 0) ? 1 : 0;

    for (int loop = 0; loop < LOOPS; loop++) {
        smem_bm_create_option_t opt{};
        opt.maxHbmSize = HBM_SIZE;
        opt.localHBMSize = HBM_SIZE;
        opt.dataOpType = SMEMB_DATA_OP_SDMA;

        smem_bm_t handle = smem_bm_create2(static_cast<uint32_t>((loop % 63) + 1), &opt);
        if (handle == nullptr) {
            fprintf(stderr, "[rank %d] loop %d create2 failed\n", rank, loop);
            nfail++;
            MpBarrier();
            continue;
        }

        int32_t ret = smem_bm_join(handle, 0);
        if (ret != 0) {
            fprintf(stderr, "[rank %d] loop %d join failed:%d\n", rank, loop, ret);
            smem_bm_destroy(handle);
            nfail++;
            MpBarrier();
            continue;
        }
        MpBarrier();
        sleep(1);

        void *peerGva = smem_bm_ptr_by_mem_type(handle, SMEM_MEM_TYPE_DEVICE, static_cast<uint16_t>(peerRank));
        void *localGva = smem_bm_ptr_by_mem_type(handle, SMEM_MEM_TYPE_DEVICE, static_cast<uint16_t>(rank));
        if (peerGva == nullptr || localGva == nullptr) {
            smem_bm_destroy(handle);
            nfail++;
            MpBarrier();
            continue;
        }

        /* L2G: write to peer */
        void *localDevBuf = nullptr;
        aclrtMalloc(&localDevBuf, COPY_SIZE, ACL_MEM_MALLOC_HUGE_FIRST);
        if (localDevBuf != nullptr) {
            std::vector<uint8_t> hbuf(COPY_SIZE);
            uint32_t myTag = 0x80U | static_cast<uint32_t>(loop & 0x3F);
            uint32_t *p = reinterpret_cast<uint32_t *>(hbuf.data());
            for (size_t i = 0; i < COPY_SIZE / 4; i++) {
                p[i] = Pat(myTag, i);
            }
            aclrtMemcpy(localDevBuf, COPY_SIZE, hbuf.data(), COPY_SIZE, ACL_MEMCPY_HOST_TO_DEVICE);

            smem_copy_params_t params{};
            params.src = localDevBuf;
            params.dest = peerGva;
            params.dataSize = COPY_SIZE;
            params.stream = nullptr;
            ret = smem_bm_copy(handle, &params, SMEMB_COPY_L2G, 0);
            if (ret != 0) {
                fprintf(stderr, "[rank %d] loop %d L2G failed:%d\n", rank, loop, ret);
                nfail++;
            }
            smem_bm_wait(handle);
            aclrtFree(localDevBuf);
        }

        MpBarrier();

        /* G2H: verify peer wrote to us */
        {
            std::vector<uint8_t> hbuf(COPY_SIZE, 0);
            smem_copy_params_t params{};
            params.src = localGva;
            params.dest = hbuf.data();
            params.dataSize = COPY_SIZE;
            params.stream = nullptr;
            ret = smem_bm_copy(handle, &params, SMEMB_COPY_G2H, 0);
            if (ret != 0) {
                fprintf(stderr, "[rank %d] loop %d G2H failed:%d\n", rank, loop, ret);
                nfail++;
            } else {
                smem_bm_wait(handle);
                uint32_t expectedTag = 0x80U | static_cast<uint32_t>(loop & 0x3F);
                uint32_t *p = reinterpret_cast<uint32_t *>(hbuf.data());
                bool ok = true;
                for (size_t i = 0; i < COPY_SIZE / 4; i++) {
                    if (p[i] != Pat(expectedTag, i)) {
                        fprintf(stderr, "[rank %d] loop %d verify FAIL at word %zu\n", rank, loop, i);
                        ok = false;
                        break;
                    }
                }
                if (!ok) {
                    nfail++;
                }
            }
        }

        smem_bm_destroy(handle);
        if ((loop + 1) % 20 == 0) {
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
        fprintf(stderr, "usage: mpirun -np <rankSize> %s <store_url> [mode]\n", argv[0]);
        fprintf(stderr, "  mode: hbm | dram | lifecycle (default: hbm)\n");
        return 1;
    }

    const char *storeUrl = argv[1];
    const char *modeStr = (argc >= 3) ? argv[2] : "hbm";

    MPI_Init(&argc, &argv);
    int rank, worldSize;
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

    printf("[rank %d] start mode:%s devId:%d rankSize:%d\n", rank, modeStr, devId, worldSize);

    aclInit(nullptr);
    aclrtSetDevice(devId);

    int32_t ret = BmInit(rank, worldSize, devId, storeUrl);
    if (ret != 0) {
        MPI_Finalize();
        return ret;
    }

    int32_t testRet = 0;
    if (strcmp(modeStr, "dram") == 0) {
        testRet = TestDram(rank, worldSize);
    } else if (strcmp(modeStr, "lifecycle") == 0) {
        testRet = TestLifecycle(rank, worldSize);
    } else {
        testRet = TestHbm(rank, worldSize);
    }

    MpBarrier();
    smem_bm_uninit(0);
    aclrtResetDevice(devId);
    aclFinalize();

    MpBarrier();
    MPI_Finalize();

    printf("[rank %d] %s\n", rank, testRet != 0 ? "TEST FAILED" : "ALL TESTS PASSED");
    return testRet != 0 ? 1 : 0;
}
