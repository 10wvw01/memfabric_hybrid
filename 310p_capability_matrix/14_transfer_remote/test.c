/*
 * 14_transfer_remote — 远端 slab 直达 H2D/D2H
 * 机器: Atlas 300I Duo (310P3, 4 卡 x 2 die = 8 dev), CANN 9.1.0, aarch64
 * 结构: MPI 双进程 rank0=exporter(dev A, 环境变量 A_DEV, 默认 0)
 *                     rank1=importer(dev B, 环境变量 B_DEV, 默认 1)  <- die 对
 *                     (或 A_DEV=0 B_DEV=2 跨卡)
 *
 * 验证: imported VA 上的直接 host↔远端 NPU 传输(不经 host 中转,一步直达)。
 *       即池上的传输能力 —— host 进程对远端 NPU 内存的直接 H2D/D2H。
 *
 * die 对判定: 同卡(A_DEV/2 == B_DEV/2)→ DEFAULT import; 跨卡 → PEER import。
 *
 * 校验 pattern: value = (rank<<28)|(tag<<24)|(idx&0xFFFFFF)
 *   rank = 写入方 MPI rank, tag 每步不同(防残留数据造成假阳性)。
 *
 * 测试项(按编号顺序执行):
 *   1  exporter malloc P2P 32MB + H2D pattern(TAG_EXP=0x14) + export key1 +
 *      MPI 发 key
 *   2  importer IPC import key1(DEFAULT if die / PEER if 跨卡) → imported VA
 *   3  直达 D2H 读远端: aclrtMemcpy(host_buf, 16MB, imported_VA, 16MB, D2H) →
 *      逐字校验(rank0/TAG_EXP)
 *   4  直达 H2D 写远端: importer 填 host pattern(TAG_REMOTE=0x1E) →
 *      aclrtMemcpy(imported_VA, 16MB, host_buf, 16MB, H2D) → MPI 通知 →
 *      exporter D2H 校验(rank1/TAG_REMOTE)
 *   5  async D2H 读远端: aclrtMemcpyAsync(host_buf, 16MB, imported_VA, 16MB,
 *      D2H, stream) + Sync → 校验(读第二 16MB, 仍是 rank0/TAG_EXP 原始 pattern)
 *   6  带宽对比: sync D2H 远端 vs async D2H 远端
 *      - async 路: 3 预热 + 30×16MB MemcpyAsync 提交 + 末尾 1 次 StreamSync
 *        (主口径, 与全矩阵其它测试一致)
 *      - sync 路: 仅作对照, 3 预热 + 30 次同步 aclrtMemcpy 调用计时
 *   7  pinned host 内存 D2H 带宽: imported VA -> aclrtMallocHost pinned buffer
 *      验证 pageable vs pinned 是带宽差异的主因
 *   8  本地 dev memory D2H 带宽: 本地 dev -> pinned host
 *      验证数据路径无差异(imported VA vs 本地 dev 带宽应一致)
 *
 * ★带宽测量铁律: 严禁同步循环测带宽!
 *   历史教训(02_ipc_die): 曾用同步 aclrtMemcpy 循环测跨 die D2D 带宽, 得到
 *   114 GB/s 假带宽 —— 失败链路上同步调用每次直接失败(507899), 循环量到的只是
 *   失败返回的延迟(~140us/次), 并非数据搬运。
 *   本测试的 sync 路径是对一条"已知可用、step3 已逐字校验通过"的 host 直达
 *   D2H 链路做 sync-vs-async 对照(每次同步调用都真实搬运 16MB 数据, 非失败
 *   返回的延迟), 因此 sync 计时反映真实搬运带宽而非失败延迟。仅在 step3 OK
 *   后才执行 sync 带宽测试 —— 若 host 直达链路本身坏则直接跳过, 不踩假带宽坑。
 *   async 路径遵循主口径(3 预热 + 30 async + 1 Sync), 严禁同步循环。
 *
 * 判定: 正向 1/2/3/4/5 全 OK -> PASS; 带宽数值仅作对比观测, 不影响判定。
 *
 * cleanup: 双方各自 aclrtIpcMemClose(key); 物理内存只由 exporter aclrtFree;
 *          跨卡时 ResetDevice 前加 aclrtDeviceDisablePeerAccess(peer)(D 态修复)。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <mpi.h>
#include "acl/acl.h"

#define IPC_KEY_LEN      65
#define DEV_MEM_SIZE     (32u * 1024u * 1024u)  /* 32 MB 主块(exporter) */
#define BW_SIZE          (16u * 1024u * 1024u)  /* 16 MB 直达/带宽块 */
#define BW_WARMUP        3
#define BW_ITERS         30

