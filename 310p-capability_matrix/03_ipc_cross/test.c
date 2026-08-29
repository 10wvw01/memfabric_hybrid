/*
 * 03_ipc_cross — IPC key 链路跨卡(PCIe)测试
 * Atlas 300I Duo (310P3, 4 卡 x 2 die = 8 dev), CANN 9.1.0, aarch64
 * MPI 双进程: rank0=exporter(dev A, A_DEV 默认 0)
 *             rank1=importer(dev B, B_DEV 默认 2)  <- 跨卡
 *
 * 校验 pattern: value = (rank<<28)|(tag<<24)|(idx&0xFFFFFF), tag 每步不同防残留假阳性。
 *
 * 测试项(执行序 1,2,5,4):
 *   1  exporter malloc P2P 32MB + H2D pattern + export key1 + MPI 发 key
 *   2  importer PEER_ACCESS import + async D2D 双向读校验 + exporter 回读
 *   5  host 直达 imported VA 的 D2H/H2D 各 16MB + 双向校验
 *   4  带宽: PEER imported VA, 3 预热 + 30x16MB async + 末尾 1 Sync(禁同步循环)
 *
 * 判定: 正向 1/2/5 全 OK -> PASS。
 * cleanup: 双方 IpcMemClose + exporter aclrtFree; ResetDevice 前双方
 *   DisablePeerAccess 拆 P2P(防 D 态挂死)。跨卡裸 DEFAULT 对照见 15_peer_needed。
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
#define BW_SIZE          (16u * 1024u * 1024u)  /* 16 MB 带宽/host直达块 */
#define BW_WARMUP        3
#define BW_ITERS         30

