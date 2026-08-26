/*
 * test_4rank.c — 4 NPU IPC 全连接测试 (每卡 1 rank, 避免同卡 P2P 槽位竞争)
 *
 * 设备映射: rank0=dev0(card0), rank1=dev2(card1), rank2=dev4(card2), rank3=dev6(card3)
 * 全部跨卡, 全部 PEER_ACCESS, 每卡只占 3 个 P2P 槽位 (≤ 8)
 *
 * 预期: 4×3=12 条有向链路全通 → PASS
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <mpi.h>
#include "acl/acl.h"

#define N               4
#define IPC_KEY_LEN     65
#define SLAB_SIZE       (16u * 1024u * 1024u)
#define SCRATCH_SIZE    (16u * 1024u * 1024u)

#define TAG_CREATE      0x07
#define TAG_WRITE_BASE  0x70

static const int32_t dev_map[N] = {0, 2, 4, 6};

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

static int check_pat(const uint32_t *buf, size_t words, uint32_t rank, uint32_t tag, int who)
{
    for (size_t i = 0; i < words; i++) {
        if (buf[i] != pat(rank, tag, i)) {
            printf("[07q] r%d [verify] mismatch at word %zu: got 0x%08x, want 0x%08x\n",
                   who, i, buf[i], pat(rank, tag, i));
            return -1;
        }
    }
    return 0;
}

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void sync_state(int *fatal)
{
    int st[2];
    st[0] = *fatal;
    st[1] = g_fail;
    MPI_Allreduce(MPI_IN_PLACE, st, 2, MPI_INT, MPI_MAX, MPI_COMM_WORLD);
    *fatal = st[0];
    g_fail = st[1];
}

#define BAIL(v) do { (v) = 1; g_fail = 1; } while (0)

int main(int argc, char *argv[])
{
    MPI_Init(&argc, &argv);
    int rank = 0, nproc = 0;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &nproc);
    if (nproc != N) {
        if (rank == 0)
            printf("[07q] FATAL: need exactly %d MPI procs, got %d\n", N, nproc);
        MPI_Finalize();
        return 1;
    }

    int32_t dev = dev_map[rank];
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("[07q] === 07_d2d_matrix 4-rank: rank%d = dev%d (card%d) ===\n",
           rank, dev, dev / 2);

    aclrtStream stream = NULL;
    void *slab = NULL, *scratch = NULL;
    uint32_t *hbuf = NULL, *vbuf = NULL;
    char mykey[IPC_KEY_LEN] = {0};
    char keys[N][IPC_KEY_LEN];
    void *ipc_va[N];
    uint64_t va_tab[N];
    int imp_ok[N];
    int fatal = 0, join_ok = 0, write_ok = 0, va_ok = 0;
    int32_t ret = 0;
    int init_ok = 0, dev_ok = 0;
    size_t words = SLAB_SIZE / sizeof(uint32_t);

    memset(keys, 0, sizeof(keys));
    for (int i = 0; i < N; i++) { ipc_va[i] = NULL; va_tab[i] = 0; imp_ok[i] = 0; }

    /* 阶段① init + create */
    ret = aclInit(NULL);
    if (ret != ACL_ERROR_NONE) {
        printf("[07q] r%d init: aclInit FAIL ret=%d\n", rank, ret);
        BAIL(fatal);
    } else {
        init_ok = 1;
        ret = aclrtSetDevice(dev);
        if (ret != ACL_ERROR_NONE) {
            printf("[07q] r%d init: aclrtSetDevice(%d) FAIL ret=%d\n", rank, dev, ret);
            BAIL(fatal);
        }
    }
    if (!fatal) {
        dev_ok = 1;
        ret = aclrtCreateStream(&stream);
        if (ret != ACL_ERROR_NONE) {
            printf("[07q] r%d init: aclrtCreateStream FAIL ret=%d\n", rank, ret);
            BAIL(fatal);
        }
    }
    if (!fatal) {
        ret = aclrtMalloc(&scratch, SCRATCH_SIZE, ACL_MEM_MALLOC_HUGE_FIRST);
        if (ret != ACL_ERROR_NONE) {
            printf("[07q] r%d init: malloc scratch FAIL ret=%d\n", rank, ret);
            BAIL(fatal);
        }
    }
    if (!fatal) {
        hbuf = (uint32_t *)malloc(SLAB_SIZE);
        vbuf = (uint32_t *)malloc(SLAB_SIZE);
        if (!hbuf || !vbuf) {
            printf("[07q] r%d init: host malloc FAIL\n", rank);
            BAIL(fatal);
        }
    }
    if (!fatal) {
        ret = aclrtMalloc(&slab, SLAB_SIZE, ACL_MEM_MALLOC_HUGE_FIRST_P2P);
        if (ret != ACL_ERROR_NONE) {
            printf("[07q] r%d create: malloc slab FAIL ret=%d\n", rank, ret);
            BAIL(fatal);
        }
    }
    if (!fatal) {
        fill_pat(hbuf, words, (uint32_t)rank, TAG_CREATE);
        ret = aclrtMemcpy(slab, SLAB_SIZE, hbuf, SLAB_SIZE, ACL_MEMCPY_HOST_TO_DEVICE);
        if (ret != ACL_ERROR_NONE) {
            printf("[07q] r%d create: H2D FAIL ret=%d\n", rank, ret);
            BAIL(fatal);
        }
    }
    if (!fatal) {
        ret = aclrtIpcMemGetExportKey(slab, SLAB_SIZE, mykey, IPC_KEY_LEN,
                                      ACL_RT_IPC_MEM_EXPORT_FLAG_DISABLE_PID_VALIDATION);
        if (ret != ACL_ERROR_NONE) {
            printf("[07q] r%d create: export key FAIL ret=%d\n", rank, ret);
            BAIL(fatal);
        }
        printf("[07q] r%d create slab @ %p, key exported\n", rank, slab);
    }
    sync_state(&fatal);

    /* 阶段② exchange keys */
    if (!fatal) {
        MPI_Allgather(mykey, IPC_KEY_LEN, MPI_CHAR,
                      &keys[0][0], IPC_KEY_LEN, MPI_CHAR, MPI_COMM_WORLD);
        printf("[07q] r%d exchange: allgathered %d keys\n", rank, N);
    }
    sync_state(&fatal);

    /* 阶段③ join: 全部跨卡, 全部 PEER_ACCESS */
    if (!fatal) {
        printf("[07q] r%d join: token-ring serial import (ALL PEER_ACCESS) begins\n", rank);
        for (int token = 0; token < N; token++) {
            MPI_Barrier(MPI_COMM_WORLD);
            if (token == rank) {
                for (int p = 0; p < N; p++) {
                    if (p == rank) {
                        va_tab[p] = (uint64_t)(uintptr_t)slab;
                        continue;
                    }
                    void *pv = NULL;
                    int32_t r = aclrtIpcMemImportByKey(&pv, keys[p],
                                                       ACL_RT_IPC_MEM_IMPORT_FLAG_ENABLE_PEER_ACCESS);
                    imp_ok[p] = (r == ACL_ERROR_NONE);
                    if (imp_ok[p]) {
                        ipc_va[p] = pv;
                        va_tab[p] = (uint64_t)(uintptr_t)pv;
                    }
                    printf("[07q]   r%d(dev%d) import r%d(dev%d) PEER: %s (ret=%d) va=0x%012lx\n",
                           rank, dev, p, dev_map[p],
                           imp_ok[p] ? "OK" : "FAIL", r, (unsigned long)va_tab[p]);
                }
                int joined = 0;
                for (int p = 0; p < N; p++)
                    if (p != rank && imp_ok[p]) joined++;
                join_ok = (joined == N - 1);
                if (!join_ok) g_fail = 1;
                printf("[07q] r%d joined %d/3 peers\n", rank, joined);
            }
            MPI_Barrier(MPI_COMM_WORLD);
        }
    }
    sync_state(&fatal);

    /* 阶段④ VA 一致性 */
    if (!fatal) {
        uint64_t va_all[N * N];
        MPI_Allgather(va_tab, N * sizeof(uint64_t), MPI_BYTE,
                      va_all, N * sizeof(uint64_t), MPI_BYTE, MPI_COMM_WORLD);
        va_ok = 1;
        for (int r = 1; r < N; r++) {
            for (int p = 0; p < N; p++) {
                if (p == r) continue;
                if (va_all[r * N + p] != va_all[0 * N + p]) {
                    va_ok = 0;
                    break;
                }
            }
            if (!va_ok) break;
        }
        if (rank == 0) {
            printf("[07q] VA matrix:\n");
            printf("[07q]         ");
            for (int p = 0; p < N; p++) printf("  r%d(dev%d)    ", p, dev_map[p]);
            printf("\n");
            for (int r = 0; r < N; r++) {
                printf("[07q] r%d(dev%d) ", r, dev_map[r]);
                for (int p = 0; p < N; p++)
                    printf("0x%012lx ", (unsigned long)va_all[r * N + p]);
                printf("\n");
            }
            printf("[07q] VA consistency: %s\n", va_ok ? "CONSISTENT" : "INCONSISTENT");
        }
        if (!va_ok) g_fail = 1;
    }
    sync_state(&fatal);

    /* 阶段⑤ 全网写+校验: 环 r→(r+1)%4 */
    if (!fatal) {
        int w_dst = (rank + 1) % N;
        int w_me = (rank + N - 1) % N;

        fill_pat(hbuf, words, (uint32_t)rank, (uint32_t)(TAG_WRITE_BASE + rank));
        ret = aclrtMemcpy(scratch, SCRATCH_SIZE, hbuf, SLAB_SIZE, ACL_MEMCPY_HOST_TO_DEVICE);
        if (ret != ACL_ERROR_NONE) {
            printf("[07q] r%d write: H2D FAIL ret=%d\n", rank, ret);
            BAIL(fatal);
        }
        if (!fatal) {
            int delivered = 0;
            if (imp_ok[w_dst]) {
                ret = aclrtMemcpyAsync(ipc_va[w_dst], SLAB_SIZE, scratch, SLAB_SIZE,
                                       ACL_MEMCPY_DEVICE_TO_DEVICE, stream);
                if (ret == ACL_ERROR_NONE)
                    ret = aclrtSynchronizeStream(stream);
                delivered = (ret == ACL_ERROR_NONE);
                printf("[07q] r%d write D2D 16MB -> r%d: %s (ret=%d)\n",
                       rank, w_dst, delivered ? "OK" : "FAIL", ret);
            } else {
                printf("[07q] r%d write D2D -> r%d: SKIP\n", rank, w_dst);
            }
            int dmap[N];
            MPI_Allgather(&delivered, 1, MPI_INT, dmap, 1, MPI_INT, MPI_COMM_WORLD);
            if (dmap[w_me]) {
                memset(vbuf, 0, SLAB_SIZE);
                ret = aclrtMemcpy(vbuf, SLAB_SIZE, slab, SLAB_SIZE, ACL_MEMCPY_DEVICE_TO_HOST);
                if (ret == ACL_ERROR_NONE) {
                    int ok = (check_pat(vbuf, words, (uint32_t)w_me,
                                        (uint32_t)(TAG_WRITE_BASE + w_me), rank) == 0);
                    write_ok = ok;
                    printf("[07q] r%d verify (writer r%d): %s\n", rank, w_me, ok ? "OK" : "FAIL");
                    if (!ok) g_fail = 1;
                } else {
                    printf("[07q] r%d verify: D2H FAIL ret=%d\n", rank, ret);
                    g_fail = 1;
                }
            } else {
                printf("[07q] r%d verify: writer r%d did not deliver\n", rank, w_me);
                g_fail = 1;
            }
        }
    }
    sync_state(&fatal);

    /* 阶段⑥ 带宽: 每 rank 对 3 个 peer 各跑 async D2D 读 */
    if (!fatal && g_fail) {
        printf("[07q] r%d bandwidth: SKIPPED\n", rank);
    }
    if (!fatal && !g_fail) {
        printf("[07q] r%d bandwidth begins\n", rank);
        for (int token = 0; token < N; token++) {
            MPI_Barrier(MPI_COMM_WORLD);
            if (token == rank) {
                for (int p = 0; p < N; p++) {
                    if (p == rank || !imp_ok[p]) continue;
                    double gb = (double)30 * (double)(4u * 1024u * 1024u) / 1e9;
                    for (int i = 0; i < 3; i++) {
                        aclrtMemcpyAsync(scratch, 4u*1024u*1024u, ipc_va[p], 4u*1024u*1024u,
                                         ACL_MEMCPY_DEVICE_TO_DEVICE, stream);
                        aclrtSynchronizeStream(stream);
                    }
                    double t0 = now_sec();
                    for (int i = 0; i < 30; i++)
                        aclrtMemcpyAsync(scratch, 4u*1024u*1024u, ipc_va[p], 4u*1024u*1024u,
                                         ACL_MEMCPY_DEVICE_TO_DEVICE, stream);
                    aclrtSynchronizeStream(stream);
                    double t1 = now_sec();
                    printf("[07q] r%d bw r%d(dev%d)->r%d(dev%d): %7.2f GB/s (%.3fs)\n",
                           rank, p, dev_map[p], rank, dev, gb / (t1 - t0), t1 - t0);
                }
            }
            MPI_Barrier(MPI_COMM_WORLD);
        }
    }
    sync_state(&fatal);

    /* RESULT */
    {
        int res[4];
        res[0] = g_fail;
        res[1] = !join_ok;
        res[2] = !write_ok;
        res[3] = !va_ok;
        MPI_Allreduce(MPI_IN_PLACE, res, 4, MPI_INT, MPI_MAX, MPI_COMM_WORLD);
        g_fail = res[0];
        int join_fail = res[1], write_fail = res[2], va_fail = res[3];
        printf("[07q] r%d RESULT: %s (join:%s write:%s VA:%s)\n", rank,
               g_fail ? "FAIL" : "PASS",
               join_fail ? "FAIL" : "OK",
               write_fail ? "FAIL" : "OK",
               va_fail ? "INCONSISTENT" : "CONSISTENT");
        fflush(stdout);
    }

    /* cleanup */
    if (init_ok) {
        aclrtIpcMemClose(mykey);
        for (int p = 0; p < N; p++)
            if (p != rank && imp_ok[p]) aclrtIpcMemClose(keys[p]);
    }
    MPI_Barrier(MPI_COMM_WORLD);
    if (slab) aclrtFree(slab);
    if (scratch) aclrtFree(scratch);
    if (stream) aclrtDestroyStream(stream);
    free(hbuf);
    free(vbuf);
    if (dev_ok) {
        for (int p = 0; p < N; p++)
            if (p != rank && imp_ok[p]) aclrtDeviceDisablePeerAccess(dev_map[p]);
        aclrtResetDevice(dev);
    }
    if (init_ok) aclFinalize();
    MPI_Finalize();
    return g_fail ? 1 : 0;
}
