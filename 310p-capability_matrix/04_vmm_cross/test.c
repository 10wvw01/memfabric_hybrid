/*
 * 04_vmm_cross — VMM shareable handle 跨卡(PCIe)测试
 *
 * 环境: Atlas 300I Duo (310P3), CANN 9.1.0, aarch64
 * 结构: MPI 双进程 rank0=exporter(dev A), rank1=importer(dev B)
 *
 * 测试步骤(执行顺序 1→16→2→3→5→4):
 *   step1:  Exporter 分配 32MB 物理内存(MEM_P2P_HUGE) + Export → sh1
 *   step16: 对照: Importer 不做 EnablePeerAccess 直接 Import(310P3 确认可行)
 *   step2:  Importer Import sh1 + async D2D 双向读写 + pattern 校验
 *   step3:  对照: ctx 技巧(同卡 die 对方案)跨卡也能用(310P3 确认)
 *   step5:  host 直达 D2H/H2D 16MB + 校验
 *   step4:  带宽测试(3 预热 + 30×16MB async + 1 Sync)
 *
 * 核心结论(310P3):
 *   - EnablePeerAccess 不是跨卡 Import 的必要条件
 *   - ctx 技巧是同卡 die 对方案,跨卡也能用但非必须
 *   - 回退链中 ACL_MEM_P2P_HUGE 命中(第 5 级);独立测试支持 6 种 memAttr
 *
 * 校验: pattern = (rank<<28)|(tag<<24)|(idx&0xFFFFFF),每步不同 tag
 * Cleanup: Importer 先释放 → MPI_Barrier → Exporter 后释放 → DisablePeerAccess
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <mpi.h>
#include "acl/acl.h"

#define VMM_SIZE     (32u * 1024u * 1024u)  /* 32 MB 主块(exporter) */
#define CTRL_SIZE    (16u * 1024u * 1024u)  /* 16 MB 对照第二块(独立 sh2) */
#define BW_SIZE      (16u * 1024u * 1024u)  /* 16 MB 带宽/host 直达粒度 */
#define P2P_CTRL_SIZE (8u * 1024u * 1024u)  /* 8 MB EnablePeerAccess 对照块 */
#define BW_WARMUP    3
#define BW_ITERS     30
#define EXPECTED_ERR 507899  /* ACL_ERROR_RT_DRV_INTERNAL_ERROR */
#define SH_INVALID   ((uint64_t)-1)  /* sentinel: export failed */

#define TAG_EXP    0x2  /* step1: exporter 写入(step2 读校验) */
#define TAG_BACK   0x3  /* step2: importer 反向写(step2/5/终验 校验) */
#define TAG_HOST_W 0x4  /* step5: importer host 直达写 */
#define TAG_CTRL   0x5  /* step3: exporter 写入对照块 */
#define TAG_P2P    0x6  /* step16: EnablePeerAccess 对照块 */

/* ctx 技巧对照结果分类(RESULT 行注释用) */
#define CTRL_NONE        (-1)  /* 未执行 */
#define CTRL_EXPECTED    0     /* 507899, 失败 */
#define CTRL_OTHER_ERR   1     /* 非 507899 错误码 */
#define CTRL_UNEXPECTED  2     /* 成功(310P3 确认可行) */

/* EnablePeerAccess 对照结果分类 */
#define P2P_CTRL_NONE        (-1)  /* 未执行 */
#define P2P_CTRL_EXPECTED    0     /* 失败(EnablePeerAccess 是必须的) */
#define P2P_CTRL_UNEXPECTED  1     /* 成功(EnablePeerAccess 不是必须的) */

static int g_fail = 0;

/* memAttr 七级回退链(310P3 回退链中第 5 级 MEM_P2P_HUGE 命中) */
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
            printf("[04]   [verify] mismatch at word %zu (off 0x%zx): got 0x%08x, want 0x%08x\n",
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
    printf("[04] step %d (%s): %-58s %s (ret=%d)\n", n, who, desc,
           ok ? "OK" : "FAIL", ret);
    fflush(stdout);
    if (!ok)
        g_fail = 1;
}

