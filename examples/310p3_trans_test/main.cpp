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
 * 310P3 TRANS Mode Functional Verification
 *
 * TRANS 注册用户已分配的 aclrtMalloc 设备内存，通过 config store 交换 IPC key，
 * 对端 import 后用 aclrtMemcpyAsync(D2D) 跨 rank 传输。
 *
 * 310P3 关键路径（与 910B 的差异）：
 *   - Export: AclrtIpcMemGetExportKey 替代 RtIpcSetMemoryName（DISABLE_PID_VALIDATION）
 *   - Import: AclrtIpcMemImportByKey 替代 RtIpcOpenMemory
 *     同卡 die 用 DEFAULT flag（不触发 P2P 前置检查，绕过 507899）
 *     跨卡用 ENABLE_PEER_ACCESS flag（自动建立双向 P2P）
 *   - CopyG2G: AclrtMemcpyAsync + AclrtSynchronizeStream（310P3 不支持 sync D2D）
 *
 * Tests:
 *   1. register_mem -> sync write -> verify（串行 round-robin 避免并发写竞争）
 *   2. async write_submit + stream sync -> verify
 *   3. deregister_mem
 *
 * Usage:
 *   mpirun -np <rankSize> ./310p3_trans_test <store_url>
 *
 *   rankSize: 2 or 4 (2-rank: same-card die pair; 4-rank: cross-card)
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
#include "smem_trans.h"
#include "smem_bm_def.h"
#include "hybm.h"

namespace {

constexpr uint64_t ALLOC_SIZE = 16ULL * 1024ULL * 1024ULL;
constexpr uint64_t COPY_SIZE = 16ULL * 1024ULL * 1024ULL;

uint32_t Pat(uint32_t tag, size_t idx)
{
    return (tag << 24) | (static_cast<uint32_t>(idx) & 0x00FFFFFFU);
}

int MpRank()
{
    int rank = 0;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    return rank;
}

int MpSize()
{
    int size = 0;
    MPI_Comm_size(MPI_COMM_WORLD, &size);
    return size;
}

void MpBarrier()
{
    MPI_Barrier(MPI_COMM_WORLD);
}

std::string PeerUniqueId(int rank, int worldSize)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "127.0.0.1:%d", 13000 + rank);
    return std::string(buf);
}

int32_t TransInit(int rank, int devId)
{
    smem_set_log_level(1);
    smem_trans_config_t config;
    smem_trans_config_init(&config);
    config.role = SMEM_TRANS_BOTH; /* 既是 sender 又是 receiver，两端互写 */
    config.deviceId = static_cast<uint32_t>(devId);
    config.initTimeout = 300;
    config.dataOpType = SMEMB_DATA_OP_SDMA; /* 310P3 唯一支持的 D2D 传输后端 */

    int32_t ret = smem_trans_init(&config);
    if (ret != 0) {
        fprintf(stderr, "[rank %d] smem_trans_init failed:%d devId:%d\n", rank, ret, devId);
        return ret;
    }
    int32_t socType = hybm_get_soc_type();
    printf("[rank %d] smem_trans_init OK socType=%d devId=%d\n", rank, socType, devId);
    return 0;
}

smem_trans_t TransCreate(int rank, const char *storeUrl)
{
    std::string uniqueId = PeerUniqueId(rank, MpSize());
    smem_trans_config_t config;
    smem_trans_config_init(&config);
    config.role = SMEM_TRANS_BOTH;
    config.deviceId = static_cast<uint32_t>(rank);
    config.initTimeout = 300;
    config.dataOpType = SMEMB_DATA_OP_SDMA;

    smem_trans_t handle = smem_trans_create(storeUrl, uniqueId.c_str(), &config);
    if (handle == nullptr) {
        fprintf(stderr, "[rank %d] smem_trans_create failed storeUrl:%s uniqueId:%s\n",
                rank, storeUrl, uniqueId.c_str());
        return nullptr;
    }
    printf("[rank %d] smem_trans_create OK uniqueId:%s\n", rank, uniqueId.c_str());
    return handle;
}

int32_t RegisterAndFill(int rank, smem_trans_t handle, void *devMem, uint32_t tag)
{
    /* 注册用户已分配的 aclrtMalloc 内存到 TRANS entity，内部通过 ACL IPC 导出 key */
    int32_t ret = smem_trans_register_mem(handle, devMem, ALLOC_SIZE, 0);
    if (ret != 0) {
        fprintf(stderr, "[rank %d] register_mem failed:%d addr:%p size:%lu\n", rank, ret, devMem, ALLOC_SIZE);
        return ret;
    }
    printf("[rank %d] register_mem OK addr:%p size:%lu\n", rank, devMem, ALLOC_SIZE);

    std::vector<uint32_t> hostBuf(COPY_SIZE / sizeof(uint32_t));
    for (size_t i = 0; i < hostBuf.size(); i++) {
        hostBuf[i] = Pat(tag, i);
    }
    ret = aclrtMemcpy(devMem, ALLOC_SIZE, hostBuf.data(), hostBuf.size() * sizeof(uint32_t),
                       ACL_MEMCPY_HOST_TO_DEVICE);
    if (ret != 0) {
        fprintf(stderr, "[rank %d] aclrtMemcpy H2D failed:%d\n", rank, ret);
        return ret;
    }
    return 0;
}

