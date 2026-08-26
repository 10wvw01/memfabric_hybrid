/*
 * 09_vmm_samedev — VMM shareable handle 同设备跨进程
 * MPI 双进程, 两 rank 同设备(dev0), 不需要 EnablePeerAccess。
 *
 * 结论(310P3, CANN 9.1.0):
 *   - VMM 同设备跨进程 import/export: OK
 *   - async D2D(同设备): OK, 读写双向 pattern 校验通过
 *   - sync D2D(同设备): 失败(ret=207000), 与跨设备/跨 die 一致, 必须用 async
 *   - host 直达(imported VA 上 H2D/D2H): OK
 *   - memAttr 七级回退: 310P3 仅 ACL_MEM_P2P_HUGE 成功, gran=2MB
 *   - 带宽(同设备 async D2D): ~88 GB/s(设备内部拷贝, 非跨 PCIe)
 *
 * 测试项(执行顺序 1→2→3→5→4):
 *   1  exporter: MallocPhysical(七级回退)+Reserve+Map+SetAccess+H2D+Export+MPI发sh
 *   2  importer: Import+Reserve+Map+SetAccess + async D2D 读写双向校验
 *   3  对照: sync D2D → 预期失败
 *   5  host 直达: imported VA 上 D2H/H2D 各 16MB + 校验
 *   4  带宽: async D2D 3预热+30x16MB+1Sync(仅正确性全过后执行)
 *
 * 判定: 1/2/5 全 OK 且 3 如期失败 → PASS; 3 意外成功 → 记录但不影响 PASS。
 * cleanup: importer 先释放 → barrier → exporter 释放; DisablePeerAccess best-effort。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <mpi.h>
#include "acl/acl.h"

#define VMM_SIZE     (32u * 1024u * 1024u)  /* 32 MB 主块(exporter) */
#define BW_SIZE      (16u * 1024u * 1024u)  /* 16 MB 带宽/host 直达粒度 */
#define BW_WARMUP    3
#define BW_ITERS     30

#define TAG_EXP    0x91  /* step1: exporter 写入(step2 读校验) */
#define TAG_BACK   0x92  /* step2: importer 反向写(step2/3/5/终验 校验) */
#define TAG_HOST_W 0x93  /* step5: importer host 直达写 */

static int g_fail = 0;

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
            printf("[09]   [verify] mismatch at word %zu (off 0x%zx): got 0x%08x, want 0x%08x\n",
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

/* 正向步骤: ok=0 记 FAIL */
static void step_line(int n, const char *who, const char *desc, int ok, int32_t ret)
{
    printf("[09] step %d (%s): %-58s %s (ret=%d)\n", n, who, desc,
           ok ? "OK" : "FAIL", ret);
    fflush(stdout);
    if (!ok)
        g_fail = 1;
}

/* 观测项: 不影响 PASS/FAIL */
static void obs_line(int n, const char *who, const char *desc, int ok, int32_t ret)
{
    printf("[09] step %d (%s): %-58s [OBS] %s (ret=%d)\n", n, who, desc,
           ok ? "OK" : "FAIL", ret);
    fflush(stdout);
}

/* 阶段栅栏: fatal/g_fail 全局同步 */
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

/* SetAccess(RW, devId): 失败仅 obs, 数据校验为准 */
static int32_t set_access_rw(void *va, size_t size, int32_t devId)
{
    aclrtMemAccessDesc desc;
    memset(&desc, 0, sizeof(desc));
    desc.flags = ACL_RT_MEM_ACCESS_FLAGS_READWRITE;
    desc.location.type = ACL_MEM_LOCATION_TYPE_DEVICE;
    desc.location.id = (uint32_t)devId;
    return aclrtMemSetAccess(va, size, &desc, 1);
}

/*
 * memAttr 七级回退 MallocPhysical: 首个成功即用。
 * 310P3: ACL_MEM_P2P_HUGE 成功, gran=2MB。
 */
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
        printf("[09]   [%s attr %d/7 %-14s] gran ret=%d gran=%zu; "
               "MallocPhysical(%uMB) ret=%d\n",
               label, i + 1, g_chainName[i], gr, gran,
               (unsigned)(size >> 20), m);
        fflush(stdout);
        if (m == ACL_ERROR_NONE) {
            *chosenIdx = i;
            if (gran != 0 && size % gran != 0)
                printf("[09]   [%s] WARN: size %zu NOT multiple of gran %zu "
                       "(MapMem size must equal MallocPhysical size!)\n",
                       label, size, gran);
            else
                printf("[09]   [%s] chosen %-14s, %uMB = %zu x gran %zuB\n",
                       label, g_chainName[i], (unsigned)(size >> 20),
                       gran ? size / gran : 0, gran);
            fflush(stdout);
            return 0;
        }
        last = m;
        *handle = NULL;
    }
    printf("[09]   [%s] all 7 attrs FAILED (last ret=%d)\n", label, last);
    fflush(stdout);
    return last != 0 ? last : -1;
}

