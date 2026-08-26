/*
 * 08_host_swap — host 中转兜底(D2H + H2D 跨设备)
 * 机器: Atlas 300I Duo (310P3, 4 卡 x 2 die = 8 dev), CANN 9.1.0, aarch64
 * 结构: MPI 双进程 rank0=exporter(A_DEV, 默认 0) / rank1=importer(B_DEV, 默认 1)
 *
 * 结论(实测): host-swap 是任何 IPC 链路上的保底路径, 带宽 ~3 GB/s,
 *   与 direct D2D 的差距 die 对 ~6x / 跨卡 ~3.5x。
 *
 * 测试项:
 *   1  exporter malloc P2P 32MB + H2D pattern(TAG_EXP) + export key + MPI 发 key
 *   2  importer import key(DEFAULT if die 对 / PEER if 跨卡) + malloc 本地 32MB
 *   3  host-swap 读: D2H(remote→host) + H2D async(host→local) + 校验
 *   4  host-swap 写: 填 local + D2H async(local→host) + H2D(host→remote) + exporter 校验
 *   5  带宽对比: direct async D2D vs host-swap
 *
 * ★带宽测量铁律: 严禁同步循环测带宽!
 *   失败链路上同步调用每次直接失败(507899), 循环量到的只是失败延迟(~140us/次,
 *   曾伪造 114 GB/s)。正确口径: 3 预热 + 30x16MB async 提交 + 末尾 1 次 Sync;
 *   host-swap 按有效交付 30x16MB=480MB 与 direct 同口径对比(总线流量 2x)。
 *
 * 判定: 步骤 1/2/3/4 全 OK -> PASS; 带宽数值仅观测, 不影响判定。
 * cleanup: 双方各自 IpcMemClose; 物理内存只由 exporter Free;
 *          跨卡时 ResetDevice 前 DisablePeerAccess(D 态修复, 见尾部注释)。
 *
 * 校验 pattern: value = (rank<<28)|(tag<<24)|(idx&0xFFFFFF), tag 每步不同防残留假阳性。
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
#define BW_SIZE          (16u * 1024u * 1024u)  /* 16 MB host-swap/带宽块 */
#define BW_WARMUP        3
#define BW_ITERS         30

#define TAG_EXP  0x8   /* step1: exporter 写入(step3 host-swap 读校验) */
#define TAG_LOC  0x9   /* step4: importer 本地写入(host-swap 写到远端; exporter 校验) */

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

