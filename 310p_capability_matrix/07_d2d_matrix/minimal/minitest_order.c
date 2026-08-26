/*
 * minitest_order.c — 验证 import 顺序是否影响结果
 *
 * 假设: 先 import 跨 socket (外面), 再 import 同 socket 跨卡 (里面) → 不触发
 *
 * T28: r0 import 全部 7, r1 import p0, p4, p5, p6, p7, p2, p3
 *      → 先跨 socket, 后同 socket 跨卡
 * T29: r0 import 全部 7, r1 import p0, p4, p2, p3, p5
 *      → 跨 socket 穿插在同 socket 之间
 * T30: r0 import 全部 7, r1 import p0, p4, p2, p5, p3
 *      → 每 import 一个跨 socket 就 import 一个同 socket
 * T31: r0 import 全部 7, r1 import p0, p2, p4, p3, p5
 *      → 07 原序的变体: p2 后紧跟 p4 再 p3
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <mpi.h>
#include "acl/acl.h"

#define N           8
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

static int is_die_pair(int i, int j)
{
    return i != j && (i / 2) == (j / 2);
}

static int32_t do_import(int rank, int target, char keys[N][IPC_KEY_LEN])
{
    int die = is_die_pair(rank, target);
    uint32_t flag = die ? ACL_RT_IPC_MEM_IMPORT_FLAG_DEFAULT
                        : ACL_RT_IPC_MEM_IMPORT_FLAG_ENABLE_PEER_ACCESS;
    void *pv = NULL;
    int32_t ret = aclrtIpcMemImportByKey(&pv, keys[target], flag);
    const char *type = die ? "die/DEFAULT" : (target >= 4 ? "cross-sock/PEER" : "same-sock/PEER");
    printf("    r%d import p%-2d (%-15s): %s\n", rank, target, type, ret_str(ret));
    return ret;
}

static void common(int rank, int32_t dev, int tnum)
{
    void *slab = NULL;
    char mykey[IPC_KEY_LEN] = {0}, keys[N][IPC_KEY_LEN];
    int imp_ok[N] = {0};

    int32_t ret = aclrtMalloc(&slab, SLAB_SIZE, ACL_MEM_MALLOC_HUGE_FIRST_P2P);
    if (ret == ACL_ERROR_NONE)
        aclrtIpcMemGetExportKey(slab, SLAB_SIZE, mykey, IPC_KEY_LEN,
                                ACL_RT_IPC_MEM_EXPORT_FLAG_DISABLE_PID_VALIDATION);
    printf("  r%d(dev%d): slab %s\n", rank, dev, (ret == 0) ? "OK" : "FAIL");

    MPI_Allgather(mykey, IPC_KEY_LEN, MPI_CHAR,
                  &keys[0][0], IPC_KEY_LEN, MPI_CHAR, MPI_COMM_WORLD);
    MPI_Barrier(MPI_COMM_WORLD);

    if (rank == 0) {
        printf("  r0: import all 7...\n");
        for (int p = 0; p < N; p++) {
            if (p != rank) imp_ok[p] = (do_import(rank, p, keys) == 0);
        }
    }
    MPI_Barrier(MPI_COMM_WORLD);

    if (rank == 1) {
        printf("  r1: import sequence for T%d...\n", tnum);
        switch (tnum) {
        case 28:
            printf("  [先跨 socket, 后同 socket 跨卡]\n");
            imp_ok[0] = (do_import(1, 0, keys) == 0);
            imp_ok[4] = (do_import(1, 4, keys) == 0);
            imp_ok[5] = (do_import(1, 5, keys) == 0);
            imp_ok[6] = (do_import(1, 6, keys) == 0);
            imp_ok[7] = (do_import(1, 7, keys) == 0);
            imp_ok[2] = (do_import(1, 2, keys) == 0);
            imp_ok[3] = (do_import(1, 3, keys) == 0);
            break;
        case 29:
            printf("  [跨 socket 穿插在同 socket 之间]\n");
            imp_ok[0] = (do_import(1, 0, keys) == 0);
            imp_ok[4] = (do_import(1, 4, keys) == 0);
            imp_ok[2] = (do_import(1, 2, keys) == 0);
            imp_ok[3] = (do_import(1, 3, keys) == 0);
            imp_ok[5] = (do_import(1, 5, keys) == 0);
            break;
        case 30:
            printf("  [每个跨 socket 后跟一个同 socket]\n");
            imp_ok[0] = (do_import(1, 0, keys) == 0);
            imp_ok[4] = (do_import(1, 4, keys) == 0);
            imp_ok[2] = (do_import(1, 2, keys) == 0);
            imp_ok[5] = (do_import(1, 5, keys) == 0);
            imp_ok[3] = (do_import(1, 3, keys) == 0);
            break;
        case 31:
            printf("  [p2 后紧跟 p4 再 p3]\n");
            imp_ok[0] = (do_import(1, 0, keys) == 0);
            imp_ok[2] = (do_import(1, 2, keys) == 0);
            imp_ok[4] = (do_import(1, 4, keys) == 0);
            imp_ok[3] = (do_import(1, 3, keys) == 0);
            imp_ok[5] = (do_import(1, 5, keys) == 0);
            break;
        }
    }
    MPI_Barrier(MPI_COMM_WORLD);

    for (int p = 0; p < N; p++)
        if (p != rank && imp_ok[p]) aclrtIpcMemClose(keys[p]);
    aclrtIpcMemClose(mykey);
    MPI_Barrier(MPI_COMM_WORLD);
    if (slab) aclrtFree(slab);
    for (int p = 0; p < N; p++)
        if (p != rank && imp_ok[p] && !is_die_pair(rank, p)) aclrtDeviceDisablePeerAccess(p);
    aclrtResetDevice(dev);
}

int main(int argc, char *argv[])
{
    MPI_Init(&argc, &argv);
    int rank, nproc;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &nproc);
    if (nproc != N) { if (!rank) printf("need -np %d\n", N); MPI_Finalize(); return 1; }

    int tnum = env_int("T", 28);
    int32_t dev = rank;

    setvbuf(stdout, NULL, _IONBF, 0);
    if (!rank) printf("=== T%d: import 顺序测试 ===\n", tnum);

    aclInit(NULL);
    aclrtSetDevice(dev);
    common(rank, dev, tnum);
    if (!rank) printf("=== T%d done ===\n\n", tnum);

    aclrtResetDevice(dev);
    aclFinalize();
    MPI_Finalize();
    return 0;
}
