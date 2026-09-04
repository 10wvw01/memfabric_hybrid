/*
 * 17_trans_verify — TRANS 模式外部内存共享路径验证
 *
 * 背景：TRANS 注册用户已分配的 aclrtMalloc 内存，当前用 HybmDevUserLegacySegment
 * (RtIpcSetMemoryName/RtIpcOpenMemory)，310P3 同卡 die import 返回 507899。
 *
 * 验证三种路径能否处理 aclrtMalloc 外部内存：
 *   A. ACL IPC:  aclrtIpcMemGetExportKey + aclrtIpcMemImportByKey(DEFAULT)
 *   B. VMM:      aclrtMemExportToShareableHandle 对 aclrtMalloc 内存（无句柄，预期失败）
 *   C. VMM 基线: aclrtMallocPhysical + Export + Import(ctx 技巧) — 已验证可用
 *
 * 结构: MPI 双进程, rank0=exporter(dev0), rank1=importer(dev1), 同卡 die 对
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <mpi.h>
#include "acl/acl.h"

#define MEM_SIZE    (16u * 1024u * 1024u)
#define IPC_KEY_LEN 65

#define TAG_EXP  0xA1
#define TAG_BACK 0xA2

static int g_pass = 0;
static int g_fail = 0;

static uint32_t pat(uint32_t rank, uint32_t tag, size_t idx)
{
    return ((rank & 0xFu) << 28) | ((tag & 0xFu) << 24) | ((uint32_t)idx & 0x00FFFFFFu);
}

static void fill_pat(uint32_t *buf, size_t words, uint32_t rank, uint32_t tag)
{
    for (size_t i = 0; i < words; i++)
        buf[i] = pat(rank, tag, i);
}

static int check_pat(const uint32_t *buf, size_t words, uint32_t rank, uint32_t tag)
{
    for (size_t i = 0; i < words; i++) {
        if (buf[i] != pat(rank, tag, i)) {
            fprintf(stderr, "[17]   verify FAIL at word %zu: got 0x%08x, want 0x%08x\n",
                    i, buf[i], pat(rank, tag, i));
            return -1;
        }
    }
    return 0;
}

/* ===== Path A: ACL IPC ===== */