/* 观测项行(不影响 PASS/FAIL) */
static void obs_line(int n, const char *who, const char *desc, int ok, int32_t ret)
{
    printf("[04] step %d (%s): %-58s [OBS] %s (ret=%d)\n", n, who, desc,
           ok ? "OK" : "FAIL", ret);
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

/* SetAccess(RW, devId) */
static int32_t set_access_rw(void *va, size_t size, int32_t devId)
{
    aclrtMemAccessDesc desc;
    memset(&desc, 0, sizeof(desc));
    desc.flags = ACL_RT_MEM_ACCESS_FLAGS_READWRITE;
    desc.location.type = ACL_MEM_LOCATION_TYPE_DEVICE;
    desc.location.id = (uint32_t)devId;
    return aclrtMemSetAccess(va, size, &desc, 1);
}

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
        printf("[04]   [%s attr %d/7 %-14s] gran ret=%d gran=%zu; "
               "MallocPhysical(%uMB) ret=%d\n",
               label, i + 1, g_chainName[i], gr, gran,
               (unsigned)(size >> 20), m);
        fflush(stdout);
        if (m == ACL_ERROR_NONE) {
            *chosenIdx = i;
            if (gran != 0 && size % gran != 0)
                printf("[04]   [%s] WARN: size %zu NOT multiple of gran %zu "
                       "(MapMem size must equal MallocPhysical size!)\n",
                       label, size, gran);
            else
                printf("[04]   [%s] chosen %-14s, %uMB = %zu x gran %zuB\n",
                       label, g_chainName[i], (unsigned)(size >> 20),
                       gran ? size / gran : 0, gran);
            fflush(stdout);
            return 0;
        }
        last = m;
        *handle = NULL;
    }
    printf("[04]   [%s] all 7 attrs FAILED (last ret=%d)\n", label, last);
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
            printf("[04] FATAL: need exactly 2 MPI procs (-np 2), got %d\n", nproc);
        MPI_Finalize();
        return 1;
    }

    const char *role = (rank == 0) ? "r0/exporter" : "r1/importer";
    const char *who = (rank == 0) ? "r0" : "r1";
    int32_t devA = env_dev("A_DEV", 0);
    int32_t devB = env_dev("B_DEV", 2);
    int32_t dev = (rank == 0) ? devA : devB;

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("[04] === 04_vmm_cross: VMM shareable handle link cross-card (PCIe), "
           "rank%d %s dev%d ===\n", rank, role, dev);
    printf("[04] %s: A_DEV=%d B_DEV=%d, 32MB main + 16MB ctrl block, "
           "7-attr fallback chain, pattern=(rank<<28)|(tag<<24)|(idx&0xFFFFFF)\n",
           role, devA, devB);
    if (devA == devB)
        printf("[04] %s: [WARN] A_DEV==B_DEV==%d — 同设备, 非跨卡场景(结果如实记录)\n",
               role, devA);

    aclrtStream stream = NULL;      /* r1 的 async 流 */
    /* r0: 自有 VMM 主块(32MB) + 对照块(16MB) + P2P 对照块(8MB) */
    aclrtDrvMemHandle h_own = NULL, h_ctrl = NULL, h_p2p_ctrl = NULL;
    void *ownVA = NULL, *ctrlVA = NULL, *p2p_ctrlVA = NULL;
    int own_reserved = 0, own_mapped = 0, ctrl_reserved = 0, ctrl_mapped = 0;
    int p2p_ctrl_reserved = 0, p2p_ctrl_mapped = 0;
    int own_attr = -1, ctrl_attr = -1, p2p_ctrl_attr = -1;
    /* r1: 导入主块 */
    aclrtDrvMemHandle h_imp = NULL;
    void *vmmVA = NULL;
    int vmm_reserved = 0, vmm_mapped = 0;
    void *local = NULL;             /* r1 本地 32MB 缓冲 */
    uint32_t *hbuf = NULL, *vbuf = NULL;
    uint64_t sh1 = 0, sh2 = 0, sh_p2p = 0;
    int imp_ok = 0;
    int r1_setA = 0;                /* r1 ctx 技巧是否成功 SetDevice(A) 过 */
    int ctrl_kind = CTRL_NONE;
    int32_t ctrl_ret = -1;
    int p2p_ctrl_kind = P2P_CTRL_NONE;
    int32_t p2p_ctrl_ret = -1;
    int fatal = 0;
    int32_t ret = 0;
    int init_ok = 0, dev_ok = 0;
    size_t words = VMM_SIZE / sizeof(uint32_t);
    size_t hw = BW_SIZE / sizeof(uint32_t);
    size_t cw = CTRL_SIZE / sizeof(uint32_t);
    size_t p2p_w = P2P_CTRL_SIZE / sizeof(uint32_t);

    /* ---- phase 0: init ---- */
    ret = aclInit(NULL);
    if (ret != ACL_ERROR_NONE) {
        printf("[04] init: %s aclInit FAIL ret=%d\n", role, ret);
        BAIL(fatal);
    } else {
        init_ok = 1;
        ret = aclrtSetDevice(dev);
        if (ret != ACL_ERROR_NONE) {
            printf("[04] init: %s aclrtSetDevice(%d) FAIL ret=%d\n", role, dev, ret);
            BAIL(fatal);
        }
    }
    if (!fatal) {
        dev_ok = 1;
        if (rank == 1) {
            ret = aclrtCreateStream(&stream);
            if (ret != ACL_ERROR_NONE) {
                printf("[04] init: r1 aclrtCreateStream FAIL ret=%d\n", ret);
                BAIL(fatal);
            }
        }
        hbuf = (uint32_t *)malloc(VMM_SIZE);
        vbuf = (uint32_t *)malloc(VMM_SIZE);
        if (!hbuf || !vbuf) {
            printf("[04] init: %s host malloc 2x32MB FAIL\n", role);
            BAIL(fatal);
        }
    }
    if (!fatal)
        printf("[04] init: %s aclInit/SetDevice(%d)%s OK\n", role, dev,
               (rank == 1) ? "/CreateStream" : "");
    sync_state(&fatal);

    /* ---- phase 1 (step 1): exporter EnableP2P(B) + MallocPhysical(七级回退)
     *     + Reserve + Map + SetAccess + H2D pattern + Export sh1 + MPI 发 sh1 ---- */
    if (!fatal && rank == 0) {
        int ok = 0;
        /* 双向 P2P 之 exporter 侧: 在 export 前先开 B */
        int32_t ena = aclrtDeviceEnablePeerAccess(devB, 0);
        obs_line(1, who, "EnablePeerAccess(B_dev,0) BEFORE export (bidir P2P)",
                 ena == ACL_ERROR_NONE, ena);
        ret = malloc_physical_chain(devA, VMM_SIZE, &h_own, "main", &own_attr);
        if (ret == ACL_ERROR_NONE)
            ret = aclrtReserveMemAddress(&ownVA, VMM_SIZE, 0, NULL, 0);
        if (ret == ACL_ERROR_NONE) {
            own_reserved = 1;
            ret = aclrtMapMem(ownVA, VMM_SIZE, 0, h_own, 0);
        }
        if (ret == ACL_ERROR_NONE) {
            own_mapped = 1;
            int32_t sa = set_access_rw(ownVA, VMM_SIZE, devA);
            obs_line(1, who, "SetAccess(RW, A_dev) on own VA", sa == ACL_ERROR_NONE, sa);
            fill_pat(hbuf, words, 0, TAG_EXP);
            ret = aclrtMemcpy(ownVA, VMM_SIZE, hbuf, VMM_SIZE,
                              ACL_MEMCPY_HOST_TO_DEVICE);
        }
        int export_ok = 0;
        if (ret == ACL_ERROR_NONE) {
            ret = aclrtMemExportToShareableHandle(
                h_own, ACL_MEM_HANDLE_TYPE_NONE,
                ACL_RT_VMM_EXPORT_FLAG_DISABLE_PID_VALIDATION, &sh1);
            export_ok = (ret == ACL_ERROR_NONE);
        }
        if (export_ok) {
            if (MPI_Send(&sh1, 1, MPI_UINT64_T, 1, 100, MPI_COMM_WORLD) == MPI_SUCCESS)
                ok = 1;
        }
        if (!ok) {
            uint64_t bad = SH_INVALID;
            MPI_Send(&bad, 1, MPI_UINT64_T, 1, 100, MPI_COMM_WORLD);
            printf("[04] step 1 (r0): exporter chain FAILED before send "
                   "(chosen attr idx=%d, sh1=0x%llx)\n", own_attr,
                   (unsigned long long)sh1);
        } else {
            printf("[04] step 1 (r0): sh1=0x%llx sent\n",
                   (unsigned long long)sh1);
        }
        step_line(1, who,
                  "MallocPhysical(7attr)+Reserve+Map+H2D pat+Export sh1+send",
                  ok, ret);
        if (!ok)
            BAIL(fatal);
    } else if (!fatal && rank == 1) {
        MPI_Recv(&sh1, 1, MPI_UINT64_T, 0, 100, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        if (sh1 == SH_INVALID) {
            printf("[04] step 1 (r1): received SH_INVALID (exporter failed)\n");
            sh1 = 0;
        }
    }
    sync_state(&fatal);

    /* ---- phase 16 (step 16): EnablePeerAccess 对照实验 ----
     * 目的: 验证不做 EnablePeerAccess 直接 Import 是否可行
     * 流程: exporter 分配独立 8MB 物理内存 export sh_p2p;
     *       importer 不做 EnablePeerAccess，直接尝试 Import(310P3 确认可行) */
    if (!fatal && rank == 0) {
        int ok = 0;
        ret = malloc_physical_chain(devA, P2P_CTRL_SIZE, &h_p2p_ctrl, "p2p_ctrl", &p2p_ctrl_attr);
        if (ret == ACL_ERROR_NONE)
            ret = aclrtReserveMemAddress(&p2p_ctrlVA, P2P_CTRL_SIZE, 0, NULL, 0);
        if (ret == ACL_ERROR_NONE) {
            p2p_ctrl_reserved = 1;
            ret = aclrtMapMem(p2p_ctrlVA, P2P_CTRL_SIZE, 0, h_p2p_ctrl, 0);
        }
        if (ret == ACL_ERROR_NONE) {
            p2p_ctrl_mapped = 1;
            int32_t sa = set_access_rw(p2p_ctrlVA, P2P_CTRL_SIZE, devA);
            obs_line(16, who, "SetAccess(RW, A_dev) on p2p_ctrl VA", sa == ACL_ERROR_NONE, sa);
            fill_pat(hbuf, p2p_w, 0, TAG_P2P);
            ret = aclrtMemcpy(p2p_ctrlVA, P2P_CTRL_SIZE, hbuf, P2P_CTRL_SIZE,
                              ACL_MEMCPY_HOST_TO_DEVICE);
        }
        int export_ok_p2p = 0;
        if (ret == ACL_ERROR_NONE) {
            ret = aclrtMemExportToShareableHandle(
                h_p2p_ctrl, ACL_MEM_HANDLE_TYPE_NONE,
                ACL_RT_VMM_EXPORT_FLAG_DISABLE_PID_VALIDATION, &sh_p2p);
            export_ok_p2p = (ret == ACL_ERROR_NONE);
        }
        if (export_ok_p2p) {
            if (MPI_Send(&sh_p2p, 1, MPI_UINT64_T, 1, 116, MPI_COMM_WORLD) == MPI_SUCCESS)
                ok = 1;
        }
        if (!ok) {
            uint64_t bad = SH_INVALID;
            MPI_Send(&bad, 1, MPI_UINT64_T, 1, 116, MPI_COMM_WORLD);
            printf("[04] step 16 (r0): p2p_ctrl chain FAILED before send "
                   "(chosen attr idx=%d, sh_p2p=0x%llx)\n", p2p_ctrl_attr,
                   (unsigned long long)sh_p2p);
        } else {
            printf("[04] step 16 (r0): sh_p2p=0x%llx sent\n",
                   (unsigned long long)sh_p2p);
        }
        step_line(16, who,
                  "MallocPhysical 8MB + Export sh_p2p (for EnablePeerAccess ctrl test)",
                  ok, ret);
        if (!ok)
            BAIL(fatal);
    } else if (!fatal && rank == 1) {
        MPI_Recv(&sh_p2p, 1, MPI_UINT64_T, 0, 116, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        if (sh_p2p == SH_INVALID) {
            printf("[04] step 16 (r1): received SH_INVALID (exporter p2p_ctrl failed)\n");
            sh_p2p = 0;
        }
    }
    sync_state(&fatal);

    /* ---- phase 16b: importer 不做 EnablePeerAccess，直接尝试 Import(310P3 确认可行) ---- */
    if (!fatal && rank == 1 && sh_p2p != 0) {
        printf("[04] step 16 (r1): %-58s 尝试 Import WITHOUT EnablePeerAccess...\n",
               "ctrl: Import(sh_p2p, B_dev)");
        fflush(stdout);
        aclrtDrvMemHandle h_p2p_test = NULL;
        p2p_ctrl_ret = aclrtMemImportFromShareableHandle(sh_p2p, devB, &h_p2p_test);
        if (p2p_ctrl_ret == ACL_ERROR_NONE) {
            p2p_ctrl_kind = P2P_CTRL_UNEXPECTED;
            printf("[04] step 16 (r1): %-58s 成功 ret=0, 310P3 已验证 EnablePeerAccess 不是必须的\n",
                   "ctrl: Import WITHOUT EnablePeerAccess");
            aclrtFreePhysical(h_p2p_test);
        } else {
            p2p_ctrl_kind = P2P_CTRL_EXPECTED;
            printf("[04] step 16 (r1): %-58s 失败 ret=%d (EnablePeerAccess 是必须的)\n",
                   "ctrl: Import WITHOUT EnablePeerAccess", p2p_ctrl_ret);
        }
        fflush(stdout);
    }
    sync_state(&fatal);

    /* ---- phase 2a (step 2): importer EnableP2P(A) + 本进程 ctx import + map
     *     + async D2D 读 + D2H 逐字校验 ---- */
    if (!fatal && rank == 1 && sh1 != 0) {
        int ok = 0;
        /* 跨卡序列: import 前开 P2P(B->A),310P3 实测非必须但保留 */
        int32_t ena = aclrtDeviceEnablePeerAccess(devA, 0);
        obs_line(2, who, "EnablePeerAccess(A_dev,0) BEFORE import (bidir P2P)",
                 ena == ACL_ERROR_NONE, ena);
        /* 本进程 ctx(当前 device=B)import */
        ret = aclrtMemImportFromShareableHandle(sh1, devB, &h_imp);
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
            int32_t sa = set_access_rw(vmmVA, VMM_SIZE, devB);
            obs_line(2, who, "SetAccess(RW, B_dev) on imported VA",
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
                  "import(own-ctx B)+Reserve+Map + async D2D read (VMM->local) + verify",
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
    sync_state(&fatal);   /* 反向写落地后 exporter 才校验 */

    /* ---- phase 2c (step 2): exporter D2H 校验反向 pattern ---- */
    if (!fatal && rank == 0 && own_mapped) {
        int ok = 0;
        memset(vbuf, 0, VMM_SIZE);
        ret = aclrtMemcpy(vbuf, VMM_SIZE, ownVA, VMM_SIZE, ACL_MEMCPY_DEVICE_TO_HOST);
        if (ret == ACL_ERROR_NONE)
            ok = (check_pat(vbuf, words, 1, TAG_BACK) == 0);
        step_line(2, who, "D2H verify reverse-written pattern (r1/TAG3) on own VA",
                  ok, ret);
        if (!ok)
            BAIL(fatal);
    }
    sync_state(&fatal);

    /* ---- phase 3a (step 3): exporter 第二块独立 16MB + export sh2 ---- */
    if (!fatal && rank == 0) {
        int ok = 0;
        ret = malloc_physical_chain(devA, CTRL_SIZE, &h_ctrl, "ctrl", &ctrl_attr);
        if (ret == ACL_ERROR_NONE)
            ret = aclrtReserveMemAddress(&ctrlVA, CTRL_SIZE, 0, NULL, 0);
        if (ret == ACL_ERROR_NONE) {
            ctrl_reserved = 1;
            ret = aclrtMapMem(ctrlVA, CTRL_SIZE, 0, h_ctrl, 0);
        }
        if (ret == ACL_ERROR_NONE) {
            ctrl_mapped = 1;
            int32_t sa = set_access_rw(ctrlVA, CTRL_SIZE, devA);
            obs_line(3, who, "SetAccess(RW, A_dev) on ctrl VA", sa == ACL_ERROR_NONE, sa);
            fill_pat(hbuf, cw, 0, TAG_CTRL);
            ret = aclrtMemcpy(ctrlVA, CTRL_SIZE, hbuf, CTRL_SIZE,
                              ACL_MEMCPY_HOST_TO_DEVICE);
        }
        int export_ok2 = 0;
        if (ret == ACL_ERROR_NONE) {
            ret = aclrtMemExportToShareableHandle(
                h_ctrl, ACL_MEM_HANDLE_TYPE_NONE,
                ACL_RT_VMM_EXPORT_FLAG_DISABLE_PID_VALIDATION, &sh2);
            export_ok2 = (ret == ACL_ERROR_NONE);
        }
        if (export_ok2) {
            if (MPI_Send(&sh2, 1, MPI_UINT64_T, 1, 101, MPI_COMM_WORLD) == MPI_SUCCESS)
                ok = 1;
        }
        if (!ok) {
            uint64_t bad = SH_INVALID;
            MPI_Send(&bad, 1, MPI_UINT64_T, 1, 101, MPI_COMM_WORLD);
            printf("[04] step 3 (r0): ctrl chain FAILED before send "
                   "(chosen attr idx=%d, sh2=0x%llx)\n", ctrl_attr,
                   (unsigned long long)sh2);
        } else {
            printf("[04] step 3 (r0): sh2=0x%llx sent\n",
                   (unsigned long long)sh2);
        }
        step_line(3, who, "MallocPhysical 16MB #2 + H2D pattern + Export sh2 + MPI-send",
                  ok, ret);
        if (!ok)
            BAIL(fatal);
    } else if (!fatal && rank == 1) {
        MPI_Recv(&sh2, 1, MPI_UINT64_T, 0, 101, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        if (sh2 == SH_INVALID) {
            printf("[04] step 3 (r1): received SH_INVALID (exporter ctrl block failed)\n");
            sh2 = 0;
        }
    }
    sync_state(&fatal);

    /* ---- phase 3b (step 3, 对照): ctx 技巧(同卡 die 对方案)跨卡也能用(310P3 确认) ----
     * 独立 sh2 规避"同一 sh 同进程二次 import"的干扰, 观测纯粹的 ctx 技巧行为。 */
    if (!fatal && rank == 1 && sh2 != 0) {
        ret = aclrtSetDevice(devA);   /* ctx 技巧: 借导出方 ctx */
        if (ret == ACL_ERROR_NONE) {
            aclrtDrvMemHandle h2 = NULL;
            r1_setA = 1;
            ctrl_ret = aclrtMemImportFromShareableHandle(sh2, devB, &h2);
            if (ctrl_ret == ACL_ERROR_NONE) {
                ctrl_kind = CTRL_UNEXPECTED;
                printf("[04] step 3 (r1): %-58s 成功 ret=0, 310P3 已验证 ctx 技巧跨卡"
                       "也能用但非必须(仅观测 import, 不建窗口)\n",
                       "ctrl: SetDevice(A)+Import(sh2,B)");
                aclrtFreePhysical(h2);
            } else if (ctrl_ret == EXPECTED_ERR) {
                ctrl_kind = CTRL_EXPECTED;
                printf("[04] step 3 (r1): %-58s FAIL ret=%d\n",
                       "ctrl: SetDevice(A)+Import(sh2,B)", ctrl_ret);
            } else {
                ctrl_kind = CTRL_OTHER_ERR;
                printf("[04] step 3 (r1): %-58s FAIL ret=%d  [注意: 非"
                       "预期 507899, 如实记录]\n",
                       "ctrl: SetDevice(A)+Import(sh2,B)", ctrl_ret);
            }
            fflush(stdout);
        } else {
            printf("[04] step 3 (r1): %-58s SKIP (SetDevice(A) FAIL ret=%d)\n",
                   "ctrl: SetDevice(A)+Import(sh2,B)", ret);
            fflush(stdout);
        }
        /* 测完切回本进程 ctx(B) */
        ret = aclrtSetDevice(devB);
        obs_line(3, who, "SetDevice(B_dev) restore ctx", ret == ACL_ERROR_NONE, ret);
    }
    sync_state(&fatal);

    /* ---- phase 5a (step 5): importer 对 imported VA host 直达 D2H 读 16MB ---- */
    if (!fatal && rank == 1 && imp_ok && vmm_mapped) {
        /* 前 16MB 此时为 step2 反向写的 TAG_BACK(rank1) */
        int ok = 0;
        memset(vbuf, 0, BW_SIZE);
        ret = aclrtMemcpy(vbuf, BW_SIZE, vmmVA, BW_SIZE, ACL_MEMCPY_DEVICE_TO_HOST);
        if (ret == ACL_ERROR_NONE)
            ok = (check_pat(vbuf, hw, 1, TAG_BACK) == 0);
        step_line(5, who, "host direct D2H read remote 16MB (imported VA) + verify",
                  ok, ret);
        if (ret != ACL_ERROR_NONE)
            BAIL(fatal);
    }

    /* ---- phase 5b (step 5): importer 对 imported VA host 直达 H2D 写 16MB ---- */
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

    /* ---- phase 5c (step 5): exporter 复核 host 直达写入的 16MB ---- */
    if (!fatal && rank == 0 && own_mapped) {
        int ok = 0;
        memset(vbuf, 0, BW_SIZE);
        ret = aclrtMemcpy(vbuf, BW_SIZE, ownVA, BW_SIZE, ACL_MEMCPY_DEVICE_TO_HOST);
        if (ret == ACL_ERROR_NONE)
            ok = (check_pat(vbuf, hw, 1, TAG_HOST_W) == 0);
        step_line(5, who, "D2H verify host-direct-written 16MB (r1/TAG4) on own VA",
                  ok, ret);
        if (!ok)
            BAIL(fatal);
    }
    sync_state(&fatal);

    /* ---- phase 6 (step 4): 带宽, 仅正确性全过后 ----
     * ★严禁同步循环测带宽(历史教训见文件头): 正确口径 3 次预热 + 30x16MB
     *   async 提交 + 末尾 1 次 StreamSync。
     * 读/写用互不重叠半区: 读 VMM[16,32MB)->local[16,32MB),
     * 写 local[0,16MB)->VMM[0,16MB) —— 写方向恰好把 step5 的 TAG_HOST_W 区恢复
     * 为对齐 TAG_BACK, 终态全 32MB TAG_BACK(rank1), 供 exporter 终验。 */
    if (!fatal && g_fail) {
        if (rank == 1)
            printf("[04] step 4 (r1): bandwidth SKIPPED (correctness not passed)\n");
    }
    if (!fatal && !g_fail && rank == 1 && vmm_mapped) {
        double t0, t1, gb;
        printf("[04] step 4 (r1): bandwidth on imported VMM VA, %u MB x %d iters + %d "
               "warmup, async submits + 1 final Sync ONLY (no sync loop!)\n",
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
        printf("[04] bw   D2D-read  (VMM->local) : %7.2f GB/s "
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
        printf("[04] bw   D2D-write (local->VMM) : %7.2f GB/s "
               "(30x16MB async + 1 Sync, total %.3f s)\n", gb / (t1 - t0), t1 - t0);
        fflush(stdout);
    }
    sync_state(&fatal);

    /* ---- phase 7 (step 2 终验): r1 全部完成后 exporter D2H 校验反向 pattern
     *     (全 32MB TAG_BACK —— 含带宽写方向恢复的 [0,16MB) 区) ---- */
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
        printf("[04] step 2 (r0): final verify SKIPPED (correctness not passed)\n");
    }
    sync_state(&fatal);

    /* ---- cleanup: r1 先全释放(Unmap -> FreePhysical -> Release), barrier 后
     *     r0 才释放自有物理内存(同序) ---- */
    if (rank == 1) {
        if (vmm_mapped) {
            ret = aclrtUnmapMem(vmmVA);
            printf("[04] cleanup (r1): UnmapMem(imported VA) ret=%d\n", ret);
        }
        if (h_imp) {
            ret = aclrtFreePhysical(h_imp);
            printf("[04] cleanup (r1): FreePhysical(imported) ret=%d\n", ret);
        }
        if (vmm_reserved) {
            ret = aclrtReleaseMemAddress(vmmVA);
            printf("[04] cleanup (r1): ReleaseMemAddress(imported VA) ret=%d\n", ret);
        }
        if (local)
            aclrtFree(local);
        if (stream)
            aclrtDestroyStream(stream);
    }
    MPI_Barrier(MPI_COMM_WORLD);   /* importer 全部释放后 exporter 才 free 物理内存 */
    if (rank == 0) {
        if (own_mapped) {
            ret = aclrtUnmapMem(ownVA);
            printf("[04] cleanup (r0): UnmapMem(main VA) ret=%d\n", ret);
        }
        if (h_own) {
            ret = aclrtFreePhysical(h_own);
            printf("[04] cleanup (r0): FreePhysical(main) ret=%d\n", ret);
        }
        if (own_reserved) {
            ret = aclrtReleaseMemAddress(ownVA);
            printf("[04] cleanup (r0): ReleaseMemAddress(main VA) ret=%d\n", ret);
        }
        if (ctrl_mapped) {
            ret = aclrtUnmapMem(ctrlVA);
            printf("[04] cleanup (r0): UnmapMem(ctrl VA) ret=%d\n", ret);
        }
        if (h_ctrl) {
            ret = aclrtFreePhysical(h_ctrl);
            printf("[04] cleanup (r0): FreePhysical(ctrl) ret=%d\n", ret);
        }
        if (ctrl_reserved) {
            ret = aclrtReleaseMemAddress(ctrlVA);
            printf("[04] cleanup (r0): ReleaseMemAddress(ctrl VA) ret=%d\n", ret);
        }
        if (p2p_ctrl_mapped) {
            ret = aclrtUnmapMem(p2p_ctrlVA);
            printf("[04] cleanup (r0): UnmapMem(p2p_ctrl VA) ret=%d\n", ret);
        }
        if (h_p2p_ctrl) {
            ret = aclrtFreePhysical(h_p2p_ctrl);
            printf("[04] cleanup (r0): FreePhysical(p2p_ctrl) ret=%d\n", ret);
        }
        if (p2p_ctrl_reserved) {
            ret = aclrtReleaseMemAddress(p2p_ctrlVA);
            printf("[04] cleanup (r0): ReleaseMemAddress(p2p_ctrl VA) ret=%d\n", ret);
        }
    }
    free(hbuf);
    free(vbuf);
    fflush(stdout);
    /* DisablePeerAccess: 防止退出时 D 态挂死(SRCU 死锁) */
    if (dev_ok) {
        int32_t peer = (rank == 0) ? devB : devA;
        aclError dr = aclrtDeviceDisablePeerAccess(peer);
        printf("[04] cleanup (r%d): DisablePeerAccess(%d) ret=%d\n", rank, peer, dr);
        aclrtResetDevice(dev);
    }
    if (rank == 1 && r1_setA)
        aclrtResetDevice(devA);
    if (init_ok)
        aclFinalize();

    /* ---- RESULT(聚合两 rank 状态; 两 rank 打印同一行) ---- */
    {
        int res[5];
        res[0] = g_fail;
        res[1] = (ctrl_kind == CTRL_NONE) ? 0 : ctrl_kind;
        res[2] = (int32_t)ctrl_ret;
        res[3] = (p2p_ctrl_kind == P2P_CTRL_NONE) ? 0 : p2p_ctrl_kind;
        res[4] = (int32_t)p2p_ctrl_ret;
        MPI_Allreduce(MPI_IN_PLACE, res, 5, MPI_INT, MPI_MAX, MPI_COMM_WORLD);
        g_fail = res[0];
        ctrl_kind = res[1];
        ctrl_ret = res[2];
        p2p_ctrl_kind = res[3];
        p2p_ctrl_ret = res[4];

        printf("[04] RESULT: %s", g_fail ? "FAIL" : "PASS");
        if (!g_fail) {
            if (p2p_ctrl_kind == P2P_CTRL_EXPECTED)
                printf("  ★step16: 无 EnablePeerAccess 直接 Import 失败(ret=%d)",
                       p2p_ctrl_ret);
            else if (p2p_ctrl_kind == P2P_CTRL_UNEXPECTED)
                printf("  ★step16: 无 EnablePeerAccess 直接 Import 成功(310P3 已验证), "
                       "EnablePeerAccess 不是必须的");
            if (ctrl_kind == CTRL_EXPECTED)
                printf("  ★step3: ctx 技巧 import 跨卡 FAIL ret=%d",
                       ctrl_ret);
            else if (ctrl_kind == CTRL_OTHER_ERR)
                printf("  ★step3: ctx 技巧 import FAIL ret=%d(非 507899, 见 step3)",
                       ctrl_ret);
            else if (ctrl_kind == CTRL_UNEXPECTED)
                printf("  ★step3: ctx 技巧(同卡 die 对方案)跨卡也能用(310P3 已验证, ret=0), "
                       "但跨卡非必须");
        }
        printf("\n");
        fflush(stdout);
    }

    MPI_Finalize();
    return g_fail ? 1 : 0;
}