#define TAG_EXP    0x14   /* step1: exporter 写入(step3 sync D2H 读校验; step5 async 读第二块校验) */
#define TAG_REMOTE 0x1E   /* step4: importer host 直达写远端(exporter 校验) */

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

static int check_pat_offset(const uint32_t *buf, size_t words, uint32_t rank, uint32_t tag, size_t idx_offset)
{
    for (size_t i = 0; i < words; i++) {
        if (buf[i] != pat(rank, tag, i + idx_offset)) {
            printf("[14]   [verify] mismatch at word %zu (off 0x%zx): got 0x%08x, want 0x%08x\n",
                   i, i * sizeof(uint32_t), buf[i], pat(rank, tag, i + idx_offset));
            return -1;
        }
    }
    return 0;
}

static int check_pat(const uint32_t *buf, size_t words, uint32_t rank, uint32_t tag)
{
    return check_pat_offset(buf, words, rank, tag, 0);
}

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static int32_t env_dev(const char *name, int32_t dflt)
{
    const char *s = getenv(name);
    return s ? (int32_t)atoi(s) : dflt;
}

/* 正向步骤行: ok=0 记 FAIL */
static void step_line(int n, const char *who, const char *desc, int ok, int32_t ret)
{
    printf("[14] step %d (%s): %-58s %s (ret=%d)\n", n, who, desc,
           ok ? "OK" : "FAIL", ret);
    fflush(stdout);
    if (!ok)
        g_fail = 1;
}

