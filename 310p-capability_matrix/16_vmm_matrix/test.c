/*
 * 16_vmm_matrix — 8 NPU VMM 内存池全连接(对照 07_d2d_matrix 的 IPC 版本)
 * 机器: Atlas 300I Duo (310P3, 4 卡 x 2 die = 8 dev), CANN 9.1.0, aarch64
 * 结构: MPI 8 进程, rank k = dev k(恒等映射)。die 对 = (0,1)(2,3)(4,5)(6,7)。
 *
 * 背景(07 失败根因):
 *   IPC 8-rank 全连接不可达 —— 每卡 8 个入向 P2P 授权槽位(4 卡 x 8 = 32 上限),
 *   全连接需 card0 参与 12 条跨卡关系(12 > 8), PEER_ACCESS flag import 内部的
 *   halDeviceEnableP2P 必然触顶(507899)。
 *   本测试验证: VMM shareable handle 路径(MallocPhysical/ImportFromShareableHandle)
 *   全程不调用 EnablePeerAccess(04 step16 实测 import 不依赖 EPA), 是否可绕过
 *   P2P 槽位配额, 实现 56 条有向链路全连接。
 *
 * 建池方式(gva_pool 同款槽位窗口):
 *   每 rank 预留 128MB VA 窗口(N x 16MB slot), slot p = base + p*16MB;
 *   own 物理内存 MapMem 到 slot r, import 的 peer 物理内存 MapMem 到 slot p
 *   -> 所有 rank 的 slot 布局一致(VA 矩阵按构造对称)。
 *
 * 阶段:
 *   ① init+create: aclInit -> SetDevice(k) -> stream + 16MB scratch ->
 *      MallocPhysical 16MB(七级 memAttr 回退链, 310P3 仅 ACL_MEM_P2P_HUGE 成功,
 *      gran=2MB) -> ReserveMemAddress(128MB, NULL hint) -> MapMem own@slot r ->
 *      SetAccess(RW,k) -> H2D pattern(rank,tag=0x16) ->
 *      ExportToShareableHandle(DISABLE_PID_VALIDATION) -> sh(u64)。
 *   ② 交换: MPI_Allgather 8 个 sh(u64 x 8)。
 *   ③ join(token 环串行): token=0..7, 两 Barrier 间仅 token==my_rank 的 rank
 *      串行 import 7 个 peer。import 路径(均无显式 EnablePeerAccess):
 *        die 对  -> method2 ctx 技巧(05 验证的主路径): SetDevice(p_dev) ->
 *                   Import(sh[p], my_dev) -> SetDevice(my_dev)
 *                   (05 对照: die 对 own-ctx import 确定性 507899)
 *        跨卡    -> method1 本进程 ctx: Import(sh[p], my_dev, &h)
 *                   (04 step16 实测: 跨卡 import 不依赖显式 EPA;
 *                    507899 时 200ms 重试 1 次; 仍失败回退 method3 ctx 技巧)
 *      import 成功后 MapMem@slot p + SetAccess(RW, my_dev)。
 *      打印 `[16] r%d joined %d/7 peers`。
 *   ④ VA 矩阵: Allgather 每 rank va_tab[0..7](slot 布局) -> rank0 打印 8x8
 *      矩阵 + 比对一致性(OBS, 不判 FAIL —— NULL hint 下 base 由各进程 VA 分配器
 *      决定, 一致性是平台观测值而非构造保证)。
 *   ⑤ 全网写+校验: 每 rank r 写 p=(r+1)%8(H2D pattern(rank,tag=0x60+r) ->
 *      scratch -> async D2D 16MB -> slot p + Sync) -> Allgather delivered ->
 *      每 rank D2H own slab(slot r) 逐字校验 writer w=(r+7)%8 的 pattern。
 *   ⑥ 带宽(正确性全过后, token 环串行): 每 rank 对 die partner(r^1)和跨卡
 *      peer((r+2)%8)各跑 async D2D 读(3 预热 + 30x4MB async + 末尾 1 Sync)。
 *   ⑦ cleanup: UnmapMem(各 import slot) -> FreePhysical(各 imported handle)
 *      -> Barrier(全体 import 释放后才 free 物理内存) -> UnmapMem(own slot) ->
 *      FreePhysical(own) -> ReleaseMemAddress(128MB 窗口) -> Free(scratch) ->
 *      DestroyStream -> DisablePeerAccess(7 peer, best-effort, 本测试从未
 *      enable, 预期全 507899 无害) -> ResetDevice(dev) -> ResetDevice(ctx 技巧
 *      开过的额外 dev) -> Finalize。
 *
 * 校验 pattern: value = (rank<<28)|(tag<<24)|(idx&0xFFFFFF), tag 每步不同
 *   (0x16 建池 / 0x60+r 写)防残留假阳性。
 *
 * 判定: RESULT: PASS = (8 rank join 全 7/7) && (写校验全 OK)。
 *   VA 一致性仅 OBS 报告。
 *
 * ★带宽测量铁律: 严禁同步循环测带宽(历史教训 02/03): 正确口径 3 预热 +
 *   30x4MB MemcpyAsync 提交 + 末尾 1 次 StreamSync。
 * ★本测试全程不调用 aclrtDeviceEnablePeerAccess —— 这是与 07 的本质差异,
 *   也是"VMM 是否绕过 P2P 槽位配额"这一探针问题的干净性保证。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <mpi.h>
#include "acl/acl.h"

#define N               8
#define SLAB_SIZE       (16u * 1024u * 1024u)   /* 16 MB 每 rank slab(默认, SLAB_MB 可调) */
#define SCRATCH_SIZE    (16u * 1024u * 1024u)   /* 16 MB 本地暂存 */
#define BW_SIZE         (4u  * 1024u * 1024u)    /* 4 MB 带宽块(clamp 到 slab 大小) */
#define BW_WARMUP       3
#define BW_ITERS        30
#define EXPECTED_ERR    507899  /* ACL_ERROR_RT_DRV_INTERNAL_ERROR */

