/*
 * minitest_final.c — 最终验证测试
 * 修正 cleanup 逻辑，确保每次测试独立
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

static int32_t do_import(int rank, int target, char keys[N][IPC_KEY_LEN], int *imp_ok)
{
    int die = is_die_pair(rank, target);
    uint32_t flag = die ? ACL_RT_IPC_MEM_IMPORT_FLAG_DEFAULT
                        : ACL_RT_IPC_MEM_IMPORT_FLAG_ENABLE_PEER_ACCESS;
    void *pv = NULL;
    int32_t ret = aclrtIpcMemImportByKey(&pv, keys[target], flag);
    imp_ok[target] = (ret == 0);
    printf("    r%d import p%-2d: %s\n", rank, target, ret_str(ret));
    return ret;
}

static void test(int rank, int32_t dev, int tnum)
{
    void *slab = NULL;
    char mykey[IPC_KEY_LEN] = {0}, keys[N][IPC_KEY_LEN];
    int imp_ok[N] = {0};

    int32_t ret = aclrtMalloc(&slab, SLAB_SIZE, ACL_MEM_MALLOC_HUGE_FIRST_P2P);
    if (ret == ACL_ERROR_NONE)
        aclrtIpcMemGetExportKey(slab, SLAB_SIZE, mykey, IPC_KEY_LEN,
                                ACL_RT_IPC_MEM_EXPORT_FLAG_DISABLE_PID_VALIDATION);

    MPI_Allgather(mykey, IPC_KEY_LEN, MPI_CHAR,
                  &keys[0][0], IPC_KEY_LEN, MPI_CHAR, MPI_COMM_WORLD);
    MPI_Barrier(MPI_COMM_WORLD);

    /* r0 import 全部 7 个 */
    if (rank == 0) {
        printf("  r0: import all 7...\n");
        for (int p = 1; p < N; p++) {
            do_import(0, p, keys, imp_ok);
        }
    }
    MPI_Barrier(MPI_COMM_WORLD);

    /* r1 按测试序列 import */
    if (rank == 1) {
        printf("  r1: T%d...\n", tnum);
        switch (tnum) {
        case 36: /* p0, p4, p5 (3个) */
            do_import(1, 0, keys, imp_ok);
            do_import(1, 4, keys, imp_ok);
            do_import(1, 5, keys, imp_ok);
            break;
        case 37: /* p0, p4, p5, p6 (4个) */
            do_import(1, 0, keys, imp_ok);
            do_import(1, 4, keys, imp_ok);
            do_import(1, 5, keys, imp_ok);
            do_import(1, 6, keys, imp_ok);
            break;
        case 38: /* p4, p5, p6 (3个,无die pair) */
            do_import(1, 4, keys, imp_ok);
            do_import(1, 5, keys, imp_ok);
            do_import(1, 6, keys, imp_ok);
            break;
        case 39: /* p4, p5, p6, p7 (4个,无die pair) */
            do_import(1, 4, keys, imp_ok);
            do_import(1, 5, keys, imp_ok);
            do_import(1, 6, keys, imp_ok);
            do_import(1, 7, keys, imp_ok);
            break;
        }
    }
    MPI_Barrier(MPI_COMM_WORLD);

    /* cleanup */
    for (int p = 0; p < N; p++) {
        if (p != rank && imp_ok[p]) {
            aclrtIpcMemClose(keys[p]);
        }
    }
    aclrtIpcMemClose(mykey);
    MPI_Barrier(MPI_COMM_WORLD);
    if (slab) aclrtFree(slab);
    for (int p = 0; p < N; p++) {
        if (p != rank && imp_ok[p] && !is_die_pair(rank, p)) {
            aclrtDeviceDisablePeerAccess(p);
        }
    }
    aclrtResetDevice(dev);
}

int main(int argc, char *argv[])
{
    MPI_Init(&argc, &argv);
    int rank, nproc;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &nproc);
    if (nproc != N) { if (!rank) printf("need -np %d\n", N); MPI_Finalize(); return 1; }

    int tnum = env_int("T", 36);
    int32_t dev = rank;

    setvbuf(stdout, NULL, _IONBF, 0);
    if (!rank) printf("=== T%d ===\n", tnum);

    aclInit(NULL);
    aclrtSetDevice(dev);
    test(rank, dev, tnum);
    if (!rank) printf("=== T%d done ===\n", tnum);

    aclrtResetDevice(dev);
    aclFinalize();
    MPI_Finalize();
    return 0;
}