int32_t VerifyData(int rank, void *devMem, uint32_t expectedTag)
{
    std::vector<uint32_t> hostBuf(COPY_SIZE / sizeof(uint32_t));
    int32_t ret = aclrtMemcpy(hostBuf.data(), hostBuf.size() * sizeof(uint32_t), devMem, ALLOC_SIZE,
                              ACL_MEMCPY_DEVICE_TO_HOST);
    if (ret != 0) {
        fprintf(stderr, "[rank %d] aclrtMemcpy D2H failed:%d\n", rank, ret);
        return ret;
    }

    uint32_t errors = 0;
    for (size_t i = 0; i < hostBuf.size(); i++) {
        uint32_t expected = Pat(expectedTag, i);
        if (hostBuf[i] != expected) {
            if (errors < 10) {
                fprintf(stderr, "[rank %d] verify FAIL at idx:%zu got:0x%08x expected:0x%08x\n",
                        rank, i, hostBuf[i], expected);
            }
            errors++;
        }
    }
    if (errors > 0) {
        fprintf(stderr, "[rank %d] verify FAIL total_errors:%u expectedTag:%u\n", rank, errors, expectedTag);
        return -1;
    }
    printf("[rank %d] verify PASS expectedTag:%u\n", rank, expectedTag);
    return 0;
}

int32_t TestSyncWriteRead(int rank, int worldSize, smem_trans_t handle, void *devMem)
{
    printf("[rank %d] === TestSyncWriteRead ===\n", rank);
    uint32_t myTag = static_cast<uint32_t>(rank + 1) << 4;
    int32_t ret = RegisterAndFill(rank, handle, devMem, myTag);
    if (ret != 0) {
        return ret;
    }
    MpBarrier();

    int nextRank = (rank + 1) % worldSize;
    std::string peerId = PeerUniqueId(nextRank, worldSize);

    /* 串行 round-robin：rank0 先写，对端 verify 后再反向写，避免并发写同一地址 */
    if (rank == 0) {
        ret = smem_trans_write(handle, devMem, peerId.c_str(), devMem, COPY_SIZE, 0);
        if (ret != 0) {
            fprintf(stderr, "[rank %d] trans_write to rank:%d failed:%d\n", rank, nextRank, ret);
            return ret;
        }
        printf("[rank %d] trans_write to rank:%d OK size:%lu\n", rank, nextRank, COPY_SIZE);
    }
    MpBarrier();

    uint32_t prevTag = static_cast<uint32_t>(nextRank + 1) << 4;
    if (rank != 0) {
        ret = VerifyData(rank, devMem, prevTag);
        if (ret != 0) {
            return ret;
        }
    }
    MpBarrier();

    if (rank != 0) {
        ret = smem_trans_write(handle, devMem, peerId.c_str(), devMem, COPY_SIZE, 0);
        if (ret != 0) {
            fprintf(stderr, "[rank %d] trans_write to rank:%d failed:%d\n", rank, nextRank, ret);
            return ret;
        }
        printf("[rank %d] trans_write to rank:%d OK size:%lu\n", rank, nextRank, COPY_SIZE);
    }
    MpBarrier();
    return 0;
}

