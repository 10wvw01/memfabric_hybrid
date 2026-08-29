/*
 * test_default.c — 8 NPU IPC 全连接测试, 全部使用 DEFAULT flag
 *
 * 目的: 验证 DEFAULT flag 是否占用 P2P 槽位, 能否绕过 310P Duo 每卡 8 槽位限制
 *
 * 与 test.c 的区别:
 *   - test.c: die 对用 DEFAULT, 跨卡用 ENABLE_PEER_ACCESS
 *   - test_default.c: 全部用 DEFAULT (不触发 EnablePeerAccess)
 *
 * 预期:
 *   - 如果 DEFAULT 不占用 P2P 槽位 → 56 条链路全通 → PASS
 *   - 如果 DEFAULT 也占用 P2P 槽位 → 仍然失败
 *   - 如果 DEFAULT 不支持跨卡 D2D → 写校验失败
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <mpi.h>
#include "acl/acl.h"

#define N               8
#define IPC_KEY_LEN     65
#define SLAB_SIZE       (16u * 1024u * 1024u)
#define SCRATCH_SIZE    (16u * 1024u * 1024u)
#define BW_SIZE         (4u  * 1024u * 1024u)
#define BW_WARMUP       3
#define BW_ITERS        30
#define WARM_MALLOC     (4u * 1024u)

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
            printf("[07d] r%d [verify] mismatch at word %zu: got 0x%08x, want 0x%08x\n",
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
            printf("[07d] FATAL: need exactly %d MPI procs, got %d\n", N, nproc);
        MPI_Finalize();
        return 1;
    }

    int32_t dev = rank;
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("[07d] === 07_d2d_matrix DEFAULT: 8 NPU IPC pool, rank%d = dev%d ===\n", rank, dev);
    printf("[07d] r%d: 16MB slab, ALL DEFAULT flag (no EnablePeerAccess)\n", rank);

    aclrtStream stream = NULL;
    void *slab = NULL, *scratch = NULL, *warm = NULL;
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
        printf("[07d] r%d init: aclInit FAIL ret=%d\n", rank, ret);
        BAIL(fatal);
    } else {
        init_ok = 1;
        ret = aclrtSetDevice(dev);
        if (ret != ACL_ERROR_NONE) {
            printf("[07d] r%d init: aclrtSetDevice(%d) FAIL ret=%d\n", rank, dev, ret);
            BAIL(fatal);
        }
    }
    if (!fatal) {
        dev_ok = 1;
        aclrtMalloc(&warm, WARM_MALLOC, ACL_MEM_MALLOC_HUGE_FIRST);
        ret = aclrtCreateStream(&stream);
        if (ret != ACL_ERROR_NONE) {
            printf("[07d] r%d init: aclrtCreateStream FAIL ret=%d\n", rank, ret);
            BAIL(fatal);
        }
        if (!fatal) {
            ret = aclrtMalloc(&scratch, SCRATCH_SIZE, ACL_MEM_MALLOC_HUGE_FIRST);
            if (ret != ACL_ERROR_NONE) {
                printf("[07d] r%d init: malloc scratch FAIL ret=%d\n", rank, ret);
                BAIL(fatal);
            }
        }
        if (!fatal) {
            hbuf = (uint32_t *)malloc(SLAB_SIZE);
            vbuf = (uint32_t *)malloc(SLAB_SIZE);
            if (!hbuf || !vbuf) {
                printf("[07d] r%d init: host malloc FAIL\n", rank);
                BAIL(fatal);
            }
        }
    }
    if (!fatal) {
        ret = aclrtMalloc(&slab, SLAB_SIZE, ACL_MEM_MALLOC_HUGE_FIRST_P2P);
        if (ret != ACL_ERROR_NONE) {
            printf("[07d] r%d create: malloc slab FAIL ret=%d\n", rank, ret);
            BAIL(fatal);
        }
    }
    if (!fatal) {
        fill_pat(hbuf, words, (uint32_t)rank, TAG_CREATE);
        ret = aclrtMemcpy(slab, SLAB_SIZE, hbuf, SLAB_SIZE, ACL_MEMCPY_HOST_TO_DEVICE);
        if (ret != ACL_ERROR_NONE) {
            printf("[07d] r%d create: H2D pattern FAIL ret=%d\n", rank, ret);
            BAIL(fatal);
        }
    }
    if (!fatal) {
        ret = aclrtIpcMemGetExportKey(slab, SLAB_SIZE, mykey, IPC_KEY_LEN,
                                      ACL_RT_IPC_MEM_EXPORT_FLAG_DISABLE_PID_VALIDATION);
        if (ret != ACL_ERROR_NONE) {
            printf("[07d] r%d create: export key FAIL ret=%d\n", rank, ret);
            BAIL(fatal);
        }
        printf("[07d] r%d create slab @ %p, key exported\n", rank, slab);
    }
    sync_state(&fatal);

    /* 阶段② 交换 keys */
    if (!fatal) {
        MPI_Allgather(mykey, IPC_KEY_LEN, MPI_CHAR,
                      &keys[0][0], IPC_KEY_LEN, MPI_CHAR, MPI_COMM_WORLD);
        printf("[07d] r%d exchange: allgathered %d keys\n", rank, N);
    }
    sync_state(&fatal);

    /* 阶段③ join: 全部使用 DEFAULT flag */
    if (!fatal) {
        printf("[07d] r%d join: token-ring serial import (ALL DEFAULT) begins\n", rank);
        for (int token = 0; token < N; token++) {
            MPI_Barrier(MPI_COMM_WORLD);
            if (token == rank) {
                for (int p = 0; p < N; p++) {
                    if (p == rank) {
                        va_tab[p] = (uint64_t)(uintptr_t)slab;
                        continue;
                    }
                    /* 关键区别: 全部使用 DEFAULT, 不区分 die 对/跨卡 */
                    uint32_t flag = ACL_RT_IPC_MEM_IMPORT_FLAG_DEFAULT;
                    void *pv = NULL;
                    int32_t r = aclrtIpcMemImportByKey(&pv, keys[p], flag);
                    imp_ok[p] = (r == ACL_ERROR_NONE);
                    if (imp_ok[p]) {
                        ipc_va[p] = pv;
                        va_tab[p] = (uint64_t)(uintptr_t)pv;
                    }
                    printf("[07d]   r%d import slab p%d (DEFAULT): %s (ret=%d) va=0x%012lx\n",
                           rank, p, imp_ok[p] ? "OK" : "FAIL", r, (unsigned long)va_tab[p]);
                }
                int joined = 0;
                for (int p = 0; p < N; p++)
                    if (p != rank && imp_ok[p]) joined++;
                join_ok = (joined == N - 1);
                if (!join_ok)
                    g_fail = 1;
                printf("[07d] r%d joined %d/7 peers\n", rank, joined);
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
                if (va_all[r * N + p] != va_all[0 * N + p]) {
                    va_ok = 0;
                    break;
                }
            }
            if (!va_ok) break;
        }
        if (rank == 0) {
            printf("[07d] VA consistency: %s\n", va_ok ? "CONSISTENT" : "INCONSISTENT");
        }
        if (!va_ok) g_fail = 1;
    }
    sync_state(&fatal);

    /* 阶段⑤ 全网写+校验 */
    if (!fatal) {
        int w_dst = (rank + 1) % N;
        int w_me = (rank + N - 1) % N;

        fill_pat(hbuf, words, (uint32_t)rank, (uint32_t)(TAG_WRITE_BASE + rank));
        ret = aclrtMemcpy(scratch, SCRATCH_SIZE, hbuf, SLAB_SIZE, ACL_MEMCPY_HOST_TO_DEVICE);
        if (ret != ACL_ERROR_NONE) {
            printf("[07d] r%d write: H2D own pattern FAIL ret=%d\n", rank, ret);
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
                printf("[07d] r%d write D2D 16MB -> p%d: %s (ret=%d)\n",
                       rank, w_dst, delivered ? "OK" : "FAIL", ret);
            } else {
                printf("[07d] r%d write D2D 16MB -> p%d: SKIP (import not ok)\n", rank, w_dst);
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
                    printf("[07d] r%d verify own slab (writer p%d): %s\n",
                           rank, w_me, ok ? "OK" : "FAIL");
                    if (!ok) g_fail = 1;
                } else {
                    printf("[07d] r%d verify: D2H FAIL ret=%d\n", rank, ret);
                    g_fail = 1;
                }
            } else {
                printf("[07d] r%d verify: writer p%d did not deliver, skip\n", rank, w_me);
                g_fail = 1;
            }
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
        printf("[07d] r%d RESULT: %s (join:%s write:%s VA:%s)\n", rank,
               g_fail ? "FAIL" : "PASS",
               join_fail ? "FAIL" : "OK",
               write_fail ? "FAIL" : "OK",
               va_fail ? "INCONSISTENT" : "CONSISTENT");
        fflush(stdout);
    }

    /* cleanup: 不需要 DisablePeerAccess (全部用 DEFAULT) */
    if (init_ok) {
        int32_t cr = aclrtIpcMemClose(mykey);
        printf("[07d] r%d cleanup: IpcMemClose(own key) ret=%d\n", rank, cr);
        for (int p = 0; p < N; p++) {
            if (p != rank && imp_ok[p]) {
                cr = aclrtIpcMemClose(keys[p]);
                printf("[07d] r%d cleanup: IpcMemClose(key[p%d]) ret=%d\n", rank, p, cr);
            }
        }
    }
    MPI_Barrier(MPI_COMM_WORLD);
    if (slab) aclrtFree(slab);
    if (scratch) aclrtFree(scratch);
    if (warm) aclrtFree(warm);
    if (stream) aclrtDestroyStream(stream);
    free(hbuf);
    free(vbuf);
    /* 不需要 DisablePeerAccess */
    if (dev_ok) aclrtResetDevice(dev);
    if (init_ok) aclFinalize();

    MPI_Finalize();
    return g_fail ? 1 : 0;
}
