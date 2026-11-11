/*
 * 07_d2d_matrix — 8 NPU IPC 内存池全链路
 * 机器: Atlas 300I Duo (310P3, 4 卡 x 2 die = 8 dev), CANN 9.1.0, aarch64
 * 结构: MPI 8 进程, rank k = dev k(恒等映射)。die 对 = (0,1)(2,3)(4,5)(6,7)。
 *
 * 验证目标:
 *   8 NPU IPC 内存池 —— 每 rank 建 slab + export key -> 全体交换 ->
 *   token 环串行 join(rank 序 import, die 对用 DEFAULT / 跨卡用 PEER_ACCESS) ->
 *   全网写+校验 -> VA 一致性比对。
 *
 * 阶段:
 *   ① init + create: aclInit -> SetDevice(k) -> warm malloc(4KB) -> stream +
 *      16MB scratch -> slab_k = aclrtMalloc(16MB, HUGE_FIRST_P2P) -> H2D pattern
 *      (rank=k, tag=0x07) -> aclrtIpcMemGetExportKey(slab, 16MB, key, 65,
 *      DISABLE_PID_VALIDATION)。打印 `[07] r%d create slab @ %p, key exported`。
 *   ② 交换: MPI_Allgather 8 个 key(65B x 8, MPI_CHAR)。
 *   ③ join(token 环串行): token=0..7, 仅 token==my_rank 的 rank 在两 Barrier 间
 *      串行 import 全部 7 个 peer(die 对 DEFAULT / 跨卡 PEER_ACCESS)。打印
 *      `[07] r%d joined %d/7 peers`。8 rank 同序 import 是阶段④ VA 一致性的前提。
 *   ④ VA 一致性: MPI_Allgather 每 rank 的 va_tab[0..7](8 x u64) -> rank0 比对
 *      8 行是否完全一致 -> 打印矩阵 + CONSISTENT/INCONSISTENT。
 *   ⑤ 全网写+校验(简化): 每 rank r 写 1 个 peer p=(r+1)%8(async D2D 16MB
 *      own_pattern -> ipc_va[p] + Sync) -> MPI_Allgather delivered bitmap ->
 *      每 rank 校验自己被 writer w=(r+7)%8 写入的 pattern(tag=0x70+w) D2H 逐字校验。
 *   ⑥ 带宽(可选, 正确性全过后): token 环串行, 每 rank 对 die partner(r^1)和 1 个
 *      跨卡 peer((r+2)%8)各跑一次 async D2D 读(3 预热 + 30x4MB async + 1 Sync)。
 *   ⑦ cleanup: 先 MPI_Allreduce 聚合 RESULT -> 再 cleanup(参考 gva_pool.c 正常退出
 *      顺序): IpcMemClose(own + 各 import) -> Barrier -> Free(slab/scratch/warm)
 *      -> DestroyStream -> ★DisablePeerAccess(各跨卡 peer) -> ResetDevice ->
 *      Finalize -> MPI_Finalize -> return。
 *
 * ★★★ D 态修复(最重要) ★★★
 *   本测试 8 rank 跨卡大量 EnablePeerAccess(PEER_ACCESS flag 内部双向 enable)。
 *   cleanup 必须对每个跨卡 peer 调 aclrtDeviceDisablePeerAccess(peer) before
 *   aclrtResetDevice(dev)。否则 8x进程退出时全部 D 态挂死(exit_mmap ->
 *   synchronize_srcu 永久等待, SIGKILL 不可回收), hugepages 耗尽, 机器要重启。
 *   根因同 03/04/12: 残留 P2P 映射 + 退出时 mmu notifier = 死锁。
 *   具体: cleanup 遍历所有 peer p(p != my_rank 且 imp_ok[p] 且 !is_die_pair, 即
 *   跨卡 PEER import 过的 p), aclrtDeviceDisablePeerAccess(p)。
 *
 * 校验 pattern: value = (rank<<28)|(tag<<24)|(idx&0xFFFFFF)
 *   rank = 写入方 MPI rank, tag 每步不同(0x07 建池 / 0x70+r 写)防残留假阳性。
 *
 * 判定: RESULT: PASS = (join 7/7 全 OK) && (写校验 OK) && (VA 一致)。
 *
 * ★带宽测量铁律: 严禁同步循环测带宽(历史教训 02/03): 正确口径 3 预热 +
 *   30x4MB MemcpyAsync 提交 + 末尾 1 次 StreamSync。
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
#define SLAB_SIZE       (16u * 1024u * 1024u)   /* 16 MB 每 rank slab */
#define SCRATCH_SIZE    (16u * 1024u * 1024u)   /* 16 MB 本地暂存(写 pattern 源 / 带宽落点) */
#define BW_SIZE         (4u  * 1024u * 1024u)    /* 4 MB 带宽块 */
#define BW_WARMUP       3
#define BW_ITERS        30
#define WARM_MALLOC     (4u * 1024u)            /* 4 KB warm malloc */

