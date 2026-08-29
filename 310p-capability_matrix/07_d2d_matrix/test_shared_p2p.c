/*
 * test_shared_p2p.c — 卡内两 die 共享 P2P 假设验证 (4-rank 探针)
 *
 * 机器: Atlas 300I Duo, 4 卡 x 2 die。本探针只用 2 张卡:
 *   card0: rank0=dev0(主die), rank1=dev1(从die)
 *   card1: rank2=dev2(主die), rank3=dev3(从die)
 *
 * 假设: PCIe 物理通路是卡级路径。若一条 (dev0,dev2) P2P 关系能让
 *       整对卡的全部 4 个 die 组合互相 import, 则全连接只需每卡 3 条
 *       关系 (每张远端卡 1 条), 远低于 8 槽配额。
 *
 * 实验设计:
 *   Phase A: 只建一条 P2P: rank0 EnablePeerAccess(2), rank2 EnablePeerAccess(0)
 *            (主die<->主die 双向, 每卡各耗 1 槽)。用 CanAccessPeer 确认生效。
 *   Phase B: 全部 4 rank 用 DEFAULT flag(0) import 其他 3 peer (token-ring 串行):
 *            Q1  dev0 -> dev2 : 直接 enable 对 (hcomm 模式: 显式enable+DEFAULT import)
 *            Q2  dev1 -> dev2 : 本地共享 (dev1 从未 enable, 走 dev0 建的通路?)
 *            Q3  dev0 -> dev3 : 远端共享 (dev3 未被 enable, 走到 dev2 的通路?)
 *            Q4  dev1 -> dev3 : 双重共享
 *            以及 4 个反方向 Q1'..Q4'
 *   Phase C: 矩阵数据校验: 每 rank w 向所有成功 import 的 peer p 的 slab
 *            [w*4MB, (w+1)*4MB) 段写 pattern(w,0x80,绝对idx), 全部落地后
 *            各 rank D2H 回读逐字校验 (验证数据通路真实可用, 非 import 假成功)
 *   Phase D: cleanup: IpcMemClose -> Barrier -> Free -> DisablePeerAccess(仅
 *            rank0->2 / rank2->0) -> ResetDevice -> Finalize
 *
 * 判定矩阵:
 *   Q1 OK, Q2-Q4 FAIL : hcomm 模式成立但无共享 -> 关系是 die 对粒度
 *   Q1,Q2 OK, Q3,Q4 FAIL : 本地共享(dev0的enable覆盖dev1) -> 每卡6条仍可全连接? 见 stage2
 *   Q1-Q4 全 OK : 卡对粒度共享成立! 全连接每卡仅需 3 条
 *   Q1 也 FAIL : 显式 enable + DEFAULT import 不通 -> 只能 PEER_ACCESS flag
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <mpi.h>
#include "acl/acl.h"

#define N               4
#define SLAB_SIZE       (16u * 1024u * 1024u)
#define SEG_SIZE        (SLAB_SIZE / N)            /* 4MB 每 writer 段 */
#define IPC_KEY_LEN     65
#define TAG_DATA        0x80

static int g_fail = 0;