int32_t TestAsyncSubmit(int rank, int worldSize, smem_trans_t handle, void *devMem)
{
    printf("[rank %d] === TestAsyncSubmit ===\n", rank);
    uint32_t myTag = static_cast<uint32_t>(rank + 1) << 8;
    std::vector<uint32_t> hostBuf(COPY_SIZE / sizeof(uint32_t));
    for (size_t i = 0; i < hostBuf.size(); i++) {
        hostBuf[i] = Pat(myTag, i);
    }
    int32_t ret = aclrtMemcpy(devMem, ALLOC_SIZE, hostBuf.data(), hostBuf.size() * sizeof(uint32_t),
                              ACL_MEMCPY_HOST_TO_DEVICE);
    if (ret != 0) {
        fprintf(stderr, "[rank %d] H2D fill failed:%d\n", rank, ret);
        return ret;
    }

    int nextRank = (rank + 1) % worldSize;
    std::string peerId = PeerUniqueId(nextRank, worldSize);
    aclrtStream stream = nullptr;
    ret = aclrtCreateStream(&stream);
    if (ret != 0) {
        fprintf(stderr, "[rank %d] create stream failed:%d\n", rank, ret);
        return ret;
    }

    if (rank == 0) {
        ret = smem_trans_write_submit(handle, devMem, peerId.c_str(), devMem, COPY_SIZE, stream, 0);
        if (ret != 0) {
            fprintf(stderr, "[rank %d] write_submit to rank:%d failed:%d\n", rank, nextRank, ret);
            aclrtDestroyStream(stream);
            return ret;
        }
        ret = aclrtSynchronizeStream(stream);
        if (ret != 0) {
            fprintf(stderr, "[rank %d] sync stream failed:%d\n", rank, ret);
            aclrtDestroyStream(stream);
            return ret;
        }
        printf("[rank %d] write_submit to rank:%d OK\n", rank, nextRank);
    }
    MpBarrier();

    uint32_t prevTag = static_cast<uint32_t>(nextRank + 1) << 8;
    if (rank != 0) {
        ret = VerifyData(rank, devMem, prevTag);
        if (ret != 0) {
            aclrtDestroyStream(stream);
            return ret;
        }
    }
    MpBarrier();

    if (rank != 0) {
        ret = smem_trans_write_submit(handle, devMem, peerId.c_str(), devMem, COPY_SIZE, stream, 0);
        if (ret != 0) {
            fprintf(stderr, "[rank %d] write_submit to rank:%d failed:%d\n", rank, nextRank, ret);
            aclrtDestroyStream(stream);
            return ret;
        }
        ret = aclrtSynchronizeStream(stream);
        if (ret != 0) {
            fprintf(stderr, "[rank %d] sync stream failed:%d\n", rank, ret);
            aclrtDestroyStream(stream);
            return ret;
        }
        printf("[rank %d] write_submit to rank:%d OK\n", rank, nextRank);
    }
    MpBarrier();
    return 0;
}

int32_t TestDeregister(int rank, smem_trans_t handle, void *devMem)
{
    printf("[rank %d] === TestDeregister ===\n", rank);
    int32_t ret = smem_trans_deregister_mem(handle, devMem);
    if (ret != 0) {
        fprintf(stderr, "[rank %d] deregister_mem failed:%d addr:%p\n", rank, ret, devMem);
        return ret;
    }
    printf("[rank %d] deregister_mem OK addr:%p\n", rank, devMem);
    return 0;
}

} // namespace

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <store_url>\n", argv[0]);
        return 1;
    }
    const char *storeUrl = argv[1];

    MPI_Init(&argc, &argv);
    int rank = MpRank();
    int worldSize = MpSize();
    int devId = rank;

    aclrtSetDevice(devId);

    int32_t ret = smem_init(0);
    if (ret != 0) {
        fprintf(stderr, "[rank %d] smem_init failed:%d\n", rank, ret);
        MPI_Finalize();
        return ret;
    }

    if (rank == 0) {
        ret = smem_create_config_store(storeUrl, 0);
        if (ret != 0) {
            fprintf(stderr, "[rank 0] smem_create_config_store failed:%d storeUrl:%s\n", ret, storeUrl);
            MPI_Abort(MPI_COMM_WORLD, ret);
        }
    }
    MpBarrier();

    ret = TransInit(rank, devId);
    if (ret != 0) {
        MPI_Finalize();
        return ret;
    }

    smem_trans_t handle = TransCreate(rank, storeUrl);
    if (handle == nullptr) {
        smem_trans_uninit(0);
        MPI_Finalize();
        return 1;
    }
    MpBarrier();
    sleep(1);

    void *devMem = nullptr;
    ret = aclrtMalloc(&devMem, ALLOC_SIZE, ACL_MEM_MALLOC_HUGE_FIRST);
    if (ret != 0) {
        fprintf(stderr, "[rank %d] aclrtMalloc failed:%d size:%lu\n", rank, ret, ALLOC_SIZE);
        smem_trans_destroy(handle, 0);
        smem_trans_uninit(0);
        MPI_Finalize();
        return ret;
    }
    printf("[rank %d] aclrtMalloc OK addr:%p size:%lu\n", rank, devMem, ALLOC_SIZE);

    ret = TestSyncWriteRead(rank, worldSize, handle, devMem);
    if (ret != 0) {
        goto cleanup;
    }

    ret = TestAsyncSubmit(rank, worldSize, handle, devMem);
    if (ret != 0) {
        goto cleanup;
    }

    ret = TestDeregister(rank, handle, devMem);
    if (ret != 0) {
        goto cleanup;
    }

    printf("[rank %d] ALL TESTS PASS\n", rank);

cleanup:
    if (devMem != nullptr) {
        aclrtFree(devMem);
    }
    smem_trans_destroy(handle, 0);
    smem_trans_uninit(0);
    MpBarrier();
    if (rank == 0) {
        smem_destroy_config_store(storeUrl);
    }
    aclrtResetDevice(devId);
    aclFinalize();
    MPI_Finalize();
    return ret;
}