#define TAG_CREATE      0x07   /* 阶段①: 建池 pattern(rank=k) */
#define TAG_WRITE_BASE  0x70  /* 阶段⑤: writer r 用 tag=0x70+r */

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

/* check_pat: who = 调用方 MPI rank(并发 8 rank 输出区分用) */
static int check_pat(const uint32_t *buf, size_t words, uint32_t rank, uint32_t tag, int who)
{
    for (size_t i = 0; i < words; i++) {
        if (buf[i] != pat(rank, tag, i)) {
            printf("[07] r%d [verify] mismatch at word %zu (off 0x%zx): got 0x%08x, want 0x%08x\n",
                   who, i, i * sizeof(uint32_t), buf[i], pat(rank, tag, i));
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

/* 同卡两 die(0,1)(2,3)(4,5)(6,7) */
static int is_die_pair(int i, int j)
{
    return i != j && (i / 2) == (j / 2);
}

/* 阶段栅栏 + fatal/g_fail 全局同步: 任一 rank 置位, 全体都看到 */
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
            printf("[07] FATAL: need exactly %d MPI procs (-np %d), got %d\n", N, N, nproc);
        MPI_Finalize();
        return 1;
    }

    int32_t dev = rank;   /* 恒等映射: rank k = dev k */
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("[07] === 07_d2d_matrix: 8 NPU IPC pool, rank%d = dev%d ===\n", rank, dev);
    printf("[07] r%d: 16MB slab, key len %dB, pattern=(rank<<28)|(tag<<24)|(idx&0xFFFFFF)\n",
           rank, IPC_KEY_LEN);

    aclrtStream stream = NULL;
    void *slab = NULL;          /* 本 rank 导出的 16MB slab */
    void *scratch = NULL;       /* 16MB 本地暂存(写 pattern 源 + 带宽落点) */
    void *warm = NULL;          /* 4KB warm malloc */
    uint32_t *hbuf = NULL, *vbuf = NULL;
    char mykey[IPC_KEY_LEN] = {0};
    char keys[N][IPC_KEY_LEN];
    void *ipc_va[N];           /* 本 rank import 的各 peer slab VA(NULL=未 import) */
    uint64_t va_tab[N];        /* 本 rank 看到的各 slab VA(对角=own slab) */
    int imp_ok[N];             /* import 成功标记 */
    int fatal = 0, join_ok = 0, write_ok = 0, va_ok = 0;
    int32_t ret = 0;
    int init_ok = 0, dev_ok = 0;
    size_t words = SLAB_SIZE / sizeof(uint32_t);

    memset(keys, 0, sizeof(keys));
    for (int i = 0; i < N; i++) { ipc_va[i] = NULL; va_tab[i] = 0; imp_ok[i] = 0; }

    /* ---- 阶段① init + create ---- */
    ret = aclInit(NULL);
    if (ret != ACL_ERROR_NONE) {
        printf("[07] r%d init: aclInit FAIL ret=%d\n", rank, ret);
        BAIL(fatal);
    } else {
        init_ok = 1;
        ret = aclrtSetDevice(dev);
        if (ret != ACL_ERROR_NONE) {
            printf("[07] r%d init: aclrtSetDevice(%d) FAIL ret=%d\n", rank, dev, ret);
            BAIL(fatal);
        }
    }
    if (!fatal) {
        dev_ok = 1;
        /* warm malloc 4KB(预热设备分配器, 失败不致命) */
        aclrtMalloc(&warm, WARM_MALLOC, ACL_MEM_MALLOC_HUGE_FIRST);
        ret = aclrtCreateStream(&stream);
        if (ret != ACL_ERROR_NONE) {
            printf("[07] r%d init: aclrtCreateStream FAIL ret=%d\n", rank, ret);
            BAIL(fatal);
        }
        if (!fatal) {
            ret = aclrtMalloc(&scratch, SCRATCH_SIZE, ACL_MEM_MALLOC_HUGE_FIRST);
            if (ret != ACL_ERROR_NONE) {
                printf("[07] r%d init: malloc scratch 16MB FAIL ret=%d\n", rank, ret);
                BAIL(fatal);
            }
        }
        if (!fatal) {
            hbuf = (uint32_t *)malloc(SLAB_SIZE);
            vbuf = (uint32_t *)malloc(SLAB_SIZE);
            if (!hbuf || !vbuf) {
                printf("[07] r%d init: host malloc 2x16MB FAIL\n", rank);
                BAIL(fatal);
            }
        }
    }
    if (!fatal) {
        ret = aclrtMalloc(&slab, SLAB_SIZE, ACL_MEM_MALLOC_HUGE_FIRST_P2P);
        if (ret != ACL_ERROR_NONE) {
            printf("[07] r%d create: malloc slab 16MB (P2P) FAIL ret=%d\n", rank, ret);
            BAIL(fatal);
        }
    }
    if (!fatal) {
        fill_pat(hbuf, words, (uint32_t)rank, TAG_CREATE);
        ret = aclrtMemcpy(slab, SLAB_SIZE, hbuf, SLAB_SIZE, ACL_MEMCPY_HOST_TO_DEVICE);
        if (ret != ACL_ERROR_NONE) {
            printf("[07] r%d create: H2D pattern FAIL ret=%d\n", rank, ret);
            BAIL(fatal);
        }
    }
    if (!fatal) {
        ret = aclrtIpcMemGetExportKey(slab, SLAB_SIZE, mykey, IPC_KEY_LEN,
                                      ACL_RT_IPC_MEM_EXPORT_FLAG_DISABLE_PID_VALIDATION);
        if (ret != ACL_ERROR_NONE) {
            printf("[07] r%d create: export key FAIL ret=%d\n", rank, ret);
            BAIL(fatal);
        }
        printf("[07] r%d create slab @ %p, key exported\n", rank, slab);
    }
    sync_state(&fatal);

    /* ---- 阶段② 交换 keys(全体 Allgather 8 个 key) ---- */
    if (!fatal) {
        MPI_Allgather(mykey, IPC_KEY_LEN, MPI_CHAR,
                      &keys[0][0], IPC_KEY_LEN, MPI_CHAR, MPI_COMM_WORLD);
        printf("[07] r%d exchange: allgathered %d keys\n", rank, N);
    }
    sync_state(&fatal);

    /* ---- 阶段③ join(token 环串行, rank 序 import) ----
     * token=0..7: 两 Barrier 间仅 token==my_rank 的 rank 串行 import 7 个 peer。
     * 8 rank 同序 import 是阶段④ VA 一致性的前提(同序 -> 同 VA 分配序列)。 */
    if (!fatal) {
        printf("[07] r%d join: token-ring serial import begins\n", rank);
        for (int token = 0; token < N; token++) {
            MPI_Barrier(MPI_COMM_WORLD);
            if (token == rank) {
                for (int p = 0; p < N; p++) {
                    if (p == rank) {
                        va_tab[p] = (uint64_t)(uintptr_t)slab;
                        continue;
                    }
                    int die = is_die_pair(rank, p);
                    uint32_t flag = die ? ACL_RT_IPC_MEM_IMPORT_FLAG_DEFAULT
                                        : ACL_RT_IPC_MEM_IMPORT_FLAG_ENABLE_PEER_ACCESS;
                    void *pv = NULL;
                    int32_t r = aclrtIpcMemImportByKey(&pv, keys[p], flag);
                    imp_ok[p] = (r == ACL_ERROR_NONE);
                    if (imp_ok[p]) {
                        ipc_va[p] = pv;
                        va_tab[p] = (uint64_t)(uintptr_t)pv;
                    }
                    printf("[07]   r%d import slab p%d (%s): %s (ret=%d) va=0x%012lx\n",
                           rank, p, die ? "die/DEFAULT" : "cross/PEER",
                           imp_ok[p] ? "OK" : "FAIL", r, (unsigned long)va_tab[p]);
                }
                int joined = 0;
                for (int p = 0; p < N; p++)
                    if (p != rank && imp_ok[p]) joined++;
                join_ok = (joined == N - 1);
                if (!join_ok)
                    g_fail = 1;
                printf("[07] r%d joined %d/7 peers\n", rank, joined);
            }
            MPI_Barrier(MPI_COMM_WORLD);
        }
    }
    sync_state(&fatal);

    /* ---- 阶段④ VA 一致性(8 rank 同序 import -> 期望 VA 表一致) ---- */
    if (!fatal) {
        uint64_t va_all[N * N];   /* va_all[r*N + p] = rank r 看到的 slab p VA */
        MPI_Allgather(va_tab, N * sizeof(uint64_t), MPI_BYTE,
                      va_all, N * sizeof(uint64_t), MPI_BYTE, MPI_COMM_WORLD);
        /* 比对: 8 行是否完全一致(每列 p 的 VA 在所有 rank 间相同) */
        va_ok = 1;
        for (int r = 1; r < N; r++) {
            for (int p = 0; p < N; p++) {
                if (va_all[r * N + p] != va_all[0 * N + p]) {
                    va_ok = 0;
                    break;
                }
            }
            if (!va_ok)
                break;
        }
        if (rank == 0) {
            printf("[07] VA matrix (row=rank observer, col=slab owner):\n");
            printf("[07]        ");
            for (int p = 0; p < N; p++) printf("  p%d        ", p);
            printf("\n");
            for (int r = 0; r < N; r++) {
                printf("[07] r%d  ", r);
                for (int p = 0; p < N; p++)
                    printf("0x%012lx ", (unsigned long)va_all[r * N + p]);
                printf("\n");
            }
            printf("[07] VA consistency: %s\n",
                   va_ok ? "CONSISTENT (all 8 rows identical)" : "INCONSISTENT");
        }
        if (!va_ok)
            g_fail = 1;
    }
    sync_state(&fatal);

    /* ---- 阶段⑤ 全网写+校验(简化: 每 rank r 写 1 个 peer p=(r+1)%8) ----
     * 每 rank r: H2D own_pattern(tag=0x70+r) -> scratch -> async D2D 16MB ->
     * ipc_va[(r+1)%8] + Sync -> MPI_Allgather delivered bitmap。
     * 每 rank 校验自己被 writer w=(r+7)%8 写入的 pattern D2H 逐字校验。 */
    if (!fatal) {
        int w_dst = (rank + 1) % N;       /* 我写谁 */
        int w_me = (rank + N - 1) % N;    /* 谁写我 = (r+7)%8 */

        /* 准备 own write pattern(rank, tag=0x70+rank) H2D 到 scratch */
        fill_pat(hbuf, words, (uint32_t)rank, (uint32_t)(TAG_WRITE_BASE + rank));
        ret = aclrtMemcpy(scratch, SCRATCH_SIZE, hbuf, SLAB_SIZE, ACL_MEMCPY_HOST_TO_DEVICE);
        if (ret != ACL_ERROR_NONE) {
            printf("[07] r%d write: H2D own pattern FAIL ret=%d\n", rank, ret);
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
                printf("[07] r%d write D2D 16MB -> p%d: %s (ret=%d)\n",
                       rank, w_dst, delivered ? "OK" : "FAIL", ret);
            } else {
                printf("[07] r%d write D2D 16MB -> p%d: SKIP (import not ok)\n", rank, w_dst);
            }
            /* Allgather delivered bitmap(集体同步: 所有写落地后才校验) */
            int dmap[N];
            MPI_Allgather(&delivered, 1, MPI_INT, dmap, 1, MPI_INT, MPI_COMM_WORLD);
            /* 校验: 我的 slab 被 w_me 写过? */
            if (dmap[w_me]) {
                memset(vbuf, 0, SLAB_SIZE);
                ret = aclrtMemcpy(vbuf, SLAB_SIZE, slab, SLAB_SIZE, ACL_MEMCPY_DEVICE_TO_HOST);
                if (ret == ACL_ERROR_NONE) {
                    int ok = (check_pat(vbuf, words, (uint32_t)w_me,
                                        (uint32_t)(TAG_WRITE_BASE + w_me), rank) == 0);
                    write_ok = ok;
                    printf("[07] r%d verify own slab (writer p%d, tag=0x%x): %s\n",
                           rank, w_me, TAG_WRITE_BASE + w_me, ok ? "OK" : "FAIL");
                    if (!ok)
                        g_fail = 1;
                } else {
                    printf("[07] r%d verify: D2H FAIL ret=%d\n", rank, ret);
                    g_fail = 1;
                }
            } else {
                printf("[07] r%d verify: writer p%d did not deliver, skip\n", rank, w_me);
                g_fail = 1;
            }
        }
    }
    sync_state(&fatal);

    /* ---- 阶段⑥ 带宽(可选, 正确性全过后; token 环串行避免 8 路并发争用链路) ----
     * 每 rank 对 die partner(r^1)和 1 个跨卡 peer((r+2)%8)各跑一次 async D2D 读
     * (3 预热 + 30x4MB async 提交 + 末尾 1 次 StreamSync)。严禁同步循环测带宽。 */
    if (!fatal && g_fail) {
        printf("[07] r%d bandwidth: SKIPPED (correctness not passed)\n", rank);
    }
    if (!fatal && !g_fail) {
        int die_p = rank ^ 1;
        int cross_p = (rank + 2) % N;
        double gb = (double)BW_ITERS * (double)BW_SIZE / 1e9;
        printf("[07] r%d bandwidth (token-ring serial): die p%d + cross p%d, "
               "%uMB x %d iters + %d warmup, async+1Sync\n",
               rank, die_p, cross_p, BW_SIZE >> 20, BW_ITERS, BW_WARMUP);
        for (int token = 0; token < N; token++) {
            MPI_Barrier(MPI_COMM_WORLD);
            if (token == rank) {
                /* die partner 读 */
                if (imp_ok[die_p]) {
                    for (int i = 0; i < BW_WARMUP; i++) {
                        aclrtMemcpyAsync(scratch, BW_SIZE, ipc_va[die_p], BW_SIZE,
                                         ACL_MEMCPY_DEVICE_TO_DEVICE, stream);
                        aclrtSynchronizeStream(stream);
                    }
                    double t0 = now_sec();
                    for (int i = 0; i < BW_ITERS; i++)
                        aclrtMemcpyAsync(scratch, BW_SIZE, ipc_va[die_p], BW_SIZE,
                                         ACL_MEMCPY_DEVICE_TO_DEVICE, stream);
                    aclrtSynchronizeStream(stream);
                    double t1 = now_sec();
                    printf("[07] r%d bw die  p%d->scratch: %7.2f GB/s "
                           "(%dx%uMB async + 1Sync, %.3fs)\n",
                           rank, die_p, gb / (t1 - t0), BW_ITERS, BW_SIZE >> 20, t1 - t0);
                }
                /* 跨卡 peer 读 */
                if (imp_ok[cross_p]) {
                    for (int i = 0; i < BW_WARMUP; i++) {
                        aclrtMemcpyAsync(scratch, BW_SIZE, ipc_va[cross_p], BW_SIZE,
                                         ACL_MEMCPY_DEVICE_TO_DEVICE, stream);
                        aclrtSynchronizeStream(stream);
                    }
                    double t0 = now_sec();
                    for (int i = 0; i < BW_ITERS; i++)
                        aclrtMemcpyAsync(scratch, BW_SIZE, ipc_va[cross_p], BW_SIZE,
                                         ACL_MEMCPY_DEVICE_TO_DEVICE, stream);
                    aclrtSynchronizeStream(stream);
                    double t1 = now_sec();
                    printf("[07] r%d bw cross p%d->scratch: %7.2f GB/s "
                           "(%dx%uMB async + 1Sync, %.3fs)\n",
                           rank, cross_p, gb / (t1 - t0), BW_ITERS, BW_SIZE >> 20, t1 - t0);
                }
                fflush(stdout);
            }
            MPI_Barrier(MPI_COMM_WORLD);
        }
    }
    sync_state(&fatal);

    /* ---- RESULT(先 Allreduce 聚合, 再 cleanup; 参考 gva_pool.c 正常退出顺序) ---- */
    {
        int res[4];
        res[0] = g_fail;       /* 全局失败(含 fatal) */
        res[1] = !join_ok;     /* join 未全 OK */
        res[2] = !write_ok;    /* 写校验未过 */
        res[3] = !va_ok;       /* VA 不一致 */
        MPI_Allreduce(MPI_IN_PLACE, res, 4, MPI_INT, MPI_MAX, MPI_COMM_WORLD);
        g_fail = res[0];
        int join_fail = res[1], write_fail = res[2], va_fail = res[3];
        printf("[07] r%d RESULT: %s (join:%s write:%s VA:%s)\n", rank,
               g_fail ? "FAIL" : "PASS",
               join_fail ? "FAIL" : "OK",
               write_fail ? "FAIL" : "OK",
               va_fail ? "INCONSISTENT" : "CONSISTENT");
        fflush(stdout);
    }

    /* ---- 阶段⑦ cleanup ----
     * 顺序: IpcMemClose(own + 各 import) -> Barrier(所有 import unmap 后才 free 物理)
     *       -> Free(slab/scratch/warm) -> DestroyStream ->
     *       ★DisablePeerAccess(各跨卡 peer) -> ResetDevice -> Finalize。
     * ★D态修复: 跨卡 PEER import 内部 EnablePeerAccess 的 P2P 状态必须显式拆除,
     *   否则 ResetDevice 后进程退出 -> mmu notifier 与驱动 SRCU 死锁 -> D 态挂死。 */
    if (init_ok) {
        /* close 自己导出的 key + 各 import 的 key(双方都 close, 同 02/03 语义) */
        int32_t cr = aclrtIpcMemClose(mykey);
        printf("[07] r%d cleanup: IpcMemClose(own key) ret=%d\n", rank, cr);
        for (int p = 0; p < N; p++) {
            if (p != rank && imp_ok[p]) {
                cr = aclrtIpcMemClose(keys[p]);
                printf("[07] r%d cleanup: IpcMemClose(key[p%d]) ret=%d\n", rank, p, cr);
            }
        }
    }
    MPI_Barrier(MPI_COMM_WORLD);   /* 所有 rank close 完(import unmap)后才 free 物理内存 */
    if (slab)
        aclrtFree(slab);
    if (scratch)
        aclrtFree(scratch);
    if (warm)
        aclrtFree(warm);
    if (stream)
        aclrtDestroyStream(stream);
    free(hbuf);
    free(vbuf);
    /* ★D态修复: 遍历所有跨卡 peer(PEER import 过的 p)DisablePeerAccess。
     * die 对用 DEFAULT import, 无 EnablePeerAccess, 不需 disable(同 02 无 D态)。
     * 当前 device 仍为 my_rank(SetDevice 后未变), DisablePeerAccess(p) 撤销 my_rank->p 的 P2P。 */
    if (dev_ok) {
        for (int p = 0; p < N; p++) {
            if (p != rank && imp_ok[p] && !is_die_pair(rank, p)) {
                aclError dr = aclrtDeviceDisablePeerAccess(p);
                printf("[07] r%d cleanup: DisablePeerAccess(p%d) ret=%d\n", rank, p, dr);
            }
        }
        aclrtResetDevice(dev);
    }
    if (init_ok)
        aclFinalize();

    MPI_Finalize();
    return g_fail ? 1 : 0;
}
