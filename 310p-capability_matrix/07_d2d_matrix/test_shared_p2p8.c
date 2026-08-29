/*
 * test_shared_p2p8.c — 8-rank 全连接 + 显式按需 P2P (hcomm 模式)
 *
 * 基于 4-rank 探针结论:
 *   - 显式 EnablePeerAccess + DEFAULT import 可行 (hcomm 模式, Q1 成立)
 *   - P2P 关系是 die-pair 粒度, 无卡级共享 (Q2/Q3/Q4 均不成立)
 *   - 关系为无序对, 双向共享同一条 (r0 import p2 与 r2 import p0 共用 (0,2))
 *
 * 全连接所需关系数 (每卡视角, 跨卡 die 对):
 *   card0 参与: (0,x) x=2..7 共 6 条 + (1,x) x=2..7 共 6 条 = 12 条 > 8 配额
 *   → 全连接仍不可行, 但本测试验证: 显式 enable 模式下配额边界在哪里,
 *     以及 "能建多少建多少" 的最大连通度。
 *
 * 实验设计:
 *   Phase A: 全体 8 rank 显式 EnablePeerAccess 到所有跨卡 peer
 *            (按 token-ring 顺序逐个 enable, 记录成功/失败, 观察精确配额边界)
 *   Phase B: 全部用 DEFAULT flag import (只对 enable 成功的对)
 *   Phase C: 环形写+校验 (r 写 (r+1)%8, 验证 (r+7)%8 写来的数据)
 *   Phase D: cleanup: IpcMemClose -> Barrier -> Free -> DisablePeerAccess(成功的) -> Reset
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <mpi.h>
#include "acl/acl.h"

#define N               8
#define SLAB_SIZE       (16u * 1024u * 1024u)
#define IPC_KEY_LEN     65
#define TAG_CREATE      0x07
#define TAG_WRITE_BASE  0x70

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
            printf("[S8] r%d [verify] mismatch at word %zu: got 0x%08x want 0x%08x\n",
                   who, i, buf[i], pat(rank, tag, i));
            return -1;
        }
    }
    return 0;
}

static int is_die_pair(int i, int j)
{
    return i != j && (i / 2) == (j / 2);
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
            printf("[S8] FATAL: need exactly %d MPI procs, got %d\n", N, nproc);
        MPI_Finalize();
        return 1;
    }

    int32_t dev = rank;
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("[S8] === 8-rank explicit-P2P + DEFAULT import: rank%d = dev%d (card%d) ===\n",
           rank, dev, dev / 2);

    aclrtStream stream = NULL;
    void *slab = NULL, *scratch = NULL;
    uint32_t *hbuf = NULL, *vbuf = NULL;
    char mykey[IPC_KEY_LEN] = {0};
    char keys[N][IPC_KEY_LEN];
    void *ipc_va[N];
    int imp_ok[N];
    int ena_ok[N];              /* 显式 EnablePeerAccess 成功标记 */
    int fatal = 0, join_ok = 0, write_ok = 0;
    int32_t ret = 0;
    int init_ok = 0, dev_ok = 0;
    size_t words = SLAB_SIZE / sizeof(uint32_t);

    memset(keys, 0, sizeof(keys));
    for (int i = 0; i < N; i++) { ipc_va[i] = NULL; imp_ok[i] = 0; ena_ok[i] = 0; }

    /* ---- Phase 0: init + slab + export + exchange ---- */
    ret = aclInit(NULL);
    if (ret != ACL_ERROR_NONE) {
        printf("[S8] r%d init FAIL ret=%d\n", rank, ret);
        BAIL(fatal);
    } else {
        init_ok = 1;
        ret = aclrtSetDevice(dev);
        if (ret != ACL_ERROR_NONE) {
            printf("[S8] r%d SetDevice(%d) FAIL ret=%d\n", rank, dev, ret);
            BAIL(fatal);
        }
    }
    if (!fatal) {
        dev_ok = 1;
        ret = aclrtCreateStream(&stream);
        if (ret != ACL_ERROR_NONE) {
            printf("[S8] r%d CreateStream FAIL ret=%d\n", rank, ret);
            BAIL(fatal);
        }
        if (!fatal) {
            ret = aclrtMalloc(&scratch, SLAB_SIZE, ACL_MEM_MALLOC_HUGE_FIRST);
            if (ret != ACL_ERROR_NONE) {
                printf("[S8] r%d malloc scratch FAIL ret=%d\n", rank, ret);
                BAIL(fatal);
            }
        }
        if (!fatal) {
            hbuf = (uint32_t *)malloc(SLAB_SIZE);
            vbuf = (uint32_t *)malloc(SLAB_SIZE);
            if (!hbuf || !vbuf) {
                printf("[S8] r%d host malloc FAIL\n", rank);
                BAIL(fatal);
            }
        }
    }
    if (!fatal) {
        ret = aclrtMalloc(&slab, SLAB_SIZE, ACL_MEM_MALLOC_HUGE_FIRST_P2P);
        if (ret != ACL_ERROR_NONE) {
            printf("[S8] r%d malloc slab FAIL ret=%d\n", rank, ret);
            BAIL(fatal);
        }
    }
    if (!fatal) {
        fill_pat(hbuf, words, (uint32_t)rank, TAG_CREATE);
        ret = aclrtMemcpy(slab, SLAB_SIZE, hbuf, SLAB_SIZE, ACL_MEMCPY_HOST_TO_DEVICE);
        if (ret != ACL_ERROR_NONE) {
            printf("[S8] r%d H2D pattern FAIL ret=%d\n", rank, ret);
            BAIL(fatal);
        }
    }
    if (!fatal) {
        ret = aclrtIpcMemGetExportKey(slab, SLAB_SIZE, mykey, IPC_KEY_LEN,
                                      ACL_RT_IPC_MEM_EXPORT_FLAG_DISABLE_PID_VALIDATION);
        if (ret != ACL_ERROR_NONE) {
            printf("[S8] r%d export key FAIL ret=%d\n", rank, ret);
            BAIL(fatal);
        }
        printf("[S8] r%d create slab @ %p, key exported\n", rank, slab);
    }
    sync_state(&fatal);

    if (!fatal) {
        MPI_Allgather(mykey, IPC_KEY_LEN, MPI_CHAR,
                      &keys[0][0], IPC_KEY_LEN, MPI_CHAR, MPI_COMM_WORLD);
        printf("[S8] r%d exchange: allgathered %d keys\n", rank, N);
    }
    sync_state(&fatal);

    /* ---- Phase A: 显式 EnablePeerAccess (token-ring 串行, 跨卡 peer) ----
     * 与 test.c 的区别: 不靠 PEER_ACCESS import flag 隐式触发,
     * 而是显式调用 (hcomm 模式), 且只对跨卡 peer。 */
    if (!fatal) {
        printf("[S8] r%d explicit EnablePeerAccess phase begins\n", rank);
        for (int token = 0; token < N; token++) {
            MPI_Barrier(MPI_COMM_WORLD);
            if (token == rank) {
                for (int p = 0; p < N; p++) {
                    if (p == rank || is_die_pair(rank, p))
                        continue;   /* die 对走 HCCS, 不需要也不允许 P2P */
                    int32_t r = aclrtDeviceEnablePeerAccess(p, 0);
                    ena_ok[p] = (r == ACL_ERROR_NONE);
                    printf("[S8]   r%d(dev%d) EnablePeerAccess(dev%d): %s (ret=%d)\n",
                           rank, dev, p, ena_ok[p] ? "OK" : "FAIL", r);
                }
                int enabled = 0;
                for (int p = 0; p < N; p++)
                    if (ena_ok[p]) enabled++;
                printf("[S8] r%d enabled %d/6 cross-card peers\n", rank, enabled);
            }
            MPI_Barrier(MPI_COMM_WORLD);
        }
    }
    sync_state(&fatal);

    /* ---- Phase B: 全部 DEFAULT import (token-ring 串行) ---- */
    if (!fatal) {
        printf("[S8] r%d import phase: ALL DEFAULT flag\n", rank);
        for (int token = 0; token < N; token++) {
            MPI_Barrier(MPI_COMM_WORLD);
            if (token == rank) {
                for (int p = 0; p < N; p++) {
                    if (p == rank) continue;
                    void *pv = NULL;
                    int32_t r = aclrtIpcMemImportByKey(&pv, keys[p], 0UL);
                    imp_ok[p] = (r == ACL_ERROR_NONE);
                    if (imp_ok[p]) ipc_va[p] = pv;
                    printf("[S8]   r%d import p%d (%s) DEFAULT: %s (ret=%d) va=%p\n",
                           rank, p, is_die_pair(rank, p) ? "die" : "cross",
                           imp_ok[p] ? "OK" : "FAIL", r, pv);
                }
                int joined = 0;
                for (int p = 0; p < N; p++)
                    if (p != rank && imp_ok[p]) joined++;
                join_ok = (joined == N - 1);
                printf("[S8] r%d joined %d/7 peers\n", rank, joined);
            }
            MPI_Barrier(MPI_COMM_WORLD);
        }
    }
    sync_state(&fatal);

    /* ---- Phase C: 环形写+校验 ---- */
    if (!fatal) {
        int w_dst = (rank + 1) % N;
        int w_me = (rank + N - 1) % N;

        fill_pat(hbuf, words, (uint32_t)rank, (uint32_t)(TAG_WRITE_BASE + rank));
        ret = aclrtMemcpy(scratch, SLAB_SIZE, hbuf, SLAB_SIZE, ACL_MEMCPY_HOST_TO_DEVICE);
        if (ret != ACL_ERROR_NONE) {
            printf("[S8] r%d write: H2D FAIL ret=%d\n", rank, ret);
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
                printf("[S8] r%d write D2D 16MB -> p%d: %s (ret=%d)\n",
                       rank, w_dst, delivered ? "OK" : "FAIL", ret);
            } else {
                printf("[S8] r%d write D2D -> p%d: SKIP (import not ok)\n", rank, w_dst);
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
                    printf("[S8] r%d verify (writer p%d): %s\n", rank, w_me, ok ? "OK" : "FAIL");
                    if (!ok) g_fail = 1;
                } else {
                    printf("[S8] r%d verify: D2H FAIL ret=%d\n", rank, ret);
                    g_fail = 1;
                }
            } else {
                printf("[S8] r%d verify: writer p%d did not deliver\n", rank, w_me);
                g_fail = 1;
            }
        }
    }
    sync_state(&fatal);

    /* ---- 汇总统计 ---- */
    {
        int mat[N][N];
        memset(mat, 0, sizeof(mat));
        for (int p = 0; p < N; p++)
            if (p != rank) mat[rank][p] = imp_ok[p] ? 1 : 0;
        int gmat[N * N];
        MPI_Allreduce(mat, gmat, N * N, MPI_INT, MPI_MAX, MPI_COMM_WORLD);

        int emat[N][N];
        memset(emat, 0, sizeof(emat));
        for (int p = 0; p < N; p++)
            if (p != rank) emat[rank][p] = ena_ok[p] ? 1 : 0;
        int gemat[N * N];
        MPI_Allreduce(emat, gemat, N * N, MPI_INT, MPI_MAX, MPI_COMM_WORLD);

        if (rank == 0) {
            printf("[S8] === Enable matrix (row=enabler, col=peer) ===\n");
            printf("[S8]        ");
            for (int p = 0; p < N; p++) printf(" p%d     ", p);
            printf("\n");
            for (int r = 0; r < N; r++) {
                printf("[S8] r%d  ", r);
                for (int p = 0; p < N; p++) {
                    if (r == p) printf("  ---   ");
                    else if (is_die_pair(r, p)) printf(" (die)  ");
                    else printf("  %-5s  ", gemat[r * N + p] ? "OK" : "FAIL");
                }
                printf("\n");
            }
            printf("[S8] === Import matrix (row=importer, col=exporter) ===\n");
            printf("[S8]        ");
            for (int p = 0; p < N; p++) printf(" p%d     ", p);
            printf("\n");
            for (int r = 0; r < N; r++) {
                printf("[S8] r%d  ", r);
                for (int p = 0; p < N; p++) {
                    if (r == p) printf("  ---   ");
                    else printf("  %-5s  ", gmat[r * N + p] ? "OK" : "FAIL");
                }
                printf("\n");
            }
            /* 每卡统计: 参与的关系数 (无序 die 对, 跨卡) */
            printf("[S8] === Per-card relation count (quota=8/card) ===\n");
            for (int c = 0; c < 4; c++) {
                int cnt = 0;
                for (int a = 0; a < N; a++) {
                    if (a / 2 != c) continue;
                    for (int b = 0; b < N; b++) {
                        if (b / 2 == c || a == b) continue;
                        if (gmat[a * N + b] || gemat[a * N + b]) cnt++;
                    }
                }
                printf("[S8] card%d: %d/8 relations\n", c, cnt);
            }
        }
    }

    /* ---- RESULT ---- */
    {
        int res[3];
        res[0] = g_fail;
        res[1] = !join_ok;
        res[2] = !write_ok;
        MPI_Allreduce(MPI_IN_PLACE, res, 3, MPI_INT, MPI_MAX, MPI_COMM_WORLD);
        g_fail = res[0];
        printf("[S8] r%d RESULT: %s (join:%s write:%s)\n", rank,
               g_fail ? "FAIL" : "PASS",
               res[1] ? "FAIL" : "OK", res[2] ? "FAIL" : "OK");
        fflush(stdout);
    }

    /* ---- Phase D: cleanup ---- */
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
        for (int p = 0; p < N; p++) {
            if (p != rank && ena_ok[p] && !is_die_pair(rank, p)) {
                aclError dr = aclrtDeviceDisablePeerAccess(p);
                printf("[S8] r%d DisablePeerAccess(p%d) ret=%d\n", rank, p, dr);
            }
        }
        aclrtResetDevice(dev);
    }
    if (init_ok) aclFinalize();

    MPI_Finalize();
    return g_fail ? 1 : 0;
}