int main(int argc, char *argv[])
{
    MPI_Init(&argc, &argv);
    int rank = 0, nproc = 0;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &nproc);
    if (nproc != 2) {
        if (rank == 0)
            printf("[09] FATAL: need exactly 2 MPI procs (-np 2), got %d\n", nproc);
        MPI_Finalize();
        return 1;
    }

    const char *role = (rank == 0) ? "r0/exporter" : "r1/importer";
    const char *who = (rank == 0) ? "r0" : "r1";
    int32_t dev = env_dev("DEV", 0);   /* 两 rank 同设备 */

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("[09] === 09_vmm_samedev: VMM shareable handle link same-device cross-proc, "
           "rank%d %s dev%d ===\n", rank, role, dev);
    printf("[09] %s: DEV=%d (both ranks same device), 32MB main, "
           "7-attr fallback chain, pattern=(rank<<28)|(tag<<24)|(idx&0xFFFFFF)\n",
           role, dev);

    aclrtStream stream = NULL;      /* r1 的 async 流 */
    /* r0: 自有 VMM 主块(32MB) */
    aclrtDrvMemHandle h_own = NULL;
    void *ownVA = NULL;
    int own_reserved = 0, own_mapped = 0;
    int own_attr = -1;
    /* r1: 导入主块 */
    aclrtDrvMemHandle h_imp = NULL;
    void *vmmVA = NULL;
    int vmm_reserved = 0, vmm_mapped = 0;
    void *local = NULL;             /* r1 本地 32MB 缓冲 */
    uint32_t *hbuf = NULL, *vbuf = NULL;
    uint64_t sh1 = 0;
    int imp_ok = 0;
    int overturn_sync = 0;          /* step3 sync D2D 意外失败标志 */
    int fatal = 0;
    int32_t ret = 0;
    int init_ok = 0, dev_ok = 0;
    size_t words = VMM_SIZE / sizeof(uint32_t);
    size_t hw = BW_SIZE / sizeof(uint32_t);

    /* ---- phase 0: init ---- */
    ret = aclInit(NULL);
    if (ret != ACL_ERROR_NONE) {
        printf("[09] init: %s aclInit FAIL ret=%d\n", role, ret);
        BAIL(fatal);
    } else {
        init_ok = 1;
        ret = aclrtSetDevice(dev);
        if (ret != ACL_ERROR_NONE) {
            printf("[09] init: %s aclrtSetDevice(%d) FAIL ret=%d\n", role, dev, ret);
            BAIL(fatal);
        }
    }
    if (!fatal) {
        dev_ok = 1;
        if (rank == 1) {
            ret = aclrtCreateStream(&stream);
            if (ret != ACL_ERROR_NONE) {
                printf("[09] init: r1 aclrtCreateStream FAIL ret=%d\n", ret);
                BAIL(fatal);
            }
        }
        hbuf = (uint32_t *)malloc(VMM_SIZE);
        vbuf = (uint32_t *)malloc(VMM_SIZE);
        if (!hbuf || !vbuf) {
            printf("[09] init: %s host malloc 2x32MB FAIL\n", role);
            BAIL(fatal);
        }
    }
    if (!fatal)
        printf("[09] init: %s aclInit/SetDevice(%d)%s OK\n", role, dev,
               (rank == 1) ? "/CreateStream" : "");
    sync_state(&fatal);

    /* ---- phase 1 (step 1): exporter MallocPhysical+Reserve+Map+SetAccess+H2D+Export+MPI发sh ---- */
    if (!fatal && rank == 0) {
        int ok = 0;
        ret = malloc_physical_chain(dev, VMM_SIZE, &h_own, "main", &own_attr);
        if (ret == ACL_ERROR_NONE)
            ret = aclrtReserveMemAddress(&ownVA, VMM_SIZE, 0, NULL, 0);
        if (ret == ACL_ERROR_NONE) {
            own_reserved = 1;
            ret = aclrtMapMem(ownVA, VMM_SIZE, 0, h_own, 0);
        }
        if (ret == ACL_ERROR_NONE) {
            own_mapped = 1;
            int32_t sa = set_access_rw(ownVA, VMM_SIZE, dev);
            obs_line(1, who, "SetAccess(RW, dev) on own VA", sa == ACL_ERROR_NONE, sa);
            fill_pat(hbuf, words, 0, TAG_EXP);
            ret = aclrtMemcpy(ownVA, VMM_SIZE, hbuf, VMM_SIZE,
                              ACL_MEMCPY_HOST_TO_DEVICE);
        }
        if (ret == ACL_ERROR_NONE)
            ret = aclrtMemExportToShareableHandle(
                h_own, ACL_MEM_HANDLE_TYPE_NONE,
                ACL_RT_VMM_EXPORT_FLAG_DISABLE_PID_VALIDATION, &sh1);
        if (ret == ACL_ERROR_NONE && sh1 != 0) {
            if (MPI_Send(&sh1, 1, MPI_UINT64_T, 1, 100, MPI_COMM_WORLD) == MPI_SUCCESS)
                ok = 1;
        }
        /* 无论成败都发 sh, 避免对端 Recv 悬挂 */
        if (!ok) {
            uint64_t bad = 0;
            MPI_Send(&bad, 1, MPI_UINT64_T, 1, 100, MPI_COMM_WORLD);
            printf("[09] step 1 (r0): exporter chain FAILED before send "
                   "(chosen attr idx=%d, sh1=0x%llx)\n", own_attr,
                   (unsigned long long)sh1);
        } else {
            printf("[09] step 1 (r0): sh1=0x%llx sent\n",
                   (unsigned long long)sh1);
        }
        step_line(1, who,
                  "MallocPhysical(7attr)+Reserve+Map+H2D pat+Export sh1+send",
                  ok, ret);
        if (!ok)
            BAIL(fatal);
    } else if (!fatal && rank == 1) {
        MPI_Recv(&sh1, 1, MPI_UINT64_T, 0, 100, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        if (sh1 == 0)
            printf("[09] step 1 (r1): received sh1=0 (exporter failed)\n");
    }
    sync_state(&fatal);

    /* ---- phase 2a (step 2): importer import+map+async D2D 读+校验 ---- */
    if (!fatal && rank == 1 && sh1 != 0) {
        int ok = 0;
        /* 本进程 ctx import, devId=dev0 */
        ret = aclrtMemImportFromShareableHandle(sh1, dev, &h_imp);
        if (ret == ACL_ERROR_NONE) {
            imp_ok = 1;
            ret = aclrtReserveMemAddress(&vmmVA, VMM_SIZE, 0, NULL, 0);
        }
        if (ret == ACL_ERROR_NONE) {
            vmm_reserved = 1;
            ret = aclrtMapMem(vmmVA, VMM_SIZE, 0, h_imp, 0);
        }
        if (ret == ACL_ERROR_NONE) {
            vmm_mapped = 1;
            int32_t sa = set_access_rw(vmmVA, VMM_SIZE, dev);
            obs_line(2, who, "SetAccess(RW, dev) on imported VA",
                     sa == ACL_ERROR_NONE, sa);
            ret = aclrtMalloc(&local, VMM_SIZE, ACL_MEM_MALLOC_HUGE_FIRST);
        }
        if (ret == ACL_ERROR_NONE)
            ret = aclrtMemcpyAsync(local, VMM_SIZE, vmmVA, VMM_SIZE,
                                   ACL_MEMCPY_DEVICE_TO_DEVICE, stream);
        if (ret == ACL_ERROR_NONE)
            ret = aclrtSynchronizeStream(stream);
        if (ret == ACL_ERROR_NONE) {
            memset(vbuf, 0, VMM_SIZE);
            ret = aclrtMemcpy(vbuf, VMM_SIZE, local, VMM_SIZE,
                              ACL_MEMCPY_DEVICE_TO_HOST);
            if (ret == ACL_ERROR_NONE)
                ok = (check_pat(vbuf, words, 0, TAG_EXP) == 0);
        }
        step_line(2, who,
                  "import(own-ctx dev0)+Reserve+Map+async D2D read+verify",
                  ok, ret);
        if (!ok)
            BAIL(fatal);
    }
    sync_state(&fatal);

    /* ---- phase 2b (step 2): importer async D2D 反向写(local->VMM) ---- */
    if (!fatal && rank == 1 && imp_ok) {
        int ok = 0;
        fill_pat(hbuf, words, 1, TAG_BACK);
        ret = aclrtMemcpy(local, VMM_SIZE, hbuf, VMM_SIZE, ACL_MEMCPY_HOST_TO_DEVICE);
        if (ret == ACL_ERROR_NONE)
            ret = aclrtMemcpyAsync(vmmVA, VMM_SIZE, local, VMM_SIZE,
                                   ACL_MEMCPY_DEVICE_TO_DEVICE, stream);
        if (ret == ACL_ERROR_NONE)
            ret = aclrtSynchronizeStream(stream);
        ok = (ret == ACL_ERROR_NONE);
        step_line(2, who, "async D2D reverse write (local->VMM) + StreamSync", ok, ret);
        if (!ok)
            BAIL(fatal);
    }
    sync_state(&fatal);

    /* ---- phase 2c (step 2): exporter D2H 校验反向 pattern ---- */
    if (!fatal && rank == 0 && own_mapped) {
        int ok = 0;
        memset(vbuf, 0, VMM_SIZE);
        ret = aclrtMemcpy(vbuf, VMM_SIZE, ownVA, VMM_SIZE, ACL_MEMCPY_DEVICE_TO_HOST);
        if (ret == ACL_ERROR_NONE)
            ok = (check_pat(vbuf, words, 1, TAG_BACK) == 0);
        step_line(2, who, "D2H verify reverse-written pattern (r1/TAG_92) on own VA",
                  ok, ret);
        if (!ok)
            BAIL(fatal);
    }
    sync_state(&fatal);

    /* ---- phase 3 (step 3, 对照): sync D2D → 预期失败 ---- */
    if (!fatal && rank == 1 && imp_ok && vmm_mapped) {
        int32_t d2d_ret = aclrtMemcpy(local, BW_SIZE, vmmVA, BW_SIZE,
                                      ACL_MEMCPY_DEVICE_TO_DEVICE);
        if (d2d_ret != ACL_ERROR_NONE) {
            printf("[09] step 3 (r1): %-58s EXPECTED-FAIL (sync D2D ret=%d, same-dev also fails)\n",
                   "ctrl: sync D2D (imported->local) — expect FAIL (same-dev)",
                   d2d_ret);
        } else {
            printf("[09] step 3 (r1): %-58s !!!推翻预期!!! 意外成功 d2d_ret=0"
                   " (同设备 sync D2D 应不可用)\n",
                   "ctrl: sync D2D (imported->local) — expect FAIL (same-dev)");
            overturn_sync = 1;
        }
        fflush(stdout);
    }
    sync_state(&fatal);

    /* ---- phase 5a (step 5): importer host 直达 D2H 读 16MB ---- */
    if (!fatal && rank == 1 && imp_ok && vmm_mapped) {
        /* 前 16MB 此时为 step2 反向写的 TAG_BACK(rank1) */
        int ok = 0;
        memset(vbuf, 0, BW_SIZE);
        ret = aclrtMemcpy(vbuf, BW_SIZE, vmmVA, BW_SIZE, ACL_MEMCPY_DEVICE_TO_HOST);
        if (ret == ACL_ERROR_NONE)
            ok = (check_pat(vbuf, hw, 1, TAG_BACK) == 0);
        step_line(5, who, "host direct D2H read remote 16MB (imported VA) + verify",
                  ok, ret);
        if (!ok)
            BAIL(fatal);
    }

    /* ---- phase 5b (step 5): importer host 直达 H2D 写 16MB ---- */
    if (!fatal && rank == 1 && imp_ok && vmm_mapped) {
        int ok = 0;
        fill_pat(hbuf, hw, 1, TAG_HOST_W);
        ret = aclrtMemcpy(vmmVA, BW_SIZE, hbuf, BW_SIZE, ACL_MEMCPY_HOST_TO_DEVICE);
        ok = (ret == ACL_ERROR_NONE);
        step_line(5, who, "host direct H2D write remote 16MB (imported VA)", ok, ret);
        if (!ok)
            BAIL(fatal);
    }
    sync_state(&fatal);

    /* ---- phase 5c (step 5): exporter 复核 host 直达写入 ---- */
    if (!fatal && rank == 0 && own_mapped) {
        int ok = 0;
        memset(vbuf, 0, BW_SIZE);
        ret = aclrtMemcpy(vbuf, BW_SIZE, ownVA, BW_SIZE, ACL_MEMCPY_DEVICE_TO_HOST);
        if (ret == ACL_ERROR_NONE)
            ok = (check_pat(vbuf, hw, 1, TAG_HOST_W) == 0);
        step_line(5, who, "D2H verify host-direct-written 16MB (r1/TAG_93) on own VA",
                  ok, ret);
        if (!ok)
            BAIL(fatal);
    }
    sync_state(&fatal);

    /* ---- phase 6 (step 4): 带宽(仅正确性全过后) ----
     * 口径: 3预热+30x16MB async+1Sync; 读/写用互不重叠半区。 */
    if (!fatal && g_fail) {
        if (rank == 1)
            printf("[09] step 4 (r1): bandwidth SKIPPED (correctness not passed)\n");
    }
    if (!fatal && !g_fail && rank == 1 && vmm_mapped) {
        double t0, t1, gb;
        printf("[09] step 4 (r1): bandwidth on imported VMM VA (same-dev), %u MB x %d "
               "iters + %d warmup, async submits + 1 final Sync ONLY (no sync loop!)\n",
               BW_SIZE >> 20, BW_ITERS, BW_WARMUP);
        fflush(stdout);
        gb = (double)BW_ITERS * (double)BW_SIZE / 1e9;
        void *vmmLo = vmmVA;                              /* [0,16MB) 写方向 dst */
        void *vmmHi = (void *)((uint8_t *)vmmVA + BW_SIZE);  /* [16,32MB) 读方向 src */
        void *locLo = local;                              /* [0,16MB) 写方向 src */
        void *locHi = (void *)((uint8_t *)local + BW_SIZE);   /* [16,32MB) 读方向 dst */

        /* 读方向: VMM -> local */
        for (int i = 0; i < BW_WARMUP; i++) {
            aclrtMemcpyAsync(locHi, BW_SIZE, vmmHi, BW_SIZE,
                             ACL_MEMCPY_DEVICE_TO_DEVICE, stream);
            aclrtSynchronizeStream(stream);
        }
        t0 = now_sec();
        for (int i = 0; i < BW_ITERS; i++)
            aclrtMemcpyAsync(locHi, BW_SIZE, vmmHi, BW_SIZE,
                             ACL_MEMCPY_DEVICE_TO_DEVICE, stream);
        aclrtSynchronizeStream(stream);
        t1 = now_sec();
        printf("[09] bw   D2D-read  (VMM->local) : %7.2f GB/s "
               "(30x16MB async + 1 Sync, total %.3f s)\n", gb / (t1 - t0), t1 - t0);
        fflush(stdout);

        /* 写方向: local -> VMM */
        for (int i = 0; i < BW_WARMUP; i++) {
            aclrtMemcpyAsync(vmmLo, BW_SIZE, locLo, BW_SIZE,
                             ACL_MEMCPY_DEVICE_TO_DEVICE, stream);
            aclrtSynchronizeStream(stream);
        }
        t0 = now_sec();
        for (int i = 0; i < BW_ITERS; i++)
            aclrtMemcpyAsync(vmmLo, BW_SIZE, locLo, BW_SIZE,
                             ACL_MEMCPY_DEVICE_TO_DEVICE, stream);
        aclrtSynchronizeStream(stream);
        t1 = now_sec();
        printf("[09] bw   D2D-write (local->VMM) : %7.2f GB/s "
               "(30x16MB async + 1 Sync, total %.3f s)\n", gb / (t1 - t0), t1 - t0);
        fflush(stdout);
    }
    sync_state(&fatal);

    /* ---- phase 7 (step 2 终验): exporter D2H 校验全 32MB 反向 pattern ---- */
    if (!fatal && !g_fail && rank == 0 && own_mapped) {
        int ok = 0;
        memset(vbuf, 0, VMM_SIZE);
        ret = aclrtMemcpy(vbuf, VMM_SIZE, ownVA, VMM_SIZE, ACL_MEMCPY_DEVICE_TO_HOST);
        if (ret == ACL_ERROR_NONE)
            ok = (check_pat(vbuf, words, 1, TAG_BACK) == 0);
        step_line(2, who, "final D2H verify reverse pattern (full 32MB) after r1 done",
                  ok, ret);
        if (!ok)
            BAIL(fatal);
    } else if (!fatal && g_fail && rank == 0) {
        printf("[09] step 2 (r0): final verify SKIPPED (correctness not passed)\n");
    }
    sync_state(&fatal);

    /* ---- cleanup: importer 先释放 → barrier → exporter 释放 ---- */
    if (rank == 1) {
        if (vmm_mapped) {
            ret = aclrtUnmapMem(vmmVA);
            printf("[09] cleanup (r1): UnmapMem(imported VA) ret=%d\n", ret);
        }
        if (h_imp) {
            ret = aclrtFreePhysical(h_imp);
            printf("[09] cleanup (r1): FreePhysical(imported) ret=%d\n", ret);
        }
        if (vmm_reserved) {
            ret = aclrtReleaseMemAddress(vmmVA);
            printf("[09] cleanup (r1): ReleaseMemAddress(imported VA) ret=%d\n", ret);
        }
        if (local)
            aclrtFree(local);
        if (stream)
            aclrtDestroyStream(stream);
    }
    MPI_Barrier(MPI_COMM_WORLD);
    if (rank == 0) {
        if (own_mapped) {
            ret = aclrtUnmapMem(ownVA);
            printf("[09] cleanup (r0): UnmapMem(main VA) ret=%d\n", ret);
        }
        if (h_own) {
            ret = aclrtFreePhysical(h_own);
            printf("[09] cleanup (r0): FreePhysical(main) ret=%d\n", ret);
        }
        if (own_reserved) {
            ret = aclrtReleaseMemAddress(ownVA);
            printf("[09] cleanup (r0): ReleaseMemAddress(main VA) ret=%d\n", ret);
        }
    }
    free(hbuf);
    free(vbuf);
    fflush(stdout);
    /* D 态修复(best-effort): 同设备无 peer 可 disable */
    if (dev_ok) {
        aclError dr = aclrtDeviceDisablePeerAccess(dev);
        printf("[09] cleanup (r%d): DisablePeerAccess(%d) ret=%d (best-effort, same-dev)\n",
               rank, dev, dr);
        aclrtResetDevice(dev);
    }
    if (init_ok)
        aclFinalize();

    /* ---- RESULT: 聚合两 rank 状态 ---- */
    {
        int res[2];
        res[0] = g_fail;
        res[1] = overturn_sync;
        MPI_Allreduce(MPI_IN_PLACE, res, 2, MPI_INT, MPI_MAX, MPI_COMM_WORLD);
        g_fail = res[0];
        overturn_sync = res[1];

        printf("[09] RESULT: %s", g_fail ? "FAIL" : "PASS");
        if (!g_fail && overturn_sync) {
            printf("  ★★推翻预期: 同设备 sync D2D 意外成功(历史结论需修订!)★★");
        }
        printf("\n");
        fflush(stdout);
    }

    MPI_Finalize();
    return g_fail ? 1 : 0;
}