static int test_path_a_ipc(int rank, int devA, int devB)
{
    printf("[17] === Path A: ACL IPC (aclrtIpcMemGetExportKey/ImportByKey DEFAULT) ===\n");
    aclrtStream stream = NULL;
    aclrtCreateStream(&stream);

    if (rank == 0) {
        void *devMem = NULL;
        aclrtMalloc(&devMem, MEM_SIZE, ACL_MEM_MALLOC_HUGE_FIRST);
        if (!devMem) { fprintf(stderr, "[17-A] exporter aclrtMalloc failed\n"); g_fail++; return -1; }

        uint32_t *host = (uint32_t *)malloc(MEM_SIZE);
        fill_pat(host, MEM_SIZE / 4, 0, TAG_EXP);
        aclrtMemcpy(devMem, MEM_SIZE, host, MEM_SIZE, ACL_MEMCPY_HOST_TO_DEVICE);

        char key[IPC_KEY_LEN] = {0};
        aclError ret = aclrtIpcMemGetExportKey(devMem, MEM_SIZE, key, IPC_KEY_LEN,
                                                ACL_RT_IPC_MEM_EXPORT_FLAG_DISABLE_PID_VALIDATION);
        printf("[17-A] exporter aclrtIpcMemGetExportKey ret=%d\n", ret);
        if (ret != 0) { g_fail++; free(host); aclrtFree(devMem); return -1; }

        MPI_Send(key, IPC_KEY_LEN, MPI_CHAR, 1, 0, MPI_COMM_WORLD);
        MPI_Barrier(MPI_COMM_WORLD);

        MPI_Barrier(MPI_COMM_WORLD);
        aclrtMemcpy(host, MEM_SIZE, devMem, MEM_SIZE, ACL_MEMCPY_DEVICE_TO_HOST);
        int ok = check_pat(host, MEM_SIZE / 4, 1, TAG_BACK);
        if (ok == 0) { printf("[17-A] exporter verify back-write PASS\n"); g_pass++; }
        else { g_fail++; }

        free(host);
        aclrtFree(devMem);
    } else {
        char key[IPC_KEY_LEN] = {0};
        MPI_Recv(key, IPC_KEY_LEN, MPI_CHAR, 0, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        MPI_Barrier(MPI_COMM_WORLD);

        void *localMem = NULL;
        aclrtMalloc(&localMem, MEM_SIZE, ACL_MEM_MALLOC_HUGE_FIRST);
        if (!localMem) { fprintf(stderr, "[17-A] importer aclrtMalloc failed\n"); g_fail++; return -1; }

        void *importedMem = NULL;
        aclError ret = aclrtIpcMemImportByKey(&importedMem, key, ACL_RT_IPC_MEM_IMPORT_FLAG_DEFAULT);
        printf("[17-A] importer aclrtIpcMemImportByKey(DEFAULT) ret=%d importedMem=%p\n", ret, importedMem);

        if (ret != 0) {
            printf("[17-A] *** Path A FAIL: import ret=%d ***\n", ret);
            g_fail++;
            aclrtFree(localMem);
            return -1;
        }

        ret = aclrtMemcpyAsync(localMem, MEM_SIZE, importedMem, MEM_SIZE,
                                ACL_MEMCPY_DEVICE_TO_DEVICE, stream);
        aclrtSynchronizeStream(stream);
        uint32_t *host = (uint32_t *)malloc(MEM_SIZE);
        aclrtMemcpy(host, MEM_SIZE, localMem, MEM_SIZE, ACL_MEMCPY_DEVICE_TO_HOST);
        int ok = check_pat(host, MEM_SIZE / 4, 0, TAG_EXP);
        if (ok == 0) printf("[17-A] importer D2D read verify PASS\n");

        fill_pat(host, MEM_SIZE / 4, 1, TAG_BACK);
        aclrtMemcpy(localMem, MEM_SIZE, host, MEM_SIZE, ACL_MEMCPY_HOST_TO_DEVICE);
        aclrtMemcpyAsync(importedMem, MEM_SIZE, localMem, MEM_SIZE,
                          ACL_MEMCPY_DEVICE_TO_DEVICE, stream);
        aclrtSynchronizeStream(stream);

        if (ok == 0) { printf("[17-A] *** Path A PASS ***\n"); g_pass++; }
        else { g_fail++; }

        MPI_Barrier(MPI_COMM_WORLD);
        free(host);
        aclrtIpcMemClose(key);
        aclrtFree(localMem);
    }
    aclrtDestroyStream(stream);
    return 0;
}

/* ===== Path B: VMM export of aclrtMalloc memory (expected to fail) ===== */

static int test_path_b_vmm_extmem(int rank, int devA, int devB)
{
    printf("[17] === Path B: VMM export of aclrtMalloc memory (no handle, expect fail) ===\n");
    printf("[17-B] SKIPPED — aclrtMalloc has no aclrtDrvMemHandle, VMM export segfaults (expected)\n");
    printf("[17-B] CONCLUSION: VMM export requires aclrtMallocPhysical, cannot export aclrtMalloc memory\n");
    MPI_Barrier(MPI_COMM_WORLD);
    return 0;
}

/* ===== Path C: VMM baseline (aclrtMallocPhysical, already verified) ===== */

static int test_path_c_vmm_baseline(int rank, int devA, int devB)
{
    printf("[17] === Path C: VMM baseline (aclrtMallocPhysical + ctx trick) ===\n");
    aclrtStream stream = NULL;
    aclrtCreateStream(&stream);

    if (rank == 0) {
        aclrtPhysicalMemProp prop;
        memset(&prop, 0, sizeof(prop));
        prop.handleType = ACL_MEM_HANDLE_TYPE_NONE;
        prop.allocationType = ACL_MEM_ALLOCATION_TYPE_PINNED;
        prop.location.type = ACL_MEM_LOCATION_TYPE_DEVICE;
        prop.location.id = (uint32_t)devA;
        prop.memAttr = ACL_MEM_P2P_HUGE;

        size_t granularity = 0;
        aclrtMemGetAllocationGranularity(&prop, ACL_RT_MEM_ALLOC_GRANULARITY_RECOMMENDED, &granularity);
        if (granularity == 0) granularity = 2 * 1024 * 1024;
        size_t allocSize = ((MEM_SIZE + granularity - 1) / granularity) * granularity;

        aclrtDrvMemHandle physHandle = NULL;
        aclError ret = aclrtMallocPhysical(&physHandle, allocSize, &prop, 0);
        printf("[17-C] exporter aclrtMallocPhysical ret=%d handle=%p size=%zu\n", ret, physHandle, allocSize);
        if (ret != 0) { g_fail++; return -1; }

        void *va = NULL;
        aclrtReserveMemAddress(&va, allocSize, 0, NULL, 0);
        ret = aclrtMapMem(va, allocSize, 0, physHandle, 0);
        printf("[17-C] exporter MapMem ret=%d va=%p\n", ret, va);
        if (ret != 0) { g_fail++; aclrtFreePhysical(physHandle); return -1; }

        uint32_t *host = (uint32_t *)malloc(MEM_SIZE);
        fill_pat(host, MEM_SIZE / 4, 0, TAG_EXP);
        aclrtMemcpy(va, MEM_SIZE, host, MEM_SIZE, ACL_MEMCPY_HOST_TO_DEVICE);

        uint64_t shareableHandle = 0;
        ret = aclrtMemExportToShareableHandle(physHandle, ACL_MEM_HANDLE_TYPE_NONE,
                                               ACL_RT_VMM_EXPORT_FLAG_DISABLE_PID_VALIDATION,
                                               &shareableHandle);
        printf("[17-C] exporter ExportToShareableHandle ret=%d sh=0x%lx\n", ret, shareableHandle);
        if (ret != 0) { g_fail++; free(host); aclrtUnmapMem(va); aclrtReleaseMemAddress(va); aclrtFreePhysical(physHandle); return -1; }

        MPI_Send(&shareableHandle, sizeof(shareableHandle), MPI_BYTE, 1, 0, MPI_COMM_WORLD);
        MPI_Barrier(MPI_COMM_WORLD);
        MPI_Barrier(MPI_COMM_WORLD);

        aclrtMemcpy(host, MEM_SIZE, va, MEM_SIZE, ACL_MEMCPY_DEVICE_TO_HOST);
        int ok = check_pat(host, MEM_SIZE / 4, 1, TAG_BACK);
        if (ok == 0) { printf("[17-C] exporter verify back-write PASS\n"); g_pass++; }
        else { g_fail++; }

        free(host);
        aclrtUnmapMem(va);
        aclrtReleaseMemAddress(va);
        aclrtFreePhysical(physHandle);
    } else {
        uint64_t shareableHandle = 0;
        MPI_Recv(&shareableHandle, sizeof(shareableHandle), MPI_BYTE, 0, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        MPI_Barrier(MPI_COMM_WORLD);

        void *localMem = NULL;
        aclrtMalloc(&localMem, MEM_SIZE, ACL_MEM_MALLOC_HUGE_FIRST);
        if (!localMem) { fprintf(stderr, "[17-C] importer aclrtMalloc failed\n"); g_fail++; return -1; }

        /* ctx trick */
        aclrtSetDevice(devA);
        aclrtDrvMemHandle handle = NULL;
        aclError ret = aclrtMemImportFromShareableHandle(shareableHandle, devB, &handle);
        aclrtSetDevice(devB);
        printf("[17-C] importer ImportFromShareableHandle(ctx trick) ret=%d\n", ret);
        if (ret != 0) { g_fail++; aclrtFree(localMem); return -1; }

        void *importedVa = NULL;
        aclrtReserveMemAddress(&importedVa, MEM_SIZE, 0, NULL, 0);
        ret = aclrtMapMem(importedVa, MEM_SIZE, 0, handle, 0);
        printf("[17-C] importer MapMem ret=%d va=%p\n", ret, importedVa);
        if (ret != 0) { g_fail++; aclrtReleaseMemAddress(importedVa); aclrtFree(localMem); return -1; }

        aclrtMemcpyAsync(localMem, MEM_SIZE, importedVa, MEM_SIZE,
                          ACL_MEMCPY_DEVICE_TO_DEVICE, stream);
        aclrtSynchronizeStream(stream);
        uint32_t *host = (uint32_t *)malloc(MEM_SIZE);
        aclrtMemcpy(host, MEM_SIZE, localMem, MEM_SIZE, ACL_MEMCPY_DEVICE_TO_HOST);
        int ok = check_pat(host, MEM_SIZE / 4, 0, TAG_EXP);
        if (ok == 0) printf("[17-C] importer D2D read verify PASS\n");

        fill_pat(host, MEM_SIZE / 4, 1, TAG_BACK);
        aclrtMemcpy(localMem, MEM_SIZE, host, MEM_SIZE, ACL_MEMCPY_HOST_TO_DEVICE);
        aclrtMemcpyAsync(importedVa, MEM_SIZE, localMem, MEM_SIZE,
                          ACL_MEMCPY_DEVICE_TO_DEVICE, stream);
        aclrtSynchronizeStream(stream);

        if (ok == 0) { printf("[17-C] *** Path C PASS ***\n"); g_pass++; }
        else { g_fail++; }

        MPI_Barrier(MPI_COMM_WORLD);
        free(host);
        aclrtUnmapMem(importedVa);
        aclrtReleaseMemAddress(importedVa);
        aclrtFreePhysical(handle);
        aclrtFree(localMem);
    }
    aclrtDestroyStream(stream);
    return 0;
}

int main(int argc, char *argv[])
{
    MPI_Init(&argc, &argv);
    int rank = 0, worldSize = 0;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &worldSize);
    int devA = 0;
    int devB = 1;

    printf("[17] rank=%d worldSize=%d devA=%d devB=%d\n", rank, worldSize, devA, devB);

    aclInit(NULL);
    aclrtSetDevice(rank == 0 ? devA : devB);

    test_path_a_ipc(rank, devA, devB);
    MPI_Barrier(MPI_COMM_WORLD);

    test_path_b_vmm_extmem(rank, devA, devB);
    MPI_Barrier(MPI_COMM_WORLD);

    test_path_c_vmm_baseline(rank, devA, devB);
    MPI_Barrier(MPI_COMM_WORLD);

    printf("[17] ===== SUMMARY: pass=%d fail=%d =====\n", g_pass, g_fail);
    if (g_fail > 0) {
        printf("[17] ===== OVERALL: FAIL =====\n");
    } else {
        printf("[17] ===== OVERALL: PASS =====\n");
    }

    aclrtResetDevice(rank == 0 ? devA : devB);
    aclFinalize();
    MPI_Finalize();
    return (g_fail > 0) ? 1 : 0;
}
