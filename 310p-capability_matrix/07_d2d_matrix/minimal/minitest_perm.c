/*
 * minitest_perm.c — 穷举 r1 的 PEER import 组合, 验证是否真的是"2 个上限"
 *
 * r0 先 import 全部 7, 然后 r1 按不同组合 import
 *
 * 全跨 socket (不含 die pair):
 * T40: p4          (1个)
 * T41: p4, p5      (2个)
 * T42: p4, p6      (2个, 不同卡)
 * T43: p4, p7      (2个, 不同卡)
 * T44: p6, p7      (2个, card3)
 * T45: p4, p5, p6  (3个)
 * T46: p4, p5, p7  (3个)
 * T47: p4, p6, p7  (3个)
 * T48: p5, p6, p7  (3个)
 * T49: p4, p5, p6, p7 (4个)
 *
 * 含同 socket 跨卡:
 * T50: p2          (1个)
 * T51: p3          (1个)
 * T52: p2, p3      (2个, 同 card1)
 * T53: p2, p4      (2个, 不同 card)
 * T54: p3, p4      (2个, 不同 card)
 * T55: p2, p3, p4  (3个)
 * T56: p2, p4, p5  (3个)
 * T57: p3, p4, p5  (3个)
 * T58: p2, p3, p4, p5 (4个)
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
    int card = target / 2;
    printf("    r%d import p%-2d (card%d): %s\n", rank, target, card, ret_str(ret));
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

    if (rank == 0) {
        for (int p = 1; p < N; p++)
            do_import(0, p, keys, imp_ok);
        memset(imp_ok, 0, sizeof(imp_ok));
    }
    MPI_Barrier(MPI_COMM_WORLD);

    if (rank == 1) {
        int seq[8], cnt = 0;
        switch (tnum) {
        case 40: seq[0]=4; cnt=1; break;
        case 41: seq[0]=4; seq[1]=5; cnt=2; break;
        case 42: seq[0]=4; seq[1]=6; cnt=2; break;
        case 43: seq[0]=4; seq[1]=7; cnt=2; break;
        case 44: seq[0]=6; seq[1]=7; cnt=2; break;
        case 45: seq[0]=4; seq[1]=5; seq[2]=6; cnt=3; break;
        case 46: seq[0]=4; seq[1]=5; seq[2]=7; cnt=3; break;
        case 47: seq[0]=4; seq[1]=6; seq[2]=7; cnt=3; break;
        case 48: seq[0]=5; seq[1]=6; seq[2]=7; cnt=3; break;
        case 49: seq[0]=4; seq[1]=5; seq[2]=6; seq[3]=7; cnt=4; break;
        case 50: seq[0]=2; cnt=1; break;
        case 51: seq[0]=3; cnt=1; break;
        case 52: seq[0]=2; seq[1]=3; cnt=2; break;
        case 53: seq[0]=2; seq[1]=4; cnt=2; break;
        case 54: seq[0]=3; seq[1]=4; cnt=2; break;
        case 55: seq[0]=2; seq[1]=3; seq[2]=4; cnt=3; break;
        case 56: seq[0]=2; seq[1]=4; seq[2]=5; cnt=3; break;
        case 57: seq[0]=3; seq[1]=4; seq[2]=5; cnt=3; break;
        case 58: seq[0]=2; seq[1]=3; seq[2]=4; seq[3]=5; cnt=4; break;
        }
        printf("  r1: T%d (%d peers)...\n", tnum, cnt);
        for (int i = 0; i < cnt; i++)
            do_import(1, seq[i], keys, imp_ok);
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

    int tnum = env_int("T", 40);
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
