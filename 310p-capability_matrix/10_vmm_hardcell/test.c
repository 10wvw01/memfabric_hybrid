/*
 * 10_vmm_hardcell — VMM 跨卡 import 验证
 * 机器: Atlas 300I Duo (310P3, 4 卡 x 2 die = 8 dev), CANN 9.1.0, aarch64
 * 结构: MPI 双进程 rank0=exporter(dev A, 环境变量 A_DEV, 默认 1)
 *                     rank1=importer(dev B, 环境变量 B_DEV, 默认 4)
 *
 * 实测结论(CANN 9.1.0):
 *   - 跨卡对(如 dev1→dev4): 方式一/二/三均成功(ret=0)。
 *   - 同卡跨 die 对(如 dev0→dev1, 两 die 共享一个 PCIe 设备): 方式一 FAIL 507899,
 *     方式二/三成功。原因(源码验证): import 内部用当前 ctx 设备调
 *     EnableP2PWithOtherDevice(api_impl.cc:7338), 同卡 die 对的 P2P 被内核
 *     devdrv_p2p_para_check 显式拒绝(devdrv_ctrl.c:4482 same-pdev 检查 +
 *     :4424 "2DIe : 1PCIe 2PF" 检查, 返回 -EINVAL→507899), import 在自动
 *     P2P 步骤夭折, import ioctl 根本没发出。ctx 落在导出方设备时
 *     raw_device.cc:2206 phyId==peerPhyId 直接跳过 P2P, import ioctl 畅通
 *     (内核 import 路径无同卡限制, svm_master_mem_share.c:1135)。
 *   - IPC-PEER 链路一直可用(见 03)。
 *
 * 本测试验证三种 VMM import 方式的可达性,记录实际观测结果。
 *
 * 校验 pattern: value = (rank<<28)|(tag<<24)|(idx&0xFFFFFF), tag 0x2/0x5/0x7
 * 每步不同,防残留数据假阳性(本测试虽不回读,仍写入以保持链路真实性与链上测试一致)。
 *
 * 测试项:
 *   1  exporter: EnablePeerAccess(B)+ MallocPhysical 16MB
 *      (七级回退; 2MB gran 对齐; MapMem size == MallocPhysical size)+ Reserve+Map+
 *      SetAccess(RW,A)+ H2D pattern + Export sh1(DISABLE_PID) + MPI 发 sh1(u64)
 *   2  importer 方式一(本进程 ctx): EnablePeerAccess(A,0)+
 *      aclrtMemImportFromShareableHandle(sh1, B, &h)(ctx=B,本进程)-> 记录结果
 *   3  exporter 第二块独立 16MB 物理内存 export sh2(规避同 sh 二次 import 干扰);
 *      importer 方式二(ctx 技巧): SetDevice(A)(借导出方 primary ctx)->
 *      aclrtMemImportFromShareableHandle(sh2, B, &h2)(ctx=A=导出方, devId=B)->
 *      记录结果; 测完 SetDevice(B) 切回本进程 ctx
 *   4  exporter 第三块独立 16MB 物理内存 export sh3; importer 方式三(替代方式,
 *      不用 SetDevice): aclrtCreateContext(&ctx, A) 显式建导出方 ctx +
 *      aclrtSetCurrentContext(ctx) 切换 + Import(sh3, B, &h3) + SetDevice(B)
 *      切回 + DestroyContext。与方式二等效(ctx 落在导出方→跳过 P2P), 但 ctx
 *      生命周期显式可控, 不 lazy 创建 primary ctx。
 *
 * 判定: exporter 正向 1/3/5 全 OK(import 链路建得起来)+ 三种 import 方式都有结果
 *       (成功 / OTHER_ERR / FAIL 均为有效观测) -> PASS。
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
#define ALT_SIZE     (16u * 1024u * 1024u)  /* 16 MB 第三块(方式三 sh3) */

#define TAG_EXP    0x2  /* step1: exporter 写入(链路真实性) */
#define TAG_CTRL   0x5  /* step3: exporter 写入对照块 */
#define TAG_ALT    0x7  /* step4: exporter 写入方式三块 */

