/*
 * 02_ipc_die — IPC key 链路 die 对(同卡双 die)正反对照测试
 * 机器: Atlas 300I Duo (310P3, 4 卡 x 2 die = 8 dev), CANN 9.1.0, aarch64
 * 结构: MPI 双进程 rank0=exporter(dev A, 环境变量 A_DEV, 默认 0)
 *                     rank1=importer(dev B, 环境变量 B_DEV, 默认 1)
 *
 * 校验 pattern: value = (rank<<28)|(tag<<24)|(idx&0xFFFFFF)
 *   rank = 写入方 MPI rank, tag 每步不同(防残留数据造成假阳性)。
 *
 * 测试项(编号即测试项; 实际执行顺序 1,2,3,6,5,7,4):
 *   1  正向: exporter malloc P2P 32MB + H2D pattern + export key1(65B,
 *      DISABLE_PID_VALIDATION) + MPI 发 key
 *   2  正向: importer DEFAULT import + async D2D 读(p->local)+D2H 校验 +
 *      async D2D 反向写(local->p) + exporter D2H 校验
 *   3  反向对照A(预期失败): 第二块 16MB + key2, PEER_ACCESS import -> 预期 507899
 *   6  正向: host 直达远端 — importer 对 imported VA 直接 D2H(读)/H2D(写) 各 16MB+校验
 *   5  带宽(正确性过后): async D2D 读+写, 3 次预热 + 30x16MB async 提交 + 末尾 1 次 Sync
 *   7  观测(不影响判定): 同步 D2D 读校验 — rank0 写 fresh pattern(TAG_SYNC_RD),
 *      rank1 用同步 aclrtMemcpy(D2D) 读 p->loc + D2H 逐字校验;
 *      若 507899 则与 step4 一致(同步 D2D 不可用), 若 OK 则推翻 step4 结论
 *   4  反向对照B(预期失败, 放在一切之后): DEFAULT import 的 VA 上同步 aclrtMemcpy(D2D)
 *      -> 预期 507899; 失败后不再使用该 VA, 故最后执行
 *
 * 507899 = ACL_ERROR_RT_DRV_INTERNAL_ERROR (rt_error_codes.h:148):
 *   驱动层对 die 对上 PEER_ACCESS import / 同步 D2D 不支持路径的确定性回复。
 *
 * 带宽铁律: 严禁同步循环测带宽! 同步 D2D 必失败(507899), 循环量到的只是
 *   失败延迟(~140us/次), 非数据搬运。正确口径: 3 次预热 + 30 次 16MB
 *   MemcpyAsync 提交 + 末尾 1 次 StreamSync。
 *
 * 判定: 正向 1/2/6 全 OK 且对照 A/B 如期失败(507899) -> PASS;
 *       对照意外成功也算 PASS, 但显著标注"推翻预期"; 正向任一失败 -> FAIL。
 *
 * cleanup: 双方各自 aclrtIpcMemClose(key); 物理内存只由 exporter aclrtFree;
 *   对照A(PEER_ACCESS import)若意外成功(overturn), 双方在 ResetDevice 前
 *   aclrtDeviceDisablePeerAccess 拆除 P2P 状态(PEER_ACCESS 内部做双向
 *   EnablePeerAccess); 正常路径(对照A 如期 507899)无 P2P 状态, 不调用。
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
#define SECOND_MEM_SIZE  (16u * 1024u * 1024u)  /* 16 MB 对照A 第二块 */
#define BW_SIZE          (16u * 1024u * 1024u)  /* 16 MB 带宽/host直达块 */
#define BW_WARMUP        3
#define BW_ITERS         30
#define EXPECTED_ERR     507899  /* ACL_ERROR_RT_DRV_INTERNAL_ERROR */