static size_t g_slab = SLAB_SIZE;    /* 每 rank slab 大小(SLAB_MB 环境变量覆盖) */

#define TAG_CREATE      0x16   /* 阶段①: 建池 pattern(rank=k) */
#define TAG_WRITE_BASE  0x60   /* 阶段⑤: writer r 用 tag=0x60+r */

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
            printf("[16] r%d [verify] mismatch at word %zu (off 0x%zx): got 0x%08x, want 0x%08x\n",
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

/* SetAccess(RW, devId): 失败仅 obs, 数据校验为准(同 04/05/09) */
static int32_t set_access_rw(void *va, size_t size, int32_t devId)
{
    aclrtMemAccessDesc desc;
    memset(&desc, 0, sizeof(desc));
    desc.flags = ACL_RT_MEM_ACCESS_FLAGS_READWRITE;
    desc.location.type = ACL_MEM_LOCATION_TYPE_DEVICE;
    desc.location.id = (uint32_t)devId;
    return aclrtMemSetAccess(va, size, &desc, 1);
}

/* memAttr 七级回退链(310P3 仅 ACL_MEM_P2P_HUGE 成功) */
static const aclrtMemAttr g_chain[7] = {
    ACL_HBM_MEM_P2P_HUGE, ACL_HBM_MEM_P2P_NORMAL,
    ACL_DDR_MEM_P2P_HUGE, ACL_DDR_MEM_P2P_NORMAL,
    ACL_MEM_P2P_HUGE,     ACL_MEM_P2P_NORMAL,
    ACL_HBM_MEM_HUGE,
};
static const char *g_chainName[7] = {
    "HBM_P2P_HUGE", "HBM_P2P_NORMAL",
    "DDR_P2P_HUGE", "DDR_P2P_NORMAL",
    "MEM_P2P_HUGE", "MEM_P2P_NORMAL",
    "HBM_HUGE",
};

/* memAttr 七级回退 MallocPhysical: 依次尝试, 首个成功即用 */
static int32_t malloc_physical_chain(int32_t devId, size_t size,
                                      aclrtDrvMemHandle *handle, const char *label,
                                      int *chosenIdx)
{
    int32_t last = 0;
    for (int i = 0; i < 7; i++) {
        aclrtPhysicalMemProp prop;
        memset(&prop, 0, sizeof(prop));
        prop.handleType = ACL_MEM_HANDLE_TYPE_NONE;
        prop.allocationType = ACL_MEM_ALLOCATION_TYPE_PINNED;
        prop.location.type = ACL_MEM_LOCATION_TYPE_DEVICE;
        prop.location.id = (uint32_t)devId;
        prop.memAttr = g_chain[i];
        size_t gran = 0;
        aclError gr = aclrtMemGetAllocationGranularity(
            &prop, ACL_RT_MEM_ALLOC_GRANULARITY_RECOMMENDED, &gran);
        aclError m = aclrtMallocPhysical(handle, size, &prop, 0);
        printf("[16] r%d [%s attr %d/7 %-14s] gran ret=%d gran=%zu; "
               "MallocPhysical(%uMB) ret=%d\n",
               devId, label, i + 1, g_chainName[i], gr, gran,
               (unsigned)(size >> 20), m);
        fflush(stdout);
        if (m == ACL_ERROR_NONE) {
            *chosenIdx = i;
            if (gran != 0 && size % gran != 0)
                printf("[16]   [%s] WARN: size %zu NOT multiple of gran %zu "
                       "(MapMem size must equal MallocPhysical size!)\n",
                       label, size, gran);
            else
                printf("[16]   [%s] chosen %-14s, %uMB = %zu x gran %zuB\n",
                       label, g_chainName[i], (unsigned)(size >> 20),
                       gran ? size / gran : 0, gran);
            fflush(stdout);
            return 0;
        }
        last = m;
        *handle = NULL;
    }
    printf("[16]   [%s] all 7 attrs FAILED (last ret=%d)\n", label, last);
    fflush(stdout);
    return last != 0 ? last : -1;
}

int main(int argc, char *argv[])
{
    MPI_Init(&argc, &argv);
    int rank = 0, nproc = 0;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &nproc);
    if (nproc != N) {
        if (rank == 0)
            printf("[16] FATAL: need exactly %d MPI procs (-np %d), got %d\n", N, N, nproc);
        MPI_Finalize();
        return 1;
    }

    int32_t dev = rank;   /* 恒等映射: rank k = dev k */
    {
        const char *e = getenv("SLAB_MB");
        if (e && e[0]) {
            long mb = atol(e);
            if (mb >= 2) g_slab = (size_t)mb * 1024u * 1024u;
        }
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("[16] === 16_vmm_matrix: 8 NPU VMM pool (NO EnablePeerAccess), rank%d = dev%d ===\n",
           rank, dev);
    printf("[16] r%d: %zuMB slab x8 slot window(%zuMB), 7-attr chain, "
           "pattern=(rank<<28)|(tag<<24)|(idx&0xFFFFFF)\n",
           rank, g_slab >> 20, g_slab * N >> 20);

    aclrtStream stream = NULL;
    aclrtDrvMemHandle h_own = NULL;     /* 本 rank 16MB 物理内存 */
    uint64_t sh_own = 0;                /* 本 rank 导出的 shareable handle */
    void *pool = NULL;                  /* 128MB 槽位窗口(slot p = pool + p*SLAB) */
    int pool_reserved = 0, own_mapped = 0;
    int own_attr = -1;
    aclrtDrvMemHandle h_imp[N];         /* import 的各 peer 物理内存 handle */
    int imp_ok[N];                      /* import+map 成功标记 */
    int imp_mapped[N];
    int imp_method[N];                  /* 1=own-ctx, 2=ctx-trick */
    int extra_ctx[N];                   /* ctx 技巧在本进程开过的额外 device ctx */
    void *scratch = NULL;               /* 16MB 本地暂存(写 pattern 源 + 带宽落点) */
    uint32_t *hbuf = NULL, *vbuf = NULL;
    uint64_t sh_all[N];
    uint64_t va_tab[N];
    int fatal = 0, join_ok = 0, write_ok = 0, va_ok = 0;
    int32_t ret = 0;
    int init_ok = 0, dev_ok = 0;
    size_t words = g_slab / sizeof(uint32_t);

    memset(sh_all, 0, sizeof(sh_all));
    for (int i = 0; i < N; i++) {
        h_imp[i] = NULL; imp_ok[i] = 0; imp_mapped[i] = 0;
        imp_method[i] = 0; extra_ctx[i] = 0; va_tab[i] = 0;
    }

    /* ---- 阶段① init + create ---- */
    ret = aclInit(NULL);
    if (ret != ACL_ERROR_NONE) {
        printf("[16] r%d init: aclInit FAIL ret=%d\n", rank, ret);
        BAIL(fatal);
    } else {
        init_ok = 1;
        ret = aclrtSetDevice(dev);
        if (ret != ACL_ERROR_NONE) {
            printf("[16] r%d init: aclrtSetDevice(%d) FAIL ret=%d\n", rank, dev, ret);
            BAIL(fatal);
        }
    }
    if (!fatal) {
        dev_ok = 1;
        ret = aclrtCreateStream(&stream);
        if (ret != ACL_ERROR_NONE) {
            printf("[16] r%d init: aclrtCreateStream FAIL ret=%d\n", rank, ret);
            BAIL(fatal);
        }
        if (!fatal) {
            ret = aclrtMalloc(&scratch, SCRATCH_SIZE, ACL_MEM_MALLOC_HUGE_FIRST);
            if (ret != ACL_ERROR_NONE) {
                printf("[16] r%d init: malloc scratch 16MB FAIL ret=%d\n", rank, ret);
                BAIL(fatal);
            }
        }
        if (!fatal) {
            hbuf = (uint32_t *)malloc(g_slab);
            vbuf = (uint32_t *)malloc(g_slab);
            if (!hbuf || !vbuf) {
                printf("[16] r%d init: host malloc 2x16MB FAIL\n", rank);
                BAIL(fatal);
            }
        }
    }
    if (!fatal)
        ret = malloc_physical_chain(dev, g_slab, &h_own, "slab", &own_attr);
    if (!fatal && ret == ACL_ERROR_NONE)
        ret = aclrtReserveMemAddress(&pool, g_slab * N, 0, NULL, 0);
    if (!fatal && ret == ACL_ERROR_NONE) {
        pool_reserved = 1;
        /* own 物理内存映射到 slot r */
        void *own_slot = (uint8_t *)pool + (uint64_t)rank * g_slab;
        ret = aclrtMapMem(own_slot, g_slab, 0, h_own, 0);
        if (ret == ACL_ERROR_NONE) {
            own_mapped = 1;
            int32_t sa = set_access_rw(own_slot, g_slab, dev);
            printf("[16] r%d create: pool base=%p own slot%d @ %p (SetAccess ret=%d)\n",
                   rank, pool, rank, own_slot, sa);
            fill_pat(hbuf, words, (uint32_t)rank, TAG_CREATE);
            ret = aclrtMemcpy(own_slot, g_slab, hbuf, g_slab,
                              ACL_MEMCPY_HOST_TO_DEVICE);
        }
    }
    if (!fatal && ret == ACL_ERROR_NONE) {
        ret = aclrtMemExportToShareableHandle(
            h_own, ACL_MEM_HANDLE_TYPE_NONE,
            ACL_RT_VMM_EXPORT_FLAG_DISABLE_PID_VALIDATION, &sh_own);
        if (ret == ACL_ERROR_NONE && sh_own != 0)
            printf("[16] r%d create: export sh=0x%llx\n",
                   rank, (unsigned long long)sh_own);
        else
            printf("[16] r%d create: export FAIL ret=%d sh=0x%llx\n",
                   rank, ret, (unsigned long long)sh_own);
    }
    if (ret != ACL_ERROR_NONE)
        BAIL(fatal);
    sync_state(&fatal);

    /* ---- 阶段② 交换 shareable handles(u64 x 8) ---- */
    if (!fatal) {
        MPI_Allgather(&sh_own, 1, MPI_UINT64_T,
                      sh_all, 1, MPI_UINT64_T, MPI_COMM_WORLD);
        printf("[16] r%d exchange: allgathered %d handles\n", rank, N);
    }
    sync_state(&fatal);

    /* ---- 阶段③ join(token 环串行, rank 序 import, 全程无 EnablePeerAccess) ---- */
    if (!fatal) {
        printf("[16] r%d join: token-ring serial import begins (no EnablePeerAccess)\n", rank);
        for (int token = 0; token < N; token++) {
            MPI_Barrier(MPI_COMM_WORLD);
            if (token == rank) {
                for (int p = 0; p < N; p++) {
                    if (p == rank)
                        continue;
                    if (sh_all[p] == 0) {
                        printf("[16]   r%d import p%d: SKIP (peer export failed)\n", rank, p);
                        continue;
                    }
                    void *slot = (uint8_t *)pool + (uint64_t)p * g_slab;
                    aclrtDrvMemHandle h = NULL;
                    int method = 0;
                    int32_t r = -1;
                    int die = is_die_pair(rank, p);
                    if (die) {
                        /* die 对: ctx 技巧(05 验证的主路径)
                         * 05 对照实验: die 对 own-ctx import 确定性 507899,
                         * ctx 技巧(SetDevice(导出方)->Import(sh,my_dev)->
                         * SetDevice(my_dev))绕过 runtime P2P 前置, 可用 */
                        method = 2;
                        int32_t sd = aclrtSetDevice(p);
                        if (sd == ACL_ERROR_NONE) {
                            extra_ctx[p] = 1;
                            r = aclrtMemImportFromShareableHandle(sh_all[p], dev, &h);
                            int32_t rb = aclrtSetDevice(dev);
                            if (rb != ACL_ERROR_NONE)
                                printf("[16]   r%d import p%d: WARN SetDevice back(%d) ret=%d\n",
                                       rank, p, dev, rb);
                        } else {
                            r = sd;
                        }
                    } else {
                        /* 跨卡: method1 本进程 ctx 直接 import
                         * (04 step16 实测验证: 跨卡 import 不依赖显式 EPA;
                         *  本测试探针要求全程零显式 EPA, 恰好用此路径。
                         *  证据: cleanup DisablePeerAccess ok 数 == 跨卡 import
                         *  成功数 -> import 内部隐式 enable P2P, 消耗同一配额) */
                        method = 1;
                        r = aclrtMemImportFromShareableHandle(sh_all[p], dev, &h);
                        if (r != ACL_ERROR_NONE) {
                            /* drv 偶发 507899, 200ms 重试 1 次(gva_pool 经验) */
                            usleep(200000);
                            r = aclrtMemImportFromShareableHandle(sh_all[p], dev, &h);
                        }
                        if (r != ACL_ERROR_NONE) {
                            /* method3 回退: ctx 技巧(10 实测跨卡 ctx 技巧也可用) */
                            method = 3;
                            int32_t sd = aclrtSetDevice(p);
                            if (sd == ACL_ERROR_NONE) {
                                extra_ctx[p] = 1;
                                r = aclrtMemImportFromShareableHandle(sh_all[p], dev, &h);
                                int32_t rb = aclrtSetDevice(dev);
                                if (rb != ACL_ERROR_NONE)
                                    printf("[16]   r%d import p%d: WARN SetDevice back(%d) ret=%d\n",
                                           rank, p, dev, rb);
                            } else {
                                r = sd;
                            }
                        }
                    }
                    if (r == ACL_ERROR_NONE && h != NULL) {
                        h_imp[p] = h;
                        imp_method[p] = method;
                        int32_t m = aclrtMapMem(slot, g_slab, 0, h, 0);
                        if (m == ACL_ERROR_NONE) {
                            imp_mapped[p] = 1;
                            int32_t sa = set_access_rw(slot, g_slab, dev);
                            imp_ok[p] = 1;
                            printf("[16]   r%d import p%d (%s): OK method=%d "
                                   "va=0x%012lx map ret=%d sa ret=%d\n",
                                   rank, p, is_die_pair(rank, p) ? "die" : "cross",
                                   method, (unsigned long)(uintptr_t)slot, m, sa);
                        } else {
                            printf("[16]   r%d import p%d (%s): import OK but MAP FAIL ret=%d\n",
                                   rank, p, is_die_pair(rank, p) ? "die" : "cross", m);
                            aclrtFreePhysical(h_imp[p]);
                            h_imp[p] = NULL;
                        }
                    } else {
                        printf("[16]   r%d import p%d (%s): FAIL ret=%d\n",
                               rank, p, is_die_pair(rank, p) ? "die" : "cross", r);
                    }
                }
                int joined = 0, m1_cnt = 0, m2_cnt = 0, m3_cnt = 0;
                for (int p = 0; p < N; p++) {
                    if (p != rank && imp_ok[p]) {
                        joined++;
                        if (imp_method[p] == 1) m1_cnt++;
                        else if (imp_method[p] == 2) m2_cnt++;
                        else m3_cnt++;
                    }
                }
                join_ok = (joined == N - 1);
                if (!join_ok)
                    g_fail = 1;
                printf("[16] r%d joined %d/7 peers (m1 own-ctx=%d, m2 ctx-trick(die)=%d, m3 ctx-trick(fallback)=%d)\n",
                       rank, joined, m1_cnt, m2_cnt, m3_cnt);
            }
            MPI_Barrier(MPI_COMM_WORLD);
        }
    }
    sync_state(&fatal);

    /* ---- 阶段④ VA 矩阵(slot 布局, OBS 不判 FAIL) ---- */
    if (!fatal) {
        for (int p = 0; p < N; p++)
            va_tab[p] = (uint64_t)(uintptr_t)((uint8_t *)pool + (uint64_t)p * g_slab);
        uint64_t va_all[N * N];   /* va_all[r*N + p] = rank r 看到的 slab p VA */
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
            if (!va_ok)
                break;
        }
        if (rank == 0) {
            printf("[16] VA matrix (row=rank observer, col=slab owner, slot layout):\n");
            printf("[16]        ");
            for (int p = 0; p < N; p++) printf("  p%d        ", p);
            printf("\n");
            for (int r = 0; r < N; r++) {
                printf("[16] r%d  ", r);
                for (int p = 0; p < N; p++)
                    printf("0x%012lx ", (unsigned long)va_all[r * N + p]);
                printf("\n");
            }
            printf("[16] VA consistency: %s %s\n",
                   va_ok ? "CONSISTENT (all 8 rows identical)" : "INCONSISTENT",
                   va_ok ? "" : "(OBS: NULL-hint reserve base differs per process)");
        }
    }
    sync_state(&fatal);

    /* ---- 阶段⑤ 全网写+校验(每 rank r 写 1 个 peer p=(r+1)%8) ---- */
    if (!fatal) {
        int w_dst = (rank + 1) % N;       /* 我写谁 */
        int w_me = (rank + N - 1) % N;    /* 谁写我 = (r+7)%8 */
        void *own_slot = (uint8_t *)pool + (uint64_t)rank * g_slab;

        fill_pat(hbuf, words, (uint32_t)rank, (uint32_t)(TAG_WRITE_BASE + rank));
        ret = aclrtMemcpy(scratch, SCRATCH_SIZE, hbuf, g_slab, ACL_MEMCPY_HOST_TO_DEVICE);
        if (ret != ACL_ERROR_NONE) {
            printf("[16] r%d write: H2D own pattern FAIL ret=%d\n", rank, ret);
            BAIL(fatal);
        }
        if (!fatal) {
            int delivered = 0;
            if (imp_ok[w_dst]) {
                void *dst_slot = (uint8_t *)pool + (uint64_t)w_dst * g_slab;
                ret = aclrtMemcpyAsync(dst_slot, g_slab, scratch, g_slab,
                                       ACL_MEMCPY_DEVICE_TO_DEVICE, stream);
                if (ret == ACL_ERROR_NONE)
                    ret = aclrtSynchronizeStream(stream);
                delivered = (ret == ACL_ERROR_NONE);
                printf("[16] r%d write D2D 16MB -> p%d: %s (ret=%d)\n",
                       rank, w_dst, delivered ? "OK" : "FAIL", ret);
            } else {
                printf("[16] r%d write D2D 16MB -> p%d: SKIP (import not ok)\n", rank, w_dst);
            }
            int dmap[N];
            MPI_Allgather(&delivered, 1, MPI_INT, dmap, 1, MPI_INT, MPI_COMM_WORLD);
            if (dmap[w_me]) {
                memset(vbuf, 0, g_slab);
                ret = aclrtMemcpy(vbuf, g_slab, own_slot, g_slab,
                                  ACL_MEMCPY_DEVICE_TO_HOST);
                if (ret == ACL_ERROR_NONE) {
                    int ok = (check_pat(vbuf, words, (uint32_t)w_me,
                                        (uint32_t)(TAG_WRITE_BASE + w_me), rank) == 0);
                    write_ok = ok;
                    printf("[16] r%d verify own slab (writer p%d, tag=0x%x): %s\n",
                           rank, w_me, TAG_WRITE_BASE + w_me, ok ? "OK" : "FAIL");
                    if (!ok)
                        g_fail = 1;
                } else {
                    printf("[16] r%d verify: D2H FAIL ret=%d\n", rank, ret);
                    g_fail = 1;
                }
            } else {
                printf("[16] r%d verify: writer p%d did not deliver, skip\n", rank, w_me);
                g_fail = 1;
            }
        }
    }
    sync_state(&fatal);

    /* ---- 阶段⑥ 带宽(正确性全过后; token 环串行避免 8 路并发争用链路) ---- */
    if (!fatal && g_fail) {
        printf("[16] r%d bandwidth: SKIPPED (correctness not passed)\n", rank);
    }
    if (!fatal && !g_fail) {
        int die_p = rank ^ 1;
        int cross_p = (rank + 2) % N;
        size_t bw_size = (BW_SIZE < g_slab) ? BW_SIZE : g_slab;
        double gb = (double)BW_ITERS * (double)bw_size / 1e9;
        printf("[16] r%d bandwidth (token-ring serial): die p%d + cross p%d, "
               "%zuMB x %d iters + %d warmup, async+1Sync\n",
               rank, die_p, cross_p, bw_size >> 20, BW_ITERS, BW_WARMUP);
        for (int token = 0; token < N; token++) {
            MPI_Barrier(MPI_COMM_WORLD);
            if (token == rank) {
                int peers[2] = { die_p, cross_p };
                const char *names[2] = { "die ", "cross" };
                for (int k = 0; k < 2; k++) {
                    int p = peers[k];
                    if (!imp_ok[p])
                        continue;
                    void *src_slot = (uint8_t *)pool + (uint64_t)p * g_slab;
                    for (int i = 0; i < BW_WARMUP; i++) {
                        aclrtMemcpyAsync(scratch, bw_size, src_slot, bw_size,
                                         ACL_MEMCPY_DEVICE_TO_DEVICE, stream);
                        aclrtSynchronizeStream(stream);
                    }
                    double t0 = now_sec();
                    for (int i = 0; i < BW_ITERS; i++)
                        aclrtMemcpyAsync(scratch, bw_size, src_slot, bw_size,
                                         ACL_MEMCPY_DEVICE_TO_DEVICE, stream);
                    aclrtSynchronizeStream(stream);
                    double t1 = now_sec();
                    printf("[16] r%d bw %s p%d->scratch: %7.2f GB/s "
                           "(%dx%zuMB async + 1Sync, %.3fs)\n",
                           rank, names[k], p, gb / (t1 - t0),
                           BW_ITERS, bw_size >> 20, t1 - t0);
                }
                fflush(stdout);
            }
            MPI_Barrier(MPI_COMM_WORLD);
        }
    }
    sync_state(&fatal);

    /* ---- RESULT(先 Allreduce 聚合, 再 cleanup) ---- */
    {
        int res[4];
        res[0] = g_fail;       /* 全局失败(含 fatal) */
        res[1] = !join_ok;     /* join 未全 OK */
        res[2] = !write_ok;    /* 写校验未过 */
        res[3] = !va_ok;       /* VA 不一致(仅 OBS, 不计入 g_fail) */
        MPI_Allreduce(MPI_IN_PLACE, res, 4, MPI_INT, MPI_MAX, MPI_COMM_WORLD);
        g_fail = res[0];
        int join_fail = res[1], write_fail = res[2], va_fail = res[3];
        printf("[16] r%d RESULT: %s (join:%s write:%s VA:%s%s)\n", rank,
               g_fail ? "FAIL" : "PASS",
               join_fail ? "FAIL" : "OK",
               write_fail ? "FAIL" : "OK",
               va_fail ? "INCONSISTENT" : "CONSISTENT",
               va_fail ? "[obs]" : "");
        fflush(stdout);
    }

    /* ---- 阶段⑦ cleanup ----
     * 顺序: UnmapMem(各 import slot) -> FreePhysical(imported handles) ->
     *   Barrier(全体 import 释放后 owner 才 free 物理内存) -> UnmapMem(own slot)
     *   -> FreePhysical(own) -> ReleaseMemAddress(128MB 窗口) -> Free(scratch)
     *   -> DestroyStream -> DisablePeerAccess(7 peer best-effort, 本测试从未
     *   enable, 预期 507899 无害) -> ResetDevice(dev) -> ResetDevice(ctx 技巧
     *   额外开的 dev) -> Finalize。 */
    for (int p = 0; p < N; p++) {
        if (p != rank && imp_mapped[p]) {
            void *slot = (uint8_t *)pool + (uint64_t)p * g_slab;
            ret = aclrtUnmapMem(slot);
            if (ret != ACL_ERROR_NONE)
                printf("[16] r%d cleanup: UnmapMem(slot p%d) ret=%d\n", rank, p, ret);
        }
    }
    for (int p = 0; p < N; p++) {
        if (p != rank && h_imp[p]) {
            ret = aclrtFreePhysical(h_imp[p]);
            if (ret != ACL_ERROR_NONE)
                printf("[16] r%d cleanup: FreePhysical(imp p%d) ret=%d\n", rank, p, ret);
        }
    }
    MPI_Barrier(MPI_COMM_WORLD);   /* 所有 rank 的 import 引用释放后才 free 物理内存 */
    if (own_mapped) {
        void *own_slot = (uint8_t *)pool + (uint64_t)rank * g_slab;
        ret = aclrtUnmapMem(own_slot);
        if (ret != ACL_ERROR_NONE)
            printf("[16] r%d cleanup: UnmapMem(own slot) ret=%d\n", rank, ret);
    }
    if (h_own) {
        ret = aclrtFreePhysical(h_own);
        if (ret != ACL_ERROR_NONE)
            printf("[16] r%d cleanup: FreePhysical(own) ret=%d\n", rank, ret);
    }
    if (pool_reserved) {
        ret = aclrtReleaseMemAddress(pool);
        if (ret != ACL_ERROR_NONE)
            printf("[16] r%d cleanup: ReleaseMemAddress(pool) ret=%d\n", rank, ret);
    }
    if (scratch)
        aclrtFree(scratch);
    if (stream)
        aclrtDestroyStream(stream);
    free(hbuf);
    free(vbuf);
    fflush(stdout);
    /* ★D 态防护(best-effort): 本测试全程未 EnablePeerAccess, 理论无 P2P 残留;
     *   仍按套件纪律对 7 个 peer 对称拆除(未 enable 过返回 507899, 无害)。 */
    if (dev_ok) {
        int dis_ok = 0, dis_err = 0;
        for (int p = 0; p < N; p++) {
            if (p == rank)
                continue;
            aclError dr = aclrtDeviceDisablePeerAccess(p);
            if (dr == ACL_ERROR_NONE) dis_ok++; else dis_err++;
        }
        printf("[16] r%d cleanup: DisablePeerAccess x7 best-effort (ok=%d err=%d, "
               "err=507899 expected since never enabled)\n", rank, dis_ok, dis_err);
        aclrtResetDevice(dev);
        /* ctx 技巧开过的额外 device context */
        for (int p = 0; p < N; p++) {
            if (p != rank && extra_ctx[p])
                aclrtResetDevice(p);
        }
    }
    if (init_ok)
        aclFinalize();

    MPI_Finalize();
    return g_fail ? 1 : 0;
}
