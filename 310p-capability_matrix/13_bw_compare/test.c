/*
 * 13_bw_compare — 同链路 IPC vs VMM vs host-swap 带宽对比
 * 机器: Atlas 300I Duo (310P3, 4 卡 x 2 die = 8 dev), CANN 9.1.0, aarch64
 * 结构: MPI 双进程 rank0=exporter(dev A, A_DEV 默认 0)
 *                     rank1=importer(dev B, B_DEV 默认 1, die 对)
 *   A_DEV/B_DEV 参数化: die 对(0,1)/(2,3)... 或跨卡(0,2)/(0,4)...
 *
 * 验证: 同一设备对上, 三种传输后端的带宽对比(统一口径, 公平比较)。
 *
 * 拓扑判定: is_die_pair = (devA != devB) && (devA/2 == devB/2)
 *   die 对: IPC DEFAULT import / VMM ctx 技巧(SetDevice(A)→Import→SetDevice(B))
 *   跨卡:   IPC PEER import + EnablePeerAccess / VMM 正常路径(EnablePeerAccess→Import)
 *
 * 校验 pattern: value = (rank<<28)|(tag<<24)|(idx&0xFFFFFF)
 *   IPC 块 tag=0x2, VMM 块 tag=0x3, 每步不同 tag 防残留假阳性。
 *
 * ★带宽测量铁律: 严禁同步循环测带宽!
 *   历史教训(02/03): 失败链路上同步调用只量到失败返回延迟, 伪造假带宽。
 *   正确口径: 3 次预热 + 30 次 16MB MemcpyAsync 提交 + 末尾 1 次 StreamSync。
 *   host-swap 口径: 30 次 (D2H async + H2D async) + 1 Sync, 两步合计计时,
 *   有效带宽 = 30×16MB / 时间(搬运 16MB 远端→本地, 经 host 中转)。
 *
 * 测试项(编号即测试项):
 *   1  IPC DEFAULT 后端: exporter malloc(P2P,16MB) + H2D pattern + export key
 *      (DISABLE_PID) + send; importer import + async D2D 读(imported→local) + 校验
 *   2  VMM ctx 技巧后端(仅 die 对; 跨卡用正常 VMM 路径):
 *      exporter MallocPhysical(P2P_HUGE,16MB) + Reserve+Map+SetAccess + H2D pattern
 *      + Export(DISABLE_PID) + send sh;
 *      importer [die 对: SetDevice(A)+Import(sh,B)+SetDevice(B)+Reserve+Map+SetAccess(B);
 *                跨卡: EnablePeerAccess(A)+Import(sh,B)+Reserve+Map+SetAccess(B)]
 *      + async D2D 读 + 校验
 *   3  host-swap 后端: 用 IPC imported VA → D2H(imported→host_buf) + H2D(host_buf→local)
 *      + 校验(host 中转两步)
 *   4  三种路径各跑带宽: 3 预热 + 30×16MB async 提交 + 1 Sync;
 *      host-swap 口径 = 30×(D2H async + H2D async) + 1 Sync
 *   5  打印对比表
 *   6  cleanup: IPC IpcMemClose + aclrtFree;
 *      VMM UnmapMem→FreePhysical→ReleaseMemAddress;
 *      DisablePeerAccess(跨卡时); ResetDevice(A)(die 对 VMM ctx 技巧时) +
 *      ResetDevice(B) → Finalize
 *
 * 判定: 三后端各自 setup+校验 OK → PASS; 任一后端 setup 或校验失败 → FAIL。
 *       带宽数值如实报告(含 setup 失败的 N/A)。
 *
 * D 态修复: 跨卡 EnablePeerAccess 的 rank 在 cleanup 时 DisablePeerAccess(peer)
 *   before ResetDevice, 否则进程退出时 mmu notifier 释放与驱动 SRCU 死锁
 *   (exit_mmap → synchronize_srcu 永久等待, SIGKILL 不可回收, 耗尽 hugepages)。
 *   根因见 0824-test/README.md: 02(die 对无 EnablePeer)正常 / 03(跨卡 EnablePeer)挂死。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <mpi.h>
#include "acl/acl.h"

#define BLOCK_SIZE  (16u * 1024u * 1024u)   /* 16 MB 统一块大小 */
#define BW_WARMUP   3
#define BW_ITERS    30
#define IPC_KEY_LEN 65

