/*
 * minitest6.c — 6-rank 测试, 验证跨 socket PEER 连接数量限制
 *
 * 假设: 每个 socket 的跨 socket PEER 连接槽位有限
 *
 * 设备布局:
 *   Socket 0: card0 (dev0,1), card1 (dev2,3)
 *   Socket 1: card2 (dev4,5), card3 (dev6,7)
 *
 * T18: r0(dev0) PEER import dev4,5,6,7 (4 个跨 socket), 然后 r1(dev1) PEER import dev4
 *      → 验证: r0 用完跨 socket 槽位后, r1 是否失败
 * T19: r0(dev0) PEER import dev2,3 (2 个同 socket), 然后 r1(dev1) PEER import dev4 (跨 socket)
 *      → 验证: 同 socket import 是否影响跨 socket
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

/* ================================================================
 *  T18: r0(dev0) PEER import dev4,5,6,7, 然后 r1(dev1) PEER import dev4
 *  6 ranks: r0=dev0, r1=dev1, r2=dev4, r3=dev5, r4=dev6, r5=dev7
 * ================================================================ */
static void test18(int rank, int32_t dev)
{
    void *slab = NULL, *p = NULL;
    char mykey[IPC_KEY_LEN] = {0};
    char keys_from_exporters[4][IPC_KEY_LEN];  /* keys from r2,r3,r4,r5 (dev4,5,6,7) */
    int exp_ok = 0;

    /* 所有 rank 建 slab */
    int32_t ret = aclrtMalloc(&slab, SLAB_SIZE, ACL_MEM_MALLOC_HUGE_FIRST_P2P);
    if (ret == ACL_ERROR_NONE) {
        ret = aclrtIpcMemGetExportKey(slab, SLAB_SIZE, mykey, IPC_KEY_LEN,
                                      ACL_RT_IPC_MEM_EXPORT_FLAG_DISABLE_PID_VALIDATION);
        if (ret == ACL_ERROR_NONE) exp_ok = 1;
    }
    printf("  r%d(dev%d): create slab %s\n", rank, dev, exp_ok ? "OK" : "FAIL");

    /* 交换 key: exporters (r2-r5) 发送 key 给 r0 和 r1 */
    if (rank >= 2) {
        MPI_Send(mykey, IPC_KEY_LEN, MPI_CHAR, 0, 200, MPI_COMM_WORLD);
        MPI_Send(mykey, IPC_KEY_LEN, MPI_CHAR, 1, 200, MPI_COMM_WORLD);
    }
    if (rank <= 1) {
        for (int i = 0; i < 4; i++) {
            MPI_Recv(keys_from_exporters[i], IPC_KEY_LEN, MPI_CHAR, 2 + i, 200, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        }
    }
    MPI_Barrier(MPI_COMM_WORLD);

    /* r0 PEER import dev4,5,6,7 (4 个跨 socket) */
    if (rank == 0) {
        for (int i = 0; i < 4; i++) {
            int32_t target_dev = 4 + i;
            ret = aclrtIpcMemImportByKey(&p, keys_from_exporters[i], ACL_RT_IPC_MEM_IMPORT_FLAG_ENABLE_PEER_ACCESS);
            printf("  r0(dev%d): PEER import dev%d [跨socket] → %s\n", dev, target_dev, ret_str(ret));
        }
    }
    MPI_Barrier(MPI_COMM_WORLD);

    /* r1 PEER import dev4 (1 个跨 socket) */
    if (rank == 1) {
        ret = aclrtIpcMemImportByKey(&p, keys_from_exporters[0], ACL_RT_IPC_MEM_IMPORT_FLAG_ENABLE_PEER_ACCESS);
        printf("  r1(dev%d): PEER import dev%d [跨socket] → %s\n", dev, 4, ret_str(ret));
    }
    MPI_Barrier(MPI_COMM_WORLD);

    /* cleanup */
    if (rank == 0) {
        for (int i = 0; i < 4; i++) aclrtIpcMemClose(keys_from_exporters[i]);
        if (exp_ok) aclrtIpcMemClose(mykey);
        for (int i = 4; i < 8; i++) aclrtDeviceDisablePeerAccess(i);
    }
    if (rank == 1) {
        for (int i = 0; i < 4; i++) aclrtIpcMemClose(keys_from_exporters[i]);
        if (exp_ok) aclrtIpcMemClose(mykey);
        /* 如果 import 成功才 disable */
    }
    if (rank >= 2) {
        if (exp_ok) aclrtIpcMemClose(mykey);
    }
    MPI_Barrier(MPI_COMM_WORLD);
    if (slab) aclrtFree(slab);
    aclrtResetDevice(dev);
}

/* ================================================================
 *  T19: r0(dev0) PEER import dev2,3 (同 socket), 然后 r1(dev1) PEER import dev4 (跨 socket)
 *  5 ranks: r0=dev0, r1=dev1, r2=dev2, r3=dev3, r4=dev4
 * ================================================================ */
static void test19(int rank, int32_t dev)
{
    void *slab = NULL, *p = NULL;
    char mykey[IPC_KEY_LEN] = {0};
    char keys_from_exporters[3][IPC_KEY_LEN];  /* keys from r2,r3,r4 (dev2,3,4) */
    int exp_ok = 0;

    int32_t ret = aclrtMalloc(&slab, SLAB_SIZE, ACL_MEM_MALLOC_HUGE_FIRST_P2P);
    if (ret == ACL_ERROR_NONE) {
        ret = aclrtIpcMemGetExportKey(slab, SLAB_SIZE, mykey, IPC_KEY_LEN,
                                      ACL_RT_IPC_MEM_EXPORT_FLAG_DISABLE_PID_VALIDATION);
        if (ret == ACL_ERROR_NONE) exp_ok = 1;
    }
    printf("  r%d(dev%d): create slab %s\n", rank, dev, exp_ok ? "OK" : "FAIL");

    /* 交换 key */
    if (rank >= 2) {
        MPI_Send(mykey, IPC_KEY_LEN, MPI_CHAR, 0, 200, MPI_COMM_WORLD);
        MPI_Send(mykey, IPC_KEY_LEN, MPI_CHAR, 1, 200, MPI_COMM_WORLD);
    }
    if (rank <= 1) {
        for (int i = 0; i < 3; i++) {
            MPI_Recv(keys_from_exporters[i], IPC_KEY_LEN, MPI_CHAR, 2 + i, 200, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        }
    }
    MPI_Barrier(MPI_COMM_WORLD);

    /* r0 PEER import dev2,3 (2 个同 socket) */
    if (rank == 0) {
        for (int i = 0; i < 2; i++) {
            int32_t target_dev = 2 + i;
            ret = aclrtIpcMemImportByKey(&p, keys_from_exporters[i], ACL_RT_IPC_MEM_IMPORT_FLAG_ENABLE_PEER_ACCESS);
            printf("  r0(dev%d): PEER import dev%d [同socket] → %s\n", dev, target_dev, ret_str(ret));
        }
    }
    MPI_Barrier(MPI_COMM_WORLD);

    /* r1 PEER import dev4 (1 个跨 socket) */
    if (rank == 1) {
        ret = aclrtIpcMemImportByKey(&p, keys_from_exporters[2], ACL_RT_IPC_MEM_IMPORT_FLAG_ENABLE_PEER_ACCESS);
        printf("  r1(dev%d): PEER import dev%d [跨socket] → %s\n", dev, 4, ret_str(ret));
    }
    MPI_Barrier(MPI_COMM_WORLD);

    /* cleanup */
    if (rank == 0) {
        for (int i = 0; i < 3; i++) aclrtIpcMemClose(keys_from_exporters[i]);
        if (exp_ok) aclrtIpcMemClose(mykey);
        aclrtDeviceDisablePeerAccess(2);
        aclrtDeviceDisablePeerAccess(3);
    }
    if (rank == 1) {
        for (int i = 0; i < 3; i++) aclrtIpcMemClose(keys_from_exporters[i]);
        if (exp_ok) aclrtIpcMemClose(mykey);
    }
    if (rank >= 2) {
        if (exp_ok) aclrtIpcMemClose(mykey);
    }
    MPI_Barrier(MPI_COMM_WORLD);
    if (slab) aclrtFree(slab);
    aclrtResetDevice(dev);
}

int main(int argc, char *argv[])
{
    MPI_Init(&argc, &argv);
    int rank, nproc;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &nproc);

    int tnum = env_int("T", 18);
    int expected_nproc = (tnum == 18) ? 6 : 5;

    if (nproc != expected_nproc) {
        if (rank == 0) printf("T%d need -np %d, got %d\n", tnum, expected_nproc, nproc);
        MPI_Finalize();
        return 1;
    }

    /* 设备分配 */
    int32_t dev;
    if (tnum == 18) {
        /* r0=dev0, r1=dev1, r2=dev4, r3=dev5, r4=dev6, r5=dev7 */
        int32_t devs[] = {0, 1, 4, 5, 6, 7};
        dev = devs[rank];
    } else {
        /* T19: r0=dev0, r1=dev1, r2=dev2, r3=dev3, r4=dev4 */
        int32_t devs[] = {0, 1, 2, 3, 4};
        dev = devs[rank];
    }

    setvbuf(stdout, NULL, _IONBF, 0);
    if (rank == 0)
        printf("=== T%d (%d-rank) ===\n", tnum, expected_nproc);

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
    case 18: test18(rank, dev); break;
    case 19: test19(rank, dev); break;
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
