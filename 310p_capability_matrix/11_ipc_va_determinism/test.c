/*
 * 11_ipc_va_determinism — IPC VA 确定性测试
 * 验证: 多进程同序 import 相同 IPC keys, VA 是否一致
 *
 * 结构: rank0=dev1(exporter), rank1=dev3(跨卡), rank2=dev1(同设备)
 * 结论: importer 间 VA 一致, exporter 自 import 不同
 * 带宽: dev1→dev3 ~4 GB/s (同 socket dst=副 die)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <mpi.h>
#include "acl/acl.h"

#define IPC_KEY_LEN   65
#define NR_BLOCKS     4
#define BLOCK_SIZE    (16u * 1024u * 1024u)   /* 16 MB 每块 */
#define BW_WARMUP     3
#define BW_ITERS      30
#define NRANKS        3

/* tag */
static const uint32_t TAG_BLOCK[NR_BLOCKS] = { 0x2, 0x3, 0x4, 0x5 };

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
            printf("[11]   [verify] mismatch at word %zu (off 0x%zx): got 0x%08x, want 0x%08x\n",
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

/* parse DEV_LIST */
static void parse_dev_list(int32_t *devs, int nranks)
{
    static char buf[128];
    const char *s = getenv("DEV_LIST");
    if (!s)
        s = "1,3,1";
    strncpy(buf, s, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    char *tok = strtok(buf, ",");
    for (int i = 0; i < nranks && tok; i++) {
        devs[i] = (int32_t)atoi(tok);
        tok = strtok(NULL, ",");
    }
    for (int i = 0; i < nranks; i++) {
        if (devs[i] < 0)
            devs[i] = 0;
    }
}

/* step_line: ok=0 记 FAIL */
static void step_line(int n, const char *who, const char *desc, int ok, int32_t ret)
{
    printf("[11] step %d (%s): %-58s %s (ret=%d)\n", n, who, desc,
           ok ? "OK" : "FAIL", ret);
    fflush(stdout);
    if (!ok)
        g_fail = 1;
}

/* sync_state: Allreduce fatal/g_fail */
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
    if (nproc != NRANKS) {
        if (rank == 0)
            printf("[11] FATAL: need exactly %d MPI procs (-np %d), got %d\n",
                   NRANKS, NRANKS, nproc);
        MPI_Finalize();
        return 1;
    }

    int32_t devs[NRANKS] = {0, 0, 0};
    parse_dev_list(devs, NRANKS);
    int32_t dev = devs[rank];
    int32_t exp_dev = devs[0];   /* exporter 的 dev = rank0 的 dev */

/* import flag */
    uint64_t imp_flag = (dev == exp_dev)
                      ? ACL_RT_IPC_MEM_IMPORT_FLAG_DEFAULT
                      : ACL_RT_IPC_MEM_IMPORT_FLAG_ENABLE_PEER_ACCESS;
    const char *imp_flag_name = (dev == exp_dev) ? "DEFAULT" : "PEER";

    char role_buf[64];
    snprintf(role_buf, sizeof(role_buf), "r%d/dev%d%s", rank, dev,
             (rank == 0) ? "/exporter" : ((dev != exp_dev) ? "/cross" : "/same-dev"));
    const char *who = role_buf;

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("[11] === 11_ipc_va_determinism: rank%d %s, import flag=%s ===\n",
           rank, role_buf, imp_flag_name);
    printf("[11] DEV_LIST: %d,%d,%d  exp_dev=%d, pattern=(rank<<28)|(tag<<24)|(idx&0xFFFFFF)\n",
           devs[0], devs[1], devs[2], exp_dev);

    aclrtStream stream = NULL;
    void *da[NR_BLOCKS] = {0};                    /* rank0: 原始 4 块设备内存 */
    void *vas[NR_BLOCKS] = {0};                   /* 各 rank: imported VA */
    char keys[NR_BLOCKS][IPC_KEY_LEN];
    memset(keys, 0, sizeof(keys));
    int imp_ok[NR_BLOCKS] = {0};
    void *local = NULL;                            /* rank1: 本地 16MB 缓冲 */
    uint32_t *hbuf = NULL, *vbuf = NULL;
    int fatal = 0;
    int32_t ret = 0;
    int init_ok = 0, dev_ok = 0;
    size_t words = BLOCK_SIZE / sizeof(uint32_t);

/* init */
    ret = aclInit(NULL);
    if (ret != ACL_ERROR_NONE) {
        printf("[11] init: %s aclInit FAIL ret=%d\n", role_buf, ret);
        BAIL(fatal);
    } else {
        init_ok = 1;
        ret = aclrtSetDevice(dev);
        if (ret != ACL_ERROR_NONE) {
            printf("[11] init: %s aclrtSetDevice(%d) FAIL ret=%d\n", role_buf, dev, ret);
            BAIL(fatal);
        }
    }
    if (!fatal) {
        dev_ok = 1;
        if (rank == 1) {
            ret = aclrtCreateStream(&stream);
            if (ret != ACL_ERROR_NONE) {
                printf("[11] init: r1 aclrtCreateStream FAIL ret=%d\n", ret);
                BAIL(fatal);
            }
            ret = aclrtMalloc(&local, BLOCK_SIZE, ACL_MEM_MALLOC_HUGE_FIRST);
            if (ret != ACL_ERROR_NONE) {
                printf("[11] init: r1 aclrtMalloc local 16MB FAIL ret=%d\n", ret);
                BAIL(fatal);
            }
        }
        if (rank == 0)
            hbuf = (uint32_t *)malloc(BLOCK_SIZE);
        else if (rank == 1)
            vbuf = (uint32_t *)malloc(BLOCK_SIZE);
        if ((rank == 0 && !hbuf) || (rank == 1 && !vbuf)) {
            printf("[11] init: %s host malloc 16MB FAIL\n", role_buf);
            BAIL(fatal);
        }
    }
    if (!fatal)
        printf("[11] init: %s aclInit/SetDevice(%d)%s OK\n", role_buf, dev,
               (rank == 1) ? "/CreateStream/MallocLocal" : "");
    sync_state(&fatal);

/* step 1: export */
    if (!fatal) {
        if (rank == 0) {
            int ok = 0;
            ret = ACL_ERROR_NONE;
            for (int j = 0; j < NR_BLOCKS && ret == ACL_ERROR_NONE; j++) {
                ret = aclrtMalloc(&da[j], BLOCK_SIZE, ACL_MEM_MALLOC_HUGE_FIRST_P2P);
                if (ret == ACL_ERROR_NONE) {
                    fill_pat(hbuf, words, 0, TAG_BLOCK[j]);
                    ret = aclrtMemcpy(da[j], BLOCK_SIZE, hbuf, BLOCK_SIZE,
                                      ACL_MEMCPY_HOST_TO_DEVICE);
                }
                if (ret == ACL_ERROR_NONE)
                    ret = aclrtIpcMemGetExportKey(da[j], BLOCK_SIZE, keys[j],
                                                 IPC_KEY_LEN,
                                                 ACL_RT_IPC_MEM_EXPORT_FLAG_DISABLE_PID_VALIDATION);
                if (ret == ACL_ERROR_NONE)
                    printf("[11]   key[%d] origin VA=%p, export OK\n", j, da[j]);
            }
            if (ret == ACL_ERROR_NONE)
                ok = 1;
            step_line(1, who, "alloc 4x16MB + H2D pattern + export 4 keys", ok, ret);
            if (!ok)
                BAIL(fatal);
        }
    }
    if (!fatal) {
/* Bcast keys */
        MPI_Bcast(keys, NR_BLOCKS * IPC_KEY_LEN, MPI_CHAR, 0, MPI_COMM_WORLD);
        if (rank != 0)
            printf("[11] step 1 (%s): received 4 IPC keys via Bcast\n", role_buf);
    }
    sync_state(&fatal);

/* step 2: import */
    if (!fatal) {
        int all_ok = 1;
        ret = ACL_ERROR_NONE;
        for (int j = 0; j < NR_BLOCKS; j++) {
            ret = aclrtIpcMemImportByKey(&vas[j], keys[j], imp_flag);
            if (ret == ACL_ERROR_NONE) {
                imp_ok[j] = 1;
            } else {
                all_ok = 0;
                printf("[11]   key[%d] import FAIL ret=%d\n", j, ret);
            }
        }
        step_line(2, who, "import 4 keys (same order, flag=DEFAULT or PEER)", all_ok,
                  all_ok ? 0 : ret);
        if (!all_ok)
            BAIL(fatal);
    }
    sync_state(&fatal);

/* step 3: VA compare */
    if (!fatal) {
        uint64_t my_vas[NR_BLOCKS];
        uint64_t all_vas[NRANKS * NR_BLOCKS];
        for (int j = 0; j < NR_BLOCKS; j++)
            my_vas[j] = imp_ok[j] ? (uint64_t)(uintptr_t)vas[j] : 0;
        MPI_Allgather(my_vas, NR_BLOCKS, MPI_UINT64_T,
                      all_vas, NR_BLOCKS, MPI_UINT64_T, MPI_COMM_WORLD);

/* 比对 VA */
        int all_match = 1;
        printf("[11] step 3: VA comparison (Allgather %d ranks x %d keys):\n", NRANKS, NR_BLOCKS);
        for (int j = 0; j < NR_BLOCKS; j++) {
/* 找参考 VA */
            uint64_t ref = 0;
            for (int r = 0; r < NRANKS; r++) {
                if (all_vas[r * NR_BLOCKS + j] != 0) {
                    ref = all_vas[r * NR_BLOCKS + j];
                    break;
                }
            }
            int match = 1;
            for (int r = 0; r < NRANKS; r++) {
                uint64_t v = all_vas[r * NR_BLOCKS + j];
                if (v != 0 && v != ref)
                    match = 0;
            }
            if (!match)
                all_match = 0;
            printf("[11]   key[%d] tag=0x%x:", j, TAG_BLOCK[j]);
            for (int r = 0; r < NRANKS; r++)
                printf(" r%d=0x%012llx", r,
                       (unsigned long long)all_vas[r * NR_BLOCKS + j]);
            printf("  [%s]\n", match ? "MATCH" : "MISMATCH");
        }
        fflush(stdout);

        if (rank == 0) {
            if (all_match) {
                printf("[11] step 3: VA ALL MATCH — 对称 GVA 可行(所有进程同序 import 得同 VA)\n");
            } else {
                printf("[11] step 3: VA MISMATCH — 需查表(VA 不一致, 功能仍可用, 降级查表模式)\n");
            }
            fflush(stdout);
        }
    }
    sync_state(&fatal);

/* step 4: D2D verify */
    if (!fatal && rank == 1) {
        int all_ok = 1;
        ret = ACL_ERROR_NONE;
        for (int j = 0; j < NR_BLOCKS; j++) {
            int ok = 0;
            ret = aclrtMemcpyAsync(local, BLOCK_SIZE, vas[j], BLOCK_SIZE,
                                    ACL_MEMCPY_DEVICE_TO_DEVICE, stream);
            if (ret == ACL_ERROR_NONE)
                ret = aclrtSynchronizeStream(stream);
            if (ret == ACL_ERROR_NONE) {
                memset(vbuf, 0, BLOCK_SIZE);
                ret = aclrtMemcpy(vbuf, BLOCK_SIZE, local, BLOCK_SIZE,
                                  ACL_MEMCPY_DEVICE_TO_HOST);
                if (ret == ACL_ERROR_NONE)
                    ok = (check_pat(vbuf, words, 0, TAG_BLOCK[j]) == 0);
            }
            if (!ok)
                all_ok = 0;
            printf("[11]   VA[%d] async D2D read + D2H verify (tag=0x%x): %s\n",
                   j, TAG_BLOCK[j], ok ? "OK" : "FAIL");
            fflush(stdout);
        }
        step_line(4, who, "async D2D read + verify on 4 imported VAs (data path)", all_ok,
                  all_ok ? 0 : ret);
        if (!all_ok)
            BAIL(fatal);
    }
    sync_state(&fatal);

/* 带宽: 3 预热 + 30×16MB async + 1 Sync */
    if (!fatal && g_fail) {
        if (rank == 1)
            printf("[11] step 5 (r1): bandwidth SKIPPED (correctness not passed)\n");
    }
    if (!fatal && !g_fail && rank == 1) {
        double gb = (double)BW_ITERS * (double)BLOCK_SIZE / 1e9;
        printf("[11] step 5 (r1): bandwidth on 4 imported VAs, %u MB x %d iters + %d warmup, "
               "async submits + 1 final Sync ONLY (no sync loop!)\n",
               BLOCK_SIZE >> 20, BW_ITERS, BW_WARMUP);
        for (int j = 0; j < NR_BLOCKS; j++) {
            double t0, t1;
            for (int i = 0; i < BW_WARMUP; i++) {
                aclrtMemcpyAsync(local, BLOCK_SIZE, vas[j], BLOCK_SIZE,
                                 ACL_MEMCPY_DEVICE_TO_DEVICE, stream);
                aclrtSynchronizeStream(stream);
            }
            t0 = now_sec();
            for (int i = 0; i < BW_ITERS; i++)
                aclrtMemcpyAsync(local, BLOCK_SIZE, vas[j], BLOCK_SIZE,
                                 ACL_MEMCPY_DEVICE_TO_DEVICE, stream);
            aclrtSynchronizeStream(stream);
            t1 = now_sec();
            printf("[11] bw   VA[%d] (tag=0x%x): %7.2f GB/s "
                   "(30x16MB async + 1 Sync, total %.3f s)\n",
                   j, TAG_BLOCK[j], gb / (t1 - t0), t1 - t0);
        }
        fflush(stdout);
    }
    sync_state(&fatal);

/* step 6: cleanup */
    for (int j = 0; j < NR_BLOCKS; j++) {
        if (imp_ok[j]) {
            ret = aclrtIpcMemClose(keys[j]);
            printf("[11] step 6 (%s): IpcMemClose(key[%d]) ret=%d\n", role_buf, j, ret);
        }
    }
    MPI_Barrier(MPI_COMM_WORLD);   /* 全部 close 后 exporter 才 free 物理内存 */
    if (rank == 0) {
        for (int j = 0; j < NR_BLOCKS; j++) {
            if (da[j])
                aclrtFree(da[j]);
        }
        printf("[11] step 6 (r0): physical mem freed by exporter only\n");
    }
    if (rank == 1 && local)
        aclrtFree(local);
    if (stream)
        aclrtDestroyStream(stream);
    free(hbuf);
    free(vbuf);
    fflush(stdout);
/* cleanup: DisablePeerAccess before ResetDevice (防 D 态挂死) */
    if (dev_ok) {
        if (dev != exp_dev) {
            aclError dr = aclrtDeviceDisablePeerAccess(exp_dev);
            printf("[11] step 6 (%s): DisablePeerAccess(%d) ret=%d\n",
                   role_buf, exp_dev, dr);
        }
        aclrtResetDevice(dev);
    }
    if (init_ok)
        aclFinalize();

    /* ---- RESULT(聚合三方状态) ---- */
    {
        int res[1];
        res[0] = g_fail;
        MPI_Allreduce(MPI_IN_PLACE, res, 1, MPI_INT, MPI_MAX, MPI_COMM_WORLD);
        g_fail = res[0];

        printf("[11] RESULT: %s\n", g_fail ? "FAIL" : "PASS");
        fflush(stdout);
    }

    MPI_Finalize();
    return g_fail ? 1 : 0;
}