#define TAG_EXP    0x2   /* step1: exporter 写入(step2 读校验) */
#define TAG_BACK   0x3   /* step2: importer 反向写(step5 读校验) */
#define TAG_HOST_W 0x4   /* step5: importer host 直达写 */

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
            printf("[03]   [verify] mismatch at word %zu (off 0x%zx): got 0x%08x, want 0x%08x\n",
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

static void step_line(int n, const char *who, const char *desc, int ok, int32_t ret)
{
    printf("[03] step %d (%s): %-58s %s (ret=%d)\n", n, who, desc,
           ok ? "OK" : "FAIL", ret);
    fflush(stdout);
    if (!ok)
        g_fail = 1;
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
    if (nproc != 2) {
        if (rank == 0)
            printf("[03] FATAL: need exactly 2 MPI procs (-np 2), got %d\n", nproc);
        MPI_Finalize();
        return 1;
    }

    const char *role = (rank == 0) ? "r0/exporter" : "r1/importer";
    const char *who = (rank == 0) ? "r0" : "r1";
    int32_t dev = (rank == 0) ? env_dev("A_DEV", 0) : env_dev("B_DEV", 2);

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("[03] === 03_ipc_cross: IPC key link cross-card (PCIe), rank%d %s dev%d ===\n",
           rank, role, dev);
    printf("[03] %s: %s=%d, 32MB P2P main, key len %dB, "
           "pattern=(rank<<28)|(tag<<24)|(idx&0xFFFFFF)\n",
           role, (rank == 0) ? "A_DEV" : "B_DEV", dev, IPC_KEY_LEN);

    aclrtStream stream = NULL;      /* importer 的 async 流 */
    void *da = NULL;                /* exporter: 主块 */
    void *p = NULL;                 /* importer: PEER import VA */
    void *loc = NULL;               /* importer: 本地 dev 缓冲(32MB) */
    uint32_t *hbuf = NULL, *vbuf = NULL;
    char key1[IPC_KEY_LEN] = {0};
    int exp1_ok = 0, imp1_ok = 0;
    int fatal = 0;
    int32_t ret = 0;
    int init_ok = 0, dev_ok = 0;
    size_t words = DEV_MEM_SIZE / sizeof(uint32_t);
    size_t hw = BW_SIZE / sizeof(uint32_t);

    /* ---- phase 0: init ---- */
    ret = aclInit(NULL);
    if (ret != ACL_ERROR_NONE) {
        printf("[03] init: %s aclInit FAIL ret=%d\n", role, ret);
        BAIL(fatal);
    } else {
        init_ok = 1;
        ret = aclrtSetDevice(dev);
        if (ret != ACL_ERROR_NONE) {
            printf("[03] init: %s aclrtSetDevice(%d) FAIL ret=%d\n", role, dev, ret);
            BAIL(fatal);
        }
    }
    if (!fatal) {
        dev_ok = 1;
        if (rank == 1) {
            ret = aclrtCreateStream(&stream);
            if (ret != ACL_ERROR_NONE) {
                printf("[03] init: r1 aclrtCreateStream FAIL ret=%d\n", ret);
                BAIL(fatal);
            }
        }
        hbuf = (uint32_t *)malloc(DEV_MEM_SIZE);
        vbuf = (uint32_t *)malloc(DEV_MEM_SIZE);
        if (!hbuf || !vbuf) {
            printf("[03] init: %s host malloc 2x32MB FAIL\n", role);
            BAIL(fatal);
        }
    }
    if (!fatal)
        printf("[03] init: %s aclInit/SetDevice(%d)%s OK\n", role, dev,
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

    /* ---- phase 2 (step 2): importer PEER_ACCESS import + 双向 async D2D 校验 ---- */
    if (!fatal) {
        if (rank == 1) {
            int ok = 0;
            ret = aclrtIpcMemImportByKey(&p, key1,
                                         ACL_RT_IPC_MEM_IMPORT_FLAG_ENABLE_PEER_ACCESS);
            if (ret == ACL_ERROR_NONE) {
                imp1_ok = 1;
                ret = aclrtMalloc(&loc, DEV_MEM_SIZE, ACL_MEM_MALLOC_HUGE_FIRST);
            }
            if (ret == ACL_ERROR_NONE)
                ret = aclrtMemcpyAsync(loc, DEV_MEM_SIZE, p, DEV_MEM_SIZE,
                                       ACL_MEMCPY_DEVICE_TO_DEVICE, stream);
            if (ret == ACL_ERROR_NONE)
                ret = aclrtSynchronizeStream(stream);
            if (ret == ACL_ERROR_NONE) {
                memset(vbuf, 0, DEV_MEM_SIZE);
                ret = aclrtMemcpy(vbuf, DEV_MEM_SIZE, loc, DEV_MEM_SIZE,
                                  ACL_MEMCPY_DEVICE_TO_HOST);
                if (ret == ACL_ERROR_NONE)
                    ok = (check_pat(vbuf, words, 0, TAG_EXP) == 0);
            }
            step_line(2, who, "import PEER_ACCESS + async D2D read (p->local) + D2H verify",
                      ok, ret);
            if (!ok)
                BAIL(fatal);
        }
    }
    if (!fatal) {
        if (rank == 1) {
            int ok = 0;
            fill_pat(hbuf, words, 1, TAG_BACK);
            ret = aclrtMemcpy(loc, DEV_MEM_SIZE, hbuf, DEV_MEM_SIZE,
                              ACL_MEMCPY_HOST_TO_DEVICE);
            if (ret == ACL_ERROR_NONE)
                ret = aclrtMemcpyAsync(p, DEV_MEM_SIZE, loc, DEV_MEM_SIZE,
                                       ACL_MEMCPY_DEVICE_TO_DEVICE, stream);
            if (ret == ACL_ERROR_NONE)
                ret = aclrtSynchronizeStream(stream);
            ok = (ret == ACL_ERROR_NONE);
            step_line(2, who, "async D2D reverse write (local->p) + StreamSync", ok, ret);
            if (!ok)
                BAIL(fatal);
        }
    }
    sync_state(&fatal);   /* 反向写落地后 exporter 才校验 */
    if (!fatal) {
        if (rank == 0) {
            int ok = 0;
            memset(vbuf, 0, DEV_MEM_SIZE);
            ret = aclrtMemcpy(vbuf, DEV_MEM_SIZE, da, DEV_MEM_SIZE,
                              ACL_MEMCPY_DEVICE_TO_HOST);
            if (ret == ACL_ERROR_NONE)
                ok = (check_pat(vbuf, words, 1, TAG_BACK) == 0);
            step_line(2, who, "D2H verify reverse-written pattern (r1/TAG3) on own VA",
                      ok, ret);
            if (!ok)
                BAIL(fatal);
        }
    }
    sync_state(&fatal);

    /* ---- phase 4 (step 5): host 直达远端 D2H 读 / H2D 写, 各 16MB ---- */
    if (!fatal) {
        if (rank == 1) {
            int ok = 0;
            /* 读: p 前 16MB 是 step2 反向写的 TAG_BACK 数据 */
            memset(vbuf, 0, BW_SIZE);
            ret = aclrtMemcpy(vbuf, BW_SIZE, p, BW_SIZE, ACL_MEMCPY_DEVICE_TO_HOST);
            if (ret == ACL_ERROR_NONE)
                ok = (check_pat(vbuf, hw, 1, TAG_BACK) == 0);
            step_line(5, who, "host direct D2H read remote 16MB (imported VA) + verify",
                      ok, ret);
            if (ret != ACL_ERROR_NONE)
                BAIL(fatal);
            if (!fatal) {
                ok = 0;
                fill_pat(hbuf, hw, 1, TAG_HOST_W);
                ret = aclrtMemcpy(p, BW_SIZE, hbuf, BW_SIZE, ACL_MEMCPY_HOST_TO_DEVICE);
                ok = (ret == ACL_ERROR_NONE);
                step_line(5, who, "host direct H2D write remote 16MB (imported VA)", ok,
                          ret);
                if (!ok)
                    BAIL(fatal);
            }
        }
    }
    sync_state(&fatal);
    if (!fatal) {
        if (rank == 0) {
            int ok = 0;
            memset(vbuf, 0, BW_SIZE);
            ret = aclrtMemcpy(vbuf, BW_SIZE, da, BW_SIZE, ACL_MEMCPY_DEVICE_TO_HOST);
            if (ret == ACL_ERROR_NONE)
                ok = (check_pat(vbuf, hw, 1, TAG_HOST_W) == 0);
            step_line(5, who, "D2H verify host-direct-written 16MB (r1/TAG4) on own VA",
                      ok, ret);
            if (!ok)
                BAIL(fatal);
        }
    }
    sync_state(&fatal);

    /* ---- phase 5 (step 4): 带宽, 仅正确性全过后; async 提交 + 末尾 1 Sync ---- */
    if (!fatal && g_fail) {
        if (rank == 1)
            printf("[03] step 4 (r1): bandwidth SKIPPED (correctness not passed)\n");
    }
    if (!fatal && !g_fail) {
        if (rank == 1) {
            double t0, t1, gb;
            printf("[03] step 4 (r1): bandwidth on PEER imported VA, %u MB x %d iters + %d warmup, "
                   "async submits + 1 final Sync ONLY (no sync loop!)\n",
                   BW_SIZE >> 20, BW_ITERS, BW_WARMUP);
            gb = (double)BW_ITERS * (double)BW_SIZE / 1e9;

            /* 读方向: p -> local */
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
            printf("[03] bw   D2D-read  (p->local) : %7.2f GB/s "
                   "(30x16MB async + 1 Sync, total %.3f s)\n", gb / (t1 - t0), t1 - t0);

            /* 写方向: local -> p */
            for (int i = 0; i < BW_WARMUP; i++) {
                aclrtMemcpyAsync(p, BW_SIZE, loc, BW_SIZE,
                                 ACL_MEMCPY_DEVICE_TO_DEVICE, stream);
                aclrtSynchronizeStream(stream);
            }
            t0 = now_sec();
            for (int i = 0; i < BW_ITERS; i++)
                aclrtMemcpyAsync(p, BW_SIZE, loc, BW_SIZE,
                                 ACL_MEMCPY_DEVICE_TO_DEVICE, stream);
            aclrtSynchronizeStream(stream);
            t1 = now_sec();
            printf("[03] bw   D2D-write (local->p) : %7.2f GB/s "
                   "(30x16MB async + 1 Sync, total %.3f s)\n", gb / (t1 - t0), t1 - t0);
            fflush(stdout);
        }
    }
    sync_state(&fatal);

    /* ---- cleanup: 双方各自 close key; 物理内存只由 exporter free ---- */
    if (rank == 1) {
        if (imp1_ok) {
            ret = aclrtIpcMemClose(key1);
            printf("[03] cleanup (r1): IpcMemClose(key1) ret=%d\n", ret);
        }
    } else {
        if (exp1_ok) {
            ret = aclrtIpcMemClose(key1);
            printf("[03] cleanup (r0): IpcMemClose(key1) ret=%d\n", ret);
        }
    }
    MPI_Barrier(MPI_COMM_WORLD);   /* 双方 close 完成后 exporter 才 free 物理内存 */
    if (rank == 0) {
        if (da)
            aclrtFree(da);
        printf("[03] cleanup (r0): physical mem freed by exporter only\n");
    }
    if (rank == 1 && loc)
        aclrtFree(loc);
    if (stream)
        aclrtDestroyStream(stream);
    free(hbuf);
    free(vbuf);
    /* ResetDevice 前拆 P2P, 防 D 态挂死 */
    if (dev_ok) {
        int32_t peer = (rank == 0) ? env_dev("B_DEV", 2) : env_dev("A_DEV", 0);
        if (imp1_ok || exp1_ok) {
            aclError dr = aclrtDeviceDisablePeerAccess(peer);
            printf("[03] cleanup (r%d): DisablePeerAccess(%d) ret=%d\n", rank, peer, dr);
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
        printf("[03] RESULT: %s\n", g_fail ? "FAIL" : "PASS");
        fflush(stdout);
    }

    MPI_Finalize();
    return g_fail ? 1 : 0;
}
