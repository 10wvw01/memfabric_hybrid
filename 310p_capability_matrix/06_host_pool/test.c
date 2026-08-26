/*
 * 06_host_pool — host DRAM 跨进程共享池测试
 * 机器: Atlas 300I Duo (310P3, 4 卡 x 2 die = 8 dev), CANN 9.1.0, aarch64
 * 结构: MPI 双进程 rank0=host-owner(dev A, A_DEV 默认 0)
 *                     rank1=consumer  (dev B, B_DEV 默认 0)  <- 默认同设备, 跨进程
 * (host DRAM 池焦点: 设备仅作 H2D/D2H 目标, 无跨卡 P2P, 无 D 态风险)
 *
 * 验证能力:
 *   - host pinned 内存(aclrtMallocHost)基线 ✓(H2D/D2H 往返 + 带宽)
 *   - POSIX shm 跨进程 host DRAM 共享 ✓(MAP_SHARED, 两进程同物理页, host 侧直接可见)
 *   - 共享 host 内存 + H2D 链路 ✓(rank1 从共享池 H2D 到本 rank dev + D2H 校验;
 *     rank0 改共享区后 rank1 再 H2D 读新数据, 验证跨进程更新可见)
 *   - aclrtHostRegister 边界(预期 207000): 310P3 不支持对任意 mmap 内存做 host register
 *     (pinned host 必须来自 aclrtMallocHost; 用户 mmap 区不可 page-lock) —— 文档化此边界
 *
 * 校验 pattern: value = (rank<<28)|(tag<<24)|(idx&0xFFFFFF)
 *   rank = 写入方 MPI rank, tag 每步不同(防残留数据假阳性)。
 *
 * 测试项(执行顺序 1->5):
 *   1  rank0: aclrtMallocHost 32MB + H2D 写 pattern 到 dev + D2H 读回校验 +
 *      带宽(H2D/D2H 各 3 预热 + 30x16MB sync 调用计时, host pinned 同步可用)
 *   2  rank0: shm_open(O_CREAT|O_RDWR) + ftruncate(32MB) + mmap(MAP_SHARED) +
 *      写 pattern; barrier; rank1: shm_open + mmap(MAP_SHARED) + 直接读校验
 *      (host 侧直接可见, 无需 ACL)
 *   3  rank1: 从共享 host 内存 aclrtMemcpy(H2D) 写到本 rank dev + D2H 读回校验;
 *      rank0 改共享区(新 pattern) -> barrier -> rank1 再 H2D 读新数据(跨进程更新 + H2D 链路)
 *   4  rank0: 对 mmap 的共享区调 aclrtHostRegister -> 预期 207000(310P3 不支持);
 *      若意外成功如实记录(并 HostUnregister 清理)
 *   5  cleanup: 双方 munmap + close fd; rank0 shm_unlink; 各自 ACL 资源释放
 *
 * 判定: step 1/2/3 全 OK -> PASS; step 4 为文档化边界(预期失败/意外成功均不影响判定)。
 *
 * cleanup(无跨卡 P2P, 无 D 态风险, 但仍规范):
 *   aclrtFreeHost + aclrtFree + munmap + close + shm_unlink + ResetDevice + Finalize
 */
#define _GNU_SOURCE   /* 保证 shm_open / ftruncate 在 glibc 下声明 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <mpi.h>
#include "acl/acl.h"

#define SHM_NAME       "/hybm_test_shm"
#define POOL_SIZE      (32u * 1024u * 1024u)   /* 32 MB 共享 host 池 / aclrtMallocHost 块 */
#define DEV_MEM_SIZE    (32u * 1024u * 1024u)   /* 32 MB 设备内存块 */
#define BW_SIZE         (16u * 1024u * 1024u)   /* 16 MB 带宽块 */
#define BW_WARMUP      3
#define BW_ITERS       30
#define EXPECTED_ERR   207000   /* ACL_ERROR_RT_FEATURE_NOT_SUPPORT */