static uint32_t pat(uint32_t rank, uint32_t tag, size_t idx)
{
    return ((rank & 0xFu) << 28) | ((tag & 0xFu) << 24) | ((uint32_t)idx & 0x00FFFFFFu);
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
            printf("[SP] FATAL: need exactly %d MPI procs, got %d\n", N, nproc);
        MPI_Finalize();
        return 1;
    }

    int32_t dev = rank;   /* rank0=dev0 rank1=dev1 rank2=dev2 rank3=dev3 */
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("[SP] === shared-P2P probe: rank%d = dev%d (card%d) ===\n",
           rank, dev, dev / 2);

    aclrtStream stream = NULL;
    void *slab = NULL, *scratch = NULL;
    uint32_t *hbuf = NULL, *vbuf = NULL;
    char mykey[IPC_KEY_LEN] = {0};
    char keys[N][IPC_KEY_LEN];
    void *ipc_va[N];
    int imp_ok[N];
    int fatal = 0;
    int32_t ret = 0;
    int init_ok = 0, dev_ok = 0;
    size_t seg_words = SEG_SIZE / sizeof(uint32_t);
    size_t words = SLAB_SIZE / sizeof(uint32_t);

    memset(keys, 0, sizeof(keys));
    for (int i = 0; i < N; i++) { ipc_va[i] = NULL; imp_ok[i] = 0; }

    /* ---- Phase 0: init + slab + export ---- */
    ret = aclInit(NULL);
    if (ret != ACL_ERROR_NONE) {
        printf("[SP] r%d init FAIL ret=%d\n", rank, ret);
        BAIL(fatal);
    } else {
        init_ok = 1;
        ret = aclrtSetDevice(dev);
        if (ret != ACL_ERROR_NONE) {
            printf("[SP] r%d SetDevice(%d) FAIL ret=%d\n", rank, dev, ret);
            BAIL(fatal);
        }
    }
    if (!fatal) {
        dev_ok = 1;
        ret = aclrtCreateStream(&stream);
        if (ret != ACL_ERROR_NONE) {
            printf("[SP] r%d CreateStream FAIL ret=%d\n", rank, ret);
            BAIL(fatal);
        }
        if (!fatal) {
            ret = aclrtMalloc(&scratch, SLAB_SIZE, ACL_MEM_MALLOC_HUGE_FIRST);
            if (ret != ACL_ERROR_NONE) {
                printf("[SP] r%d malloc scratch FAIL ret=%d\n", rank, ret);
                BAIL(fatal);
            }
        }
        if (!fatal) {
            hbuf = (uint32_t *)malloc(SLAB_SIZE);
            vbuf = (uint32_t *)malloc(SLAB_SIZE);
            if (!hbuf || !vbuf) {
                printf("[SP] r%d host malloc FAIL\n", rank);
                BAIL(fatal);
            }
        }
    }
    if (!fatal) {
        ret = aclrtMalloc(&slab, SLAB_SIZE, ACL_MEM_MALLOC_HUGE_FIRST_P2P);
        if (ret != ACL_ERROR_NONE) {
            printf("[SP] r%d malloc slab FAIL ret=%d\n", rank, ret);
            BAIL(fatal);
        }
    }
    if (!fatal) {
        ret = aclrtIpcMemGetExportKey(slab, SLAB_SIZE, mykey, IPC_KEY_LEN,
                                      ACL_RT_IPC_MEM_EXPORT_FLAG_DISABLE_PID_VALIDATION);
        if (ret != ACL_ERROR_NONE) {
            printf("[SP] r%d export key FAIL ret=%d\n", rank, ret);
            BAIL(fatal);
        }
        printf("[SP] r%d create slab @ %p, key exported\n", rank, slab);
    }
    sync_state(&fatal);

    if (!fatal) {
        MPI_Allgather(mykey, IPC_KEY_LEN, MPI_CHAR,
                      &keys[0][0], IPC_KEY_LEN, MPI_CHAR, MPI_COMM_WORLD);
        printf("[SP] r%d exchange: allgathered %d keys\n", rank, N);
    }
    sync_state(&fatal);

    /* ---- Phase A: 只建一条 P2P (主die<->主die, 双向) ---- */
    if (!fatal) {
        if (rank == 0) {
            ret = aclrtDeviceEnablePeerAccess(2, 0);
            printf("[SP] r0 EnablePeerAccess(dev2) ret=%d\n", ret);
            if (ret != ACL_ERROR_NONE) BAIL(fatal);
        } else if (rank == 2) {
            ret = aclrtDeviceEnablePeerAccess(0, 0);
            printf("[SP] r2 EnablePeerAccess(dev0) ret=%d\n", ret);
            if (ret != ACL_ERROR_NONE) BAIL(fatal);
        }
    }
    MPI_Barrier(MPI_COMM_WORLD);   /* 双向 enable 都落地 */
    if (!fatal) {
        /* CanAccessPeer 自检: 本设备能否访问各 peer */
        for (int p = 0; p < N; p++) {
            if (p == rank) continue;
            int32_t can = -1;
            ret = aclrtDeviceCanAccessPeer(&can, dev, p);
            printf("[SP] r%d CanAccessPeer(dev%d -> dev%d) ret=%d can=%d\n",
                   rank, dev, p, ret, can);
        }
    }
    sync_state(&fatal);

    /* ---- Phase B: 全部 DEFAULT import (token-ring 串行) ---- */
    if (!fatal) {
        printf("[SP] r%d import phase: ALL DEFAULT flag\n", rank);
        for (int token = 0; token < N; token++) {
            MPI_Barrier(MPI_COMM_WORLD);
            if (token == rank) {
                for (int p = 0; p < N; p++) {
                    if (p == rank) continue;
                    void *pv = NULL;
                    int32_t r = aclrtIpcMemImportByKey(&pv, keys[p], 0UL);
                    imp_ok[p] = (r == ACL_ERROR_NONE);
                    if (imp_ok[p]) ipc_va[p] = pv;
                    printf("[SP]   r%d(dev%d) import p%d(dev%d) DEFAULT: %s (ret=%d) va=%p\n",
                           rank, dev, p, p, imp_ok[p] ? "OK" : "FAIL", r, pv);
                }
                int joined = 0;
                for (int p = 0; p < N; p++)
                    if (p != rank && imp_ok[p]) joined++;
                printf("[SP] r%d joined %d/3 peers\n", rank, joined);
            }
            MPI_Barrier(MPI_COMM_WORLD);
        }
    }
    sync_state(&fatal);

    /* ---- Phase C: 矩阵数据校验 ----
     * 每 rank w 向所有 imp_ok 的 peer p (含自身: 本地 D2D) 的 slab
     * 写 4MB 段 [w*SEG, (w+1)*SEG), pattern = pat(w, 0x80, 绝对 word idx)。
     * 全部落地后各 rank D2H 回读, 逐段校验 (哪些 writer 导入过我, 全局矩阵决定)。 */
    if (!fatal) {
        /* 汇总全局 import 矩阵: mat[w][p] = rank w 是否成功 import p */
        int mat[N][N];
        memset(mat, 0, sizeof(mat));
        for (int p = 0; p < N; p++) mat[rank][p] = imp_ok[p];
        int gmat[N * N];
        MPI_Allreduce(mat, gmat, N * N, MPI_INT, MPI_MAX, MPI_COMM_WORLD);

        if (rank == 0) {
            printf("[SP] import matrix (row=importer, col=exporter):\n");
            printf("[SP]        ");
            for (int p = 0; p < N; p++) printf("  p%d      ", p);
            printf("\n");
            for (int r = 0; r < N; r++) {
                printf("[SP] r%d  ", r);
                for (int p = 0; p < N; p++) {
                    if (r == p) printf("   self   ");
                    else printf("  %-6s  ", gmat[r * N + p] ? "OK" : "FAIL");
                }
                printf("\n");
            }
        }

        /* 准备自己的 4MB 段: pat(rank, 0x80, rank*seg_words + i) */
        uint32_t *seg = hbuf;
        for (size_t i = 0; i < seg_words; i++)
            seg[i] = pat((uint32_t)rank, TAG_DATA, (size_t)rank * seg_words + i);
        ret = aclrtMemcpy((uint8_t *)scratch + (size_t)rank * SEG_SIZE, SEG_SIZE,
                          seg, SEG_SIZE, ACL_MEMCPY_HOST_TO_DEVICE);
        if (ret != ACL_ERROR_NONE) {
            printf("[SP] r%d write: H2D own segment FAIL ret=%d\n", rank, ret);
            BAIL(fatal);
        }

        if (!fatal) {
            int delivered[N];
            for (int p = 0; p < N; p++) delivered[p] = 0;
            /* 写每个 import 成功的 peer + 自己 */
            for (int p = 0; p < N; p++) {
                void *dst = (p == rank) ? slab : ipc_va[p];
                int usable = (p == rank) ? 1 : imp_ok[p];
                if (!usable || !dst) continue;
                ret = aclrtMemcpyAsync((uint8_t *)dst + (size_t)rank * SEG_SIZE, SEG_SIZE,
                                       (uint8_t *)scratch + (size_t)rank * SEG_SIZE, SEG_SIZE,
                                       ACL_MEMCPY_DEVICE_TO_DEVICE, stream);
                if (ret == ACL_ERROR_NONE)
                    ret = aclrtSynchronizeStream(stream);
                delivered[p] = (ret == ACL_ERROR_NONE);
                printf("[SP] r%d seg-write 4MB -> p%d: %s (ret=%d)\n",
                       rank, p, delivered[p] ? "OK" : "FAIL", ret);
                if (!delivered[p]) g_fail = 1;
            }
            int dmat[N * N];
            MPI_Allreduce(delivered, dmat, N, MPI_INT, MPI_MAX, MPI_COMM_WORLD);
            (void)dmat;

            /* 回读校验: writer w 写过我 <=> gmat[w*N + rank] (w 成功 import 我) */
            MPI_Barrier(MPI_COMM_WORLD);
            memset(vbuf, 0, SLAB_SIZE);
            ret = aclrtMemcpy(vbuf, SLAB_SIZE, slab, SLAB_SIZE, ACL_MEMCPY_DEVICE_TO_HOST);
            if (ret != ACL_ERROR_NONE) {
                printf("[SP] r%d verify: D2H FAIL ret=%d\n", rank, ret);
                g_fail = 1;
            } else {
                int seg_ok = 0, seg_tot = 0;
                for (int w = 0; w < N; w++) {
                    if (!gmat[w * N + rank]) continue;   /* w 没 import 我, 无段可验 */
                    seg_tot++;
                    int ok = 1;
                    uint32_t *base = vbuf + (size_t)w * seg_words;
                    for (size_t i = 0; i < seg_words; i++) {
                        if (base[i] != pat((uint32_t)w, TAG_DATA, (size_t)w * seg_words + i)) {
                            printf("[SP] r%d [verify] seg(w=%d) mismatch at +%zu: got 0x%08x want 0x%08x\n",
                                   rank, w, i, base[i],
                                   pat((uint32_t)w, TAG_DATA, (size_t)w * seg_words + i));
                            ok = 0;
                            break;
                        }
                    }
                    printf("[SP] r%d verify seg(writer p%d): %s\n", rank, w, ok ? "OK" : "FAIL");
                    if (!ok) g_fail = 1;
                    seg_ok += ok;
                }
                printf("[SP] r%d verify summary: %d/%d segments OK\n", rank, seg_ok, seg_tot);
            }
        }
    }
    sync_state(&fatal);

    /* ---- RESULT ---- */
    {
        int res = g_fail;
        MPI_Allreduce(MPI_IN_PLACE, &res, 1, MPI_INT, MPI_MAX, MPI_COMM_WORLD);
        g_fail = res;
        int joined = 0;
        for (int p = 0; p < N; p++)
            if (p != rank && imp_ok[p]) joined++;
        printf("[SP] r%d RESULT: %s (import:%d/3, data-check:%s)\n", rank,
               g_fail ? "FAIL" : "PASS", joined,
               g_fail ? "see above" : "OK");
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
    /* 只 disable 自己 enable 过的: rank0->dev2, rank2->dev0 */
    if (dev_ok) {
        if (rank == 0) {
            aclError dr = aclrtDeviceDisablePeerAccess(2);
            printf("[SP] r0 DisablePeerAccess(dev2) ret=%d\n", dr);
        } else if (rank == 2) {
            aclError dr = aclrtDeviceDisablePeerAccess(0);
            printf("[SP] r2 DisablePeerAccess(dev0) ret=%d\n", dr);
        }
        aclrtResetDevice(dev);
    }
    if (init_ok) aclFinalize();

    MPI_Finalize();
    return g_fail ? 1 : 0;
}