/* 阶段栅栏 + fatal/g_fail 全局同步: 任一 rank 置位, 两边都看到 */
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
    if (nproc != 2) {
        if (rank == 0)
            printf("[14] FATAL: need exactly 2 MPI procs (-np 2), got %d\n", nproc);
        MPI_Finalize();
        return 1;
    }

    const char *role = (rank == 0) ? "r0/exporter" : "r1/importer";
    const char *who = (rank == 0) ? "r0" : "r1";
    int32_t a_dev = env_dev("A_DEV", 0);
    int32_t b_dev = env_dev("B_DEV", 1);
    int32_t dev = (rank == 0) ? a_dev : b_dev;
    /* die 对判定: 同卡(dev/2 相等)→ DEFAULT import; 跨卡 → PEER import */
    int cross_card = (a_dev / 2) != (b_dev / 2);
    uint32_t imp_flag = cross_card ? ACL_RT_IPC_MEM_IMPORT_FLAG_ENABLE_PEER_ACCESS
                                   : ACL_RT_IPC_MEM_IMPORT_FLAG_DEFAULT;

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("[14] === 14_transfer_remote: 远端 slab 直达 H2D/D2H, rank%d %s dev%d, %s ===\n",
           rank, role, dev, cross_card ? "cross-card(PEER import)" : "die-pair(DEFAULT import)");
    printf("[14] %s: A_DEV=%d B_DEV=%d, 32MB P2P main, 16MB remote-direct block, "
           "pattern=(rank<<28)|(tag<<24)|(idx&0xFFFFFF)\n", role, a_dev, b_dev);

    aclrtStream stream = NULL;
    void *da = NULL;
    void *p = NULL;
    uint32_t *hbuf = NULL, *vbuf = NULL;
    void *pinned_buf = NULL;
    void *local_dev = NULL;
    char key1[IPC_KEY_LEN] = {0};
    int exp1_ok = 0, imp1_ok = 0;
    int fatal = 0, init_ok = 0, dev_ok = 0;
    int32_t ret = 0;
    size_t words = DEV_MEM_SIZE / sizeof(uint32_t);
    size_t hw = BW_SIZE / sizeof(uint32_t);

    /* ---- phase 0: init ---- */
    ret = aclInit(NULL);
    if (ret != ACL_ERROR_NONE) {
        printf("[14] init: %s aclInit FAIL ret=%d\n", role, ret);
        BAIL(fatal);
    } else {
        init_ok = 1;
        ret = aclrtSetDevice(dev);
        if (ret != ACL_ERROR_NONE) {
            printf("[14] init: %s aclrtSetDevice(%d) FAIL ret=%d\n", role, dev, ret);
            BAIL(fatal);
        }
    }
    if (!fatal) {
        dev_ok = 1;
        if (rank == 1) {
            ret = aclrtCreateStream(&stream);
            if (ret != ACL_ERROR_NONE) {
                printf("[14] init: r1 aclrtCreateStream FAIL ret=%d\n", ret);
                BAIL(fatal);
            }
        }
        hbuf = (uint32_t *)malloc(DEV_MEM_SIZE);
        vbuf = (uint32_t *)malloc(DEV_MEM_SIZE);
        if (!hbuf || !vbuf) {
            printf("[14] init: %s host malloc 2x32MB FAIL\n", role);
            BAIL(fatal);
        }
        if (rank == 1) {
            ret = aclrtMallocHost(&pinned_buf, DEV_MEM_SIZE);
            if (ret != ACL_ERROR_NONE) {
                printf("[14] init: r1 aclrtMallocHost 32MB FAIL ret=%d\n", ret);
                BAIL(fatal);
            }
        }
    }
    if (!fatal)
        printf("[14] init: %s aclInit/SetDevice(%d)%s OK\n", role, dev,
               (rank == 1) ? "/CreateStream" : "");
    sync_state(&fatal);

    /* ---- phase 1 (step 1): exporter 建块/写 pattern/export key1/发 key ---- */
    if (!fatal) {
        if (rank == 0) {
            int ok = 0;
            ret = aclrtMalloc(&da, DEV_MEM_SIZE, ACL_MEM_MALLOC_HUGE_FIRST_P2P);
            if (ret == ACL_ERROR_NONE) {
                fill_pat(hbuf, words, 0, TAG_EXP);
                ret = aclrtMemcpy(da, DEV_MEM_SIZE, hbuf, DEV_MEM_SIZE,
                                  ACL_MEMCPY_HOST_TO_DEVICE);
            }
            if (ret == ACL_ERROR_NONE)
                ret = aclrtIpcMemGetExportKey(da, DEV_MEM_SIZE, key1, IPC_KEY_LEN,
                                              ACL_RT_IPC_MEM_EXPORT_FLAG_DISABLE_PID_VALIDATION);
            if (ret == ACL_ERROR_NONE) {
                exp1_ok = 1;
                if (MPI_Send(key1, IPC_KEY_LEN, MPI_CHAR, 1, 100, MPI_COMM_WORLD) == MPI_SUCCESS)
                    ok = 1;
            }
            step_line(1, who, "malloc P2P 32MB + H2D pattern + export key1 + MPI-send",
                      ok, ret);
            if (!ok)
                BAIL(fatal);
        } else {
            MPI_Recv(key1, IPC_KEY_LEN, MPI_CHAR, 0, 100, MPI_COMM_WORLD,
                     MPI_STATUS_IGNORE);
        }
    }
    sync_state(&fatal);

    /* ---- phase 2 (step 2): importer IPC import → imported VA ---- */
    if (!fatal) {
        if (rank == 1) {
            int ok = 0;
            ret = aclrtIpcMemImportByKey(&p, key1, imp_flag);
            if (ret == ACL_ERROR_NONE)
                imp1_ok = 1;
            ok = (ret == ACL_ERROR_NONE);
            step_line(2, who,
                      cross_card ? "import PEER_ACCESS -> imported VA (remote dev A visible)"
                                 : "import DEFAULT -> imported VA (remote dev A visible)",
                      ok, ret);
            if (!ok)
                BAIL(fatal);
        }
    }
    sync_state(&fatal);

    /* ---- phase 3 (step 3): 直达 D2H 读远端(imported VA 第一 16MB) + 逐字校验 ---- */
    if (!fatal) {
        if (rank == 1) {
            int ok = 0;
            memset(vbuf, 0, BW_SIZE);
            ret = aclrtMemcpy(vbuf, BW_SIZE, p, BW_SIZE, ACL_MEMCPY_DEVICE_TO_HOST);
            if (ret == ACL_ERROR_NONE)
                ok = (check_pat(vbuf, hw, 0, TAG_EXP) == 0);
            step_line(3, who, "host direct D2H read remote 16MB (imported VA) + verify",
                      ok, ret);
            if (!ok)
                BAIL(fatal);
        }
    }
    sync_state(&fatal);

    /* ---- phase 4 (step 4): 直达 H2D 写远端(imported VA 第一 16MB) + exporter 校验 ---- */
    if (!fatal) {
        if (rank == 1) {
            int ok = 0;
            fill_pat(hbuf, hw, 1, TAG_REMOTE);
            ret = aclrtMemcpy(p, BW_SIZE, hbuf, BW_SIZE, ACL_MEMCPY_HOST_TO_DEVICE);
            ok = (ret == ACL_ERROR_NONE);
            step_line(4, who, "host direct H2D write remote 16MB (imported VA)",
                      ok, ret);
            if (!ok)
                BAIL(fatal);
        }
    }
    sync_state(&fatal);   /* 写落地后 exporter 才校验 */
    if (!fatal) {
        if (rank == 0) {
            int ok = 0;
            memset(vbuf, 0, BW_SIZE);
            ret = aclrtMemcpy(vbuf, BW_SIZE, da, BW_SIZE, ACL_MEMCPY_DEVICE_TO_HOST);
            if (ret == ACL_ERROR_NONE)
                ok = (check_pat(vbuf, hw, 1, TAG_REMOTE) == 0);
            step_line(4, who, "D2H verify host-direct-written 16MB (r1/TAG_REMOTE) on own VA",
                      ok, ret);
            if (!ok)
                BAIL(fatal);
        }
    }
    sync_state(&fatal);

    /* ---- phase 5 (step 5): async D2H 读远端(imported VA 第二 16MB, 仍是 rank0/TAG_EXP 原始 pattern) ---- */
    if (!fatal) {
        if (rank == 1) {
            int ok = 0;
            void *p2 = (void *)((char *)p + BW_SIZE);   /* 第二 16MB, step4 未触及 */
            memset(vbuf, 0, BW_SIZE);
            ret = aclrtMemcpyAsync(vbuf, BW_SIZE, p2, BW_SIZE,
                                   ACL_MEMCPY_DEVICE_TO_HOST, stream);
            if (ret == ACL_ERROR_NONE)
                ret = aclrtSynchronizeStream(stream);
            if (ret == ACL_ERROR_NONE)
                ok = (check_pat_offset(vbuf, hw, 0, TAG_EXP, hw) == 0);
            step_line(5, who, "async D2H read remote 16MB #2 (imported VA + 16MB) + verify",
                      ok, ret);
            if (!ok)
                BAIL(fatal);
        }
    }
    sync_state(&fatal);

    /* ---- phase 6 (step 6): 带宽对比 sync D2H 远端 vs async D2H 远端 ----
     * ★严禁同步循环测带宽(历史教训见文件头)。本测试 sync 路径仅作对照:
     *   - 仅在 step3 已逐字校验通过(host 直达 D2H 链路真实可用)后才执行 sync
     *     带宽测试 —— 每次同步调用都真实搬运 16MB 数据, 非失败返回的延迟;
     *   - async 路径遵循主口径 3 预热 + 30x16MB async 提交 + 末尾 1 次 Sync。
     *   若 host 直达链路本身坏(step3 FAIL 已 fatal), 直接跳过, 不踩假带宽坑。 */
    if (!fatal && g_fail) {
        if (rank == 1)
            printf("[14] step 6 (r1): bandwidth SKIPPED (correctness not passed)\n");
    }
    if (!fatal && !g_fail) {
        if (rank == 1) {
            double t0, t1, gb;
            printf("[14] step 6 (r1): bandwidth sync D2H vs async D2H on imported VA, %u MB x %d iters + %d warmup\n",
                   BW_SIZE >> 20, BW_ITERS, BW_WARMUP);
            gb = (double)BW_ITERS * (double)BW_SIZE / 1e9;

            /* async 路(主口径): 3 预热 + 30x16MB MemcpyAsync + 1 次 StreamSync */
            for (int i = 0; i < BW_WARMUP; i++) {
                aclrtMemcpyAsync(vbuf, BW_SIZE, p, BW_SIZE,
                                 ACL_MEMCPY_DEVICE_TO_HOST, stream);
                aclrtSynchronizeStream(stream);
            }
            t0 = now_sec();
            for (int i = 0; i < BW_ITERS; i++)
                aclrtMemcpyAsync(vbuf, BW_SIZE, p, BW_SIZE,
                                 ACL_MEMCPY_DEVICE_TO_HOST, stream);
            aclrtSynchronizeStream(stream);
            t1 = now_sec();
            printf("[14] bw   async D2H-read (remote->host) : %7.2f GB/s "
                   "(30x16MB async + 1 Sync, total %.3f s)\n", gb / (t1 - t0), t1 - t0);

            /* sync 路(对照): 仅 step3 已校验通过的可用链路上; 3 预热 + 30 次同步调用计时
             *   —— 每次同步调用都真实搬运 16MB(非失败延迟), 反映真实 sync 搬运带宽。 */
            for (int i = 0; i < BW_WARMUP; i++)
                aclrtMemcpy(vbuf, BW_SIZE, p, BW_SIZE, ACL_MEMCPY_DEVICE_TO_HOST);
            t0 = now_sec();
            for (int i = 0; i < BW_ITERS; i++)
                aclrtMemcpy(vbuf, BW_SIZE, p, BW_SIZE, ACL_MEMCPY_DEVICE_TO_HOST);
            t1 = now_sec();
            printf("[14] bw   sync  D2H-read (remote->host) : %7.2f GB/s "
                   "(30x16MB sync calls, total %.3f s) [对照, 已知可用链路]\n",
                   gb / (t1 - t0), t1 - t0);
            fflush(stdout);
        }
    }
    sync_state(&fatal);

    /* ---- phase 7 (step 7): pinned host 内存 D2H 带宽 (验证 pageable 是主因) ---- */
    if (!fatal && !g_fail) {
        if (rank == 1) {
            double t0, t1, gb;
            printf("[14] step 7 (r1): D2H imported VA -> pinned host (aclrtMallocHost), %u MB x %d iters + %d warmup\n",
                   BW_SIZE >> 20, BW_ITERS, BW_WARMUP);
            gb = (double)BW_ITERS * (double)BW_SIZE / 1e9;

            for (int i = 0; i < BW_WARMUP; i++)
                aclrtMemcpy(pinned_buf, BW_SIZE, p, BW_SIZE, ACL_MEMCPY_DEVICE_TO_HOST);
            t0 = now_sec();
            for (int i = 0; i < BW_ITERS; i++)
                aclrtMemcpy(pinned_buf, BW_SIZE, p, BW_SIZE, ACL_MEMCPY_DEVICE_TO_HOST);
            t1 = now_sec();
            printf("[14] bw   sync D2H pinned (remote->host): %7.2f GB/s "
                   "(30x16MB sync calls, total %.3f s) [pinned host, DMA 直达]\n",
                   gb / (t1 - t0), t1 - t0);
            fflush(stdout);
        }
    }
    sync_state(&fatal);

    /* ---- phase 8 (step 8): 本地 dev memory D2H 带宽 (验证数据路径无差异) ---- */
    if (!fatal && !g_fail) {
        if (rank == 1) {
            double t0, t1, gb;

            ret = aclrtMalloc(&local_dev, BW_SIZE, ACL_MEM_MALLOC_HUGE_FIRST);
            if (ret == ACL_ERROR_NONE) {
                fill_pat(hbuf, hw, 1, 0x80);
                ret = aclrtMemcpy(local_dev, BW_SIZE, hbuf, BW_SIZE, ACL_MEMCPY_HOST_TO_DEVICE);
            }
            if (ret != ACL_ERROR_NONE) {
                printf("[14] step 8 (r1): local dev alloc/H2D FAIL ret=%d\n", ret);
                BAIL(fatal);
            }

            printf("[14] step 8 (r1): D2H local dev -> pinned host, %u MB x %d iters + %d warmup\n",
                   BW_SIZE >> 20, BW_ITERS, BW_WARMUP);
            gb = (double)BW_ITERS * (double)BW_SIZE / 1e9;

            for (int i = 0; i < BW_WARMUP; i++)
                aclrtMemcpy(pinned_buf, BW_SIZE, local_dev, BW_SIZE, ACL_MEMCPY_DEVICE_TO_HOST);
            t0 = now_sec();
            for (int i = 0; i < BW_ITERS; i++)
                aclrtMemcpy(pinned_buf, BW_SIZE, local_dev, BW_SIZE, ACL_MEMCPY_DEVICE_TO_HOST);
            t1 = now_sec();
            printf("[14] bw   sync D2H local  (local->host) : %7.2f GB/s "
                   "(30x16MB sync calls, total %.3f s) [本地 dev, pinned host, 对照基线]\n",
                   gb / (t1 - t0), t1 - t0);
            fflush(stdout);

            aclrtFree(local_dev);
            local_dev = NULL;
        }
    }
    sync_state(&fatal);

    /* ---- cleanup: 双方各自 close key; 物理内存只由 exporter free ---- */
    if (rank == 1) {
        if (imp1_ok) {
            ret = aclrtIpcMemClose(key1);
            printf("[14] cleanup (r1): IpcMemClose(key1) ret=%d\n", ret);
        }
    } else {
        if (exp1_ok) {
            ret = aclrtIpcMemClose(key1);
            printf("[14] cleanup (r0): IpcMemClose(key1) ret=%d\n", ret);
        }
    }
    MPI_Barrier(MPI_COMM_WORLD);   /* 双方 close 完成后 exporter 才 free 物理内存 */
    if (rank == 0) {
        if (da)
            aclrtFree(da);
        printf("[14] cleanup (r0): physical mem freed by exporter only\n");
    }
    if (stream)
        aclrtDestroyStream(stream);
    if (pinned_buf)
        aclrtFreeHost(pinned_buf);
    free(hbuf);
    free(vbuf);
    /* ★修复 D 态挂死: 跨卡 PEER import 内部 EnablePeerAccess 建立的 P2P 状态
     *   必须在 ResetDevice 前显式拆除, 否则进程退出时 mmu notifier 释放与
     *   驱动 SRCU 死锁(exit_mmap → synchronize_srcu 永久等待, SIGKILL 不可回收)。
     *   根因: 02(die 对无 EnablePeer)正常退出; 03/04(跨卡 EnablePeer)挂死。 */
    if (dev_ok) {
        int32_t peer = (rank == 0) ? b_dev : a_dev;
        if (cross_card && (imp1_ok || exp1_ok)) {
            aclError dr = aclrtDeviceDisablePeerAccess(peer);
            printf("[14] cleanup (r%d): DisablePeerAccess(%d) ret=%d\n", rank, peer, dr);
        }
        aclrtResetDevice(dev);
    }
    if (init_ok)
        aclFinalize();

    /* ---- RESULT(聚合两 rank 状态; 两 rank 打印同一行) ---- */
    {
        int res = g_fail;
        MPI_Allreduce(MPI_IN_PLACE, &res, 1, MPI_INT, MPI_MAX, MPI_COMM_WORLD);
        g_fail = res;
        printf("[14] RESULT: %s\n", g_fail ? "FAIL" : "PASS");
        fflush(stdout);
    }

    MPI_Finalize();
    return g_fail ? 1 : 0;
}