static int check_pat(const uint32_t *buf, size_t words, uint32_t rank, uint32_t tag)
{
    for (size_t i = 0; i < words; i++) {
        if (buf[i] != pat(rank, tag, i)) {
            printf("[08]   [verify] mismatch at word %zu (off 0x%zx): got 0x%08x, want 0x%08x\n",
                   i, i * sizeof(uint32_t), buf[i], pat(rank, tag, i));
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

static int32_t env_dev(const char *name, int32_t dflt)
{
    const char *s = getenv(name);
    return s ? (int32_t)atoi(s) : dflt;
}

/* 正向步骤行: ok=0 记 FAIL */
static void step_line(int n, const char *who, const char *desc, int ok, int32_t ret)
{
    printf("[08] step %d (%s): %-58s %s (ret=%d)\n", n, who, desc,
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
            printf("[08] FATAL: need exactly 2 MPI procs (-np 2), got %d\n", nproc);
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
    printf("[08] === 08_host_swap: host 中转兜底(D2H+H2D), rank%d %s dev%d, %s ===\n",
           rank, role, dev, cross_card ? "cross-card(PEER import)" : "die-pair(DEFAULT import)");
    printf("[08] %s: A_DEV=%d B_DEV=%d, 32MB P2P main, 16MB swap block, "
           "pattern=(rank<<28)|(tag<<24)|(idx&0xFFFFFF)\n", role, a_dev, b_dev);

    aclrtStream stream = NULL;      /* importer 的 async 流 */
    void *da = NULL;               /* exporter: 主块 */
    void *p = NULL;                 /* importer: imported VA(远端 dev A 的内存在本进程可见) */
    void *loc = NULL;               /* importer: 本地 dev 缓冲(32MB) */
    uint32_t *hbuf = NULL, *vbuf = NULL;
    char key1[IPC_KEY_LEN] = {0};
    int exp1_ok = 0, imp1_ok = 0;
    int fatal = 0, init_ok = 0, dev_ok = 0;
    int32_t ret = 0;
    size_t words = DEV_MEM_SIZE / sizeof(uint32_t);
    size_t hw = BW_SIZE / sizeof(uint32_t);

    /* ---- phase 0: init ---- */
    ret = aclInit(NULL);
    if (ret != ACL_ERROR_NONE) {
        printf("[08] init: %s aclInit FAIL ret=%d\n", role, ret);
        BAIL(fatal);
    } else {
        init_ok = 1;
        ret = aclrtSetDevice(dev);
        if (ret != ACL_ERROR_NONE) {
            printf("[08] init: %s aclrtSetDevice(%d) FAIL ret=%d\n", role, dev, ret);
            BAIL(fatal);
        }
    }
    if (!fatal) {
        dev_ok = 1;
        if (rank == 1) {
            ret = aclrtCreateStream(&stream);
            if (ret != ACL_ERROR_NONE) {
                printf("[08] init: r1 aclrtCreateStream FAIL ret=%d\n", ret);
                BAIL(fatal);
            }
        }
        hbuf = (uint32_t *)malloc(DEV_MEM_SIZE);
        vbuf = (uint32_t *)malloc(DEV_MEM_SIZE);
        if (!hbuf || !vbuf) {
            printf("[08] init: %s host malloc 2x32MB FAIL\n", role);
            BAIL(fatal);
        }
    }
    if (!fatal)
        printf("[08] init: %s aclInit/SetDevice(%d)%s OK\n", role, dev,
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
            if (ret == ACL_ERROR_NONE)
                exp1_ok = 1;
            /* 无条件发 key(失败时为全 0): 否则 rank1 卡死在 MPI_Recv 而 rank0 卡死在
             * sync_state 的 Allreduce -> 双进程死锁; 失败态由 sync_state 聚合后跳过 import */
            if (MPI_Send(key1, IPC_KEY_LEN, MPI_CHAR, 1, 100, MPI_COMM_WORLD) == MPI_SUCCESS
                && exp1_ok)
                ok = 1;
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

    /* ---- phase 2 (step 2): importer IPC import + malloc 本地 dev 缓冲 ---- */
    if (!fatal) {
        if (rank == 1) {
            int ok = 0;
            ret = aclrtIpcMemImportByKey(&p, key1, imp_flag);
            if (ret == ACL_ERROR_NONE) {
                imp1_ok = 1;
                ret = aclrtMalloc(&loc, DEV_MEM_SIZE, ACL_MEM_MALLOC_HUGE_FIRST);
            }
            ok = (ret == ACL_ERROR_NONE);
            step_line(2, who,
                      cross_card ? "import PEER_ACCESS -> imported VA + malloc local 32MB"
                                 : "import DEFAULT -> imported VA + malloc local 32MB",
                      ok, ret);
            if (!ok)
                BAIL(fatal);
        }
    }
    sync_state(&fatal);

    /* ---- phase 3 (step 3): host-swap 读路径 ----
     * D2H(remote→host) → H2D async(host→local) + Sync → D2H 读回校验(rank0/TAG_EXP) */
    if (!fatal) {
        if (rank == 1) {
            int ok = 0;
            ret = aclrtMemcpy(hbuf, BW_SIZE, p, BW_SIZE, ACL_MEMCPY_DEVICE_TO_HOST);
            if (ret == ACL_ERROR_NONE)
                ret = aclrtMemcpyAsync(loc, BW_SIZE, hbuf, BW_SIZE,
                                       ACL_MEMCPY_HOST_TO_DEVICE, stream);
            if (ret == ACL_ERROR_NONE)
                ret = aclrtSynchronizeStream(stream);
            if (ret == ACL_ERROR_NONE) {
                memset(vbuf, 0, BW_SIZE);
                ret = aclrtMemcpy(vbuf, BW_SIZE, loc, BW_SIZE, ACL_MEMCPY_DEVICE_TO_HOST);
                if (ret == ACL_ERROR_NONE)
                    ok = (check_pat(vbuf, hw, 0, TAG_EXP) == 0);
            }
            step_line(3, who, "host-swap READ: D2H(remote->host) + H2D async(host->local) + verify",
                      ok, ret);
            if (!ok)
                BAIL(fatal);
        }
    }
    sync_state(&fatal);

    /* ---- phase 4 (step 4): host-swap 写路径 ----
     * 填 local(TAG_LOC) → D2H async(local→host) + Sync → H2D(host→remote)
     * → exporter D2H 校验(rank1/TAG_LOC) */
    if (!fatal) {
        if (rank == 1) {
            int ok = 0;
            fill_pat(hbuf, hw, 1, TAG_LOC);
            ret = aclrtMemcpy(loc, BW_SIZE, hbuf, BW_SIZE, ACL_MEMCPY_HOST_TO_DEVICE);
            if (ret == ACL_ERROR_NONE)
                ret = aclrtMemcpyAsync(hbuf, BW_SIZE, loc, BW_SIZE,
                                       ACL_MEMCPY_DEVICE_TO_HOST, stream);
            if (ret == ACL_ERROR_NONE)
                ret = aclrtSynchronizeStream(stream);
            if (ret == ACL_ERROR_NONE)
                ret = aclrtMemcpy(p, BW_SIZE, hbuf, BW_SIZE, ACL_MEMCPY_HOST_TO_DEVICE);
            ok = (ret == ACL_ERROR_NONE);
            step_line(4, who, "host-swap WRITE: fill local + D2H async(local->host) + H2D(host->remote)",
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
                ok = (check_pat(vbuf, hw, 1, TAG_LOC) == 0);
            step_line(4, who, "D2H verify host-swap-written pattern (r1/TAG9) on own VA",
                      ok, ret);
            if (!ok)
                BAIL(fatal);
        }
    }
    sync_state(&fatal);

    /* ---- phase 5 (step 5): 带宽对比 direct async D2D vs host-swap ----
     * 口径见文件头铁律: 3 预热 + 30x16MB async + 1 Sync, 同口径对比 */
    if (!fatal && g_fail) {
        if (rank == 1)
            printf("[08] step 5 (r1): bandwidth SKIPPED (correctness not passed)\n");
    }
    if (!fatal && !g_fail) {
        if (rank == 1) {
            double t0, t1, gb;
            printf("[08] step 5 (r1): bandwidth direct async D2D vs host-swap, %u MB x %d iters + %d warmup, "
                   "async submits + 1 final Sync ONLY (no sync loop!)\n",
                   BW_SIZE >> 20, BW_ITERS, BW_WARMUP);
            gb = (double)BW_ITERS * (double)BW_SIZE / 1e9;

            /* direct async D2D 读: p -> local */
            for (int i = 0; i < BW_WARMUP; i++) {
                aclrtMemcpyAsync(loc, BW_SIZE, p, BW_SIZE,
                                 ACL_MEMCPY_DEVICE_TO_DEVICE, stream);
                aclrtSynchronizeStream(stream);
            }
            t0 = now_sec();
            for (int i = 0; i < BW_ITERS; i++)
                aclrtMemcpyAsync(loc, BW_SIZE, p, BW_SIZE,
                                 ACL_MEMCPY_DEVICE_TO_DEVICE, stream);
            aclrtSynchronizeStream(stream);
            t1 = now_sec();
            printf("[08] bw   direct     D2D-read (p->local)     : %7.2f GB/s "
                   "(30x16MB async + 1 Sync, total %.3f s)\n", gb / (t1 - t0), t1 - t0);

            /* host-swap 读: 每轮 D2H + H2D 两条 async; 有效交付按 30x16MB 计
             * (总线流量 2x, 与 direct 同口径对比) */
            for (int i = 0; i < BW_WARMUP; i++) {
                aclrtMemcpyAsync(hbuf, BW_SIZE, p, BW_SIZE,
                                 ACL_MEMCPY_DEVICE_TO_HOST, stream);
                aclrtMemcpyAsync(loc, BW_SIZE, hbuf, BW_SIZE,
                                 ACL_MEMCPY_HOST_TO_DEVICE, stream);
                aclrtSynchronizeStream(stream);
            }
            t0 = now_sec();
            for (int i = 0; i < BW_ITERS; i++) {
                aclrtMemcpyAsync(hbuf, BW_SIZE, p, BW_SIZE,
                                 ACL_MEMCPY_DEVICE_TO_HOST, stream);
                aclrtMemcpyAsync(loc, BW_SIZE, hbuf, BW_SIZE,
                                 ACL_MEMCPY_HOST_TO_DEVICE, stream);
            }
            aclrtSynchronizeStream(stream);
            t1 = now_sec();
            printf("[08] bw   host-swap read (p->host->local)   : %7.2f GB/s "
                   "(30x16MB eff, 60 async + 1 Sync, total %.3f s)\n", gb / (t1 - t0), t1 - t0);
            fflush(stdout);
        }
    }
    sync_state(&fatal);

    /* ---- cleanup: 双方各自 close key; 物理内存只由 exporter free ---- */
    if (rank == 1) {
        if (imp1_ok) {
            ret = aclrtIpcMemClose(key1);
            printf("[08] cleanup (r1): IpcMemClose(key1) ret=%d\n", ret);
        }
    } else {
        if (exp1_ok) {
            ret = aclrtIpcMemClose(key1);
            printf("[08] cleanup (r0): IpcMemClose(key1) ret=%d\n", ret);
        }
    }
    MPI_Barrier(MPI_COMM_WORLD);   /* 双方 close 完成后 exporter 才 free 物理内存 */
    if (rank == 0) {
        if (da)
            aclrtFree(da);
        printf("[08] cleanup (r0): physical mem freed by exporter only\n");
    }
    if (rank == 1 && loc)
        aclrtFree(loc);
    if (stream)
        aclrtDestroyStream(stream);
    free(hbuf);
    free(vbuf);
    /* ★D 态修复: 跨卡 PEER import 内部 EnablePeerAccess 建立的 P2P 状态必须在
     *   ResetDevice 前显式拆除, 否则退出时 mmu notifier 与驱动 SRCU 死锁
     *   (exit_mmap → synchronize_srcu 永久等待, SIGKILL 不可回收)。 */
    if (dev_ok) {
        int32_t peer = (rank == 0) ? b_dev : a_dev;
        if (cross_card && (imp1_ok || exp1_ok)) {
            aclError dr = aclrtDeviceDisablePeerAccess(peer);
            printf("[08] cleanup (r%d): DisablePeerAccess(%d) ret=%d\n", rank, peer, dr);
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
        printf("[08] RESULT: %s\n", g_fail ? "FAIL" : "PASS");
        fflush(stdout);
    }

    MPI_Finalize();
    return g_fail ? 1 : 0;
}
