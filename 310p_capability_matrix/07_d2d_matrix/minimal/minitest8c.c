/*
 * minitest8c.c — 精确隔离: r1 import p3 是否导致 p4 失败
 *
 * T25: r0 import 全部 7, r1 import p0, p2, p3, p4
 *      → 验证: 在 p4 之前加 p3 是否导致失败
 * T26: r0 import 全部 7, r1 import p0, p3, p4
 *      → 验证: 跳过 p2, 只加 p3 是否导致失败
 * T27: r0 import 全部 7, r1 import p0, p2, p3, p5
 *      → 验证: p3 是否影响所有后续跨 socket import
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

static void do_import(int rank, int target, char keys[N][IPC_KEY_LEN], void **ipc_va, int *imp_ok)
{
    int die = is_die_pair(rank, target);
    uint32_t flag = die ? ACL_RT_IPC_MEM_IMPORT_FLAG_DEFAULT
                        : ACL_RT_IPC_MEM_IMPORT_FLAG_ENABLE_PEER_ACCESS;
    void *pv = NULL;
    int32_t ret = aclrtIpcMemImportByKey(&pv, keys[target], flag);
    imp_ok[target] = (ret == 0);
    if (imp_ok[target]) ipc_va[target] = pv;
    printf("    r%d import p%d (%s): %s\n", rank, target, die ? "die/DEFAULT" : "cross/PEER", ret_str(ret));
}

static void common_setup(int rank, int32_t dev, void **slab, char mykey[IPC_KEY_LEN], char keys[N][IPC_KEY_LEN])
{
    int32_t ret = aclrtMalloc(slab, SLAB_SIZE, ACL_MEM_MALLOC_HUGE_FIRST_P2P);
    if (ret == ACL_ERROR_NONE) {
        ret = aclrtIpcMemGetExportKey(*slab, SLAB_SIZE, mykey, IPC_KEY_LEN,
                                      ACL_RT_IPC_MEM_EXPORT_FLAG_DISABLE_PID_VALIDATION);
    }
    printf("  r%d(dev%d): create slab %s\n", rank, dev, (ret == 0) ? "OK" : "FAIL");
    MPI_Allgather(mykey, IPC_KEY_LEN, MPI_CHAR,
                  &keys[0][0], IPC_KEY_LEN, MPI_CHAR, MPI_COMM_WORLD);
    MPI_Barrier(MPI_COMM_WORLD);
}

static void common_cleanup(int rank, int32_t dev, void *slab, char mykey[IPC_KEY_LEN],
                           char keys[N][IPC_KEY_LEN], int imp_ok[N])
{
    for (int p = 0; p < N; p++) {
        if (p != rank && imp_ok[p]) aclrtIpcMemClose(keys[p]);
    }
    aclrtIpcMemClose(mykey);
    MPI_Barrier(MPI_COMM_WORLD);
    if (slab) aclrtFree(slab);
    for (int p = 0; p < N; p++) {
        if (p != rank && imp_ok[p] && !is_die_pair(rank, p)) aclrtDeviceDisablePeerAccess(p);
    }
    aclrtResetDevice(dev);
}

static void test25(int rank, int32_t dev)
{
    void *slab = NULL;
    char mykey[IPC_KEY_LEN] = {0}, keys[N][IPC_KEY_LEN];
    void *ipc_va[N] = {NULL};
    int imp_ok[N] = {0};

    common_setup(rank, dev, &slab, mykey, keys);

    if (rank == 0) {
        printf("  r0: importing all 7 peers...\n");
        for (int p = 0; p < N; p++) { if (p != rank) do_import(rank, p, keys, ipc_va, imp_ok); }
    }
    MPI_Barrier(MPI_COMM_WORLD);

    if (rank == 1) {
        printf("  r1: importing p0, p2, p3, p4...\n");
        do_import(rank, 0, keys, ipc_va, imp_ok);
        do_import(rank, 2, keys, ipc_va, imp_ok);
        do_import(rank, 3, keys, ipc_va, imp_ok);
        do_import(rank, 4, keys, ipc_va, imp_ok);
    }
    MPI_Barrier(MPI_COMM_WORLD);
    common_cleanup(rank, dev, slab, mykey, keys, imp_ok);
}

static void test26(int rank, int32_t dev)
{
    void *slab = NULL;
    char mykey[IPC_KEY_LEN] = {0}, keys[N][IPC_KEY_LEN];
    void *ipc_va[N] = {NULL};
    int imp_ok[N] = {0};

    common_setup(rank, dev, &slab, mykey, keys);

    if (rank == 0) {
        printf("  r0: importing all 7 peers...\n");
        for (int p = 0; p < N; p++) { if (p != rank) do_import(rank, p, keys, ipc_va, imp_ok); }
    }
    MPI_Barrier(MPI_COMM_WORLD);

    if (rank == 1) {
        printf("  r1: importing p0, p3, p4...\n");
        do_import(rank, 0, keys, ipc_va, imp_ok);
        do_import(rank, 3, keys, ipc_va, imp_ok);
        do_import(rank, 4, keys, ipc_va, imp_ok);
    }
    MPI_Barrier(MPI_COMM_WORLD);
    common_cleanup(rank, dev, slab, mykey, keys, imp_ok);
}

static void test27(int rank, int32_t dev)
{
    void *slab = NULL;
    char mykey[IPC_KEY_LEN] = {0}, keys[N][IPC_KEY_LEN];
    void *ipc_va[N] = {NULL};
    int imp_ok[N] = {0};

    common_setup(rank, dev, &slab, mykey, keys);

    if (rank == 0) {
        printf("  r0: importing all 7 peers...\n");
        for (int p = 0; p < N; p++) { if (p != rank) do_import(rank, p, keys, ipc_va, imp_ok); }
    }
    MPI_Barrier(MPI_COMM_WORLD);

    if (rank == 1) {
        printf("  r1: importing p0, p2, p3, p5...\n");
        do_import(rank, 0, keys, ipc_va, imp_ok);
        do_import(rank, 2, keys, ipc_va, imp_ok);
        do_import(rank, 3, keys, ipc_va, imp_ok);
        do_import(rank, 5, keys, ipc_va, imp_ok);
    }
    MPI_Barrier(MPI_COMM_WORLD);
    common_cleanup(rank, dev, slab, mykey, keys, imp_ok);
}

int main(int argc, char *argv[])
{
    MPI_Init(&argc, &argv);
    int rank, nproc;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &nproc);
    if (nproc != N) {
        if (rank == 0) printf("need -np %d, got %d\n", N, nproc);
        MPI_Finalize();
        return 1;
    }

    int tnum = env_int("T", 25);
    int32_t dev = rank;

    setvbuf(stdout, NULL, _IONBF, 0);
    if (rank == 0) printf("=== T%d: 精确隔离 p3 影响 ===\n", tnum);

    aclError ret = aclInit(NULL);
    if (ret != 0) { printf("r%d: aclInit FAIL\n", rank); MPI_Finalize(); return 1; }
    ret = aclrtSetDevice(dev);
    if (ret != 0) { printf("r%d: SetDevice(%d) FAIL\n", rank, dev); aclFinalize(); MPI_Finalize(); return 1; }

    switch (tnum) {
    case 25: test25(rank, dev); break;
    case 26: test26(rank, dev); break;
    case 27: test27(rank, dev); break;
    default: if (rank == 0) printf("unknown T=%d\n", tnum); break;
    }

    if (rank == 0) printf("=== T%d done ===\n\n", tnum);
    aclrtResetDevice(dev);
    aclFinalize();
    MPI_Finalize();
    return 0;
}
