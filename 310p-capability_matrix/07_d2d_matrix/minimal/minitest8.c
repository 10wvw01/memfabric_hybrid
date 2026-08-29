/*
 * minitest8.c — 8-rank 测试, 精确复现 07 的 join 阶段
 *
 * 假设: 问题与 8 个设备全部活跃 + 特定的 import 序列有关
 *
 * T20: 完整复现 07 join 阶段
 *      - 所有 8 rank 建 slab + export key
 *      - Allgather 交换 keys
 *      - Token-ring: r0 import 全部 7 peer, 然后 r1 import 全部 7 peer
 *      - 观察 r1 的 import 结果
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

/* ================================================================
 *  T20: 完整复现 07 join 阶段
 * ================================================================ */
static void test20(int rank, int32_t dev)
{
    void *slab = NULL;
    char mykey[IPC_KEY_LEN] = {0};
    char keys[N][IPC_KEY_LEN];
    void *ipc_va[N] = {NULL};
    int imp_ok[N] = {0};

    /* 建 slab + export key */
    int32_t ret = aclrtMalloc(&slab, SLAB_SIZE, ACL_MEM_MALLOC_HUGE_FIRST_P2P);
    if (ret == ACL_ERROR_NONE) {
        ret = aclrtIpcMemGetExportKey(slab, SLAB_SIZE, mykey, IPC_KEY_LEN,
                                      ACL_RT_IPC_MEM_EXPORT_FLAG_DISABLE_PID_VALIDATION);
    }
    printf("  r%d(dev%d): create slab %s\n", rank, dev, (ret == 0) ? "OK" : "FAIL");

    /* Allgather 交换 keys */
    MPI_Allgather(mykey, IPC_KEY_LEN, MPI_CHAR,
                  &keys[0][0], IPC_KEY_LEN, MPI_CHAR, MPI_COMM_WORLD);
    MPI_Barrier(MPI_COMM_WORLD);

    /* Token-ring: r0 import 全部 7 peer */
    if (rank == 0) {
        printf("  r0: importing all 7 peers...\n");
        for (int p = 0; p < N; p++) {
            if (p == rank) continue;
            int die = is_die_pair(rank, p);
            uint32_t flag = die ? ACL_RT_IPC_MEM_IMPORT_FLAG_DEFAULT
                                : ACL_RT_IPC_MEM_IMPORT_FLAG_ENABLE_PEER_ACCESS;
            void *pv = NULL;
            ret = aclrtIpcMemImportByKey(&pv, keys[p], flag);
            imp_ok[p] = (ret == 0);
            if (imp_ok[p]) ipc_va[p] = pv;
            printf("    r0 import p%d (%s): %s\n", p, die ? "die/DEFAULT" : "cross/PEER", ret_str(ret));
        }
    }
    MPI_Barrier(MPI_COMM_WORLD);

    /* Token-ring: r1 import 全部 7 peer */
    if (rank == 1) {
        printf("  r1: importing all 7 peers...\n");
        for (int p = 0; p < N; p++) {
            if (p == rank) continue;
            int die = is_die_pair(rank, p);
            uint32_t flag = die ? ACL_RT_IPC_MEM_IMPORT_FLAG_DEFAULT
                                : ACL_RT_IPC_MEM_IMPORT_FLAG_ENABLE_PEER_ACCESS;
            void *pv = NULL;
            ret = aclrtIpcMemImportByKey(&pv, keys[p], flag);
            imp_ok[p] = (ret == 0);
            if (imp_ok[p]) ipc_va[p] = pv;
            printf("    r1 import p%d (%s): %s\n", p, die ? "die/DEFAULT" : "cross/PEER", ret_str(ret));
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

    /* DisablePeerAccess for cross-card peers */
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

    if (nproc != N) {
        if (rank == 0) printf("need -np %d, got %d\n", N, nproc);
        MPI_Finalize();
        return 1;
    }

    int32_t dev = rank;  /* rank k = dev k */

    setvbuf(stdout, NULL, _IONBF, 0);
    if (rank == 0)
        printf("=== T20: 8-rank, 完整复现 07 join 阶段 ===\n");

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

    test20(rank, dev);

    if (rank == 0)
        printf("=== T20 done ===\n\n");

    aclrtResetDevice(dev);
    aclFinalize();
    MPI_Finalize();
    return 0;
}
