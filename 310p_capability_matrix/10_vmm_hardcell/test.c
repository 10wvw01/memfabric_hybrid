/*
 * 10_vmm_hardcell — VMM ✗ 单元格边界负向测试
 * 机器: Atlas 300I Duo (310P3, 4 卡 x 2 die = 8 dev), CANN 9.1.0, aarch64
 * 结构: MPI 双进程 rank0=exporter(dev A, 环境变量 A_DEV, 默认 1)
 *                     rank1=importer(dev B, 环境变量 B_DEV, 默认 4)  <- ✗ 单元格
 *
 * 已验证背景(作为预期):
 *   - ✗ 单元格 = 跨卡但 VMM import 不可达的有向对(如 dev1→dev4、dev4→dev1、
 *     dev1→dev6 等)。这些对 aclrtDeviceCanAccessPeer can=1、
 *     aclrtDeviceEnablePeerAccess ret=0(成功),但 VMM
 *     aclrtMemImportFromShareableHandle 无论用本进程 ctx 还是 ctx 技巧
 *     (SetDevice(导出方)→Import)都确定性返回 507899
 *     (ACL_ERROR_RT_DRV_INTERNAL_ERROR)。
 *   - 这是 VMM 的边界(内核侧限制):EnablePeerAccess 能打通 P2P 通路,但 VMM
 *     shareable handle 的 import 路径在此类单元格上被驱动拒绝。
 *   - IPC-PEER 链路在此类 ✗ 单元格上可打通(03 已证),可作 VMM 不可达时的替代。
 *
 * 本测试为负向测试: 验证两种 VMM import 方式(本进程 ctx / ctx 技巧)都失败,
 * 文档化 VMM 的不可达边界。无 D2D / 无带宽(import 都失败,无 imported VA 可用)。
 *
 * 校验 pattern: value = (rank<<28)|(tag<<24)|(idx&0xFFFFFF), tag 0x2/0x5 每步
 * 不同,防残留数据假阳性(本测试虽不回读,仍写入以保持链路真实性与链上测试一致)。
 *
 * 测试项:
 *   1  exporter: EnablePeerAccess(B)(✗单元格预期 ret=0,obs)+ MallocPhysical 16MB
 *      (七级回退; 2MB gran 对齐; MapMem size == MallocPhysical size)+ Reserve+Map+
 *      SetAccess(RW,A)+ H2D pattern + Export sh1(DISABLE_PID) + MPI 发 sh1(u64)
 *   2  importer 方式一(本进程 ctx): EnablePeerAccess(A,0)(预期 ret=0,obs)+
 *      aclrtMemImportFromShareableHandle(sh1, B, &h)(ctx=B,本进程)-> 预期 507899,
 *      打印 EXPECTED-FAIL + 错误码;若意外成功打印 UNEXPECTED 并 FreePhysical 回收
 *   3  exporter 第二块独立 16MB 物理内存 export sh2(规避同 sh 二次 import 干扰);
 *      importer 方式二(ctx 技巧): SetDevice(A)(借导出方 ctx)->
 *      aclrtMemImportFromShareableHandle(sh2, B, &h2)(ctx=A=导出方, devId=B)->
 *      预期 507899, 打印 EXPECTED-FAIL + 错误码; 测完 SetDevice(B) 切回本进程 ctx
 *
 * 判定: exporter 正向 1/3 全 OK(import 链路建得起来)+ 两种 import 方式都有结果
 *       (EXPECTED-FAIL / OTHER_ERR / UNEXPECTED 均为有效观测) -> PASS;
 *       若意外成功(UNEXPECTED)也 PASS 但显著标注"推翻预期"。
 *
 * cleanup 顺序(铁律): UnmapMem -> FreePhysical -> ReleaseMemAddress;
 *   ★跨卡 P2P 状态必须在 ResetDevice 前显式拆除(aclrtDeviceDisablePeerAccess),
 *   否则进程退出时 mmu notifier 释放与驱动 SRCU 死锁(详见 03/04 README)。
 *   exporter: DisablePeerAccess(B) + ResetDevice(A);
 *   importer: DisablePeerAccess(A) + ResetDevice(B) + (若 r1_setA: ResetDevice(A))。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <mpi.h>
#include "acl/acl.h"

#define VMM_SIZE     (16u * 1024u * 1024u)  /* 16 MB 主块(方式一 sh1) */
#define CTRL_SIZE    (16u * 1024u * 1024u)  /* 16 MB 第二块(方式二 sh2) */
#define EXPECTED_ERR 507899  /* ACL_ERROR_RT_DRV_INTERNAL_ERROR */