/* import 结果分类(RESULT 行注释用) */
#define KIND_NONE        (-1)  /* 未执行 */
#define KIND_SUCCESS     0     /* 成功 */
#define KIND_OTHER_ERR   1     /* 错误 */

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
    printf("[10] === 10_vmm_hardcell: VMM 跨卡 import 验证 (两种 import 方式), "
           "rank%d %s dev%d ===\n", rank, role, dev);
    printf("[10] %s: A_DEV=%d B_DEV=%d, 16MB x3 (sh1/sh2/sh3), 7-attr fallback, "
           "pattern=(rank<<28)|(tag<<24)|(idx&0xFFFFFF)\n", role, devA, devB);

    /* r0: 三块自有 VMM 物理内存(16MB sh1 + 16MB sh2 + 16MB sh3) */
    aclrtDrvMemHandle h_own = NULL, h_ctrl = NULL, h_alt = NULL;
    void *ownVA = NULL, *ctrlVA = NULL, *altVA = NULL;
    int own_reserved = 0, own_mapped = 0, ctrl_reserved = 0, ctrl_mapped = 0;
    int alt_reserved = 0, alt_mapped = 0;
    int own_attr = -1, ctrl_attr = -1, alt_attr = -1;
    uint64_t sh1 = 0, sh2 = 0, sh3 = 0;
    uint32_t *hbuf = NULL;
    /* r1: 三种 import 结果 */
    int m1_kind = KIND_NONE, m2_kind = KIND_NONE, m3_kind = KIND_NONE;
    int32_t m1_ret = -1, m2_ret = -1, m3_ret = -1;
    int r1_setA = 0;                /* r1 是否在本进程激活过 A 侧 ctx(SetDevice/CreateContext) */
    int fatal = 0;
    int32_t ret = 0;
    int init_ok = 0, dev_ok = 0;
    size_t words = VMM_SIZE / sizeof(uint32_t);
    size_t cw = CTRL_SIZE / sizeof(uint32_t);
    size_t aw = ALT_SIZE / sizeof(uint32_t);

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

    /* ---- phase 1 (step 1): exporter EnablePeerAccess(B,0)
     *     + MallocPhysical(七级回退) + Reserve + Map + SetAccess + H2D pattern
     *     + Export sh1 + MPI 发 sh1 ---- */
    if (!fatal && rank == 0) {
        int ok = 0;
        int32_t ena = aclrtDeviceEnablePeerAccess(devB, 0);
        obs_line(1, who, "EnablePeerAccess(B_dev,0)",
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

    /* ---- phase 2 (step 2): importer 方式一(本进程 ctx) import ----
     * EnablePeerAccess(A) 打通 P2P 通路, VMM import 在本进程 ctx(B)下尝试。 */
    if (!fatal && rank == 1 && sh1 != 0) {
        int32_t ena = aclrtDeviceEnablePeerAccess(devA, 0);
        obs_line(2, who, "EnablePeerAccess(A_dev,0)",
                 ena == ACL_ERROR_NONE, ena);
        aclrtDrvMemHandle h = NULL;
        m1_ret = aclrtMemImportFromShareableHandle(sh1, devB, &h);
        if (m1_ret == ACL_ERROR_NONE) {
            m1_kind = KIND_SUCCESS;
            aclrtFreePhysical(h);
            printf("[10] step 2 (r1): %-58s OK ret=0\n",
                   "method1 own-ctx: Import(sh1,B)");
        } else {
            m1_kind = KIND_OTHER_ERR;
            printf("[10] step 2 (r1): %-58s FAIL ret=%d\n",
                   "method1 own-ctx: Import(sh1,B)", m1_ret);
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

    /* ---- phase 3b (step 3, 方式二 ctx 技巧): importer SetDevice(A) + Import(sh2,B) ----
     * 独立 sh2 规避"同一 sh 同进程二次 import"干扰, 观测纯粹的 ctx 技巧行为。 */
    if (!fatal && rank == 1 && sh2 != 0) {
        ret = aclrtSetDevice(devA);   /* ctx 技巧: 借导出方 ctx */
        if (ret == ACL_ERROR_NONE) {
            aclrtDrvMemHandle h2 = NULL;
            r1_setA = 1;
            m2_ret = aclrtMemImportFromShareableHandle(sh2, devB, &h2);
            if (m2_ret == ACL_ERROR_NONE) {
                m2_kind = KIND_SUCCESS;
                aclrtFreePhysical(h2);
                printf("[10] step 3 (r1): %-58s OK ret=0\n",
                       "method2 ctx-trick: SetDevice(A)+Import(sh2,B)");
            } else {
                m2_kind = KIND_OTHER_ERR;
                printf("[10] step 3 (r1): %-58s FAIL ret=%d\n",
                       "method2 ctx-trick: SetDevice(A)+Import(sh2,B)", m2_ret);
            }
            fflush(stdout);
        } else {
            printf("[10] step 3 (r1): %-58s SKIP (SetDevice(A) FAIL ret=%d)\n",
                   "method2 ctx-trick: SetDevice(A)+Import(sh2,B)", ret);
            fflush(stdout);
        }
        /* 测完切回本进程 ctx(B) */
        ret = aclrtSetDevice(devB);
        obs_line(3, who, "SetDevice(B_dev) restore ctx", ret == ACL_ERROR_NONE, ret);
    }
    sync_state(&fatal);

    /* ---- phase 4a (step 4): exporter 第三块独立 16MB + Export sh3 ---- */
    if (!fatal && rank == 0) {
        int ok = 0;
        ret = malloc_physical_chain(devA, ALT_SIZE, &h_alt, "alt", &alt_attr);
        if (ret == ACL_ERROR_NONE)
            ret = aclrtReserveMemAddress(&altVA, ALT_SIZE, 0, NULL, 0);
        if (ret == ACL_ERROR_NONE) {
            alt_reserved = 1;
            ret = aclrtMapMem(altVA, ALT_SIZE, 0, h_alt, 0);
        }
        if (ret == ACL_ERROR_NONE) {
            alt_mapped = 1;
            int32_t sa = set_access_rw(altVA, ALT_SIZE, devA);
            obs_line(4, who, "SetAccess(RW, A_dev) on alt VA", sa == ACL_ERROR_NONE, sa);
            fill_pat(hbuf, aw, 0, TAG_ALT);
            ret = aclrtMemcpy(altVA, ALT_SIZE, hbuf, ALT_SIZE,
                              ACL_MEMCPY_HOST_TO_DEVICE);
        }
        if (ret == ACL_ERROR_NONE)
            ret = aclrtMemExportToShareableHandle(
                h_alt, ACL_MEM_HANDLE_TYPE_NONE,
                ACL_RT_VMM_EXPORT_FLAG_DISABLE_PID_VALIDATION, &sh3);
        if (ret == ACL_ERROR_NONE && sh3 != 0) {
            if (MPI_Send(&sh3, 1, MPI_UINT64_T, 1, 102, MPI_COMM_WORLD) == MPI_SUCCESS)
                ok = 1;
        }
        if (!ok) {
            uint64_t bad = 0;
            MPI_Send(&bad, 1, MPI_UINT64_T, 1, 102, MPI_COMM_WORLD);
            printf("[10] step 4 (r0): alt chain FAILED before send "
                   "(chosen attr idx=%d, sh3=0x%llx)\n", alt_attr,
                   (unsigned long long)sh3);
        } else {
            printf("[10] step 4 (r0): sh3=0x%llx sent\n",
                   (unsigned long long)sh3);
        }
        step_line(4, who, "MallocPhysical 16MB #3 + H2D pattern + Export sh3 + MPI-send",
                  ok, ret);
        if (!ok)
            BAIL(fatal);
    } else if (!fatal && rank == 1) {
        MPI_Recv(&sh3, 1, MPI_UINT64_T, 0, 102, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        if (sh3 == 0)
            printf("[10] step 4 (r1): received sh3=0 (exporter alt block failed)\n");
    }
    sync_state(&fatal);

    /* ---- phase 4b (step 4, 方式三替代方式): importer CreateContext(A)+
     *     SetCurrentContext + Import(sh3,B), 不用 SetDevice ----
     * 与方式二等效: ctx 落在导出方设备 → runtime 跳过对同卡 die 对必然失败的
     * 自动 P2P 启用。区别: 显式 ctx 生命周期可控(Create/Destroy), 不 lazy
     * 创建 A 的 primary ctx。独立 sh3 规避同 sh 二次 import 干扰。 */
    if (!fatal && rank == 1 && sh3 != 0) {
        aclrtContext expCtx = NULL;
        ret = aclrtCreateContext(&expCtx, devA);
        if (ret == ACL_ERROR_NONE)
            ret = aclrtSetCurrentContext(expCtx);
        if (ret == ACL_ERROR_NONE) {
            aclrtDrvMemHandle h3 = NULL;
            r1_setA = 1;   /* CreateContext 同样在本进程 retain 了 A 侧设备 */
            m3_ret = aclrtMemImportFromShareableHandle(sh3, devB, &h3);
            if (m3_ret == ACL_ERROR_NONE) {
                m3_kind = KIND_SUCCESS;
                aclrtFreePhysical(h3);
                printf("[10] step 4 (r1): %-58s OK ret=0\n",
                       "method3 alt: CreateCtx(A)+SetCurCtx+Import(sh3,B)");
            } else {
                m3_kind = KIND_OTHER_ERR;
                printf("[10] step 4 (r1): %-58s FAIL ret=%d\n",
                       "method3 alt: CreateCtx(A)+SetCurCtx+Import(sh3,B)", m3_ret);
            }
            fflush(stdout);
        } else {
            printf("[10] step 4 (r1): %-58s SKIP (CreateCtx(A)/SetCurCtx FAIL ret=%d)\n",
                   "method3 alt: CreateCtx(A)+SetCurCtx+Import(sh3,B)", ret);
            fflush(stdout);
        }
        /* 测完 SetDevice(B) 切回本进程 primary ctx(同时清除显式 ctx 绑定) */
        ret = aclrtSetDevice(devB);
        obs_line(4, who, "SetDevice(B_dev) restore ctx", ret == ACL_ERROR_NONE, ret);
        if (expCtx != NULL) {
            int32_t dr = aclrtDestroyContext(expCtx);
            printf("[10] step 4 (r1): DestroyContext(expCtx A) ret=%d\n", dr);
            fflush(stdout);
        }
    }
    sync_state(&fatal);

    /* ---- cleanup ----
     * importer: 仅观测 import, 无 imported VA 可 unmap; 仅拆 P2P + reset。
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
        if (alt_mapped) {
            ret = aclrtUnmapMem(altVA);
            printf("[10] cleanup (r0): UnmapMem(alt VA) ret=%d\n", ret);
        }
        if (h_alt) {
            ret = aclrtFreePhysical(h_alt);
            printf("[10] cleanup (r0): FreePhysical(alt) ret=%d\n", ret);
        }
        if (alt_reserved) {
            ret = aclrtReleaseMemAddress(altVA);
            printf("[10] cleanup (r0): ReleaseMemAddress(alt VA) ret=%d\n", ret);
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
        aclrtResetDevice(devA);   /* 方式二/三在本进程开过的 A 侧 context */
    if (init_ok)
        aclFinalize();

    /* ---- RESULT(聚合两 rank 状态; 两 rank 打印同一行) ---- */
    {
        int res[7];
        res[0] = g_fail;
        res[1] = m1_kind;
        res[2] = m2_kind;
        res[3] = m3_kind;
        res[4] = m1_ret;
        res[5] = m2_ret;
        res[6] = m3_ret;
        MPI_Allreduce(MPI_IN_PLACE, res, 7, MPI_INT, MPI_MAX, MPI_COMM_WORLD);
        g_fail = res[0];
        m1_kind = res[1];
        m2_kind = res[2];
        m3_kind = res[3];
        m1_ret = res[4];
        m2_ret = res[5];
        m3_ret = res[6];

        printf("[10] RESULT: %s", g_fail ? "FAIL" : "PASS");
        if (!g_fail) {
            printf("  方式一本进程ctx: ");
            if (m1_kind == KIND_SUCCESS)
                printf("OK ret=%d", m1_ret);
            else if (m1_kind == KIND_OTHER_ERR)
                printf("FAIL ret=%d", m1_ret);
            else
                printf("SKIPPED");
            printf("; 方式二ctx技巧: ");
            if (m2_kind == KIND_SUCCESS)
                printf("OK ret=%d", m2_ret);
            else if (m2_kind == KIND_OTHER_ERR)
                printf("FAIL ret=%d", m2_ret);
            else
                printf("SKIPPED");
            printf("; 方式三CreateCtx替代: ");
            if (m3_kind == KIND_SUCCESS)
                printf("OK ret=%d", m3_ret);
            else if (m3_kind == KIND_OTHER_ERR)
                printf("FAIL ret=%d", m3_ret);
            else
                printf("SKIPPED");
        }
        printf("\n");
        fflush(stdout);
    }

    MPI_Finalize();
    return g_fail ? 1 : 0;
}
