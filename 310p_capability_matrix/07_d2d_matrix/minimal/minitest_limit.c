/*
 * minitest_limit.c — 验证 r1 的 import 数量上限
 *
 * 假设: r1 最多只能成功 import 3 个 peer (不含 die pair DEFAULT)
 *
 * T32: r1 import p0, p4, p5 (3 个) → 应该全成功
 * T33: r1 import p0, p4, p5, p6 (4 个) → p6 应该失败
 * T34: r1 import p0, p2, p4 (3 个, 含同 socket) → 应该全成功
 * T35: r1 import p4, p5, p6, p7 (4 个, 全跨 socket) → p7 应该失败
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
    printf("    r%d import p%-2d: %s\n", rank, target, ret_str(ret));
    return ret;
}

static void common(int rank, int32_t dev, int tnum)
{
    void *slab = NULL;
    char mykey[IPC_KEY_LEN] = {0}, keys[N][IPC_KEY_LEN];

    int32_t ret = aclrtMalloc(&slab, SLAB_SIZE, ACL_MEM_MALLOC_HUGE_FIRST_P2P);
    if (ret == ACL_ERROR_NONE)
        aclrtIpcMemGetExportKey(slab, SLAB_SIZE, mykey, IPC_KEY_LEN,
                                ACL_RT_IPC_MEM_EXPORT_FLAG_DISABLE_PID_VALIDATION);

    MPI_Allgather(mykey, IPC_KEY_LEN, MPI_CHAR,
                  &keys[0][0], IPC_KEY_LEN, MPI_CHAR, MPI_COMM_WORLD);
    MPI_Barrier(MPI_COMM_WORLD);

    if (rank == 0) {
        for (int p = 0; p < N; p++) {
            if (p != rank) do_import(rank, p, keys);
        }
    }
    MPI_Barrier(MPI_COMM_WORLD);

    if (rank == 1) {
        printf("  r1: T%d sequence...\n", tnum);
        switch (tnum) {
        case 32:
            do_import(1, 0, keys);
            do_import(1, 4, keys);
            do_import(1, 5, keys);
            break;
        case 33:
            do_import(1, 0, keys);
            do_import(1, 4, keys);
            do_import(1, 5, keys);
            do_import(1, 6, keys);
            break;
        case 34:
            do_import(1, 0, keys);
            do_import(1, 2, keys);
            do_import(1, 4, keys);
            break;
        case 35:
            do_import(1, 4, keys);
            do_import(1, 5, keys);
            do_import(1, 6, keys);
            do_import(1, 7, keys);
            break;
        }
    }
    MPI_Barrier(MPI_COMM_WORLD);

    if (slab) aclrtFree(slab);
    aclrtIpcMemClose(mykey);
    for (int p = 0; p < N; p++)
        if (p != rank) aclrtIpcMemClose(keys[p]);
    aclrtResetDevice(dev);
}

int main(int argc, char *argv[])
{
    MPI_Init(&argc, &argv);
    int rank, nproc;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &nproc);
    if (nproc != N) { if (!rank) printf("need -np %d\n", N); MPI_Finalize(); return 1; }

    int tnum = env_int("T", 32);
    int32_t dev = rank;

    setvbuf(stdout, NULL, _IONBF, 0);
    if (!rank) printf("=== T%d: import 数量上限测试 ===\n", tnum);

    aclInit(NULL);
    aclrtSetDevice(dev);
    common(rank, dev, tnum);
    if (!rank) printf("=== T%d done ===\n", tnum);

    aclrtResetDevice(dev);
    aclFinalize();
    MPI_Finalize();
    return 0;
}
