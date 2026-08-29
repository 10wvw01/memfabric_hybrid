/*
 * minitest3.c — 3-rank 最小化测试, 验证跨 socket PEER 连接竞争假设
 *
 * 假设: 同一张卡上, 当 die0 已建立跨 socket PEER 连接后, die1 不能再建立到相同跨 socket 设备的 PEER 连接
 *
 * 设备布局:
 *   Socket 0: card0 (dev0,1), card1 (dev2,3)
 *   Socket 1: card2 (dev4,5), card3 (dev6,7)
 *
 * 测试:
 *   T14: r0(dev0) PEER import r2(dev4) 先, 然后 r1(dev1) PEER import r2(dev4)
 *        → 验证: r0 先建立连接后, r1 是否失败
 *   T15: r1(dev1) PEER import r2(dev4) 先, 然后 r0(dev0) PEER import r2(dev4)
 *        → 验证: r1 先建立连接, r0 是否成功 (对照)
 *   T16: r0(dev0) DEFAULT import r2(dev4) 先, 然后 r1(dev1) PEER import r2(dev4)
 *        → 验证: DEFAULT 是否影响后续 PEER
 *   T17: r0(dev0) PEER import r2(dev2) 先 (同 socket), 然后 r1(dev1) PEER import r2(dev4) (跨 socket)
 *        → 验证: 同 socket import 是否影响跨 socket
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <mpi.h>
#include "acl/acl.h"

#define SLAB_SIZE   (16u * 1024u * 1024u)
#define IPC_KEY_LEN 65

static int32_t env_int(const char *n, int32_t d)
{
    const char *s = getenv(n);
    return s ? (int32_t)atoi(s) : d;
}

static const char *ret_str(int32_t r)
{
    return r == 0 ? "OK" : (r == 507899 ? "FAIL(507899)" : "FAIL(other)");
}

/* 交换 key: rank 2 是 exporter, rank 0 和 1 是 importers */
static void exchange_keys(int rank, char *mykey, char *key_from_2)
{
    if (rank == 2) {
        MPI_Send(mykey, IPC_KEY_LEN, MPI_CHAR, 0, 200, MPI_COMM_WORLD);
        MPI_Send(mykey, IPC_KEY_LEN, MPI_CHAR, 1, 200, MPI_COMM_WORLD);
    } else {
        MPI_Recv(key_from_2, IPC_KEY_LEN, MPI_CHAR, 2, 200, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    }
}

/* ================================================================
 *  T14: r0(dev0) PEER import r2(dev4) 先, 然后 r1(dev1) PEER import r2(dev4)
 * ================================================================ */
static void test14(int rank, int32_t dev)
{
    void *slab = NULL, *p = NULL;
    char mykey[IPC_KEY_LEN] = {0}, key_from_2[IPC_KEY_LEN] = {0};
    int exp_ok = 0;

    /* 所有 rank 建 slab */
    int32_t ret = aclrtMalloc(&slab, SLAB_SIZE, ACL_MEM_MALLOC_HUGE_FIRST_P2P);
    if (ret == ACL_ERROR_NONE) {
        ret = aclrtIpcMemGetExportKey(slab, SLAB_SIZE, mykey, IPC_KEY_LEN,
                                      ACL_RT_IPC_MEM_EXPORT_FLAG_DISABLE_PID_VALIDATION);
        if (ret == ACL_ERROR_NONE) exp_ok = 1;
    }
    printf("  r%d(dev%d): create slab %s\n", rank, dev, exp_ok ? "OK" : "FAIL");

    /* 交换 key */
    exchange_keys(rank, mykey, key_from_2);
    MPI_Barrier(MPI_COMM_WORLD);

    /* r0 先 PEER import r2 */
    if (rank == 0) {
        ret = aclrtIpcMemImportByKey(&p, key_from_2, ACL_RT_IPC_MEM_IMPORT_FLAG_ENABLE_PEER_ACCESS);
        printf("  r0(dev%d): PEER import r2(dev%d) → %s\n", dev, 4, ret_str(ret));
    }
    MPI_Barrier(MPI_COMM_WORLD);

    /* r1 再 PEER import r2 */
    if (rank == 1) {
        ret = aclrtIpcMemImportByKey(&p, key_from_2, ACL_RT_IPC_MEM_IMPORT_FLAG_ENABLE_PEER_ACCESS);
        printf("  r1(dev%d): PEER import r2(dev%d) → %s\n", dev, 4, ret_str(ret));
    }
    MPI_Barrier(MPI_COMM_WORLD);

    /* cleanup */
    if (rank == 0) { aclrtIpcMemClose(key_from_2); if (exp_ok) aclrtIpcMemClose(mykey); }
    if (rank == 1) { aclrtIpcMemClose(key_from_2); if (exp_ok) aclrtIpcMemClose(mykey); }
    if (rank == 2) { if (exp_ok) aclrtIpcMemClose(mykey); }
    MPI_Barrier(MPI_COMM_WORLD);
    if (slab) aclrtFree(slab);
    if (rank == 0) { aclrtDeviceDisablePeerAccess(4); aclrtResetDevice(dev); }
    if (rank == 1) { /* 如果 import 成功才 disable */ aclrtResetDevice(dev); }
    if (rank == 2) { aclrtResetDevice(dev); }
}

/* ================================================================
 *  T15: r1(dev1) PEER import r2(dev4) 先, 然后 r0(dev0) PEER import r2(dev4)
 * ================================================================ */
static void test15(int rank, int32_t dev)
{
    void *slab = NULL, *p = NULL;
    char mykey[IPC_KEY_LEN] = {0}, key_from_2[IPC_KEY_LEN] = {0};
    int exp_ok = 0;

    int32_t ret = aclrtMalloc(&slab, SLAB_SIZE, ACL_MEM_MALLOC_HUGE_FIRST_P2P);
    if (ret == ACL_ERROR_NONE) {
        ret = aclrtIpcMemGetExportKey(slab, SLAB_SIZE, mykey, IPC_KEY_LEN,
                                      ACL_RT_IPC_MEM_EXPORT_FLAG_DISABLE_PID_VALIDATION);
        if (ret == ACL_ERROR_NONE) exp_ok = 1;
    }
    printf("  r%d(dev%d): create slab %s\n", rank, dev, exp_ok ? "OK" : "FAIL");

    exchange_keys(rank, mykey, key_from_2);
    MPI_Barrier(MPI_COMM_WORLD);

    /* r1 先 PEER import r2 */
    if (rank == 1) {
        ret = aclrtIpcMemImportByKey(&p, key_from_2, ACL_RT_IPC_MEM_IMPORT_FLAG_ENABLE_PEER_ACCESS);
        printf("  r1(dev%d): PEER import r2(dev%d) → %s\n", dev, 4, ret_str(ret));
    }
    MPI_Barrier(MPI_COMM_WORLD);

    /* r0 再 PEER import r2 */
    if (rank == 0) {
        ret = aclrtIpcMemImportByKey(&p, key_from_2, ACL_RT_IPC_MEM_IMPORT_FLAG_ENABLE_PEER_ACCESS);
        printf("  r0(dev%d): PEER import r2(dev%d) → %s\n", dev, 4, ret_str(ret));
    }
    MPI_Barrier(MPI_COMM_WORLD);

    if (rank == 0) { aclrtIpcMemClose(key_from_2); if (exp_ok) aclrtIpcMemClose(mykey); }
    if (rank == 1) { aclrtIpcMemClose(key_from_2); if (exp_ok) aclrtIpcMemClose(mykey); }
    if (rank == 2) { if (exp_ok) aclrtIpcMemClose(mykey); }
    MPI_Barrier(MPI_COMM_WORLD);
    if (slab) aclrtFree(slab);
    if (rank == 0) { aclrtResetDevice(dev); }
    if (rank == 1) { aclrtDeviceDisablePeerAccess(4); aclrtResetDevice(dev); }
    if (rank == 2) { aclrtResetDevice(dev); }
}

/* ================================================================
 *  T16: r0(dev0) DEFAULT import r2(dev4) 先, 然后 r1(dev1) PEER import r2(dev4)
 * ================================================================ */
static void test16(int rank, int32_t dev)
{
    void *slab = NULL, *p = NULL;
    char mykey[IPC_KEY_LEN] = {0}, key_from_2[IPC_KEY_LEN] = {0};
    int exp_ok = 0;

    int32_t ret = aclrtMalloc(&slab, SLAB_SIZE, ACL_MEM_MALLOC_HUGE_FIRST_P2P);
    if (ret == ACL_ERROR_NONE) {
        ret = aclrtIpcMemGetExportKey(slab, SLAB_SIZE, mykey, IPC_KEY_LEN,
                                      ACL_RT_IPC_MEM_EXPORT_FLAG_DISABLE_PID_VALIDATION);
        if (ret == ACL_ERROR_NONE) exp_ok = 1;
    }
    printf("  r%d(dev%d): create slab %s\n", rank, dev, exp_ok ? "OK" : "FAIL");

    exchange_keys(rank, mykey, key_from_2);
    MPI_Barrier(MPI_COMM_WORLD);

    /* r0 先 DEFAULT import r2 */
    if (rank == 0) {
        ret = aclrtIpcMemImportByKey(&p, key_from_2, ACL_RT_IPC_MEM_IMPORT_FLAG_DEFAULT);
        printf("  r0(dev%d): DEFAULT import r2(dev%d) → %s\n", dev, 4, ret_str(ret));
    }
    MPI_Barrier(MPI_COMM_WORLD);

    /* r1 再 PEER import r2 */
    if (rank == 1) {
        ret = aclrtIpcMemImportByKey(&p, key_from_2, ACL_RT_IPC_MEM_IMPORT_FLAG_ENABLE_PEER_ACCESS);
        printf("  r1(dev%d): PEER import r2(dev%d) → %s\n", dev, 4, ret_str(ret));
    }
    MPI_Barrier(MPI_COMM_WORLD);

    if (rank == 0) { aclrtIpcMemClose(key_from_2); if (exp_ok) aclrtIpcMemClose(mykey); }
    if (rank == 1) { aclrtIpcMemClose(key_from_2); if (exp_ok) aclrtIpcMemClose(mykey); }
    if (rank == 2) { if (exp_ok) aclrtIpcMemClose(mykey); }
    MPI_Barrier(MPI_COMM_WORLD);
    if (slab) aclrtFree(slab);
    if (rank == 0) { aclrtResetDevice(dev); }
    if (rank == 1) { aclrtDeviceDisablePeerAccess(4); aclrtResetDevice(dev); }
    if (rank == 2) { aclrtResetDevice(dev); }
}

/* ================================================================
 *  T17: r0(dev0) PEER import r2(dev2) 先 (同 socket), 然后 r1(dev1) PEER import r2(dev4) (跨 socket)
 *  需要 4 rank: r0(dev0), r1(dev1), r2(dev2), r3(dev4)
 *  简化: 用 3 rank, r2 同时 export dev2 和 dev4 的 key
 * ================================================================ */
static void test17(int rank, int32_t dev)
{
    void *slab = NULL, *p = NULL;
    char mykey[IPC_KEY_LEN] = {0}, key_from_2[IPC_KEY_LEN] = {0};
    int exp_ok = 0;

    int32_t ret = aclrtMalloc(&slab, SLAB_SIZE, ACL_MEM_MALLOC_HUGE_FIRST_P2P);
    if (ret == ACL_ERROR_NONE) {
        ret = aclrtIpcMemGetExportKey(slab, SLAB_SIZE, mykey, IPC_KEY_LEN,
                                      ACL_RT_IPC_MEM_EXPORT_FLAG_DISABLE_PID_VALIDATION);
        if (ret == ACL_ERROR_NONE) exp_ok = 1;
    }
    printf("  r%d(dev%d): create slab %s\n", rank, dev, exp_ok ? "OK" : "FAIL");

    exchange_keys(rank, mykey, key_from_2);
    MPI_Barrier(MPI_COMM_WORLD);

    /* r0 先 PEER import r2 (dev2, 同 socket) */
    if (rank == 0) {
        ret = aclrtIpcMemImportByKey(&p, key_from_2, ACL_RT_IPC_MEM_IMPORT_FLAG_ENABLE_PEER_ACCESS);
        printf("  r0(dev%d): PEER import r2(dev%d) [同socket] → %s\n", dev, 2, ret_str(ret));
    }
    MPI_Barrier(MPI_COMM_WORLD);

    /* r1 再 PEER import r2 (dev4, 跨 socket) */
    if (rank == 1) {
        ret = aclrtIpcMemImportByKey(&p, key_from_2, ACL_RT_IPC_MEM_IMPORT_FLAG_ENABLE_PEER_ACCESS);
        printf("  r1(dev%d): PEER import r2(dev%d) [跨socket] → %s\n", dev, 4, ret_str(ret));
    }
    MPI_Barrier(MPI_COMM_WORLD);

    if (rank == 0) { aclrtIpcMemClose(key_from_2); if (exp_ok) aclrtIpcMemClose(mykey); }
    if (rank == 1) { aclrtIpcMemClose(key_from_2); if (exp_ok) aclrtIpcMemClose(mykey); }
    if (rank == 2) { if (exp_ok) aclrtIpcMemClose(mykey); }
    MPI_Barrier(MPI_COMM_WORLD);
    if (slab) aclrtFree(slab);
    if (rank == 0) { aclrtDeviceDisablePeerAccess(2); aclrtResetDevice(dev); }
    if (rank == 1) { aclrtDeviceDisablePeerAccess(4); aclrtResetDevice(dev); }
    if (rank == 2) { aclrtResetDevice(dev); }
}

int main(int argc, char *argv[])
{
    MPI_Init(&argc, &argv);
    int rank, nproc;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &nproc);
    if (nproc != 3) {
        if (rank == 0) printf("need -np 3, got %d\n", nproc);
        MPI_Finalize();
        return 1;
    }

    int tnum = env_int("T", 14);

    /* 设备分配:
     * T14-T16: r0=dev0, r1=dev1, r2=dev4
     * T17: r0=dev0, r1=dev1, r2=dev2 (但测试 r1 import dev4, 需要调整)
     */
    int32_t dev;
    if (tnum <= 16) {
        /* r0=dev0, r1=dev1, r2=dev4 */
        int32_t devs[] = {0, 1, 4};
        dev = devs[rank];
    } else {
        /* T17: r0=dev0, r1=dev1, r2=dev2 */
        int32_t devs[] = {0, 1, 2};
        dev = devs[rank];
    }

    setvbuf(stdout, NULL, _IONBF, 0);
    if (rank == 0)
        printf("=== T%d (3-rank): r0(dev0) r1(dev1) r2(dev%d) ===\n", tnum, tnum <= 16 ? 4 : 2);

    aclError ret = aclInit(NULL);
    if (ret != 0) {
        printf("r%d: aclInit FAIL ret=%d\n", rank, ret);
        MPI_Finalize();
        return 1;
    }
    ret = aclrtSetDevice(dev);
    if (ret != 0) {
        printf("r%d: SetDevice(%d) FAIL ret=%d\n", rank, dev, ret);
        aclFinalize();
        MPI_Finalize();
        return 1;
    }

    switch (tnum) {
    case 14: test14(rank, dev); break;
    case 15: test15(rank, dev); break;
    case 16: test16(rank, dev); break;
    case 17: test17(rank, dev); break;
    default:
        if (rank == 0) printf("unknown T=%d\n", tnum);
        break;
    }

    if (rank == 0)
        printf("=== T%d done ===\n\n", tnum);

    aclrtResetDevice(dev);
    aclFinalize();
    MPI_Finalize();
    return 0;
}