#define TAG_EXP    0x2   /* step1: exporter 写入(step2 读校验) */
#define TAG_BACK   0x3   /* step2: importer 反向写(step2/6 读校验) */
#define TAG_HOST_W 0x4   /* step6: importer host 直达写 */
#define TAG_SYNC_RD 0x6  /* step7: rank0 写, rank1 同步 D2D 读校验 */

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
            printf("[02]   [verify] mismatch at word %zu (off 0x%zx): got 0x%08x, want 0x%08x\n",
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
    printf("[02] step %d (%s): %-58s %s (ret=%d)\n", n, who, desc,
           ok ? "OK" : "FAIL", ret);
    fflush(stdout);
    if (!ok)
        g_fail = 1;
}

/* 对照项行: ret!=0 -> EXPECTED-FAIL(如期失败); ret==0 -> 推翻预期(显著标注) */
static void ctrl_line(int n, const char *who, const char *desc, int32_t ret, int *overturn)
{
    if (ret != 0) {
        printf("[02] step %d (%s): %-58s EXPECTED-FAIL ret=%d%s\n", n, who, desc, ret,
               (ret == EXPECTED_ERR) ? "" : "  [注意: 错误码非预期 507899]");
        *overturn = 0;
    } else {
        printf("[02] step %d (%s): %-58s !!!推翻预期!!! 意外成功 ret=0 (预期失败 %d)\n",
               n, who, desc, EXPECTED_ERR);
        *overturn = 1;
    }
    fflush(stdout);
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
            printf("[02] FATAL: need exactly 2 MPI procs (-np 2), got %d\n", nproc);
        MPI_Finalize();
        return 1;
    }

    const char *role = (rank == 0) ? "r0/exporter" : "r1/importer";
    const char *who = (rank == 0) ? "r0" : "r1";
    int32_t dev = (rank == 0) ? env_dev("A_DEV", 0) : env_dev("B_DEV", 1);

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("[02] === 02_ipc_die: IPC key link die-pair, rank%d %s dev%d ===\n",
           rank, role, dev);
    printf("[02] %s: %s=%d, 32MB P2P main + 16MB ctrlA block, key len %dB, "
           "pattern=(rank<<28)|(tag<<24)|(idx&0xFFFFFF)\n",
           role, (rank == 0) ? "A_DEV" : "B_DEV", dev, IPC_KEY_LEN);

    aclrtStream stream = NULL;      /* importer 的 async 流 */
    void *da = NULL, *da2 = NULL;   /* exporter: 主块 / 对照A 第二块 */
    void *p = NULL, *p2 = NULL;     /* importer: DEFAULT import VA / 对照A VA(若意外成功) */
    void *loc = NULL;               /* importer: 本地 dev 缓冲(32MB) */
    uint32_t *hbuf = NULL, *vbuf = NULL;
    char key1[IPC_KEY_LEN] = {0}, key2[IPC_KEY_LEN] = {0};
    int exp1_ok = 0, exp2_ok = 0, imp1_ok = 0, imp2_ok = 0;
    int fatal = 0, overturn_a = 0, overturn_b = 0;
    int32_t ret = 0;
    int init_ok = 0, dev_ok = 0;
    size_t words = DEV_MEM_SIZE / sizeof(uint32_t);
    size_t hw = BW_SIZE / sizeof(uint32_t);

    /* ---- phase 0: init ---- */
    ret = aclInit(NULL);
    if (ret != ACL_ERROR_NONE) {
        printf("[02] init: %s aclInit FAIL ret=%d\n", role, ret);
        BAIL(fatal);
    } else {
        init_ok = 1;
        ret = aclrtSetDevice(dev);
        if (ret != ACL_ERROR_NONE) {
            printf("[02] init: %s aclrtSetDevice(%d) FAIL ret=%d\n", role, dev, ret);
            BAIL(fatal);
        }
    }
    if (!fatal) {
        dev_ok = 1;
        if (rank == 1) {
            ret = aclrtCreateStream(&stream);
            if (ret != ACL_ERROR_NONE) {
                printf("[02] init: r1 aclrtCreateStream FAIL ret=%d\n", ret);
                BAIL(fatal);
            }
        }
        hbuf = (uint32_t *)malloc(DEV_MEM_SIZE);
        vbuf = (uint32_t *)malloc(DEV_MEM_SIZE);
        if (!hbuf || !vbuf) {
            printf("[02] init: %s host malloc 2x32MB FAIL\n", role);
            BAIL(fatal);
        }
    }
    if (!fatal)
        printf("[02] init: %s aclInit/SetDevice(%d)%s OK\n", role, dev,
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
                // 导出 Ipc，并禁用白名单
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

    /* ---- phase 2 (step 2): importer DEFAULT import + 双向 async D2D 校验 ---- */
    if (!fatal) {
        if (rank == 1) {
            int ok = 0;
            // rank1 接收 key1 用来访问 rank0 的地址, 设置flag关闭两个device之间的数据交互
            ret = aclrtIpcMemImportByKey(&p, key1, ACL_RT_IPC_MEM_IMPORT_FLAG_DEFAULT);
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
            step_line(2, who, "import DEFAULT + async D2D read (p->local) + D2H verify",
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

    /* ---- phase 3 (step 3, 对照A): 第二块 16MB + PEER_ACCESS import, 预期 507899 ---- */
    if (!fatal) {
        if (rank == 0) {
            int ok = 0;
            ret = aclrtMalloc(&da2, SECOND_MEM_SIZE, ACL_MEM_MALLOC_HUGE_FIRST_P2P);
            if (ret == ACL_ERROR_NONE)
                ret = aclrtIpcMemGetExportKey(da2, SECOND_MEM_SIZE, key2, IPC_KEY_LEN,
                                              ACL_RT_IPC_MEM_EXPORT_FLAG_DISABLE_PID_VALIDATION);
            if (ret == ACL_ERROR_NONE) {
                exp2_ok = 1;
                if (MPI_Send(key2, IPC_KEY_LEN, MPI_CHAR, 1, 101, MPI_COMM_WORLD) == MPI_SUCCESS)
                    ok = 1;
            }
            step_line(3, who, "malloc P2P 16MB #2 + export key2 + MPI-send (ctrlA setup)",
                      ok, ret);
            if (!ok)
                BAIL(fatal);
        } else {
            MPI_Recv(key2, IPC_KEY_LEN, MPI_CHAR, 0, 101, MPI_COMM_WORLD,
                     MPI_STATUS_IGNORE);
        }
    }
    sync_state(&fatal);
    if (!fatal) {
        if (rank == 1) {
            ret = aclrtIpcMemImportByKey(&p2, key2,
                                         ACL_RT_IPC_MEM_IMPORT_FLAG_ENABLE_PEER_ACCESS);
            if (ret == ACL_ERROR_NONE)
                imp2_ok = 1;   /* 意外成功: 推翻预期, cleanup 时 close */
            ctrl_line(3, who, "ctrlA: import key2 w/ ENABLE_PEER_ACCESS flag", ret,
                      &overturn_a);
        }
    }
    sync_state(&fatal);

    /* ---- phase 4 (step 6): host 直达远端 D2H 读 / H2D 写, 各 16MB ---- */
    if (!fatal) {
        if (rank == 1) {
            int ok = 0;
            /* 读: p 前 16MB 是 step2 反向写的 TAG_BACK 数据 */
            memset(vbuf, 0, BW_SIZE);
            ret = aclrtMemcpy(vbuf, BW_SIZE, p, BW_SIZE, ACL_MEMCPY_DEVICE_TO_HOST);
            if (ret == ACL_ERROR_NONE)
                ok = (check_pat(vbuf, hw, 1, TAG_BACK) == 0);
            step_line(6, who, "host direct D2H read remote 16MB (imported VA) + verify",
                      ok, ret);
            if (ret != ACL_ERROR_NONE)
                BAIL(fatal);
            if (!fatal) {
                ok = 0;
                fill_pat(hbuf, hw, 1, TAG_HOST_W);
                ret = aclrtMemcpy(p, BW_SIZE, hbuf, BW_SIZE, ACL_MEMCPY_HOST_TO_DEVICE);
                ok = (ret == ACL_ERROR_NONE);
                step_line(6, who, "host direct H2D write remote 16MB (imported VA)", ok,
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
            step_line(6, who, "D2H verify host-direct-written 16MB (r1/TAG4) on own VA",
                      ok, ret);
            if (!ok)
                BAIL(fatal);
        }
    }
    sync_state(&fatal);

    /* ---- phase 5 (step 5): 带宽, 仅正确性全过后 ---- */
    if (!fatal && g_fail) {
        if (rank == 1)
            printf("[02] step 5 (r1): bandwidth SKIPPED (correctness not passed)\n");
    }
    if (!fatal && !g_fail) {
        if (rank == 1) {
            double t0, t1, gb;
            printf("[02] step 5 (r1): bandwidth, %u MB x %d iters + %d warmup, "
                   "async submits + 1 final Sync ONLY (no sync loop!)\n",
                   BW_SIZE >> 20, BW_ITERS, BW_WARMUP);
            gb = (double)BW_ITERS * (double)BW_SIZE / 1e9;

            /* 读方向: p -> local。严禁同步循环测带宽(见文件头历史教训) */
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
            printf("[02] bw   D2D-read  (p->local) : %7.2f GB/s "
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
            printf("[02] bw   D2D-write (local->p) : %7.2f GB/s "
                   "(30x16MB async + 1 Sync, total %.3f s)\n", gb / (t1 - t0), t1 - t0);
            fflush(stdout);
        }
    }
    sync_state(&fatal);

    /* ---- phase 7 (step 7): 观测 — 同步 D2D 读校验 ----
     * 验证同步 aclrtMemcpy(D2D) 能否完成跨 die 数据搬运 + 校验。
     * rank0 写 fresh pattern(TAG_SYNC_RD), rank1 用同步接口读 p->loc + D2H 校验。
     * 不影响 PASS/FAIL: 若 507899 则与 step4 一致(同步 D2D 不可用);
     * 若 ret=0 且校验通过则推翻 step4 结论(同步 D2D 可用)。 */
    if (!fatal) {
        if (rank == 0) {
            int ok = 0;
            fill_pat(hbuf, hw, 0, TAG_SYNC_RD);
            ret = aclrtMemcpy(da, BW_SIZE, hbuf, BW_SIZE, ACL_MEMCPY_HOST_TO_DEVICE);
            ok = (ret == ACL_ERROR_NONE);
            step_line(7, who, "H2D write fresh pattern (TAG_SYNC_RD) for sync D2D read test",
                      ok, ret);
            if (!ok)
                BAIL(fatal);
        }
    }
    sync_state(&fatal);
    if (!fatal) {
        if (rank == 1) {
            int ok = 0;
            /* 同步 D2D 读: p -> loc (aclrtMemcpy, 非 aclrtMemcpyAsync) */
            ret = aclrtMemcpy(loc, BW_SIZE, p, BW_SIZE, ACL_MEMCPY_DEVICE_TO_DEVICE);
            if (ret == ACL_ERROR_NONE) {
                memset(vbuf, 0, BW_SIZE);
                ret = aclrtMemcpy(vbuf, BW_SIZE, loc, BW_SIZE,
                                  ACL_MEMCPY_DEVICE_TO_HOST);
                if (ret == ACL_ERROR_NONE)
                    ok = (check_pat(vbuf, hw, 0, TAG_SYNC_RD) == 0);
            }
            /* 观测项: 不影响 PASS/FAIL (sync D2D 预期失败, 见 step4) */
            printf("[02] step 7 (r1): %-58s %s (ret=%d)%s\n",
                   "sync D2D read (p->local) + D2H verify",
                   ok ? "OK" : "FAIL", ret,
                   (ret == EXPECTED_ERR) ? "  [与 step4 一致: sync D2D 不可用]" :
                   (ok ? "  ★ sync D2D 可用! 推翻 step4 结论" : ""));
            fflush(stdout);
        }
    }
    sync_state(&fatal);

    /* ---- phase 6 (step 4, 对照B, 放在一切之后): 同步 D2D, 预期 507899 ----
     * 失败后不再使用该 VA(直接进 cleanup) */
    if (!fatal) {
        if (rank == 1) {
            ret = aclrtMemcpy(loc, BW_SIZE, p, BW_SIZE, ACL_MEMCPY_DEVICE_TO_DEVICE);
            ctrl_line(4, who, "ctrlB: sync aclrtMemcpy D2D (p->local) on DEFAULT VA",
                      ret, &overturn_b);
        }
    }
    sync_state(&fatal);

    /* ---- cleanup: 双方各自 close key; 物理内存只由 exporter free ---- */
    if (rank == 1) {
        if (imp1_ok) {
            ret = aclrtIpcMemClose(key1);
            printf("[02] cleanup (r1): IpcMemClose(key1) ret=%d\n", ret);
        }
        if (imp2_ok) {
            ret = aclrtIpcMemClose(key2);
            printf("[02] cleanup (r1): IpcMemClose(key2/ctrlA) ret=%d\n", ret);
        }
    } else {
        if (exp1_ok) {
            ret = aclrtIpcMemClose(key1);
            printf("[02] cleanup (r0): IpcMemClose(key1) ret=%d\n", ret);
        }
        if (exp2_ok) {
            ret = aclrtIpcMemClose(key2);
            printf("[02] cleanup (r0): IpcMemClose(key2/ctrlA) ret=%d\n", ret);
        }
    }
    MPI_Barrier(MPI_COMM_WORLD);   /* 双方 close 完成后 exporter 才 free 物理内存 */
    if (rank == 0) {
        if (da)
            aclrtFree(da);
        if (da2)
            aclrtFree(da2);
        printf("[02] cleanup (r0): physical mem freed by exporter only\n");
    }
    if (rank == 1 && loc)
        aclrtFree(loc);
    if (stream)
        aclrtDestroyStream(stream);
    free(hbuf);
    free(vbuf);
    /* ★修复 D 态挂死风险: 对照A(PEER_ACCESS import key2)若意外成功(overturn),
     *   内部已做双向 EnablePeerAccess, 必须在 ResetDevice 前显式拆除 P2P 状态,
     *   否则进程退出时 mmu notifier 与驱动 SRCU 死锁(D 态, SIGKILL 不可回收)。
     *   正常路径(对照A 如期 507899)无 P2P 状态, 不调用。
     *   imp2_ok 仅 rank1 持有, 用 Allreduce 同步给双方(cleanup 前两 rank 都存活)。 */
    int need_disable_peer = imp2_ok;
    MPI_Allreduce(MPI_IN_PLACE, &need_disable_peer, 1, MPI_INT, MPI_MAX, MPI_COMM_WORLD);
    if (dev_ok) {
        if (need_disable_peer) {
            int32_t peer = (rank == 0) ? env_dev("B_DEV", 1) : env_dev("A_DEV", 0);
            int32_t dr = aclrtDeviceDisablePeerAccess(peer);
            printf("[02] cleanup (r%d): DisablePeerAccess(%d) ret=%d (ctrlA overturn teardown)\n",
                   rank, peer, dr);
        }
        aclrtResetDevice(dev);
    }
    if (init_ok)
        aclFinalize();

    /* ---- RESULT(聚合两 rank 状态; 两 rank 打印同一行) ---- */
    {
        int res[3];
        res[0] = g_fail;
        res[1] = overturn_a;
        res[2] = overturn_b;
        MPI_Allreduce(MPI_IN_PLACE, res, 3, MPI_INT, MPI_MAX, MPI_COMM_WORLD);
        g_fail = res[0];
        overturn_a = res[1];
        overturn_b = res[2];

        printf("[02] RESULT: %s", g_fail ? "FAIL" : "PASS");
        if (!g_fail && (overturn_a || overturn_b)) {
            printf("  ★★推翻预期: 对照%s意外成功(历史结论需修订!)★★",
                   overturn_a && overturn_b ? "A+B" : (overturn_a ? "A" : "B"));
        }
        printf("\n");
        fflush(stdout);
    }

    MPI_Finalize();
    return g_fail ? 1 : 0;
}