#define TAG_HOST   0x1   /* step1: rank0 aclrtMallocHost pattern */
#define TAG_SHM    0x2   /* step2/3a: rank0 写共享区, rank1 H2D 读 */
#define TAG_SHM2   0x3   /* step3b: rank0 改共享区, rank1 再 H2D 读 */

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
            printf("[06]   [verify] mismatch at word %zu (off 0x%zx): got 0x%08x, want 0x%08x\n",
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
    printf("[06] step %d (%s): %-58s %s (ret=%d)\n", n, who, desc,
           ok ? "OK" : "FAIL", ret);
    fflush(stdout);
    if (!ok)
        g_fail = 1;
}

/* step4 边界行(预期 207000; 意外成功也如实记录, 不影响判定)
 * 返回 1 = 意外成功(调用方需 HostUnregister); 0 = 预期失败(无需 unregister) */
static int boundary_line(int n, const char *who, const char *desc, int32_t ret)
{
    if (ret != 0) {
        printf("[06] step %d (%s): %-58s EXPECTED-FAIL ret=%d%s\n", n, who, desc, ret,
               (ret == EXPECTED_ERR) ? " (207000 FEATURE_NOT_SUPPORT)" :
                "  [注意: 错误码非预期 207000]");
        fflush(stdout);
        return 0;
    }
    printf("[06] step %d (%s): %-58s 意外成功 ret=0 <- host register 可用?(310P3 文档为不可用)\n",
           n, who, desc);
    fflush(stdout);
    return 1;
}