#define TAG_EXP    0x2  /* step1: exporter 写入(链路真实性) */
#define TAG_CTRL   0x5  /* step3: exporter 写入对照块 */

/* import 结果分类(RESULT 行注释用) */
#define KIND_NONE        (-1)  /* 未执行 */
#define KIND_EXPECTED    0     /* 507899, 符合预期 */
#define KIND_OTHER_ERR   1     /* 非 507899 错误码 */
#define KIND_UNEXPECTED  2     /* 意外成功 */

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

static int32_t env_dev(const char *name, int32_t dflt)
{
    const char *s = getenv(name);
    return s ? (int32_t)atoi(s) : dflt;
}

/* 正向步骤行: ok=0 记 FAIL */
static void step_line(int n, const char *who, const char *desc, int ok, int32_t ret)
{
    printf("[10] step %d (%s): %-58s %s (ret=%d)\n", n, who, desc,
           ok ? "OK" : "FAIL", ret);
    fflush(stdout);
    if (!ok)
        g_fail = 1;
}

/* 观测项行(不影响 PASS/FAIL) */
static void obs_line(int n, const char *who, const char *desc, int ok, int32_t ret)
{
    printf("[10] step %d (%s): %-58s [OBS] %s (ret=%d)\n", n, who, desc,
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

/*
 * memAttr 七级回退 MallocPhysical: 依次尝试, 首个成功即用。
 * 返回 0 成功(*chosenIdx 出选中项); 全败返回最后一次错误码。
 * 注: size 需为 granularity 整数倍(16MB 对 2MB gran 满足);
 *     MapMem size 必须等于 MallocPhysical size。
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
        printf("[10]   [%s attr %d/7 %-14s] gran ret=%d gran=%zu; "
               "MallocPhysical(%uMB) ret=%d\n",
               label, i + 1, g_chainName[i], gr, gran,
               (unsigned)(size >> 20), m);
        fflush(stdout);
        if (m == ACL_ERROR_NONE) {
            *chosenIdx = i;
            if (gran != 0 && size % gran != 0)
                printf("[10]   [%s] WARN: size %zu NOT multiple of gran %zu "
                       "(MapMem size must equal MallocPhysical size!)\n",
                       label, size, gran);
            else
                printf("[10]   [%s] chosen %-14s, %uMB = %zu x gran %zuB\n",
                       label, g_chainName[i], (unsigned)(size >> 20),
                       gran ? size / gran : 0, gran);
            fflush(stdout);
            return 0;
        }
        last = m;
        *handle = NULL;
    }
    printf("[10]   [%s] all 7 attrs FAILED (last ret=%d)\n", label, last);
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
            printf("[10] FATAL: need exactly 2 MPI procs (-np 2), got %d\n", nproc);
        MPI_Finalize();
        return 1;
    }

    const char *role = (rank == 0) ? "r0/exporter" : "r1/importer";
    const char *who = (rank == 0) ? "r0" : "r1";
    int32_t devA = env_dev("A_DEV", 1);
    int32_t devB = env_dev("B_DEV", 4);
    int32_t dev = (rank == 0) ? devA : devB;

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("[10] === 10_vmm_hardcell: VMM ✗ 单元格边界负向测试 (两种 import 方式预期失败), "
           "rank%d %s dev%d ===\n", rank, role, dev);
    printf("[10] %s: A_DEV=%d B_DEV=%d (✗ cell), 16MB x2 (sh1/sh2), 7-attr fallback, "
           "pattern=(rank<<28)|(tag<<24)|(idx&0xFFFFFF)\n", role, devA, devB);
    if (devA == devB)
        printf("[10] %s: [WARN] A_DEV==B_DEV==%d — 同设备, 非 ✗ 单元格场景(结果如实记录)\n",
               role, devA);

    /* r0: 两块自有 VMM 物理内存(16MB sh1 + 16MB sh2) */
    aclrtDrvMemHandle h_own = NULL, h_ctrl = NULL;
    void *ownVA = NULL, *ctrlVA = NULL;
    int own_reserved = 0, own_mapped = 0, ctrl_reserved = 0, ctrl_mapped = 0;
    int own_attr = -1, ctrl_attr = -1;
    uint64_t sh1 = 0, sh2 = 0;
    uint32_t *hbuf = NULL;
    /* r1: 两种 import 结果 */
    int m1_kind = KIND_NONE, m2_kind = KIND_NONE;
    int32_t m1_ret = -1, m2_ret = -1;
    int r1_setA = 0;                /* r1 ctx 技巧是否成功 SetDevice(A) 过 */
    int fatal = 0;
    int32_t ret = 0;
    int init_ok = 0, dev_ok = 0;
    size_t words = VMM_SIZE / sizeof(uint32_t);
    size_t cw = CTRL_SIZE / sizeof(uint32_t);

    /* ---- phase 0: init ---- */
    ret = aclInit(NULL);
    if (ret != ACL_ERROR_NONE) {
        printf("[10] init: %s aclInit FAIL ret=%d\n", role, ret);
        BAIL(fatal);
    } else {
        init_ok = 1;
        ret = aclrtSetDevice(dev);
        if (ret != ACL_ERROR_NONE) {
            printf("[10] init: %s aclrtSetDevice(%d) FAIL ret=%d\n", role, dev, ret);
            BAIL(fatal);
        }
    }
    if (!fatal) {
        dev_ok = 1;
        if (rank == 0) {
            hbuf = (uint32_t *)malloc(VMM_SIZE);
            if (!hbuf) {
                printf("[10] init: r0 host malloc 16MB FAIL\n");
                BAIL(fatal);
            }
        }
    }
    if (!fatal)
        printf("[10] init: %s aclInit/SetDevice(%d) OK\n", role, dev);
    sync_state(&fatal);

    /* ---- phase 1 (step 1): exporter EnablePeerAccess(B,0)(obs,✗cell 预期 0)
     *     + MallocPhysical(七级回退) + Reserve + Map + SetAccess + H2D pattern
     *     + Export sh1 + MPI 发 sh1 ---- */
    if (!fatal && rank == 0) {
        int ok = 0;
        /* 双向 P2P 之 exporter 侧: ✗ 单元格 EnablePeerAccess 预期 ret=0(能打通 P2P) */
        int32_t ena = aclrtDeviceEnablePeerAccess(devB, 0);
        obs_line(1, who, "EnablePeerAccess(B_dev,0) (✗cell, expect ret=0)",
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
            printf("[10] step 1 (r0): exporter chain FAILED before send "
                   "(chosen attr idx=%d, sh1=0x%llx)\n", own_attr,
                   (unsigned long long)sh1);
        } else {
            printf("[10] step 1 (r0): sh1=0x%llx sent\n",
                   (unsigned long long)sh1);
        }
        step_line(1, who,
                  "EnablePeer+MallocPhysical(7attr)+Reserve+Map+H2D pat+Export sh1+send",
                  ok, ret);
        if (!ok)
            BAIL(fatal);
    } else if (!fatal && rank == 1) {
        MPI_Recv(&sh1, 1, MPI_UINT64_T, 0, 100, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        if (sh1 == 0)
            printf("[10] step 1 (r1): received sh1=0 (exporter failed)\n");
    }
    sync_state(&fatal);

    /* ---- phase 2 (step 2): importer 方式一(本进程 ctx) import, 预期 507899 ----
     * ✗ 单元格: EnablePeerAccess(A) 预期 ret=0(能打通 P2P 通路), 但 VMM import
     * 在本进程 ctx(B)下被驱动拒绝。本进程 ctx = init 中 SetDevice(devB)。 */
    if (!fatal && rank == 1 && sh1 != 0) {
        int32_t ena = aclrtDeviceEnablePeerAccess(devA, 0);
        obs_line(2, who, "EnablePeerAccess(A_dev,0) (✗cell, expect ret=0)",
                 ena == ACL_ERROR_NONE, ena);
        aclrtDrvMemHandle h = NULL;
        m1_ret = aclrtMemImportFromShareableHandle(sh1, devB, &h);
        if (m1_ret == ACL_ERROR_NONE) {
            /* 意外成功: 推翻预期。仅 import, 不建窗口; 回收 handle 防泄漏。 */
            m1_kind = KIND_UNEXPECTED;
            aclrtFreePhysical(h);
            printf("[10] step 2 (r1): %-58s 意外成功 ret=0 <- 推翻预期! "
                   "✗ 单元格本进程 ctx import 竟可用?(如实记录; 仅观测 import, 不建窗口)\n",
                   "method1 own-ctx: Import(sh1,B) — expect 507899");
        } else if (m1_ret == EXPECTED_ERR) {
            m1_kind = KIND_EXPECTED;
            printf("[10] step 2 (r1): %-58s EXPECTED-FAIL ret=%d\n",
                   "method1 own-ctx: Import(sh1,B) — expect 507899", m1_ret);
        } else {
            m1_kind = KIND_OTHER_ERR;
            printf("[10] step 2 (r1): %-58s EXPECTED-FAIL ret=%d  [注意: 非"
                   "预期 507899, 如实记录]\n",
                   "method1 own-ctx: Import(sh1,B) — expect 507899", m1_ret);
        }
        fflush(stdout);
    }
    sync_state(&fatal);

    /* ---- phase 3a (step 3): exporter 第二块独立 16MB + Export sh2 ---- */
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
            printf("[10] step 3 (r0): ctrl chain FAILED before send "
                   "(chosen attr idx=%d, sh2=0x%llx)\n", ctrl_attr,
                   (unsigned long long)sh2);
        } else {
            printf("[10] step 3 (r0): sh2=0x%llx sent\n",
                   (unsigned long long)sh2);
        }
        step_line(3, who, "MallocPhysical 16MB #2 + H2D pattern + Export sh2 + MPI-send",
                  ok, ret);
        if (!ok)
            BAIL(fatal);
    } else if (!fatal && rank == 1) {
        MPI_Recv(&sh2, 1, MPI_UINT64_T, 0, 101, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        if (sh2 == 0)
            printf("[10] step 3 (r1): received sh2=0 (exporter ctrl block failed)\n");
    }
    sync_state(&fatal);

    /* ---- phase 3b (step 3, 方式二 ctx 技巧): importer SetDevice(A) + Import(sh2,B),
     *     预期 507899 ----
     * 独立 sh2 规避"同一 sh 同进程二次 import"干扰, 观测纯粹的 ctx 技巧行为。
     * ✗ 单元格: 即便借导出方 ctx(SetDevice(A)), import 仍被驱动拒绝。 */
    if (!fatal && rank == 1 && sh2 != 0) {
        ret = aclrtSetDevice(devA);   /* ctx 技巧: 借导出方 ctx */
        if (ret == ACL_ERROR_NONE) {
            aclrtDrvMemHandle h2 = NULL;
            r1_setA = 1;
            m2_ret = aclrtMemImportFromShareableHandle(sh2, devB, &h2);
            if (m2_ret == ACL_ERROR_NONE) {
                m2_kind = KIND_UNEXPECTED;
                aclrtFreePhysical(h2);
                printf("[10] step 3 (r1): %-58s 意外成功 ret=0 <- 推翻预期! "
                       "✗ 单元格 ctx 技巧 import 竟可用?(如实记录; 仅观测 import, 不建窗口)\n",
                       "method2 ctx-trick: SetDevice(A)+Import(sh2,B) — expect 507899");
            } else if (m2_ret == EXPECTED_ERR) {
                m2_kind = KIND_EXPECTED;
                printf("[10] step 3 (r1): %-58s EXPECTED-FAIL ret=%d\n",
                       "method2 ctx-trick: SetDevice(A)+Import(sh2,B) — expect 507899", m2_ret);
            } else {
                m2_kind = KIND_OTHER_ERR;
                printf("[10] step 3 (r1): %-58s EXPECTED-FAIL ret=%d  [注意: 非"
                       "预期 507899, 如实记录]\n",
                       "method2 ctx-trick: SetDevice(A)+Import(sh2,B) — expect 507899", m2_ret);
            }
            fflush(stdout);
        } else {
            printf("[10] step 3 (r1): %-58s SKIP (SetDevice(A) FAIL ret=%d)\n",
                   "method2 ctx-trick: SetDevice(A)+Import(sh2,B) — expect 507899", ret);
            fflush(stdout);
        }
        /* 测完切回本进程 ctx(B) */
        ret = aclrtSetDevice(devB);
        obs_line(3, who, "SetDevice(B_dev) restore ctx", ret == ACL_ERROR_NONE, ret);
    }
    sync_state(&fatal);

    /* ---- cleanup ----
     * importer: 两种 import 都预期失败, 无 imported VA 可 unmap; 仅拆 P2P + reset。
     * exporter: barrier 后释放两块物理内存(同序: Unmap -> Free -> Release)。 */
    MPI_Barrier(MPI_COMM_WORLD);   /* importer 确认不再访问后 exporter 才 free 物理内存 */
    if (rank == 0) {
        if (own_mapped) {
            ret = aclrtUnmapMem(ownVA);
            printf("[10] cleanup (r0): UnmapMem(main VA) ret=%d\n", ret);
        }
        if (h_own) {
            ret = aclrtFreePhysical(h_own);
            printf("[10] cleanup (r0): FreePhysical(main) ret=%d\n", ret);
        }
        if (own_reserved) {
            ret = aclrtReleaseMemAddress(ownVA);
            printf("[10] cleanup (r0): ReleaseMemAddress(main VA) ret=%d\n", ret);
        }
        if (ctrl_mapped) {
            ret = aclrtUnmapMem(ctrlVA);
            printf("[10] cleanup (r0): UnmapMem(ctrl VA) ret=%d\n", ret);
        }
        if (h_ctrl) {
            ret = aclrtFreePhysical(h_ctrl);
            printf("[10] cleanup (r0): FreePhysical(ctrl) ret=%d\n", ret);
        }
        if (ctrl_reserved) {
            ret = aclrtReleaseMemAddress(ctrlVA);
            printf("[10] cleanup (r0): ReleaseMemAddress(ctrl VA) ret=%d\n", ret);
        }
    }
    free(hbuf);
    fflush(stdout);
    /* ★跨卡 P2P 状态必须在 ResetDevice 前显式拆除(防 D 态挂死, 详见 03/04 README)。
     *   r0(disable devB)+ r1(disable devA)对称。 */
    if (dev_ok) {
        int32_t peer = (rank == 0) ? devB : devA;
        aclError dr = aclrtDeviceDisablePeerAccess(peer);
        printf("[10] cleanup (r%d): DisablePeerAccess(%d) ret=%d\n", rank, peer, dr);
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
        res[1] = m1_kind;
        res[2] = m2_kind;
        res[3] = m1_ret;
        res[4] = m2_ret;
        MPI_Allreduce(MPI_IN_PLACE, res, 5, MPI_INT, MPI_MAX, MPI_COMM_WORLD);
        g_fail = res[0];
        m1_kind = res[1];
        m2_kind = res[2];
        m1_ret = res[3];
        m2_ret = res[4];

        printf("[10] RESULT: %s", g_fail ? "FAIL" : "PASS");
        if (!g_fail) {
            /* 方式一(本进程 ctx) */
            printf("  ★方式一本进程ctx: ");
            if (m1_kind == KIND_EXPECTED)
                printf("EXPECTED-FAIL ret=%d", m1_ret);
            else if (m1_kind == KIND_OTHER_ERR)
                printf("OTHER-ERR ret=%d", m1_ret);
            else if (m1_kind == KIND_UNEXPECTED)
                printf("意外成功(ret=0)!推翻预期");
            else
                printf("SKIPPED");
            /* 方式二(ctx 技巧) */
            printf("; 方式二ctx技巧: ");
            if (m2_kind == KIND_EXPECTED)
                printf("EXPECTED-FAIL ret=%d", m2_ret);
            else if (m2_kind == KIND_OTHER_ERR)
                printf("OTHER-ERR ret=%d", m2_ret);
            else if (m2_kind == KIND_UNEXPECTED)
                printf("意外成功(ret=0)!推翻预期");
            else
                printf("SKIPPED");
            if (m1_kind == KIND_UNEXPECTED || m2_kind == KIND_UNEXPECTED)
                printf("  ⚠⚠⚠ 推翻预期: 出现意外成功的 import!");
        }
        printf("\n");
        fflush(stdout);
    }

    MPI_Finalize();
    return g_fail ? 1 : 0;
}
