/*
 * 13_bw_compare_reverse — 反向写入带宽测试 (push 模式)
 *
 * 与 test.c (pull 模式) 的区别:
 *   test.c:        rank0(exporter/devA) 分配内存 → rank1(importer/devB) 从远端读
 *   test_reverse:  rank1(exporter/devB) 分配内存 → rank0(importer/devA) 向远端写
 *
 * 目的: 对比 pull (dst 读 src) vs push (src 写 dst) 的带宽差异,
 *       验证 PCIe 读 vs 写的性能特征。
 *
 * 数据流:
 *   pull: rank1 发起 aclrtMemcpyAsync(local ← ipc_va)  → dst DMA 拉数据
 *   push: rank0 发起 aclrtMemcpyAsync(ipc_va ← local)  → src DMA 推数据
 *
 * 机器: Atlas 300I Duo (310P3, 4 卡 x 2 die = 8 dev), CANN 9.1.0, aarch64
 * 结构: MPI 双进程 rank0=importer/writer(dev A, A_DEV 默认 0)
 *                     rank1=exporter/verifier(dev B, B_DEV 默认 1)
 *
 * 带宽口径: 3 预热 + 30×16MB MemcpyAsync + 1 Sync (与 test.c 一致)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <mpi.h>
#include "acl/acl.h"

#define BLOCK_SIZE  (16u * 1024u * 1024u)
#define BW_WARMUP   3
#define BW_ITERS    30
#define IPC_KEY_LEN 65

#define TAG_IPC  0x2
#define TAG_VMM  0x3

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
            printf("[13r]  [verify] mismatch at word %zu (off 0x%zx): got 0x%08x, want 0x%08x\n",
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

static void step_line(int n, const char *who, const char *desc, int ok, int32_t ret)
{
    printf("[13r] step %d (%s): %-58s %s (ret=%d)\n", n, who, desc,
           ok ? "OK" : "FAIL", ret);
    fflush(stdout);
    if (!ok)
        g_fail = 1;
}

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

static int32_t set_access_rw(void *va, size_t size, int32_t devId)
{
    aclrtMemAccessDesc desc;
    memset(&desc, 0, sizeof(desc));
    desc.flags = ACL_RT_MEM_ACCESS_FLAGS_READWRITE;
    desc.location.type = ACL_MEM_LOCATION_TYPE_DEVICE;
    desc.location.id = (uint32_t)devId;
    return aclrtMemSetAccess(va, size, &desc, 1);
}

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
        printf("[13r]  [%s attr %d/7 %-14s] MallocPhysical(%uMB) ret=%d\n",
               label, i + 1, g_chainName[i], (unsigned)(size >> 20), m);
        fflush(stdout);
        if (m == ACL_ERROR_NONE)
            return 0;
        last = m;
        *handle = NULL;
    }
    printf("[13r]  [%s] all 7 attrs FAILED (last ret=%d)\n", label, last);
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
            printf("[13r] FATAL: need exactly 2 MPI procs (-np 2), got %d\n", nproc);
        MPI_Finalize();
        return 1;
    }

    int32_t devA = env_dev("A_DEV", 0);
    int32_t devB = env_dev("B_DEV", 1);
    int32_t dev = (rank == 0) ? devA : devB;
    int is_die_pair = (devA != devB) && (devA / 2 == devB / 2);
    int is_cross = !is_die_pair && (devA != devB);

    /*
     * ★ 角色反转:
     *   rank0 = importer/writer (devA): 向远端写入
     *   rank1 = exporter/verifier (devB): 分配内存, 导出, 验证
     */
    const char *role = (rank == 0) ? "r0/importer-writer" : "r1/exporter-verifier";
    const char *who = (rank == 0) ? "r0" : "r1";
    const char *topo = is_die_pair ? "die-pair" : (is_cross ? "cross-card" : "same-dev");

    const char *ipc_label = is_die_pair ? "IPC DEFAULT" : (is_cross ? "IPC PEER  " : "IPC DEFAULT");
    const char *vmm_label = is_die_pair ? "VMM ctx    " : (is_cross ? "VMM P2P    " : "VMM ctx    ");

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("[13r] === 13_bw_compare REVERSE (push): rank%d %s dev%d, %s (A_DEV=%d B_DEV=%d) ===\n",
           rank, role, dev, topo, devA, devB);
    printf("[13r] %s: r1(devB) 分配+导出, r0(devA) 导入+向远端写入\n", role);

    aclrtStream stream = NULL;
    /* IPC 后端 */
    void *ipc_da = NULL;
    void *ipc_va = NULL;
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
    void *local = NULL;
    uint32_t *hbuf = NULL, *vbuf = NULL, *host_buf = NULL;
    int r0_setB = 0;
    int fatal = 0;
    int32_t ret = 0;
    int init_ok = 0, dev_ok = 0;
    size_t words = BLOCK_SIZE / sizeof(uint32_t);

    /* ---- phase 0: init ---- */
    ret = aclInit(NULL);
    if (ret != ACL_ERROR_NONE) {
        printf("[13r] init: %s aclInit FAIL ret=%d\n", role, ret);
        BAIL(fatal);
    } else {
        init_ok = 1;
        ret = aclrtSetDevice(dev);
        if (ret != ACL_ERROR_NONE) {
            printf("[13r] init: %s aclrtSetDevice(%d) FAIL ret=%d\n", role, dev, ret);
            BAIL(fatal);
        }
    }
    if (!fatal) {
        dev_ok = 1;
        if (rank == 0) {
            ret = aclrtCreateStream(&stream);
            if (ret != ACL_ERROR_NONE) {
                printf("[13r] init: r0 aclrtCreateStream FAIL ret=%d\n", ret);
                BAIL(fatal);
            }
            ret = aclrtMalloc(&local, BLOCK_SIZE, ACL_MEM_MALLOC_HUGE_FIRST);
            if (ret != ACL_ERROR_NONE) {
                printf("[13r] init: r0 aclrtMalloc local 16MB FAIL ret=%d\n", ret);
                BAIL(fatal);
            }
            host_buf = (uint32_t *)malloc(BLOCK_SIZE);
            if (!host_buf) {
                printf("[13r] init: r0 host malloc host_buf 16MB FAIL\n");
                BAIL(fatal);
            }
        }
        hbuf = (uint32_t *)malloc(BLOCK_SIZE);
        vbuf = (uint32_t *)malloc(BLOCK_SIZE);
        if (!hbuf || !vbuf) {
            printf("[13r] init: %s host malloc 2x16MB FAIL\n", role);
            BAIL(fatal);
        }
    }
    if (!fatal)
        printf("[13r] init: %s aclInit/SetDevice(%d)%s OK\n", role, dev,
               (rank == 0) ? "/CreateStream/MallocLocal" : "");
    sync_state(&fatal);

    /* ================================================================
     * step 1: IPC 后端 (反向: r1 分配+导出, r0 导入+写入)
     * ================================================================ */
    if (!fatal && rank == 1) {
        int ok = 0;
        ret = aclrtMalloc(&ipc_da, BLOCK_SIZE, ACL_MEM_MALLOC_HUGE_FIRST_P2P);
        if (ret == ACL_ERROR_NONE) {
            ret = aclrtMemset(ipc_da, BLOCK_SIZE, 0, BLOCK_SIZE);
            ret = aclrtIpcMemGetExportKey(ipc_da, BLOCK_SIZE, ipc_key, IPC_KEY_LEN,
                                          ACL_RT_IPC_MEM_EXPORT_FLAG_DISABLE_PID_VALIDATION);
        }
        if (ret == ACL_ERROR_NONE) {
            if (MPI_Send(ipc_key, IPC_KEY_LEN, MPI_CHAR, 0, 100, MPI_COMM_WORLD) == MPI_SUCCESS)
                ok = 1;
        }
        step_line(1, who, "IPC: malloc P2P 16MB + export key + send", ok, ret);
        if (!ok)
            BAIL(fatal);
    } else if (!fatal && rank == 0) {
        MPI_Recv(ipc_key, IPC_KEY_LEN, MPI_CHAR, 1, 100, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    }
    sync_state(&fatal);

    if (!fatal && rank == 0) {
        int ok = 0;
        uint64_t flag = is_die_pair ? ACL_RT_IPC_MEM_IMPORT_FLAG_DEFAULT
                                    : ACL_RT_IPC_MEM_IMPORT_FLAG_ENABLE_PEER_ACCESS;
        ret = aclrtIpcMemImportByKey(&ipc_va, ipc_key, flag);
        if (ret == ACL_ERROR_NONE) {
            ipc_ok = 1;
            fill_pat(hbuf, words, 0, TAG_IPC);
            ret = aclrtMemcpy(local, BLOCK_SIZE, hbuf, BLOCK_SIZE, ACL_MEMCPY_HOST_TO_DEVICE);
        }
        if (ret == ACL_ERROR_NONE)
            ret = aclrtMemcpyAsync(ipc_va, BLOCK_SIZE, local, BLOCK_SIZE,
                                   ACL_MEMCPY_DEVICE_TO_DEVICE, stream);
        if (ret == ACL_ERROR_NONE)
            ret = aclrtSynchronizeStream(stream);
        if (ret == ACL_ERROR_NONE)
            ok = 1;
        step_line(1, who, "IPC: import + async D2D write to remote + sync", ok, ret);
        if (!ok)
            BAIL(fatal);
    }
    sync_state(&fatal);

    if (!fatal && rank == 1) {
        int ok = 0;
        memset(vbuf, 0, BLOCK_SIZE);
        ret = aclrtMemcpy(vbuf, BLOCK_SIZE, ipc_da, BLOCK_SIZE, ACL_MEMCPY_DEVICE_TO_HOST);
        if (ret == ACL_ERROR_NONE) {
            ok = (check_pat(vbuf, words, 0, TAG_IPC) == 0);
            ipc_verify = ok ? 1 : 0;
        }
        step_line(1, who, "IPC: verify written data on exporter side", ok, ret);
        if (!ok)
            BAIL(fatal);
    }
    sync_state(&fatal);

    /* ================================================================
     * step 2: VMM 后端 (反向: r1 分配+导出, r0 导入+写入)
     * ================================================================ */
    if (!fatal && rank == 1) {
        int ok = 0;
        if (is_cross) {
            int32_t ena = aclrtDeviceEnablePeerAccess(devA, 0);
            printf("[13r]  [r1] EnablePeerAccess(A=%d) before VMM export ret=%d\n", devA, ena);
            fflush(stdout);
        }
        ret = malloc_physical_chain(devB, BLOCK_SIZE, &vmm_handle, "vmm");
        if (ret == ACL_ERROR_NONE)
            ret = aclrtReserveMemAddress(&vmm_own_va, BLOCK_SIZE, 0, NULL, 0);
        if (ret == ACL_ERROR_NONE) {
            own_reserved = 1;
            ret = aclrtMapMem(vmm_own_va, BLOCK_SIZE, 0, vmm_handle, 0);
        }
        if (ret == ACL_ERROR_NONE) {
            own_mapped = 1;
            int32_t sa = set_access_rw(vmm_own_va, BLOCK_SIZE, devB);
            printf("[13r]  [r1] SetAccess(RW, B=%d) on own VA ret=%d\n", devB, sa);
            fflush(stdout);
            ret = aclrtMemset(vmm_own_va, BLOCK_SIZE, 0, BLOCK_SIZE);
            if (ret == ACL_ERROR_NONE)
                ret = aclrtMemExportToShareableHandle(vmm_handle, ACL_MEM_HANDLE_TYPE_NONE,
                                                      ACL_RT_VMM_EXPORT_FLAG_DISABLE_PID_VALIDATION, &sh);
        }
        if (ret == ACL_ERROR_NONE && sh != 0) {
            if (MPI_Send(&sh, 1, MPI_UINT64_T, 0, 101, MPI_COMM_WORLD) == MPI_SUCCESS)
                ok = 1;
        }
        if (!ok) {
            uint64_t bad = 0;
            MPI_Send(&bad, 1, MPI_UINT64_T, 0, 101, MPI_COMM_WORLD);
        }
        step_line(2, who, "VMM: MallocPhysical+Map+Export sh+send", ok, ret);
        if (!ok)
            BAIL(fatal);
    } else if (!fatal && rank == 0) {
        MPI_Recv(&sh, 1, MPI_UINT64_T, 1, 101, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        if (sh == 0)
            printf("[13r] step 2 (r0): received sh=0 (exporter VMM failed)\n");
    }
    sync_state(&fatal);

    if (!fatal && rank == 0 && sh != 0) {
        int ok = 0;
        if (is_die_pair) {
            ret = aclrtSetDevice(devB);
            if (ret == ACL_ERROR_NONE) {
                r0_setB = 1;
                ret = aclrtMemImportFromShareableHandle(sh, devA, &vmm_imp_handle);
                int32_t ra = aclrtSetDevice(devA);
                printf("[13r]  [r0] ctx trick: SetDevice(B=%d)+Import(sh,A=%d)+SetDevice(A=%d) "
                       "imp ret=%d, restore ret=%d\n", devB, devA, devA, ret, ra);
                fflush(stdout);
            }
        } else {
            int32_t ena = aclrtDeviceEnablePeerAccess(devB, 0);
            printf("[13r]  [r0] EnablePeerAccess(B=%d) before VMM import ret=%d\n", devB, ena);
            fflush(stdout);
            ret = aclrtMemImportFromShareableHandle(sh, devA, &vmm_imp_handle);
        }
        if (ret == ACL_ERROR_NONE)
            ret = aclrtReserveMemAddress(&vmm_va, BLOCK_SIZE, 0, NULL, 0);
        if (ret == ACL_ERROR_NONE) {
            vmm_reserved = 1;
            ret = aclrtMapMem(vmm_va, BLOCK_SIZE, 0, vmm_imp_handle, 0);
        }
        if (ret == ACL_ERROR_NONE) {
            vmm_mapped = 1;
            int32_t sa = set_access_rw(vmm_va, BLOCK_SIZE, devA);
            printf("[13r]  [r0] SetAccess(RW, A=%d) on imported VA ret=%d\n", devA, sa);
            fflush(stdout);
        }
        if (ret == ACL_ERROR_NONE) {
            fill_pat(hbuf, words, 0, TAG_VMM);
            ret = aclrtMemcpy(local, BLOCK_SIZE, hbuf, BLOCK_SIZE, ACL_MEMCPY_HOST_TO_DEVICE);
        }
        if (ret == ACL_ERROR_NONE)
            ret = aclrtMemcpyAsync(vmm_va, BLOCK_SIZE, local, BLOCK_SIZE,
                                   ACL_MEMCPY_DEVICE_TO_DEVICE, stream);
        if (ret == ACL_ERROR_NONE)
            ret = aclrtSynchronizeStream(stream);
        if (ret == ACL_ERROR_NONE) {
            vmm_ok = 1;
            ok = 1;
        }
        step_line(2, who, "VMM: import+map + async D2D write to remote + sync", ok, ret);
        if (!ok)
            BAIL(fatal);
    }
    sync_state(&fatal);

    /* rank 0 把 vmm_ok 发给 rank 1 */
    if (!fatal && rank == 0) {
        MPI_Send(&vmm_ok, 1, MPI_INT, 1, 201, MPI_COMM_WORLD);
    }
    if (!fatal && rank == 1) {
        MPI_Recv(&vmm_ok, 1, MPI_INT, 0, 201, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    }

    if (!fatal && rank == 1 && vmm_ok) {
        int ok = 0;
        memset(vbuf, 0, BLOCK_SIZE);
        ret = aclrtMemcpy(vbuf, BLOCK_SIZE, vmm_own_va, BLOCK_SIZE, ACL_MEMCPY_DEVICE_TO_HOST);
        if (ret == ACL_ERROR_NONE) {
            ok = (check_pat(vbuf, words, 0, TAG_VMM) == 0);
            vmm_verify = ok ? 1 : 0;
        }
        step_line(2, who, "VMM: verify written data on exporter side", ok, ret);
        if (!ok)
            BAIL(fatal);
    }
    sync_state(&fatal);

    /* ================================================================
     * step 3: host-swap 后端 (反向: r0 写 host_buf, r1 从 host_buf 读到 ipc_da)
     * ================================================================ */
    if (!fatal && rank == 0 && ipc_ok) {
        int ok = 0;
        fill_pat(hbuf, words, 0, TAG_IPC);
        ret = aclrtMemcpy(host_buf, BLOCK_SIZE, hbuf, BLOCK_SIZE, ACL_MEMCPY_HOST_TO_HOST);
        if (ret == ACL_ERROR_NONE)
            ret = aclrtMemcpy(ipc_va, BLOCK_SIZE, host_buf, BLOCK_SIZE,
                              ACL_MEMCPY_HOST_TO_DEVICE);
        if (ret == ACL_ERROR_NONE)
            ok = 1;
        step_line(3, who, "host-swap: write host_buf + H2D to remote", ok, ret);
        if (!ok)
            BAIL(fatal);
    }
    sync_state(&fatal);

    /* rank 0 把 ipc_ok 发给 rank 1 */
    if (!fatal && rank == 0) {
        MPI_Send(&ipc_ok, 1, MPI_INT, 1, 202, MPI_COMM_WORLD);
    }
    if (!fatal && rank == 1) {
        MPI_Recv(&ipc_ok, 1, MPI_INT, 0, 202, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    }

    if (!fatal && rank == 1 && ipc_ok) {
        int ok = 0;
        memset(vbuf, 0, BLOCK_SIZE);
        ret = aclrtMemcpy(vbuf, BLOCK_SIZE, ipc_da, BLOCK_SIZE, ACL_MEMCPY_DEVICE_TO_HOST);
        if (ret == ACL_ERROR_NONE) {
            ok = (check_pat(vbuf, words, 0, TAG_IPC) == 0);
            swap_verify = ok ? 1 : 0;
        }
        step_line(3, who, "host-swap: verify written data on exporter side", ok, ret);
        if (!ok)
            BAIL(fatal);
    }
    sync_state(&fatal);

    /* ================================================================
     * step 4: 带宽 (反向: r0 向远端写入)
     * ================================================================ */
    if (!fatal && g_fail) {
        if (rank == 0)
            printf("[13r] step 4 (r0): bandwidth SKIPPED (correctness not passed)\n");
    }
    if (!fatal && !g_fail && rank == 0) {
        double gb = (double)BW_ITERS * (double)BLOCK_SIZE / 1e9;
        printf("[13r] step 4 (r0): bandwidth (PUSH/write), %u MB x %d iters + %d warmup, "
               "async + 1 Sync ONLY\n", BLOCK_SIZE >> 20, BW_ITERS, BW_WARMUP);

        if (ipc_ok) {
            for (int i = 0; i < BW_WARMUP; i++) {
                aclrtMemcpyAsync(ipc_va, BLOCK_SIZE, local, BLOCK_SIZE,
                                 ACL_MEMCPY_DEVICE_TO_DEVICE, stream);
                aclrtSynchronizeStream(stream);
            }
            double t0 = now_sec();
            for (int i = 0; i < BW_ITERS; i++)
                aclrtMemcpyAsync(ipc_va, BLOCK_SIZE, local, BLOCK_SIZE,
                                 ACL_MEMCPY_DEVICE_TO_DEVICE, stream);
            aclrtSynchronizeStream(stream);
            double t1 = now_sec();
            ipc_bw = gb / (t1 - t0);
            printf("[13r] bw   %-10s : %7.2f GB/s (30x16MB push async + 1 Sync, total %.3f s)\n",
                   ipc_label, ipc_bw, t1 - t0);
        } else {
            printf("[13r] bw   %-10s : N/A (setup failed)\n", ipc_label);
        }

        if (vmm_ok) {
            for (int i = 0; i < BW_WARMUP; i++) {
                aclrtMemcpyAsync(vmm_va, BLOCK_SIZE, local, BLOCK_SIZE,
                                 ACL_MEMCPY_DEVICE_TO_DEVICE, stream);
                aclrtSynchronizeStream(stream);
            }
            double t0 = now_sec();
            for (int i = 0; i < BW_ITERS; i++)
                aclrtMemcpyAsync(vmm_va, BLOCK_SIZE, local, BLOCK_SIZE,
                                 ACL_MEMCPY_DEVICE_TO_DEVICE, stream);
            aclrtSynchronizeStream(stream);
            double t1 = now_sec();
            vmm_bw = gb / (t1 - t0);
            printf("[13r] bw   %-10s : %7.2f GB/s (30x16MB push async + 1 Sync, total %.3f s)\n",
                   vmm_label, vmm_bw, t1 - t0);
        } else {
            printf("[13r] bw   %-10s : N/A (setup failed)\n", vmm_label);
        }

        if (ipc_ok) {
            for (int i = 0; i < BW_WARMUP; i++) {
                aclrtMemcpyAsync(host_buf, BLOCK_SIZE, local, BLOCK_SIZE,
                                 ACL_MEMCPY_DEVICE_TO_HOST, stream);
                aclrtMemcpyAsync(ipc_va, BLOCK_SIZE, host_buf, BLOCK_SIZE,
                                 ACL_MEMCPY_HOST_TO_DEVICE, stream);
                aclrtSynchronizeStream(stream);
            }
            double t0 = now_sec();
            for (int i = 0; i < BW_ITERS; i++) {
                aclrtMemcpyAsync(host_buf, BLOCK_SIZE, local, BLOCK_SIZE,
                                 ACL_MEMCPY_DEVICE_TO_HOST, stream);
                aclrtMemcpyAsync(ipc_va, BLOCK_SIZE, host_buf, BLOCK_SIZE,
                                 ACL_MEMCPY_HOST_TO_DEVICE, stream);
            }
            aclrtSynchronizeStream(stream);
            double t1 = now_sec();
            swap_bw = gb / (t1 - t0);
            printf("[13r] bw   host-swap   : %7.2f GB/s (30x[D2H+H2D async]+1 Sync, total %.3f s)\n",
                   swap_bw, t1 - t0);
        } else {
            printf("[13r] bw   host-swap   : N/A (IPC setup failed)\n");
        }
        fflush(stdout);
    }
    sync_state(&fatal);

    /* rank 0 把带宽值发给 rank 1, 以便 rank 1 打印完整对比表 */
    if (!fatal && rank == 0) {
        double bw[3] = {ipc_bw, vmm_bw, swap_bw};
        MPI_Send(bw, 3, MPI_DOUBLE, 1, 200, MPI_COMM_WORLD);
    }
    if (!fatal && rank == 1) {
        double bw[3] = {0};
        MPI_Recv(bw, 3, MPI_DOUBLE, 0, 200, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        ipc_bw = bw[0];
        vmm_bw = bw[1];
        swap_bw = bw[2];
    }

    /* ================================================================
     * step 5: 打印对比表
     * ================================================================ */
    if (!fatal && rank == 1) {
        printf("[13r] step 5: bw compare REVERSE/PUSH (A_DEV=%d -> B_DEV=%d, 16MB, 30x async + 1 Sync):\n",
               devA, devB);
        printf("[13r]   %-10s : %7.2f GB/s  verify=%s\n", ipc_label, ipc_bw,
               ipc_verify ? "OK" : "FAIL");
        printf("[13r]   %-10s : %7.2f GB/s  verify=%s\n", vmm_label, vmm_bw,
               vmm_verify ? "OK" : "FAIL");
        printf("[13r]   host-swap : %7.2f GB/s  verify=%s\n", swap_bw,
               swap_verify ? "OK" : "FAIL");
        fflush(stdout);
    }
    sync_state(&fatal);

    /* ================================================================
     * step 6: cleanup
     * ================================================================ */
    if (rank == 0) {
        if (vmm_mapped) {
            ret = aclrtUnmapMem(vmm_va);
            printf("[13r] step 6 (r0): VMM UnmapMem(imported VA) ret=%d\n", ret);
        }
        if (vmm_imp_handle) {
            ret = aclrtFreePhysical(vmm_imp_handle);
            printf("[13r] step 6 (r0): VMM FreePhysical(imported) ret=%d\n", ret);
        }
        if (vmm_reserved) {
            ret = aclrtReleaseMemAddress(vmm_va);
            printf("[13r] step 6 (r0): VMM ReleaseMemAddress(imported VA) ret=%d\n", ret);
        }
    }
    MPI_Barrier(MPI_COMM_WORLD);
    if (rank == 1) {
        if (own_mapped) {
            ret = aclrtUnmapMem(vmm_own_va);
            printf("[13r] step 6 (r1): VMM UnmapMem(own VA) ret=%d\n", ret);
        }
        if (vmm_handle) {
            ret = aclrtFreePhysical(vmm_handle);
            printf("[13r] step 6 (r1): VMM FreePhysical(own) ret=%d\n", ret);
        }
        if (own_reserved) {
            ret = aclrtReleaseMemAddress(vmm_own_va);
            printf("[13r] step 6 (r1): VMM ReleaseMemAddress(own VA) ret=%d\n", ret);
        }
    }

    if (rank == 1 && ipc_ok) {
        ret = aclrtIpcMemClose(ipc_key);
        printf("[13r] step 6 (r1): IPC IpcMemClose ret=%d\n", ret);
    }
    if (rank == 0 && ipc_ok) {
        ret = aclrtIpcMemClose(ipc_key);
        printf("[13r] step 6 (r0): IPC IpcMemClose ret=%d\n", ret);
    }
    MPI_Barrier(MPI_COMM_WORLD);
    if (rank == 1 && ipc_da)
        aclrtFree(ipc_da);
    if (rank == 0 && local)
        aclrtFree(local);
    if (stream)
        aclrtDestroyStream(stream);
    free(hbuf);
    free(vbuf);
    free(host_buf);
    fflush(stdout);

    if (dev_ok) {
        if (is_cross) {
            int32_t peer = (rank == 0) ? devB : devA;
            aclError dr = aclrtDeviceDisablePeerAccess(peer);
            printf("[13r] step 6 (%s): DisablePeerAccess(%d) ret=%d\n", who, peer, dr);
        }
        aclrtResetDevice(dev);
    }
    if (rank == 0 && r0_setB)
        aclrtResetDevice(devB);
    if (init_ok)
        aclFinalize();

    {
        int res[1];
        res[0] = g_fail;
        MPI_Allreduce(MPI_IN_PLACE, res, 1, MPI_INT, MPI_MAX, MPI_COMM_WORLD);
        g_fail = res[0];

        printf("[13r] RESULT: %s\n", g_fail ? "FAIL" : "PASS");
        fflush(stdout);
    }

    MPI_Finalize();
    return g_fail ? 1 : 0;
}