#define TAG_IPC  0x2   /* step1: IPC 块 pattern tag */
#define TAG_VMM  0x3   /* step2: VMM 块 pattern tag */

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
            printf("[13]   [verify] mismatch at word %zu (off 0x%zx): got 0x%08x, want 0x%08x\n",
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
    printf("[13] step %d (%s): %-58s %s (ret=%d)\n", n, who, desc,
           ok ? "OK" : "FAIL", ret);
    fflush(stdout);
    if (!ok)
        g_fail = 1;
}

/* 阶段栅栏 + fatal/g_fail 全局同步 */
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

/* memAttr 七级回退 MallocPhysical: 依次尝试, 首个成功即用 */
static int32_t malloc_physical_chain(int32_t devId, size_t size,
                                      aclrtDrvMemHandle *handle, const char *label)
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
        aclError m = aclrtMallocPhysical(handle, size, &prop, 0);
        printf("[13]   [%s attr %d/7 %-14s] MallocPhysical(%uMB) ret=%d\n",
               label, i + 1, g_chainName[i], (unsigned)(size >> 20), m);
        fflush(stdout);
        if (m == ACL_ERROR_NONE)
            return 0;
        last = m;
        *handle = NULL;
    }
    printf("[13]   [%s] all 7 attrs FAILED (last ret=%d)\n", label, last);
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
            printf("[13] FATAL: need exactly 2 MPI procs (-np 2), got %d\n", nproc);
        MPI_Finalize();
        return 1;
    }

    int32_t devA = env_dev("A_DEV", 0);
    int32_t devB = env_dev("B_DEV", 1);
    int32_t dev = (rank == 0) ? devA : devB;
    int is_die_pair = (devA != devB) && (devA / 2 == devB / 2);
    int is_cross = !is_die_pair && (devA != devB);

    const char *role = (rank == 0) ? "r0/exporter" : "r1/importer";
    const char *who = (rank == 0) ? "r0" : "r1";
    const char *topo = is_die_pair ? "die-pair" : (is_cross ? "cross-card" : "same-dev");

    /* 动态标签: die 对用 ctx 技巧, 跨卡用正常 P2P 路径 */
    const char *ipc_label = is_die_pair ? "IPC DEFAULT" : (is_cross ? "IPC PEER  " : "IPC DEFAULT");
    const char *vmm_label = is_die_pair ? "VMM ctx    " : (is_cross ? "VMM P2P    " : "VMM ctx    ");

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("[13] === 13_bw_compare: rank%d %s dev%d, %s (A_DEV=%d B_DEV=%d) ===\n",
           rank, role, dev, topo, devA, devB);
    printf("[13] %s: 3 backends x 16MB, 30x async + 1 Sync, "
           "pattern=(rank<<28)|(tag<<24)|(idx&0xFFFFFF)\n", role);

    aclrtStream stream = NULL;
    /* IPC 后端 */
    void *ipc_da = NULL;                /* r0: IPC 原始设备内存 */
    void *ipc_va = NULL;                /* r1: IPC imported VA (host-swap 复用) */
    char ipc_key[IPC_KEY_LEN] = {0};
    int ipc_ok = 0, ipc_verify = 0;
    double ipc_bw = 0;
    /* VMM 后端 */
    aclrtDrvMemHandle vmm_handle = NULL, vmm_imp_handle = NULL;
    void *vmm_own_va = NULL, *vmm_va = NULL;
    int own_reserved = 0, own_mapped = 0;
    int vmm_reserved = 0, vmm_mapped = 0;
    int vmm_ok = 0, vmm_verify = 0;
    double vmm_bw = 0;
    uint64_t sh = 0;
    /* host-swap 后端 */
    int swap_verify = 0;
    double swap_bw = 0;
    /* 公共 */
    void *local = NULL;                 /* r1: 本地 16MB 缓冲 */
    uint32_t *hbuf = NULL, *vbuf = NULL, *host_buf = NULL;
    int r1_setA = 0;                    /* r1 ctx 技巧是否 SetDevice(A) 过 */
    int fatal = 0;
    int32_t ret = 0;
    int init_ok = 0, dev_ok = 0;
    size_t words = BLOCK_SIZE / sizeof(uint32_t);

    /* ---- phase 0: init ---- */
    ret = aclInit(NULL);
    if (ret != ACL_ERROR_NONE) {
        printf("[13] init: %s aclInit FAIL ret=%d\n", role, ret);
        BAIL(fatal);
    } else {
        init_ok = 1;
        ret = aclrtSetDevice(dev);
        if (ret != ACL_ERROR_NONE) {
            printf("[13] init: %s aclrtSetDevice(%d) FAIL ret=%d\n", role, dev, ret);
            BAIL(fatal);
        }
    }
    if (!fatal) {
        dev_ok = 1;
        if (rank == 1) {
            ret = aclrtCreateStream(&stream);
            if (ret != ACL_ERROR_NONE) {
                printf("[13] init: r1 aclrtCreateStream FAIL ret=%d\n", ret);
                BAIL(fatal);
            }
            ret = aclrtMalloc(&local, BLOCK_SIZE, ACL_MEM_MALLOC_HUGE_FIRST);
            if (ret != ACL_ERROR_NONE) {
                printf("[13] init: r1 aclrtMalloc local 16MB FAIL ret=%d\n", ret);
                BAIL(fatal);
            }
            host_buf = (uint32_t *)malloc(BLOCK_SIZE);
            if (!host_buf) {
                printf("[13] init: r1 host malloc host_buf 16MB FAIL\n");
                BAIL(fatal);
            }
        }
        hbuf = (uint32_t *)malloc(BLOCK_SIZE);
        vbuf = (uint32_t *)malloc(BLOCK_SIZE);
        if (!hbuf || !vbuf) {
            printf("[13] init: %s host malloc 2x16MB FAIL\n", role);
            BAIL(fatal);
        }
    }
    if (!fatal)
        printf("[13] init: %s aclInit/SetDevice(%d)%s OK\n", role, dev,
               (rank == 1) ? "/CreateStream/MallocLocal" : "");
    sync_state(&fatal);

    /* ================================================================
     * step 1: IPC DEFAULT 后端
     * ================================================================ */
    if (!fatal && rank == 0) {
        int ok = 0;
        ret = aclrtMalloc(&ipc_da, BLOCK_SIZE, ACL_MEM_MALLOC_HUGE_FIRST_P2P);
        if (ret == ACL_ERROR_NONE) {
            fill_pat(hbuf, words, 0, TAG_IPC);
            ret = aclrtMemcpy(ipc_da, BLOCK_SIZE, hbuf, BLOCK_SIZE,
                              ACL_MEMCPY_HOST_TO_DEVICE);
        }
        if (ret == ACL_ERROR_NONE)
            ret = aclrtIpcMemGetExportKey(ipc_da, BLOCK_SIZE, ipc_key, IPC_KEY_LEN,
                                          ACL_RT_IPC_MEM_EXPORT_FLAG_DISABLE_PID_VALIDATION);
        if (ret == ACL_ERROR_NONE) {
            if (MPI_Send(ipc_key, IPC_KEY_LEN, MPI_CHAR, 1, 100, MPI_COMM_WORLD) == MPI_SUCCESS)
                ok = 1;
        }
        step_line(1, who, "IPC: malloc P2P 16MB + H2D pattern + export key + send", ok, ret);
        if (!ok)
            BAIL(fatal);
    } else if (!fatal && rank == 1) {
        MPI_Recv(ipc_key, IPC_KEY_LEN, MPI_CHAR, 0, 100, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    }
    sync_state(&fatal);

    if (!fatal && rank == 1) {
        int ok = 0;
        uint64_t flag = is_die_pair ? ACL_RT_IPC_MEM_IMPORT_FLAG_DEFAULT
                                    : ACL_RT_IPC_MEM_IMPORT_FLAG_ENABLE_PEER_ACCESS;
        ret = aclrtIpcMemImportByKey(&ipc_va, ipc_key, flag);
        if (ret == ACL_ERROR_NONE) {
            ipc_ok = 1;
            ret = aclrtMemcpyAsync(local, BLOCK_SIZE, ipc_va, BLOCK_SIZE,
                                    ACL_MEMCPY_DEVICE_TO_DEVICE, stream);
        }
        if (ret == ACL_ERROR_NONE)
            ret = aclrtSynchronizeStream(stream);
        if (ret == ACL_ERROR_NONE) {
            memset(vbuf, 0, BLOCK_SIZE);
            ret = aclrtMemcpy(vbuf, BLOCK_SIZE, local, BLOCK_SIZE,
                              ACL_MEMCPY_DEVICE_TO_HOST);
            if (ret == ACL_ERROR_NONE) {
                ok = (check_pat(vbuf, words, 0, TAG_IPC) == 0);
                ipc_verify = ok ? 1 : 0;
            }
        }
        step_line(1, who, "IPC: import + async D2D read + verify", ok, ret);
        if (!ok)
            BAIL(fatal);
    }
    sync_state(&fatal);

    /* ================================================================
     * step 2: VMM ctx 技巧后端(die 对; 跨卡用正常 VMM 路径)
     * ================================================================ */
    if (!fatal && rank == 0) {
        int ok = 0;
        /* 跨卡: export 前先开 B 的双向 P2P */
        if (is_cross) {
            int32_t ena = aclrtDeviceEnablePeerAccess(devB, 0);
            printf("[13]   [r0] EnablePeerAccess(B=%d) before VMM export ret=%d\n", devB, ena);
            fflush(stdout);
        }
        ret = malloc_physical_chain(devA, BLOCK_SIZE, &vmm_handle, "vmm");
        if (ret == ACL_ERROR_NONE)
            ret = aclrtReserveMemAddress(&vmm_own_va, BLOCK_SIZE, 0, NULL, 0);
        if (ret == ACL_ERROR_NONE) {
            own_reserved = 1;
            ret = aclrtMapMem(vmm_own_va, BLOCK_SIZE, 0, vmm_handle, 0);
        }
        if (ret == ACL_ERROR_NONE) {
            own_mapped = 1;
            int32_t sa = set_access_rw(vmm_own_va, BLOCK_SIZE, devA);
            printf("[13]   [r0] SetAccess(RW, A=%d) on own VA ret=%d\n", devA, sa);
            fflush(stdout);
            fill_pat(hbuf, words, 0, TAG_VMM);
            ret = aclrtMemcpy(vmm_own_va, BLOCK_SIZE, hbuf, BLOCK_SIZE,
                              ACL_MEMCPY_HOST_TO_DEVICE);
        }
        if (ret == ACL_ERROR_NONE)
            ret = aclrtMemExportToShareableHandle(vmm_handle, ACL_MEM_HANDLE_TYPE_NONE,
                                                   ACL_RT_VMM_EXPORT_FLAG_DISABLE_PID_VALIDATION, &sh);
        if (ret == ACL_ERROR_NONE && sh != 0) {
            if (MPI_Send(&sh, 1, MPI_UINT64_T, 1, 101, MPI_COMM_WORLD) == MPI_SUCCESS)
                ok = 1;
        }
        if (!ok) {
            uint64_t bad = 0;
            MPI_Send(&bad, 1, MPI_UINT64_T, 1, 101, MPI_COMM_WORLD);
        }
        step_line(2, who, "VMM: MallocPhysical+Map+H2D+Export sh+send", ok, ret);
        if (!ok)
            BAIL(fatal);
    } else if (!fatal && rank == 1) {
        MPI_Recv(&sh, 1, MPI_UINT64_T, 0, 101, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        if (sh == 0)
            printf("[13] step 2 (r1): received sh=0 (exporter VMM failed)\n");
    }
    sync_state(&fatal);

    if (!fatal && rank == 1 && sh != 0) {
        int ok = 0;
        if (is_die_pair) {
            /* die 对: ctx 技巧 SetDevice(A) → Import(sh, B) → SetDevice(B) */
            ret = aclrtSetDevice(devA);
            if (ret == ACL_ERROR_NONE) {
                r1_setA = 1;
                ret = aclrtMemImportFromShareableHandle(sh, devB, &vmm_imp_handle);
                int32_t rb = aclrtSetDevice(devB);
                printf("[13]   [r1] ctx trick: SetDevice(A=%d)+Import(sh,B=%d)+SetDevice(B=%d) "
                       "imp ret=%d, restore ret=%d\n", devA, devB, devB, ret, rb);
                fflush(stdout);
            }
        } else {
            /* 跨卡: 正常 VMM 路径 EnablePeerAccess(A) → Import(sh, B) */
            int32_t ena = aclrtDeviceEnablePeerAccess(devA, 0);
            printf("[13]   [r1] EnablePeerAccess(A=%d) before VMM import ret=%d\n", devA, ena);
            fflush(stdout);
            ret = aclrtMemImportFromShareableHandle(sh, devB, &vmm_imp_handle);
        }
        if (ret == ACL_ERROR_NONE)
            ret = aclrtReserveMemAddress(&vmm_va, BLOCK_SIZE, 0, NULL, 0);
        if (ret == ACL_ERROR_NONE) {
            vmm_reserved = 1;
            ret = aclrtMapMem(vmm_va, BLOCK_SIZE, 0, vmm_imp_handle, 0);
        }
        if (ret == ACL_ERROR_NONE) {
            vmm_mapped = 1;
            int32_t sa = set_access_rw(vmm_va, BLOCK_SIZE, devB);
            printf("[13]   [r1] SetAccess(RW, B=%d) on imported VA ret=%d\n", devB, sa);
            fflush(stdout);
        }
        if (ret == ACL_ERROR_NONE)
            ret = aclrtMemcpyAsync(local, BLOCK_SIZE, vmm_va, BLOCK_SIZE,
                                    ACL_MEMCPY_DEVICE_TO_DEVICE, stream);
        if (ret == ACL_ERROR_NONE)
            ret = aclrtSynchronizeStream(stream);
        if (ret == ACL_ERROR_NONE) {
            memset(vbuf, 0, BLOCK_SIZE);
            ret = aclrtMemcpy(vbuf, BLOCK_SIZE, local, BLOCK_SIZE,
                              ACL_MEMCPY_DEVICE_TO_HOST);
            if (ret == ACL_ERROR_NONE) {
                ok = (check_pat(vbuf, words, 0, TAG_VMM) == 0);
                vmm_verify = ok ? 1 : 0;
            }
        }
        if (ret == ACL_ERROR_NONE)
            vmm_ok = 1;
        step_line(2, who, "VMM: import+map + async D2D read + verify", ok, ret);
        if (!ok)
            BAIL(fatal);
    }
    sync_state(&fatal);

    /* ================================================================
     * step 3: host-swap 后端(复用 IPC imported VA)
     * ================================================================ */
    if (!fatal && rank == 1 && ipc_ok) {
        int ok = 0;
        /* D2H: imported VA → host_buf; H2D: host_buf → local */
        ret = aclrtMemcpy(host_buf, BLOCK_SIZE, ipc_va, BLOCK_SIZE,
                          ACL_MEMCPY_DEVICE_TO_HOST);
        if (ret == ACL_ERROR_NONE)
            ret = aclrtMemcpy(local, BLOCK_SIZE, host_buf, BLOCK_SIZE,
                              ACL_MEMCPY_HOST_TO_DEVICE);
        if (ret == ACL_ERROR_NONE) {
            memset(vbuf, 0, BLOCK_SIZE);
            ret = aclrtMemcpy(vbuf, BLOCK_SIZE, local, BLOCK_SIZE,
                              ACL_MEMCPY_DEVICE_TO_HOST);
            if (ret == ACL_ERROR_NONE) {
                ok = (check_pat(vbuf, words, 0, TAG_IPC) == 0);
                swap_verify = ok ? 1 : 0;
            }
        }
        step_line(3, who, "host-swap: D2H(imported→host) + H2D(host→local) + verify", ok, ret);
        if (!ok)
            BAIL(fatal);
    }
    sync_state(&fatal);

    /* ================================================================
     * step 4: 带宽(三后端各跑 3 预热 + 30×16MB async + 1 Sync)
     * ★严禁同步循环测带宽! 正确口径见文件头。 */
    if (!fatal && g_fail) {
        if (rank == 1)
            printf("[13] step 4 (r1): bandwidth SKIPPED (correctness not passed)\n");
    }
    if (!fatal && !g_fail && rank == 1) {
        double gb = (double)BW_ITERS * (double)BLOCK_SIZE / 1e9;
        printf("[13] step 4 (r1): bandwidth, %u MB x %d iters + %d warmup, "
               "async + 1 Sync ONLY (no sync loop!)\n",
               BLOCK_SIZE >> 20, BW_ITERS, BW_WARMUP);

        /* IPC 后端带宽: ipc_va → local */
        if (ipc_ok) {
            for (int i = 0; i < BW_WARMUP; i++) {
                aclrtMemcpyAsync(local, BLOCK_SIZE, ipc_va, BLOCK_SIZE,
                                 ACL_MEMCPY_DEVICE_TO_DEVICE, stream);
                aclrtSynchronizeStream(stream);
            }
            double t0 = now_sec();
            for (int i = 0; i < BW_ITERS; i++)
                aclrtMemcpyAsync(local, BLOCK_SIZE, ipc_va, BLOCK_SIZE,
                                 ACL_MEMCPY_DEVICE_TO_DEVICE, stream);
            aclrtSynchronizeStream(stream);
            double t1 = now_sec();
            ipc_bw = gb / (t1 - t0);
            printf("[13] bw   %-10s : %7.2f GB/s (30x16MB async + 1 Sync, total %.3f s)\n",
                   ipc_label, ipc_bw, t1 - t0);
        } else {
            printf("[13] bw   %-10s : N/A (setup failed)\n", ipc_label);
        }

        /* VMM 后端带宽: vmm_va → local */
        if (vmm_ok) {
            for (int i = 0; i < BW_WARMUP; i++) {
                aclrtMemcpyAsync(local, BLOCK_SIZE, vmm_va, BLOCK_SIZE,
                                 ACL_MEMCPY_DEVICE_TO_DEVICE, stream);
                aclrtSynchronizeStream(stream);
            }
            double t0 = now_sec();
            for (int i = 0; i < BW_ITERS; i++)
                aclrtMemcpyAsync(local, BLOCK_SIZE, vmm_va, BLOCK_SIZE,
                                 ACL_MEMCPY_DEVICE_TO_DEVICE, stream);
            aclrtSynchronizeStream(stream);
            double t1 = now_sec();
            vmm_bw = gb / (t1 - t0);
            printf("[13] bw   %-10s : %7.2f GB/s (30x16MB async + 1 Sync, total %.3f s)\n",
                   vmm_label, vmm_bw, t1 - t0);
        } else {
            printf("[13] bw   %-10s : N/A (setup failed)\n", vmm_label);
        }

        /* host-swap 后端带宽: 30×(D2H async + H2D async) + 1 Sync
         * 有效带宽 = 30×16MB / 时间(搬运 16MB 远端→本地, 经 host 中转) */
        if (ipc_ok) {
            for (int i = 0; i < BW_WARMUP; i++) {
                aclrtMemcpyAsync(host_buf, BLOCK_SIZE, ipc_va, BLOCK_SIZE,
                                 ACL_MEMCPY_DEVICE_TO_HOST, stream);
                aclrtMemcpyAsync(local, BLOCK_SIZE, host_buf, BLOCK_SIZE,
                                 ACL_MEMCPY_HOST_TO_DEVICE, stream);
                aclrtSynchronizeStream(stream);
            }
            double t0 = now_sec();
            for (int i = 0; i < BW_ITERS; i++) {
                aclrtMemcpyAsync(host_buf, BLOCK_SIZE, ipc_va, BLOCK_SIZE,
                                 ACL_MEMCPY_DEVICE_TO_HOST, stream);
                aclrtMemcpyAsync(local, BLOCK_SIZE, host_buf, BLOCK_SIZE,
                                 ACL_MEMCPY_HOST_TO_DEVICE, stream);
            }
            aclrtSynchronizeStream(stream);
            double t1 = now_sec();
            swap_bw = gb / (t1 - t0);
            printf("[13] bw   host-swap   : %7.2f GB/s (30x[D2H+H2D async]+1 Sync, total %.3f s)\n",
                   swap_bw, t1 - t0);
        } else {
            printf("[13] bw   host-swap   : N/A (IPC setup failed)\n");
        }
        fflush(stdout);
    }
    sync_state(&fatal);

    /* ================================================================
     * step 5: 打印对比表
     * ================================================================ */
    if (!fatal && rank == 1) {
        printf("[13] step 5: bw compare (A_DEV=%d -> B_DEV=%d, 16MB, 30x async + 1 Sync):\n",
               devA, devB);
        printf("[13]   %-10s : %7.2f GB/s  verify=%s\n", ipc_label, ipc_bw,
               ipc_verify ? "OK" : "FAIL");
        printf("[13]   %-10s : %7.2f GB/s  verify=%s\n", vmm_label, vmm_bw,
               vmm_verify ? "OK" : "FAIL");
        printf("[13]   host-swap : %7.2f GB/s  verify=%s\n", swap_bw,
               swap_verify ? "OK" : "FAIL");
        fflush(stdout);
    }
    sync_state(&fatal);

    /* ================================================================
     * step 6: cleanup
     * ================================================================ */
    /* VMM cleanup: importer 先释放, barrier 后 exporter 释放 */
    if (rank == 1) {
        if (vmm_mapped) {
            ret = aclrtUnmapMem(vmm_va);
            printf("[13] step 6 (r1): VMM UnmapMem(imported VA) ret=%d\n", ret);
        }
        if (vmm_imp_handle) {
            ret = aclrtFreePhysical(vmm_imp_handle);
            printf("[13] step 6 (r1): VMM FreePhysical(imported) ret=%d\n", ret);
        }
        if (vmm_reserved) {
            ret = aclrtReleaseMemAddress(vmm_va);
            printf("[13] step 6 (r1): VMM ReleaseMemAddress(imported VA) ret=%d\n", ret);
        }
    }
    MPI_Barrier(MPI_COMM_WORLD);   /* importer 释放后 exporter 才 free 物理内存 */
    if (rank == 0) {
        if (own_mapped) {
            ret = aclrtUnmapMem(vmm_own_va);
            printf("[13] step 6 (r0): VMM UnmapMem(own VA) ret=%d\n", ret);
        }
        if (vmm_handle) {
            ret = aclrtFreePhysical(vmm_handle);
            printf("[13] step 6 (r0): VMM FreePhysical(own) ret=%d\n", ret);
        }
        if (own_reserved) {
            ret = aclrtReleaseMemAddress(vmm_own_va);
            printf("[13] step 6 (r0): VMM ReleaseMemAddress(own VA) ret=%d\n", ret);
        }
    }

    /* IPC cleanup: 双方各自 close key; 物理内存只由 exporter free */
    if (rank == 0 && ipc_ok) {
        ret = aclrtIpcMemClose(ipc_key);
        printf("[13] step 6 (r0): IPC IpcMemClose ret=%d\n", ret);
    }
    if (rank == 1 && ipc_ok) {
        ret = aclrtIpcMemClose(ipc_key);
        printf("[13] step 6 (r1): IPC IpcMemClose ret=%d\n", ret);
    }
    MPI_Barrier(MPI_COMM_WORLD);
    if (rank == 0 && ipc_da)
        aclrtFree(ipc_da);
    if (rank == 1 && local)
        aclrtFree(local);
    if (stream)
        aclrtDestroyStream(stream);
    free(hbuf);
    free(vbuf);
    free(host_buf);
    fflush(stdout);
    /* ★修复 D 态挂死: 跨卡 EnablePeerAccess 建立的 P2P 状态必须在 ResetDevice
     *   前显式拆除, 否则进程退出时 mmu notifier 释放与驱动 SRCU 死锁
     *   (exit_mmap → synchronize_srcu 永久等待, SIGKILL 不可回收, 耗尽 hugepages)。
     *   根因: 02(die 对无 EnablePeer)正常 / 03(跨卡 EnablePeer)挂死。
     *   r0(disable devB)+ r1(disable devA)对称; die 对无 EnablePeer 不需要。 */
    if (dev_ok) {
        if (is_cross) {
            int32_t peer = (rank == 0) ? devB : devA;
            aclError dr = aclrtDeviceDisablePeerAccess(peer);
            printf("[13] step 6 (%s): DisablePeerAccess(%d) ret=%d\n", who, peer, dr);
        }
        aclrtResetDevice(dev);
    }
    /* die 对 VMM ctx 技巧在本进程开过的 A 侧 context */
    if (rank == 1 && r1_setA)
        aclrtResetDevice(devA);
    if (init_ok)
        aclFinalize();

    /* ---- RESULT ---- */
    {
        int res[1];
        res[0] = g_fail;
        MPI_Allreduce(MPI_IN_PLACE, res, 1, MPI_INT, MPI_MAX, MPI_COMM_WORLD);
        g_fail = res[0];

        printf("[13] RESULT: %s\n", g_fail ? "FAIL" : "PASS");
        fflush(stdout);
    }

    MPI_Finalize();
    return g_fail ? 1 : 0;
}
