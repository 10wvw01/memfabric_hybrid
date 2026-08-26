/*
 * 05_vmm_die — VMM shareable handle 链路 die 对(同卡双 die)ctx 技巧测试
 * 机器: Atlas 300I Duo (310P3, 4 卡 x 2 die = 8 dev), CANN 9.1.0, aarch64
 * 结构: MPI 双进程 rank0=exporter(dev A, 环境变量 A_DEV, 默认 0)
 *                     rank1=importer(dev B, 环境变量 B_DEV, 默认 1)  <- die 对
 *
 * 已验证背景(作为预期):
 *   - VMM die 对 ctx 技巧: importer import 时临时 SetDevice(导出方 die A) →
 *     aclrtMemImportFromShareableHandle(sh, B, &h)(devId=B 本 die)→ SetDevice(B)
 *     切回 → Reserve+Map+SetAccess(RW,B)+D2D。ctx 技巧绕过 runtime P2P 前置
 *     (不调用 EnablePeerAccess), die 对同卡可用。
 *   - 反向对照(预期 507899): importer 用本进程 ctx(SetDevice(B) 保持)直接
 *     Import(sh2, B) → runtime P2P 前置触发 para_check 拒绝 die 对 → 507899。
 *   - die 对 async D2D 可用(读+写,双向逐字 pattern 校验); host 对 imported VA
 *     直达 H2D/D2H 可用。
 *   - memAttr 七级回退链 {HBM_P2P_HUGE, HBM_P2P_NORMAL, DDR_P2P_HUGE,
 *     DDR_P2P_NORMAL, MEM_P2P_HUGE, MEM_P2P_NORMAL, HBM_HUGE}:
 *     310P3 仅 ACL_MEM_P2P_HUGE 成功(介质前缀变体 ret=100000)。
 *
 * 校验 pattern: value = (rank<<28)|(tag<<24)|(idx&0xFFFFFF), 每步不同 tag(防
 * 残留数据造成假阳性)。
 *
 * 测试项(编号即测试项; 实际执行顺序 1,2,3,5,4 — 带宽在全正确性之后):
 *   1  exporter: MallocPhysical 32MB(七级回退; aclrtMemGetAllocationGranularity
 *      检查 RECOMMENDED 粒度; 2MB gran 对齐; MapMem size == MallocPhysical size)
 *      + Reserve(NULL hint)+Map+SetAccess(RW,A) + H2D pattern + Export sh1
 *      (DISABLE_PID_VALIDATION) + MPI 发 sh1(u64)
 *   2  importer 正向 ctx 技巧: SetDevice(A) + Import(sh1, B, &h) + SetDevice(B)
 *      + Reserve+Map+SetAccess(RW,B) + async D2D 读(VMM->local)+D2H 逐字校验 +
 *      async D2D 反向写(local->VMM) + exporter D2H 校验(带宽后另有全量终验)
 *   3  对照(预期 507899): exporter 第二块独立 16MB 物理内存 export sh2;
 *      importer 本进程 ctx(SetDevice(B) 保持) Import(sh2, B) -> 预期 507899,
 *      打印 EXPECTED-FAIL(本进程 ctx → runtime P2P 前置触发 para_check 拒绝)
 *   5  host 直达: importer 对 imported VA 直接 D2H 读/H2D 写 各 16MB + 校验
 *      (exporter 复核 host 直达写入的 16MB)
 *   4  带宽(正确性过后): imported VA async D2D 读+写, 3 预热 + 30x16MB async
 *      提交 + 末尾 1 次 Sync; 读/写用互不重叠 16MB 半区, 终态恢复全 32MB
 *      TAG_BACK(rank1), 供 exporter 终验
 *
 * 判定: 正向 1/2/5 全 OK -> PASS; 对照 3 结果如实记录(507899/其他错误码/意外
 *       成功均为有效观测, 不影响 PASS/FAIL)。
 *
 * 注意: die 对 ctx 技巧不触发 EnablePeerAccess(P2P 前置被跳过), 理论上无 D 态
 *   风险; 但 cleanup 仍 best-effort 调 DisablePeerAccess(失败无害)。
 *
 * cleanup 顺序(铁律): UnmapMem -> FreePhysical(own/imported) ->
 *   ReleaseMemAddress -> DisablePeerAccess(best-effort) -> ResetDevice ->
 *   Finalize; importer 全部释放(MPI barrier 后) exporter 才释放自有物理内存。
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
#define BW_WARMUP    3
#define BW_ITERS     30
#define EXPECTED_ERR 507899  /* ACL_ERROR_RT_DRV_INTERNAL_ERROR */