static void bw_line(const char *name, double sec, const char *note)
{
    double gb = (double)BW_ITERS * (double)BW_SIZE / 1e9;
    printf("[06] bw   %-9s: %7.2f GB/s   (%s, total %.3f s)\n",
           name, gb / sec, note, sec);
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
            printf("[06] FATAL: need exactly 2 MPI procs (-np 2), got %d\n", nproc);
        MPI_Finalize();
        return 1;
    }

    const char *role = (rank == 0) ? "r0/host-owner" : "r1/consumer";
    const char *who = (rank == 0) ? "r0" : "r1";
    int32_t dev = (rank == 0) ? env_dev("A_DEV", 0) : env_dev("B_DEV", 0);

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("[06] === 06_host_pool: host DRAM cross-process pool (POSIX shm), rank%d %s dev%d ===\n",
           rank, role, dev);
    printf("[06] %s: %s=%d, pool %u MB, pattern=(rank<<28)|(tag<<24)|(idx&0xFFFFFF)\n",
           role, (rank == 0) ? "A_DEV" : "B_DEV", dev, POOL_SIZE >> 20);

    void *da0 = NULL, *da1 = NULL, *hp = NULL;   /* rank0: da0/hp; rank1: da1 */
    uint32_t *vbuf = NULL;                         /* rank1: D2H 读回校验缓冲 */
    void *shm_ptr = NULL;                          /* 双方各自 mmap 的共享池 VA */
    int shm_fd = -1, shm_mapped = 0;              /* shm 状态标志 */
    int fatal = 0, reg_ok = 0;                    /* reg_ok: step4 意外成功标志 */
    int32_t ret = 0;
    int init_ok = 0, dev_ok = 0;
    size_t words = POOL_SIZE / sizeof(uint32_t);

    /* ---- phase 0: init ---- */
    ret = aclInit(NULL);
    if (ret != ACL_ERROR_NONE) {
        printf("[06] init: %s aclInit FAIL ret=%d\n", role, ret);
        BAIL(fatal);
    } else {
        init_ok = 1;
        ret = aclrtSetDevice(dev);
        if (ret != ACL_ERROR_NONE) {
            printf("[06] init: %s aclrtSetDevice(%d) FAIL ret=%d\n", role, dev, ret);
            BAIL(fatal);
        }
    }
    if (!fatal) {
        dev_ok = 1;
        if (rank == 0) {
            /* rank0: host-owner 建 pinned host 块 + 设备块(step1 基线用) */
            ret = aclrtMallocHost(&hp, POOL_SIZE);
            if (ret != ACL_ERROR_NONE) {
                printf("[06] init: r0 aclrtMallocHost(%uMB) FAIL ret=%d\n", POOL_SIZE >> 20, ret);
                BAIL(fatal);
            }
            if (!fatal) {
                ret = aclrtMalloc(&da0, DEV_MEM_SIZE, ACL_MEM_MALLOC_HUGE_FIRST);
                if (ret != ACL_ERROR_NONE) {
                    printf("[06] init: r0 aclrtMalloc(%uMB) FAIL ret=%d\n", DEV_MEM_SIZE >> 20, ret);
                    BAIL(fatal);
                }
            }
        } else {
            /* rank1: consumer 建 D2H 校验缓冲 + 设备块(step3 H2D 目标) */
            vbuf = (uint32_t *)malloc(POOL_SIZE);
            if (!vbuf) {
                printf("[06] init: r1 host malloc %uMB FAIL\n", POOL_SIZE >> 20);
                BAIL(fatal);
            }
            if (!fatal) {
                ret = aclrtMalloc(&da1, DEV_MEM_SIZE, ACL_MEM_MALLOC_HUGE_FIRST);
                if (ret != ACL_ERROR_NONE) {
                    printf("[06] init: r1 aclrtMalloc(%uMB) FAIL ret=%d\n", DEV_MEM_SIZE >> 20, ret);
                    BAIL(fatal);
                }
            }
        }
    }
    if (!fatal)
        printf("[06] init: %s aclInit/SetDevice(%d)%s OK\n", role, dev,
               (rank == 0) ? " + MallocHost + Malloc" : " + Malloc");
    sync_state(&fatal);

    /* ---- phase 1 (step 1, rank0): aclrtMallocHost 基线 + H2D/D2H 往返校验 + 带宽 ---- */
    if (!fatal && rank == 0) {
        int ok = 0;
        fill_pat((uint32_t *)hp, words, 0, TAG_HOST);
        ret = aclrtMemcpy(da0, DEV_MEM_SIZE, hp, POOL_SIZE, ACL_MEMCPY_HOST_TO_DEVICE);
        if (ret == ACL_ERROR_NONE) {
            memset(hp, 0, POOL_SIZE);
            ret = aclrtMemcpy(hp, POOL_SIZE, da0, DEV_MEM_SIZE, ACL_MEMCPY_DEVICE_TO_HOST);
            if (ret == ACL_ERROR_NONE)
                ok = (check_pat((uint32_t *)hp, words, 0, TAG_HOST) == 0);
        }
        step_line(1, who, "aclrtMallocHost 32MB + H2D/D2H roundtrip + verify (pinned host)", ok, ret);
        if (!ok)
            BAIL(fatal);

        /* 带宽: host pinned 同步可用, 30 次 sync 调用计时 */
        if (fatal) {
            printf("[06] step 1 (r0): bandwidth SKIPPED (correctness not passed)\n");
        } else {
            double t0, t1;
            printf("[06] step 1 (r0): bandwidth, %u MB x %d iters, %d warmup, host pinned sync\n",
                   BW_SIZE >> 20, BW_ITERS, BW_WARMUP);

            /* H2D: pinned host -> dev, 30 sync 调用计时 */
            for (int i = 0; i < BW_WARMUP; i++)
                aclrtMemcpy(da0, BW_SIZE, hp, BW_SIZE, ACL_MEMCPY_HOST_TO_DEVICE);
            t0 = now_sec();
            for (int i = 0; i < BW_ITERS; i++)
                aclrtMemcpy(da0, BW_SIZE, hp, BW_SIZE, ACL_MEMCPY_HOST_TO_DEVICE);
            t1 = now_sec();
            bw_line("H2D", t1 - t0, "30 sync aclrtMemcpy H2D, pinned host");

            /* D2H: dev -> pinned host, 30 sync 调用计时 */
            for (int i = 0; i < BW_WARMUP; i++)
                aclrtMemcpy(hp, BW_SIZE, da0, BW_SIZE, ACL_MEMCPY_DEVICE_TO_HOST);
            t0 = now_sec();
            for (int i = 0; i < BW_ITERS; i++)
                aclrtMemcpy(hp, BW_SIZE, da0, BW_SIZE, ACL_MEMCPY_DEVICE_TO_HOST);
            t1 = now_sec();
            bw_line("D2H", t1 - t0, "30 sync aclrtMemcpy D2H, pinned host");

            step_line(1, who, "bandwidth H2D/D2H (pinned host, see lines above)", 1, 0);
        }
    }
    MPI_Barrier(MPI_COMM_WORLD);   /* rank1 等待 rank0 完成 step1 基线 */
    sync_state(&fatal);

    /* ---- phase 2 (step 2): POSIX shm 跨进程共享 ---- */
    if (!fatal) {
        if (rank == 0) {
            /* rank0: 建池 + ftruncate + mmap + 写 pattern */
            int ok = 0;
            ret = 0;
            shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
            if (shm_fd < 0)
                ret = (int32_t)errno;
            if (ret == 0 && ftruncate(shm_fd, (off_t)POOL_SIZE) != 0)
                ret = (int32_t)errno;
            if (ret == 0) {
                shm_ptr = mmap(NULL, POOL_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
                if (shm_ptr == MAP_FAILED) {
                    ret = (int32_t)errno;
                    shm_ptr = NULL;
                }
            }
            if (ret == 0) {
                shm_mapped = 1;
                fill_pat((uint32_t *)shm_ptr, words, 0, TAG_SHM);
                ok = 1;
            }
            step_line(2, who, "shm_open+ftruncate+mmap 32MB + write pattern (MAP_SHARED, POSIX errno)", ok, ret);
            if (!ok)
                BAIL(fatal);
        }
    }
    MPI_Barrier(MPI_COMM_WORLD);   /* rank0 写完 pattern 后 rank1 才 open */
    if (!fatal) {
        if (rank == 1) {
            /* rank1: 打开已存在的池 + mmap + host 侧直接读 pattern(无需 ACL) */
            int ok = 0;
            ret = 0;
            shm_fd = shm_open(SHM_NAME, O_RDWR, 0666);
            if (shm_fd < 0)
                ret = (int32_t)errno;
            if (ret == 0) {
                shm_ptr = mmap(NULL, POOL_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
                if (shm_ptr == MAP_FAILED) {
                    ret = (int32_t)errno;
                    shm_ptr = NULL;
                }
            }
            if (ret == 0) {
                shm_mapped = 1;
                ok = (check_pat((uint32_t *)shm_ptr, words, 0, TAG_SHM) == 0);
            }
            step_line(2, who, "shm_open+mmap + direct read pattern (host-side, no ACL, POSIX errno)", ok, ret);
            if (!ok)
                BAIL(fatal);
        }
    }
    sync_state(&fatal);

    /* ---- phase 3 (step 3): 共享 host 内存 + H2D 链路 ---- */
    /* 3a: rank1 从共享池 H2D 到本 rank dev + D2H 读回校验(读 rank0 写的 TAG_SHM) */
    if (!fatal) {
        if (rank == 1) {
            int ok = 0;
            ret = aclrtMemcpy(da1, DEV_MEM_SIZE, shm_ptr, POOL_SIZE, ACL_MEMCPY_HOST_TO_DEVICE);
            if (ret == ACL_ERROR_NONE) {
                memset(vbuf, 0, POOL_SIZE);
                ret = aclrtMemcpy(vbuf, POOL_SIZE, da1, DEV_MEM_SIZE, ACL_MEMCPY_DEVICE_TO_HOST);
                if (ret == ACL_ERROR_NONE)
                    ok = (check_pat(vbuf, words, 0, TAG_SHM) == 0);
            }
            step_line(3, who, "H2D from shared pool -> dev + D2H readback + verify", ok, ret);
            if (!ok)
                BAIL(fatal);
        }
    }
    MPI_Barrier(MPI_COMM_WORLD);   /* rank1 读完后 rank0 才改共享区 */
    /* 3b: rank0 改共享区(新 pattern TAG_SHM2) */
    if (!fatal) {
        if (rank == 0 && shm_mapped) {
            fill_pat((uint32_t *)shm_ptr, words, 0, TAG_SHM2);
            printf("[06] step 3 (r0): updated shared pool with new pattern (TAG_SHM2)\n");
        }
    }
    MPI_Barrier(MPI_COMM_WORLD);   /* rank0 改完后 rank1 才 H2D 读新数据 */
    /* 3c: rank1 再 H2D 读新数据 + D2H 校验(验证跨进程更新可见 + H2D 链路) */
    if (!fatal) {
        if (rank == 1) {
            int ok = 0;
            ret = aclrtMemcpy(da1, DEV_MEM_SIZE, shm_ptr, POOL_SIZE, ACL_MEMCPY_HOST_TO_DEVICE);
            if (ret == ACL_ERROR_NONE) {
                memset(vbuf, 0, POOL_SIZE);
                ret = aclrtMemcpy(vbuf, POOL_SIZE, da1, DEV_MEM_SIZE, ACL_MEMCPY_DEVICE_TO_HOST);
                if (ret == ACL_ERROR_NONE)
                    ok = (check_pat(vbuf, words, 0, TAG_SHM2) == 0);
            }
            step_line(3, who, "r0 updated pool -> H2D read new pattern + D2H verify (cross-proc update)", ok, ret);
            if (!ok)
                BAIL(fatal);
        }
    }
    sync_state(&fatal);

    /* ---- phase 4 (step 4, rank0): aclrtHostRegister 边界(预期 207000) ---- */
    /* 310P3: pinned host 必须来自 aclrtMallocHost; 对任意 mmap 区做 host register 预期不支持。
     * 预期失败(207000)或意外成功均如实记录, 不影响 PASS/FAIL。 */
    if (!fatal) {
        if (rank == 0 && shm_mapped) {
            void *reg_dev_ptr = NULL;
            ret = aclrtHostRegister(shm_ptr, (uint64_t)POOL_SIZE, ACL_HOST_REGISTER_MAPPED, &reg_dev_ptr);
            int succ = boundary_line(4, who, "aclrtHostRegister on mmap shm (expect 207000, 310P3)", ret);
            if (succ) {
                /* 意外成功: 显式 unregister, 保持 cleanup 路径干净 */
                reg_ok = 1;
                aclError ur = aclrtHostUnregister(shm_ptr);
                printf("[06] cleanup (r0): aclrtHostUnregister(shm) ret=%d (unexpected-success path)\n", (int)ur);
            }
        }
    }
    sync_state(&fatal);

    /* ---- phase 5 (step 5): shm cleanup ---- */
    /* 双方各自 munmap + close; rank0 额外 shm_unlink 摘名 */
    if (shm_mapped && shm_ptr && shm_ptr != MAP_FAILED) {
        munmap(shm_ptr, POOL_SIZE);
        shm_mapped = 0;
        shm_ptr = NULL;
    }
    if (shm_fd >= 0) {
        close(shm_fd);
        shm_fd = -1;
    }
    MPI_Barrier(MPI_COMM_WORLD);   /* 双方 munmap 完成后 owner 才 shm_unlink */
    if (rank == 0) {
        int ur = shm_unlink(SHM_NAME);
        printf("[06] step 5 (r0): shm_unlink(%s) ret=%d\n", SHM_NAME, ur);
        printf("[06] step 5 (r0): shm cleanup done (munmap + close + shm_unlink)\n");
    } else {
        printf("[06] step 5 (r1): shm cleanup done (munmap + close; owner does shm_unlink)\n");
    }
    fflush(stdout);
    sync_state(&fatal);

    /* ---- final ACL cleanup ---- */
    if (rank == 0) {
        if (hp)
            aclrtFreeHost(hp);
        if (da0)
            aclrtFree(da0);
        printf("[06] cleanup (r0): aclrtFreeHost + aclrtFree done\n");
    } else {
        if (da1)
            aclrtFree(da1);
        printf("[06] cleanup (r1): aclrtFree done\n");
    }
    free(vbuf);
    if (dev_ok)
        aclrtResetDevice(dev);
    if (init_ok)
        aclFinalize();

    /* ---- RESULT(聚合两 rank 状态; 两 rank 打印同一行) ---- */
    {
        int res[2];
        res[0] = g_fail;
        res[1] = reg_ok;
        MPI_Allreduce(MPI_IN_PLACE, res, 2, MPI_INT, MPI_MAX, MPI_COMM_WORLD);
        g_fail = res[0];
        reg_ok = res[1];

        printf("[06] RESULT: %s", g_fail ? "FAIL" : "PASS");
        if (!g_fail && reg_ok)
            printf("  ★观测: aclrtHostRegister 意外成功(310P3 文档为不可用, 实测可用?)");
        printf("\n");
        fflush(stdout);
    }

    MPI_Finalize();
    return g_fail ? 1 : 0;
}