#define TAG_EXP    0xD1  /* step1: exporter 写入(step2 读校验) */
#define TAG_BACK   0xD2  /* step2: importer 反向写(step2/5/终验 校验) */
#define TAG_HOST_W 0xD3  /* step5: importer host 直达写 */
#define TAG_CTRL   0xD4  /* step3: exporter 写入对照块 */

/* ctx 技巧对照结果分类(RESULT 行注释用) */
#define CTRL_NONE        (-1)  /* 未执行 */
#define CTRL_EXPECTED    0     /* 507899, 符合预期 */
#define CTRL_OTHER_ERR   1     /* 非 507899 错误码 */
#define CTRL_UNEXPECTED  2     /* 意外成功 */

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
            printf("[05]   [verify] mismatch at word %zu (off 0x%zx): got 0x%08x, want 0x%08x\n",
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
    printf("[05] step %d (%s): %-58s %s (ret=%d)\n", n, who, desc,
           ok ? "OK" : "FAIL", ret);
    fflush(stdout);
    if (!ok)
        g_fail = 1;
}

/* 观测项行(不影响 PASS/FAIL) */
static void obs_line(int n, const char *who, const char *desc, int ok, int32_t ret)
{
    printf("[05] step %d (%s): %-58s [OBS] %s (ret=%d)\n", n, who, desc,
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

/* SetAccess(RW, devId) —— gva_pool 同款: 失败仅 WARN(obs), 数据校验为准 */
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
 * memAttr 七级回退 MallocPhysical: 依次尝试, 首个成功即用。
 * 对首个 attr(ACL_MEM_P2P_HUGE)调用 aclrtMemGetAllocationGranularity
 * (RECOMMENDED) 检查粒度(310P3 为 2MB); size 需为 granularity 整数倍。
 * 返回 0 成功(*chosenIdx 出选中项); 全败返回最后一次错误码。
 * 注: MapMem size 必须等于 MallocPhysical size。
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
        printf("[05]   [%s attr %d/7 %-14s] gran ret=%d gran=%zu; "
               "MallocPhysical(%uMB) ret=%d\n",
               label, i + 1, g_chainName[i], gr, gran,
               (unsigned)(size >> 20), m);
        fflush(stdout);
        if (m == ACL_ERROR_NONE) {
            *chosenIdx = i;
            if (gran != 0 && size % gran != 0)
                printf("[05]   [%s] WARN: size %zu NOT multiple of gran %zu "
                       "(MapMem size must equal MallocPhysical size!)\n",
                       label, size, gran);
            else
                printf("[05]   [%s] chosen %-14s, %uMB = %zu x gran %zuB\n",
                       label, g_chainName[i], (unsigned)(size >> 20),
                       gran ? size / gran : 0, gran);
            fflush(stdout);
            return 0;
        }
        last = m;
        *handle = NULL;
    }
    printf("[05]   [%s] all 7 attrs FAILED (last ret=%d)\n", label, last);
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
            printf("[05] FATAL: need exactly 2 MPI procs (-np 2), got %d\n", nproc);
        MPI_Finalize();
        return 1;
    }

    const char *role = (rank == 0) ? "r0/exporter" : "r1/importer";
    const char *who = (rank == 0) ? "r0" : "r1";
    int32_t devA = env_dev("A_DEV", 0);
    int32_t devB = env_dev("B_DEV", 1);
    int32_t dev = (rank == 0) ? devA : devB;

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("[05] === 05_vmm_die: VMM shareable handle link die-pair ctx trick, "
           "rank%d %s dev%d ===\n", rank, role, dev);
    printf("[05] %s: A_DEV=%d B_DEV=%d, 32MB main + 16MB ctrl block, "
           "7-attr fallback chain, pattern=(rank<<28)|(tag<<24)|(idx&0xFFFFFF)\n",
           role, devA, devB);
    if (devA == devB)
        printf("[05] %s: [WARN] A_DEV==B_DEV==%d — 同设备, 非die对场景(结果如实记录)\n",
               role, devA);
    else if (devA / 2 != devB / 2)
        printf("[05] %s: [WARN] A_DEV=%d B_DEV=%d 跨卡(非die对), ctx技巧预期失败\n",
               role, devA, devB);

    aclrtStream stream = NULL;      /* r1 的 async 流 */
    /* r0: 自有 VMM 主块(32MB) + 对照块(16MB) + EPA 测试块(16MB) */
    aclrtDrvMemHandle h_own = NULL, h_ctrl = NULL, h_epa = NULL;
    void *ownVA = NULL, *ctrlVA = NULL, *epaVA = NULL;
    int own_reserved = 0, own_mapped = 0, ctrl_reserved = 0, ctrl_mapped = 0;
    int epa_reserved = 0, epa_mapped = 0;
    int own_attr = -1, ctrl_attr = -1, epa_attr = -1;
    /* r1: 导入主块 */
    aclrtDrvMemHandle h_imp = NULL;
    void *vmmVA = NULL;
    int vmm_reserved = 0, vmm_mapped = 0;
    void *local = NULL;             /* r1 本地 32MB 缓冲 */
    uint32_t *hbuf = NULL, *vbuf = NULL;
    uint64_t sh1 = 0, sh2 = 0, sh3 = 0;
    int imp_ok = 0;
    int r1_setA = 0;                /* r1 ctx 技巧是否 SetDevice(A) 过(正向) */
    int ctrl_kind = CTRL_NONE;
    int32_t ctrl_ret = -1;
    int epa_kind = CTRL_NONE;       /* EPA+无ctx 对照结果 */
    int32_t epa_ret = -1;
    int fatal = 0;
    int32_t ret = 0;
    int init_ok = 0, dev_ok = 0;
    size_t words = VMM_SIZE / sizeof(uint32_t);
    size_t hw = BW_SIZE / sizeof(uint32_t);
    size_t cw = CTRL_SIZE / sizeof(uint32_t);

    /* ---- phase 0: init ---- */
    ret = aclInit(NULL);
    if (ret != ACL_ERROR_NONE) {
        printf("[05] init: %s aclInit FAIL ret=%d\n", role, ret);
        BAIL(fatal);
    } else {
        init_ok = 1;
        ret = aclrtSetDevice(dev);
        if (ret != ACL_ERROR_NONE) {
            printf("[05] init: %s aclrtSetDevice(%d) FAIL ret=%d\n", role, dev, ret);
            BAIL(fatal);
        }
    }
    if (!fatal) {
        dev_ok = 1;
        if (rank == 1) {
            ret = aclrtCreateStream(&stream);
            if (ret != ACL_ERROR_NONE) {
                printf("[05] init: r1 aclrtCreateStream FAIL ret=%d\n", ret);
                BAIL(fatal);
            }
        }
        hbuf = (uint32_t *)malloc(VMM_SIZE);
        vbuf = (uint32_t *)malloc(VMM_SIZE);
        if (!hbuf || !vbuf) {
            printf("[05] init: %s host malloc 2x32MB FAIL\n", role);
            BAIL(fatal);
        }
    }
    if (!fatal)
        printf("[05] init: %s aclInit/SetDevice(%d)%s OK\n", role, dev,
               (rank == 1) ? "/CreateStream" : "");
    sync_state(&fatal);

    /* ---- phase 1 (step 1): exporter MallocPhysical(七级回退)
     *     + Reserve + Map + SetAccess + H2D pattern + Export sh1 + MPI 发 sh1 ----
     * 注意: die 对 ctx 技巧不需要 exporter 侧 EnablePeerAccess(P2P 前置被跳过) */
    if (!fatal && rank == 0) {
        int ok = 0;
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
        if (ret == ACL_ERROR_NONE)
            ret = aclrtMemExportToShareableHandle(
                h_own, ACL_MEM_HANDLE_TYPE_NONE,
                ACL_RT_VMM_EXPORT_FLAG_DISABLE_PID_VALIDATION, &sh1);
        if (ret == ACL_ERROR_NONE && sh1 != 0) {
            if (MPI_Send(&sh1, 1, MPI_UINT64_T, 1, 100, MPI_COMM_WORLD) == MPI_SUCCESS)
                ok = 1;
        }
        /* 无论成败都发 sh(失败发 0), 避免对端 Recv 悬挂 */
        if (!ok) {
            uint64_t bad = 0;
            MPI_Send(&bad, 1, MPI_UINT64_T, 1, 100, MPI_COMM_WORLD);
            printf("[05] step 1 (r0): exporter chain FAILED before send "
                   "(chosen attr idx=%d, sh1=0x%llx)\n", own_attr,
                   (unsigned long long)sh1);
        } else {
            printf("[05] step 1 (r0): sh1=0x%llx sent\n",
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
            printf("[05] step 1 (r1): received sh1=0 (exporter failed)\n");
    }
    sync_state(&fatal);

    /* ---- phase 2a (step 2): importer 正向 ctx 技巧 import + map + async D2D 读 + 校验
     *     ctx 技巧: SetDevice(A) → Import(sh1, B, &h) → SetDevice(B)
     *     临时切到导出方 die ctx 做 import, 绕过 runtime P2P 前置。
     *     die 对同卡, import 的 devId=B(本 die); import 后切回 B ctx 做
     *     Reserve/Map/SetAccess/D2D。不调用 EnablePeerAccess。 */
    if (!fatal && rank == 1 && sh1 != 0) {
        int ok = 0;
        int32_t sd = aclrtSetDevice(devA);
        obs_line(2, who, "ctx trick: SetDevice(A_dev) before import (bypass P2P prereq)",
                 sd == ACL_ERROR_NONE, sd);
        if (sd == ACL_ERROR_NONE) {
            r1_setA = 1;
            ret = aclrtMemImportFromShareableHandle(sh1, devB, &h_imp);
        } else {
            ret = sd;
        }
        /* 无论 import 成败, 切回本进程 ctx(B) */
        if (r1_setA) {
            int32_t sd2 = aclrtSetDevice(devB);
            obs_line(2, who, "SetDevice(B_dev) restore after import",
                     sd2 == ACL_ERROR_NONE, sd2);
        }
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
                  "ctx-trick import(A->Imp sh1,B->SetDevice B)+Reserve+Map"
                  "+async D2D read+verify",
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
        step_line(2, who, "D2H verify reverse-written pattern (r1/TAG_D2) on own VA",
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
        if (ret == ACL_ERROR_NONE)
            ret = aclrtMemExportToShareableHandle(
                h_ctrl, ACL_MEM_HANDLE_TYPE_NONE,
                ACL_RT_VMM_EXPORT_FLAG_DISABLE_PID_VALIDATION, &sh2);
        if (ret == ACL_ERROR_NONE && sh2 != 0) {
            if (MPI_Send(&sh2, 1, MPI_UINT64_T, 1, 101, MPI_COMM_WORLD) == MPI_SUCCESS)
                ok = 1;
        }
        if (!ok) {
            uint64_t bad = 0;
            MPI_Send(&bad, 1, MPI_UINT64_T, 1, 101, MPI_COMM_WORLD);
            printf("[05] step 3 (r0): ctrl chain FAILED before send "
                   "(chosen attr idx=%d, sh2=0x%llx)\n", ctrl_attr,
                   (unsigned long long)sh2);
        } else {
            printf("[05] step 3 (r0): sh2=0x%llx sent\n",
                   (unsigned long long)sh2);
        }
        step_line(3, who, "MallocPhysical 16MB #2 + H2D pattern + Export sh2 + MPI-send",
                  ok, ret);
        if (!ok)
            BAIL(fatal);
    } else if (!fatal && rank == 1) {
        MPI_Recv(&sh2, 1, MPI_UINT64_T, 0, 101, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        if (sh2 == 0)
            printf("[05] step 3 (r1): received sh2=0 (exporter ctrl block failed)\n");
    }
    sync_state(&fatal);

    /* ---- phase 3b (step 3, 对照): importer 本进程 ctx import(预期 507899) ----
     * 不做 ctx 技巧(保持 SetDevice(B)), 直接 import sh2。
     * die 对本进程 ctx import 触发 runtime P2P 前置, para_check 拒绝 -> 507899。
     * 独立 sh2 规避"同一 sh 同进程二次 import"的干扰。 */
    if (!fatal && rank == 1 && sh2 != 0) {
        aclrtDrvMemHandle h2 = NULL;
        /* 本进程 ctx(B 保持) — 不做 SetDevice(A), 直接 import */
        ctrl_ret = aclrtMemImportFromShareableHandle(sh2, devB, &h2);
        if (ctrl_ret == ACL_ERROR_NONE) {
            ctrl_kind = CTRL_UNEXPECTED;
            printf("[05] step 3 (r1): %-58s 意外成功 ret=0 <- 本进程 ctx die 对"
                   "可用?(与预期相反, 如实记录; 仅观测 import, 不建窗口)\n",
                   "ctrl: own-ctx Import(sh2,B) — expect 507899");
            aclrtFreePhysical(h2);
        } else if (ctrl_ret == EXPECTED_ERR) {
            ctrl_kind = CTRL_EXPECTED;
            printf("[05] step 3 (r1): %-58s EXPECTED-FAIL ret=%d\n",
                   "ctrl: own-ctx Import(sh2,B) — expect 507899", ctrl_ret);
        } else {
            ctrl_kind = CTRL_OTHER_ERR;
            printf("[05] step 3 (r1): %-58s EXPECTED-FAIL ret=%d  [注意: 非"
                   "预期 507899, 如实记录]\n",
                   "ctrl: own-ctx Import(sh2,B) — expect 507899", ctrl_ret);
        }
        fflush(stdout);
        /* 无需 SetDevice(B) 切回 — 本进程 ctx 始终保持 B */
    }
    sync_state(&fatal);

    /* ---- phase 3c (step 6, 新增对照): EnablePeerAccess + 本进程 ctx import ----
     * 验证: 调用 EnablePeerAccess(devA) 后, 不做 ctx 技巧直接 import 是否可行。
     * exporter 分配第三块独立 16MB (sh3), importer EnablePeerAccess(devA) 后
     * 直接 Import(sh3, devB)。测试完 DisablePeerAccess(devA)。 */
    if (!fatal && rank == 0) {
        int ok = 0;
        ret = malloc_physical_chain(devA, CTRL_SIZE, &h_epa, "epa", &epa_attr);
        if (ret == ACL_ERROR_NONE)
            ret = aclrtReserveMemAddress(&epaVA, CTRL_SIZE, 0, NULL, 0);
        if (ret == ACL_ERROR_NONE) {
            epa_reserved = 1;
            ret = aclrtMapMem(epaVA, CTRL_SIZE, 0, h_epa, 0);
        }
        if (ret == ACL_ERROR_NONE) {
            epa_mapped = 1;
            int32_t sa = set_access_rw(epaVA, CTRL_SIZE, devA);
            obs_line(6, who, "SetAccess(RW, A_dev) on epa VA", sa == ACL_ERROR_NONE, sa);
            fill_pat(hbuf, cw, 0, TAG_CTRL);
            ret = aclrtMemcpy(epaVA, CTRL_SIZE, hbuf, CTRL_SIZE,
                              ACL_MEMCPY_HOST_TO_DEVICE);
        }
        if (ret == ACL_ERROR_NONE)
            ret = aclrtMemExportToShareableHandle(
                h_epa, ACL_MEM_HANDLE_TYPE_NONE,
                ACL_RT_VMM_EXPORT_FLAG_DISABLE_PID_VALIDATION, &sh3);
        if (ret == ACL_ERROR_NONE && sh3 != 0) {
            if (MPI_Send(&sh3, 1, MPI_UINT64_T, 1, 102, MPI_COMM_WORLD) == MPI_SUCCESS)
                ok = 1;
        }
        if (!ok) {
            uint64_t bad = 0;
            MPI_Send(&bad, 1, MPI_UINT64_T, 1, 102, MPI_COMM_WORLD);
            printf("[05] step 6 (r0): epa chain FAILED before send "
                   "(chosen attr idx=%d, sh3=0x%llx)\n", epa_attr,
                   (unsigned long long)sh3);
        } else {
            printf("[05] step 6 (r0): sh3=0x%llx sent\n",
                   (unsigned long long)sh3);
        }
        step_line(6, who, "MallocPhysical 16MB #3 + H2D pattern + Export sh3 + MPI-send",
                  ok, ret);
        if (!ok)
            BAIL(fatal);
    } else if (!fatal && rank == 1) {
        MPI_Recv(&sh3, 1, MPI_UINT64_T, 0, 102, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        if (sh3 == 0)
            printf("[05] step 6 (r1): received sh3=0 (exporter epa block failed)\n");
    }
    sync_state(&fatal);

    /* ---- phase 3d (step 6, 对照): EnablePeerAccess + 本进程 ctx import ----
     * importer 调用 EnablePeerAccess(devA), 然后不做 ctx 技巧直接 import sh3。
     * 验证 EnablePeerAccess 是否能替代 ctx 技巧使 die 对 import 成功。 */
    if (!fatal && rank == 1 && sh3 != 0) {
        aclrtDrvMemHandle h3 = NULL;
        void *epaImpVA = NULL;
        int epaImp_reserved = 0, epaImp_mapped = 0;
        /* 先调用 aclrtDeviceEnablePeerAccess(devA) */
        int32_t epa_enable_ret = aclrtDeviceEnablePeerAccess(devA, 0);
        printf("[05] step 6 (r1): aclrtDeviceEnablePeerAccess(devA=%d) ret=%d\n",
               devA, epa_enable_ret);
        fflush(stdout);

        /* 本进程 ctx(B 保持) — 不做 SetDevice(A), 直接 import */
        epa_ret = aclrtMemImportFromShareableHandle(sh3, devB, &h3);
        if (epa_ret == ACL_ERROR_NONE) {
            epa_kind = CTRL_UNEXPECTED;
            printf("[05] step 6 (r1): %-58s 成功 ret=0 <- EnablePeerAccess 可使"
                   "die 对 import 成功? (如实记录)\n",
                   "epa: EnablePeerAccess+own-ctx Import(sh3,B)");
            /* 尝试建立窗口并校验 */
            if (aclrtReserveMemAddress(&epaImpVA, CTRL_SIZE, 0, NULL, 0) == ACL_ERROR_NONE) {
                epaImp_reserved = 1;
                if (aclrtMapMem(epaImpVA, CTRL_SIZE, 0, h3, 0) == ACL_ERROR_NONE) {
                    epaImp_mapped = 1;
                    set_access_rw(epaImpVA, CTRL_SIZE, devB);
                    /* D2D 读 + 校验 */
                    void *epaLocal = NULL;
                    if (aclrtMalloc(&epaLocal, CTRL_SIZE, ACL_MEM_MALLOC_HUGE_FIRST) == ACL_ERROR_NONE) {
                        if (aclrtMemcpyAsync(epaLocal, CTRL_SIZE, epaImpVA, CTRL_SIZE,
                                             ACL_MEMCPY_DEVICE_TO_DEVICE, stream) == ACL_ERROR_NONE) {
                            aclrtSynchronizeStream(stream);
                            memset(vbuf, 0, CTRL_SIZE);
                            if (aclrtMemcpy(vbuf, CTRL_SIZE, epaLocal, CTRL_SIZE,
                                            ACL_MEMCPY_DEVICE_TO_HOST) == ACL_ERROR_NONE) {
                                int verify_ok = (check_pat(vbuf, cw, 0, TAG_CTRL) == 0);
                                printf("[05] step 6 (r1): D2D read+verify %s\n",
                                       verify_ok ? "OK" : "FAIL");
                            }
                        }
                        aclrtFree(epaLocal);
                    }
                }
                if (epaImp_mapped) aclrtUnmapMem(epaImpVA);
                if (epaImp_reserved) aclrtReleaseMemAddress(epaImpVA);
            }
            aclrtFreePhysical(h3);
        } else if (epa_ret == EXPECTED_ERR) {
            epa_kind = CTRL_EXPECTED;
            printf("[05] step 6 (r1): %-58s FAIL ret=%d (EnablePeerAccess 无法替代"
                   " ctx 技巧, die 对仍被拒绝)\n",
                   "epa: EnablePeerAccess+own-ctx Import(sh3,B)", epa_ret);
        } else {
            epa_kind = CTRL_OTHER_ERR;
            printf("[05] step 6 (r1): %-58s FAIL ret=%d (非 507899, 如实记录)\n",
                   "epa: EnablePeerAccess+own-ctx Import(sh3,B)", epa_ret);
        }
        fflush(stdout);
        /* DisablePeerAccess(devA) — 测试完清理 */
        int32_t epa_disable_ret = aclrtDeviceDisablePeerAccess(devA);
        printf("[05] step 6 (r1): DisablePeerAccess(devA=%d) ret=%d\n",
               devA, epa_disable_ret);
        fflush(stdout);
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
        step_line(5, who, "D2H verify host-direct-written 16MB (r1/TAG_D3) on own VA",
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
            printf("[05] step 4 (r1): bandwidth SKIPPED (correctness not passed)\n");
    }
    if (!fatal && !g_fail && rank == 1 && vmm_mapped) {
        double t0, t1, gb;
        printf("[05] step 4 (r1): bandwidth on imported VMM VA (ctx trick), %u MB x %d "
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
        printf("[05] bw   D2D-read  (VMM->local) : %7.2f GB/s "
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
        printf("[05] bw   D2D-write (local->VMM) : %7.2f GB/s "
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
        printf("[05] step 2 (r0): final verify SKIPPED (correctness not passed)\n");
    }
    sync_state(&fatal);

    /* ---- cleanup: r1 先全释放(Unmap -> FreePhysical -> Release), barrier 后
     *     r0 才释放自有物理内存(同序) ---- */
    if (rank == 1) {
        if (vmm_mapped) {
            ret = aclrtUnmapMem(vmmVA);
            printf("[05] cleanup (r1): UnmapMem(imported VA) ret=%d\n", ret);
        }
        if (h_imp) {
            ret = aclrtFreePhysical(h_imp);
            printf("[05] cleanup (r1): FreePhysical(imported) ret=%d\n", ret);
        }
        if (vmm_reserved) {
            ret = aclrtReleaseMemAddress(vmmVA);
            printf("[05] cleanup (r1): ReleaseMemAddress(imported VA) ret=%d\n", ret);
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
            printf("[05] cleanup (r0): UnmapMem(main VA) ret=%d\n", ret);
        }
        if (h_own) {
            ret = aclrtFreePhysical(h_own);
            printf("[05] cleanup (r0): FreePhysical(main) ret=%d\n", ret);
        }
        if (own_reserved) {
            ret = aclrtReleaseMemAddress(ownVA);
            printf("[05] cleanup (r0): ReleaseMemAddress(main VA) ret=%d\n", ret);
        }
        if (ctrl_mapped) {
            ret = aclrtUnmapMem(ctrlVA);
            printf("[05] cleanup (r0): UnmapMem(ctrl VA) ret=%d\n", ret);
        }
        if (h_ctrl) {
            ret = aclrtFreePhysical(h_ctrl);
            printf("[05] cleanup (r0): FreePhysical(ctrl) ret=%d\n", ret);
        }
        if (ctrl_reserved) {
            ret = aclrtReleaseMemAddress(ctrlVA);
            printf("[05] cleanup (r0): ReleaseMemAddress(ctrl VA) ret=%d\n", ret);
        }
        if (epa_mapped) {
            ret = aclrtUnmapMem(epaVA);
            printf("[05] cleanup (r0): UnmapMem(epa VA) ret=%d\n", ret);
        }
        if (h_epa) {
            ret = aclrtFreePhysical(h_epa);
            printf("[05] cleanup (r0): FreePhysical(epa) ret=%d\n", ret);
        }
        if (epa_reserved) {
            ret = aclrtReleaseMemAddress(epaVA);
            printf("[05] cleanup (r0): ReleaseMemAddress(epa VA) ret=%d\n", ret);
        }
    }
    free(hbuf);
    free(vbuf);
    fflush(stdout);
    /* ★D 态修复(best-effort): die 对 ctx 技巧不触发 EnablePeerAccess(P2P 前置
     *   被跳过), 理论上无 D 态风险; 但 cleanup 仍 best-effort 调
     *   DisablePeerAccess(失败无害)。r0(disable devB)+ r1(disable devA)对称。 */
    if (dev_ok) {
        int32_t peer = (rank == 0) ? devB : devA;
        aclError dr = aclrtDeviceDisablePeerAccess(peer);
        printf("[05] cleanup (r%d): DisablePeerAccess(%d) ret=%d (best-effort)\n",
               rank, peer, dr);
        aclrtResetDevice(dev);
    }
    if (rank == 1 && r1_setA)
        aclrtResetDevice(devA);   /* ctx 技巧在本进程开过的 A 侧 context */
    if (init_ok)
        aclFinalize();

    /* ---- RESULT(聚合两 rank 状态; 两 rank 打印同一行) ---- */
    {
        int res[5];
        res[0] = g_fail;
        res[1] = (ctrl_kind == CTRL_NONE) ? 0 : ctrl_kind;
        res[2] = (int32_t)ctrl_ret;
        res[3] = (epa_kind == CTRL_NONE) ? 0 : epa_kind;
        res[4] = (int32_t)epa_ret;
        MPI_Allreduce(MPI_IN_PLACE, res, 5, MPI_INT, MPI_MAX, MPI_COMM_WORLD);
        g_fail = res[0];
        ctrl_kind = res[1];
        ctrl_ret = res[2];
        epa_kind = res[3];
        epa_ret = res[4];

        printf("[05] RESULT: %s", g_fail ? "FAIL" : "PASS");
        if (!g_fail) {
            if (ctrl_kind == CTRL_EXPECTED)
                printf("  ★step3: 本进程 ctx import EXPECTED-FAIL ret=%d(符合预期)",
                       ctrl_ret);
            else if (ctrl_kind == CTRL_OTHER_ERR)
                printf("  ★step3: 本进程 ctx import FAIL ret=%d(非 507899)",
                       ctrl_ret);
            else if (ctrl_kind == CTRL_UNEXPECTED)
                printf("  ★step3: 本进程 ctx import 意外成功(ret=0)!");
            if (epa_kind == CTRL_UNEXPECTED)
                printf("  ★step6: EnablePeerAccess+own-ctx import 成功 ret=%d!",
                       epa_ret);
            else if (epa_kind == CTRL_EXPECTED)
                printf("  ★step6: EnablePeerAccess+own-ctx import FAIL ret=%d(EnablePeerAccess 无法替代 ctx 技巧)",
                       epa_ret);
            else if (epa_kind == CTRL_OTHER_ERR)
                printf("  ★step6: EnablePeerAccess+own-ctx import FAIL ret=%d(非 507899)",
                       epa_ret);
        }
        printf("\n");
        fflush(stdout);
    }

    MPI_Finalize();
    return g_fail ? 1 : 0;
}
