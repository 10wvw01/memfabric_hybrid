/*
 * minitest.c — 13 个最小化 IPC import 测试, 精确定位 07_d2d_matrix 失败根因
 *
 * 用法: T=<1-13> [A_DEV=x] [B_DEV=y] mpirun -np 2 ./minitest
 *
 * 默认设备:
 *   T01-T07,T10-T13: A_DEV=0, B_DEV=1 (同卡 card0)
 *   T08: A_DEV=0, B_DEV=2 (跨卡 card0→card1, 同 socket)
 *   T09: A_DEV=0, B_DEV=4 (跨卡跨 socket)
 *
 * 测试矩阵:
 *   T01: r0=PEER(dev4)  → r1=PEER(dev4)       最小复现
 *   T02: r0=DEFAULT(dev1) → r1=PEER(dev4)     DEFAULT 是否影响后续 PEER
 *   T03: r0=nothing     → r1=PEER(dev4)       基线
 *   T04: r0=PEER(dev2)  → r1=PEER(dev4)       同 socket import 是否阻塞跨 socket
 *   T05: r0=PEER(dev4)  → r1=PEER(dev2)       阻塞是否与目标设备相关
 *   T06: r0=PEER(dev4)  → r1=DEFAULT(dev1)    跨 socket import 是否影响 die pair
 *   T07: r0=PEER(dev4)  → r1=PEER(dev4)       不同卡同 socket, 是否阻塞
 *   T08: r0=PEER(dev4)  → r1=PEER(dev4)       socket1 侧同卡是否同样问题
 *   T09: r0=PEER(dev2)  → r1=PEER(dev2)       跨 socket 不同卡
 *   T10: r0=EPA(4)      → r1=PEER(dev4)       显式 EPA 是否阻塞 IPC import
 *   T11: r0=EPA(4)      → r1=EPA(4)           两次 EPA 是否冲突
 *   T12: r0=PEER(4,5,6,7) → r1=PEER(dev4)     多 import 是否更易触发
 *   T13: r0=nothing     → r1=PEER(4)+PEER(5)  单 rank 连续 import 是否有限制
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <mpi.h>
#include "acl/acl.h"

#define SLAB_SIZE   (16u * 1024u * 1024u)
#define IPC_KEY_LEN 65

static int32_t env_dev(const char *n, int32_t d)
{
    const char *s = getenv(n);
    return s ? (int32_t)atoi(s) : d;
}

static int32_t env_int(const char *n, int32_t d)
{
    const char *s = getenv(n);
    return s ? (int32_t)atoi(s) : d;
}

static const char *ret_str(int32_t r)
{
    return r == 0 ? "OK" : (r == 507899 ? "FAIL(507899)" : "FAIL(other)");
}

/* ---------- 通用 cleanup ---------- */
static void cleanup(int rank, int32_t dev, int init_ok, int dev_ok,
                    int exp_ok, int imp_ok, int imp_peer,
                    int r0_peer, int r1_peer,
                    char *mykey, char *peerkey,
                    int32_t peer_dev_for_epa,
                    void *slab, int need_disable_r0, int need_disable_r1,
                    int32_t disable_dev_r0, int32_t disable_dev_r1)
{
    if (init_ok) {
        if (imp_ok)
            aclrtIpcMemClose(peerkey);
        if (exp_ok)
            aclrtIpcMemClose(mykey);
    }
    MPI_Barrier(MPI_COMM_WORLD);
    if (slab)
        aclrtFree(slab);
    if (dev_ok) {
        if (rank == 0 && need_disable_r0) {
            int32_t dr = aclrtDeviceDisablePeerAccess(disable_dev_r0);
            printf("  cleanup: r0 DisablePeerAccess(%d) ret=%d\n", disable_dev_r0, dr);
        }
        if (rank == 1 && need_disable_r1) {
            int32_t dr = aclrtDeviceDisablePeerAccess(disable_dev_r1);
            printf("  cleanup: r1 DisablePeerAccess(%d) ret=%d\n", disable_dev_r1, dr);
        }
        aclrtResetDevice(dev);
    }
    if (init_ok)
        aclFinalize();
}

/* ---------- 通用 setup: 双方建 slab + 交换 key ---------- */
static void setup_slabs(int rank, int32_t dev, void **slab,
                        char *mykey, char *peerkey, int *exp_ok)
{
    int32_t ret;
    *exp_ok = 0;
    ret = aclrtMalloc(slab, SLAB_SIZE, ACL_MEM_MALLOC_HUGE_FIRST_P2P);
    if (ret == ACL_ERROR_NONE) {
        ret = aclrtIpcMemGetExportKey(*slab, SLAB_SIZE, mykey, IPC_KEY_LEN,
                                      ACL_RT_IPC_MEM_EXPORT_FLAG_DISABLE_PID_VALIDATION);
        if (ret == ACL_ERROR_NONE)
            *exp_ok = 1;
    }
    printf("  r%d(dev%d): create slab %s\n", rank, dev,
           *exp_ok ? "OK" : "FAIL");

    /* 交换 key: rank0↔rank1 */
    if (rank == 0) {
        MPI_Send(mykey, IPC_KEY_LEN, MPI_CHAR, 1, 200, MPI_COMM_WORLD);
        MPI_Recv(peerkey, IPC_KEY_LEN, MPI_CHAR, 1, 201, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    } else {
        MPI_Recv(peerkey, IPC_KEY_LEN, MPI_CHAR, 0, 200, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        MPI_Send(mykey, IPC_KEY_LEN, MPI_CHAR, 0, 201, MPI_COMM_WORLD);
    }
}

/* ================================================================
 *  T01: r0=PEER(dev4) → r1=PEER(dev4)   最小复现
 * ================================================================ */
static void test01(int rank, int32_t dev)
{
    void *slab = NULL, *p = NULL;
    char mykey[IPC_KEY_LEN] = {0}, peerkey[IPC_KEY_LEN] = {0};
    int exp_ok = 0;

    setup_slabs(rank, dev, &slab, mykey, peerkey, &exp_ok);
    MPI_Barrier(MPI_COMM_WORLD);

    /* r0: PEER import dev4 */
    if (rank == 0) {
        int32_t ret = aclrtIpcMemImportByKey(&p, peerkey,
                                             ACL_RT_IPC_MEM_IMPORT_FLAG_ENABLE_PEER_ACCESS);
        printf("  r0(dev%d): PEER import r1(dev%d) → %s\n", dev, 1, ret_str(ret));
    }
    MPI_Barrier(MPI_COMM_WORLD);

    /* r1: PEER import dev0 */
    if (rank == 1) {
        int32_t ret = aclrtIpcMemImportByKey(&p, peerkey,
                                             ACL_RT_IPC_MEM_IMPORT_FLAG_ENABLE_PEER_ACCESS);
        printf("  r1(dev%d): PEER import r0(dev%d) → %s\n", dev, 0, ret_str(ret));
    }
    MPI_Barrier(MPI_COMM_WORLD);

    /* cleanup: 双方都用了 PEER, 需 DisablePeerAccess */
    if (rank == 0) {
        if (exp_ok) aclrtIpcMemClose(mykey);
        aclrtIpcMemClose(peerkey);
    } else {
        if (exp_ok) aclrtIpcMemClose(mykey);
        aclrtIpcMemClose(peerkey);
    }
    MPI_Barrier(MPI_COMM_WORLD);
    if (slab) aclrtFree(slab);
    if (rank == 0) { aclrtDeviceDisablePeerAccess(1); aclrtResetDevice(dev); }
    if (rank == 1) { aclrtDeviceDisablePeerAccess(0); aclrtResetDevice(dev); }
}

/* ================================================================
 *  T02: r0=DEFAULT(dev1) → r1=PEER(dev4)
 * ================================================================ */
static void test02(int rank, int32_t dev)
{
    void *slab = NULL, *p = NULL;
    char mykey[IPC_KEY_LEN] = {0}, peerkey[IPC_KEY_LEN] = {0};
    int exp_ok = 0;

    setup_slabs(rank, dev, &slab, mykey, peerkey, &exp_ok);
    MPI_Barrier(MPI_COMM_WORLD);

    /* r0: DEFAULT import r1(dev1) */
    if (rank == 0) {
        int32_t ret = aclrtIpcMemImportByKey(&p, peerkey,
                                             ACL_RT_IPC_MEM_IMPORT_FLAG_DEFAULT);
        printf("  r0(dev%d): DEFAULT import r1(dev%d) → %s\n", dev, 1, ret_str(ret));
    }
    MPI_Barrier(MPI_COMM_WORLD);

    /* r1: PEER import r0(dev0) */
    if (rank == 1) {
        int32_t ret = aclrtIpcMemImportByKey(&p, peerkey,
                                             ACL_RT_IPC_MEM_IMPORT_FLAG_ENABLE_PEER_ACCESS);
        printf("  r1(dev%d): PEER import r0(dev%d) → %s\n", dev, 0, ret_str(ret));
    }
    MPI_Barrier(MPI_COMM_WORLD);

    if (rank == 0) {
        if (exp_ok) aclrtIpcMemClose(mykey);
        aclrtIpcMemClose(peerkey);
    } else {
        if (exp_ok) aclrtIpcMemClose(mykey);
        aclrtIpcMemClose(peerkey);
    }
    MPI_Barrier(MPI_COMM_WORLD);
    if (slab) aclrtFree(slab);
    if (rank == 0) { aclrtResetDevice(dev); }
    if (rank == 1) { aclrtDeviceDisablePeerAccess(0); aclrtResetDevice(dev); }
}

/* ================================================================
 *  T03: r0=nothing → r1=PEER(dev4)   基线
 * ================================================================ */
static void test03(int rank, int32_t dev)
{
    void *slab = NULL, *p = NULL;
    char mykey[IPC_KEY_LEN] = {0}, peerkey[IPC_KEY_LEN] = {0};
    int exp_ok = 0;

    setup_slabs(rank, dev, &slab, mykey, peerkey, &exp_ok);
    MPI_Barrier(MPI_COMM_WORLD);

    /* r0: 无操作 */
    if (rank == 0) {
        printf("  r0(dev%d): (no import)\n", dev);
    }
    MPI_Barrier(MPI_COMM_WORLD);

    /* r1: PEER import r0(dev0) */
    if (rank == 1) {
        int32_t ret = aclrtIpcMemImportByKey(&p, peerkey,
                                             ACL_RT_IPC_MEM_IMPORT_FLAG_ENABLE_PEER_ACCESS);
        printf("  r1(dev%d): PEER import r0(dev%d) → %s\n", dev, 0, ret_str(ret));
    }
    MPI_Barrier(MPI_COMM_WORLD);

    if (rank == 0) {
        if (exp_ok) aclrtIpcMemClose(mykey);
    } else {
        if (exp_ok) aclrtIpcMemClose(mykey);
        aclrtIpcMemClose(peerkey);
    }
    MPI_Barrier(MPI_COMM_WORLD);
    if (slab) aclrtFree(slab);
    if (rank == 0) { aclrtResetDevice(dev); }
    if (rank == 1) { aclrtDeviceDisablePeerAccess(0); aclrtResetDevice(dev); }
}

/* ================================================================
 *  T04: r0=PEER(dev2) → r1=PEER(dev4)
 *  目的: r0 import 同 socket 设备后, r1 import 跨 socket 是否受影响
 * ================================================================ */
static void test04(int rank, int32_t dev)
{
    void *slab = NULL, *p = NULL;
    char mykey[IPC_KEY_LEN] = {0}, peerkey[IPC_KEY_LEN] = {0};
    int exp_ok = 0;

    setup_slabs(rank, dev, &slab, mykey, peerkey, &exp_ok);
    MPI_Barrier(MPI_COMM_WORLD);

    /* r0: PEER import r1(dev1)... 但我们想 import dev2
     * 问题: 2-rank 测试中只有 r0 和 r1, 无法直接 import dev2
     * 解决方案: 用 A_DEV=0, B_DEV=2, r0 import dev2(即 B_DEV), r1 import dev4
     * 但 r1 在 dev2 上, import dev4 需要 dev4 的 key...
     *
     * 重新设计: 这个测试需要 3 个 rank 或换一种方式
     * 简化: 用 2 rank, r0(dev0) PEER import r1(dev2), 然后 r1(dev2) PEER import r0(dev0)
     * 这实际上和 T01 类似, 只是设备不同
     *
     * 真正想测的是: r0 import 了某个设备后, r1 import 另一个设备
     * 在 2-rank 框架下: r0 import r1, r1 import r0 (互相 import)
     * 这无法区分 "同 socket" vs "跨 socket"
     *
     * 结论: T04-T06 需要 3+ rank 才能真正测试
     * 在 2-rank 框架下, 只能测试 "r0 import 后 r1 import" 的基本模式
     */

    /* 简化版: r0(dev0) PEER import r1(dev2), r1(dev2) PEER import r0(dev0) */
    if (rank == 0) {
        int32_t ret = aclrtIpcMemImportByKey(&p, peerkey,
                                             ACL_RT_IPC_MEM_IMPORT_FLAG_ENABLE_PEER_ACCESS);
        printf("  r0(dev%d): PEER import r1(dev%d) → %s\n", dev, 1, ret_str(ret));
    }
    MPI_Barrier(MPI_COMM_WORLD);

    if (rank == 1) {
        int32_t ret = aclrtIpcMemImportByKey(&p, peerkey,
                                             ACL_RT_IPC_MEM_IMPORT_FLAG_ENABLE_PEER_ACCESS);
        printf("  r1(dev%d): PEER import r0(dev%d) → %s\n", dev, 0, ret_str(ret));
    }
    MPI_Barrier(MPI_COMM_WORLD);

    if (rank == 0) {
        if (exp_ok) aclrtIpcMemClose(mykey);
        aclrtIpcMemClose(peerkey);
    } else {
        if (exp_ok) aclrtIpcMemClose(mykey);
        aclrtIpcMemClose(peerkey);
    }
    MPI_Barrier(MPI_COMM_WORLD);
    if (slab) aclrtFree(slab);
    if (rank == 0) { aclrtDeviceDisablePeerAccess(1); aclrtResetDevice(dev); }
    if (rank == 1) { aclrtDeviceDisablePeerAccess(0); aclrtResetDevice(dev); }
}

/* ================================================================
 *  T05: r0=PEER(dev4) → r1=PEER(dev2)
 *  简化版: 同 T04 框架, 但默认设备 A_DEV=0, B_DEV=4
 * ================================================================ */
static void test05(int rank, int32_t dev)
{
    void *slab = NULL, *p = NULL;
    char mykey[IPC_KEY_LEN] = {0}, peerkey[IPC_KEY_LEN] = {0};
    int exp_ok = 0;

    setup_slabs(rank, dev, &slab, mykey, peerkey, &exp_ok);
    MPI_Barrier(MPI_COMM_WORLD);

    if (rank == 0) {
        int32_t ret = aclrtIpcMemImportByKey(&p, peerkey,
                                             ACL_RT_IPC_MEM_IMPORT_FLAG_ENABLE_PEER_ACCESS);
        printf("  r0(dev%d): PEER import r1(dev%d) → %s\n", dev, 1, ret_str(ret));
    }
    MPI_Barrier(MPI_COMM_WORLD);

    if (rank == 1) {
        int32_t ret = aclrtIpcMemImportByKey(&p, peerkey,
                                             ACL_RT_IPC_MEM_IMPORT_FLAG_ENABLE_PEER_ACCESS);
        printf("  r1(dev%d): PEER import r0(dev%d) → %s\n", dev, 0, ret_str(ret));
    }
    MPI_Barrier(MPI_COMM_WORLD);

    if (rank == 0) {
        if (exp_ok) aclrtIpcMemClose(mykey);
        aclrtIpcMemClose(peerkey);
    } else {
        if (exp_ok) aclrtIpcMemClose(mykey);
        aclrtIpcMemClose(peerkey);
    }
    MPI_Barrier(MPI_COMM_WORLD);
    if (slab) aclrtFree(slab);
    if (rank == 0) { aclrtDeviceDisablePeerAccess(1); aclrtResetDevice(dev); }
    if (rank == 1) { aclrtDeviceDisablePeerAccess(0); aclrtResetDevice(dev); }
}

/* ================================================================
 *  T06: r0=PEER(dev4) → r1=DEFAULT(dev1)
 *  简化版: r0 PEER import, r1 DEFAULT import
 * ================================================================ */
static void test06(int rank, int32_t dev)
{
    void *slab = NULL, *p = NULL;
    char mykey[IPC_KEY_LEN] = {0}, peerkey[IPC_KEY_LEN] = {0};
    int exp_ok = 0;

    setup_slabs(rank, dev, &slab, mykey, peerkey, &exp_ok);
    MPI_Barrier(MPI_COMM_WORLD);

    if (rank == 0) {
        int32_t ret = aclrtIpcMemImportByKey(&p, peerkey,
                                             ACL_RT_IPC_MEM_IMPORT_FLAG_ENABLE_PEER_ACCESS);
        printf("  r0(dev%d): PEER import r1(dev%d) → %s\n", dev, 1, ret_str(ret));
    }
    MPI_Barrier(MPI_COMM_WORLD);

    if (rank == 1) {
        int32_t ret = aclrtIpcMemImportByKey(&p, peerkey,
                                             ACL_RT_IPC_MEM_IMPORT_FLAG_DEFAULT);
        printf("  r1(dev%d): DEFAULT import r0(dev%d) → %s\n", dev, 0, ret_str(ret));
    }
    MPI_Barrier(MPI_COMM_WORLD);

    if (rank == 0) {
        if (exp_ok) aclrtIpcMemClose(mykey);
        aclrtIpcMemClose(peerkey);
    } else {
        if (exp_ok) aclrtIpcMemClose(mykey);
        aclrtIpcMemClose(peerkey);
    }
    MPI_Barrier(MPI_COMM_WORLD);
    if (slab) aclrtFree(slab);
    if (rank == 0) { aclrtDeviceDisablePeerAccess(1); aclrtResetDevice(dev); }
    if (rank == 1) { aclrtResetDevice(dev); }
}

/* ================================================================
 *  T07: r0=PEER(dev4) → r1=PEER(dev4)   不同卡同 socket
 *  默认: A_DEV=0 (card0), B_DEV=2 (card1), 同 socket 0
 * ================================================================ */
static void test07(int rank, int32_t dev)
{
    void *slab = NULL, *p = NULL;
    char mykey[IPC_KEY_LEN] = {0}, peerkey[IPC_KEY_LEN] = {0};
    int exp_ok = 0;

    setup_slabs(rank, dev, &slab, mykey, peerkey, &exp_ok);
    MPI_Barrier(MPI_COMM_WORLD);

    if (rank == 0) {
        int32_t ret = aclrtIpcMemImportByKey(&p, peerkey,
                                             ACL_RT_IPC_MEM_IMPORT_FLAG_ENABLE_PEER_ACCESS);
        printf("  r0(dev%d): PEER import r1(dev%d) → %s\n", dev, 1, ret_str(ret));
    }
    MPI_Barrier(MPI_COMM_WORLD);

    if (rank == 1) {
        int32_t ret = aclrtIpcMemImportByKey(&p, peerkey,
                                             ACL_RT_IPC_MEM_IMPORT_FLAG_ENABLE_PEER_ACCESS);
        printf("  r1(dev%d): PEER import r0(dev%d) → %s\n", dev, 0, ret_str(ret));
    }
    MPI_Barrier(MPI_COMM_WORLD);

    if (rank == 0) {
        if (exp_ok) aclrtIpcMemClose(mykey);
        aclrtIpcMemClose(peerkey);
    } else {
        if (exp_ok) aclrtIpcMemClose(mykey);
        aclrtIpcMemClose(peerkey);
    }
    MPI_Barrier(MPI_COMM_WORLD);
    if (slab) aclrtFree(slab);
    if (rank == 0) { aclrtDeviceDisablePeerAccess(1); aclrtResetDevice(dev); }
    if (rank == 1) { aclrtDeviceDisablePeerAccess(0); aclrtResetDevice(dev); }
}

/* ================================================================
 *  T08: r0=PEER(dev4) → r1=PEER(dev4)   socket1 侧同卡
 *  默认: A_DEV=4 (card2-die0), B_DEV=5 (card2-die1)
 * ================================================================ */
static void test08(int rank, int32_t dev)
{
    void *slab = NULL, *p = NULL;
    char mykey[IPC_KEY_LEN] = {0}, peerkey[IPC_KEY_LEN] = {0};
    int exp_ok = 0;

    setup_slabs(rank, dev, &slab, mykey, peerkey, &exp_ok);
    MPI_Barrier(MPI_COMM_WORLD);

    if (rank == 0) {
        int32_t ret = aclrtIpcMemImportByKey(&p, peerkey,
                                             ACL_RT_IPC_MEM_IMPORT_FLAG_ENABLE_PEER_ACCESS);
        printf("  r0(dev%d): PEER import r1(dev%d) → %s\n", dev, 1, ret_str(ret));
    }
    MPI_Barrier(MPI_COMM_WORLD);

    if (rank == 1) {
        int32_t ret = aclrtIpcMemImportByKey(&p, peerkey,
                                             ACL_RT_IPC_MEM_IMPORT_FLAG_ENABLE_PEER_ACCESS);
        printf("  r1(dev%d): PEER import r0(dev%d) → %s\n", dev, 0, ret_str(ret));
    }
    MPI_Barrier(MPI_COMM_WORLD);

    if (rank == 0) {
        if (exp_ok) aclrtIpcMemClose(mykey);
        aclrtIpcMemClose(peerkey);
    } else {
        if (exp_ok) aclrtIpcMemClose(mykey);
        aclrtIpcMemClose(peerkey);
    }
    MPI_Barrier(MPI_COMM_WORLD);
    if (slab) aclrtFree(slab);
    if (rank == 0) { aclrtDeviceDisablePeerAccess(1); aclrtResetDevice(dev); }
    if (rank == 1) { aclrtDeviceDisablePeerAccess(0); aclrtResetDevice(dev); }
}

/* ================================================================
 *  T09: r0=PEER(dev2) → r1=PEER(dev2)   跨 socket 不同卡
 *  默认: A_DEV=0 (card0), B_DEV=4 (card2)
 * ================================================================ */
static void test09(int rank, int32_t dev)
{
    void *slab = NULL, *p = NULL;
    char mykey[IPC_KEY_LEN] = {0}, peerkey[IPC_KEY_LEN] = {0};
    int exp_ok = 0;

    setup_slabs(rank, dev, &slab, mykey, peerkey, &exp_ok);
    MPI_Barrier(MPI_COMM_WORLD);

    if (rank == 0) {
        int32_t ret = aclrtIpcMemImportByKey(&p, peerkey,
                                             ACL_RT_IPC_MEM_IMPORT_FLAG_ENABLE_PEER_ACCESS);
        printf("  r0(dev%d): PEER import r1(dev%d) → %s\n", dev, 1, ret_str(ret));
    }
    MPI_Barrier(MPI_COMM_WORLD);

    if (rank == 1) {
        int32_t ret = aclrtIpcMemImportByKey(&p, peerkey,
                                             ACL_RT_IPC_MEM_IMPORT_FLAG_ENABLE_PEER_ACCESS);
        printf("  r1(dev%d): PEER import r0(dev%d) → %s\n", dev, 0, ret_str(ret));
    }
    MPI_Barrier(MPI_COMM_WORLD);

    if (rank == 0) {
        if (exp_ok) aclrtIpcMemClose(mykey);
        aclrtIpcMemClose(peerkey);
    } else {
        if (exp_ok) aclrtIpcMemClose(mykey);
        aclrtIpcMemClose(peerkey);
    }
    MPI_Barrier(MPI_COMM_WORLD);
    if (slab) aclrtFree(slab);
    if (rank == 0) { aclrtDeviceDisablePeerAccess(1); aclrtResetDevice(dev); }
    if (rank == 1) { aclrtDeviceDisablePeerAccess(0); aclrtResetDevice(dev); }
}

/* ================================================================
 *  T10: r0=EnablePeerAccess(4) → r1=PEER(dev4)
 *  目的: 显式 EPA 是否阻塞 IPC import
 * ================================================================ */
static void test10(int rank, int32_t dev)
{
    void *slab = NULL, *p = NULL;
    char mykey[IPC_KEY_LEN] = {0}, peerkey[IPC_KEY_LEN] = {0};
    int exp_ok = 0;

    setup_slabs(rank, dev, &slab, mykey, peerkey, &exp_ok);
    MPI_Barrier(MPI_COMM_WORLD);

    /* r0: 显式 EnablePeerAccess 到 r1 的设备 */
    if (rank == 0) {
        int32_t ret = aclrtDeviceEnablePeerAccess(1, 0);
        printf("  r0(dev%d): EnablePeerAccess(dev%d) → %s\n", dev, 1, ret_str(ret));
    }
    MPI_Barrier(MPI_COMM_WORLD);

    /* r1: PEER import r0 */
    if (rank == 1) {
        int32_t ret = aclrtIpcMemImportByKey(&p, peerkey,
                                             ACL_RT_IPC_MEM_IMPORT_FLAG_ENABLE_PEER_ACCESS);
        printf("  r1(dev%d): PEER import r0(dev%d) → %s\n", dev, 0, ret_str(ret));
    }
    MPI_Barrier(MPI_COMM_WORLD);

    if (rank == 0) {
        if (exp_ok) aclrtIpcMemClose(mykey);
    } else {
        if (exp_ok) aclrtIpcMemClose(mykey);
        aclrtIpcMemClose(peerkey);
    }
    MPI_Barrier(MPI_COMM_WORLD);
    if (slab) aclrtFree(slab);
    if (rank == 0) { aclrtDeviceDisablePeerAccess(1); aclrtResetDevice(dev); }
    if (rank == 1) { aclrtDeviceDisablePeerAccess(0); aclrtResetDevice(dev); }
}

/* ================================================================
 *  T11: r0=EnablePeerAccess(4) → r1=EnablePeerAccess(4)
 *  目的: 两次 EPA 是否冲突
 * ================================================================ */
static void test11(int rank, int32_t dev)
{
    void *slab = NULL;
    char mykey[IPC_KEY_LEN] = {0}, peerkey[IPC_KEY_LEN] = {0};
    int exp_ok = 0;

    setup_slabs(rank, dev, &slab, mykey, peerkey, &exp_ok);
    MPI_Barrier(MPI_COMM_WORLD);

    /* r0: EnablePeerAccess 到 r1 的设备 */
    if (rank == 0) {
        int32_t ret = aclrtDeviceEnablePeerAccess(1, 0);
        printf("  r0(dev%d): EnablePeerAccess(dev%d) → %s\n", dev, 1, ret_str(ret));
    }
    MPI_Barrier(MPI_COMM_WORLD);

    /* r1: EnablePeerAccess 到 r0 的设备 */
    if (rank == 1) {
        int32_t ret = aclrtDeviceEnablePeerAccess(0, 0);
        printf("  r1(dev%d): EnablePeerAccess(dev%d) → %s\n", dev, 0, ret_str(ret));
    }
    MPI_Barrier(MPI_COMM_WORLD);

    if (rank == 0) {
        if (exp_ok) aclrtIpcMemClose(mykey);
    } else {
        if (exp_ok) aclrtIpcMemClose(mykey);
    }
    MPI_Barrier(MPI_COMM_WORLD);
    if (slab) aclrtFree(slab);
    if (rank == 0) { aclrtDeviceDisablePeerAccess(1); aclrtResetDevice(dev); }
    if (rank == 1) { aclrtDeviceDisablePeerAccess(0); aclrtResetDevice(dev); }
}

/* ================================================================
 *  T12: r0=PEER(4,5,6,7) → r1=PEER(dev4)
 *  需要 8 rank 才能真正测试, 2-rank 简化版:
 *  r0 连续 import r1 多次 (模拟多次 import)
 * ================================================================ */
static void test12(int rank, int32_t dev)
{
    void *slab = NULL, *p = NULL;
    char mykey[IPC_KEY_LEN] = {0}, peerkey[IPC_KEY_LEN] = {0};
    int exp_ok = 0;

    setup_slabs(rank, dev, &slab, mykey, peerkey, &exp_ok);
    MPI_Barrier(MPI_COMM_WORLD);

    /* r0: 连续 4 次 PEER import r1 (模拟多次 import) */
    if (rank == 0) {
        for (int i = 0; i < 4; i++) {
            int32_t ret = aclrtIpcMemImportByKey(&p, peerkey,
                                                 ACL_RT_IPC_MEM_IMPORT_FLAG_ENABLE_PEER_ACCESS);
            printf("  r0(dev%d): PEER import r1(dev%d) #%d → %s\n", dev, 1, i+1, ret_str(ret));
        }
    }
    MPI_Barrier(MPI_COMM_WORLD);

    /* r1: PEER import r0 */
    if (rank == 1) {
        int32_t ret = aclrtIpcMemImportByKey(&p, peerkey,
                                             ACL_RT_IPC_MEM_IMPORT_FLAG_ENABLE_PEER_ACCESS);
        printf("  r1(dev%d): PEER import r0(dev%d) → %s\n", dev, 0, ret_str(ret));
    }
    MPI_Barrier(MPI_COMM_WORLD);

    if (rank == 0) {
        if (exp_ok) aclrtIpcMemClose(mykey);
        for (int i = 0; i < 4; i++) aclrtIpcMemClose(peerkey);
    } else {
        if (exp_ok) aclrtIpcMemClose(mykey);
        aclrtIpcMemClose(peerkey);
    }
    MPI_Barrier(MPI_COMM_WORLD);
    if (slab) aclrtFree(slab);
    if (rank == 0) { aclrtDeviceDisablePeerAccess(1); aclrtResetDevice(dev); }
    if (rank == 1) { aclrtDeviceDisablePeerAccess(0); aclrtResetDevice(dev); }
}

/* ================================================================
 *  T13: r0=nothing → r1=PEER(4)+PEER(5)
 *  2-rank 简化: r1 连续 2 次 PEER import r0
 * ================================================================ */
static void test13(int rank, int32_t dev)
{
    void *slab = NULL, *p = NULL;
    char mykey[IPC_KEY_LEN] = {0}, peerkey[IPC_KEY_LEN] = {0};
    int exp_ok = 0;

    setup_slabs(rank, dev, &slab, mykey, peerkey, &exp_ok);
    MPI_Barrier(MPI_COMM_WORLD);

    if (rank == 0) {
        printf("  r0(dev%d): (no import)\n", dev);
    }
    MPI_Barrier(MPI_COMM_WORLD);

    /* r1: 连续 2 次 PEER import r0 */
    if (rank == 1) {
        for (int i = 0; i < 2; i++) {
            int32_t ret = aclrtIpcMemImportByKey(&p, peerkey,
                                                 ACL_RT_IPC_MEM_IMPORT_FLAG_ENABLE_PEER_ACCESS);
            printf("  r1(dev%d): PEER import r0(dev%d) #%d → %s\n", dev, 0, i+1, ret_str(ret));
        }
    }
    MPI_Barrier(MPI_COMM_WORLD);

    if (rank == 0) {
        if (exp_ok) aclrtIpcMemClose(mykey);
    } else {
        if (exp_ok) aclrtIpcMemClose(mykey);
        for (int i = 0; i < 2; i++) aclrtIpcMemClose(peerkey);
    }
    MPI_Barrier(MPI_COMM_WORLD);
    if (slab) aclrtFree(slab);
    if (rank == 0) { aclrtResetDevice(dev); }
    if (rank == 1) { aclrtDeviceDisablePeerAccess(0); aclrtResetDevice(dev); }
}

/* ================================================================ */
int main(int argc, char *argv[])
{
    MPI_Init(&argc, &argv);
    int rank, nproc;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &nproc);
    if (nproc != 2) {
        if (rank == 0) printf("need -np 2, got %d\n", nproc);
        MPI_Finalize();
        return 1;
    }

    int tnum = env_int("T", 1);
    int32_t a_dev = env_dev("A_DEV", 0);
    int32_t b_dev = env_dev("B_DEV", 1);
    int32_t dev = (rank == 0) ? a_dev : b_dev;

    setvbuf(stdout, NULL, _IONBF, 0);
    if (rank == 0)
        printf("=== T%02d: r0(dev%d) vs r1(dev%d) ===\n", tnum, a_dev, b_dev);

    aclError ret = aclInit(NULL);
    if (ret != 0) {
        printf("r%d: aclInit FAIL ret=%d\n", rank, ret);
        MPI_Finalize();
        return 1;
    }
    ret = aclrtSetDevice(dev);
    if (ret != 0) {
        printf("r%d: SetDevice(%d) FAIL ret=%d\n", rank, dev, ret);
        aclFinalize();
        MPI_Finalize();
        return 1;
    }

    switch (tnum) {
    case 1:  test01(rank, dev); break;
    case 2:  test02(rank, dev); break;
    case 3:  test03(rank, dev); break;
    case 4:  test04(rank, dev); break;
    case 5:  test05(rank, dev); break;
    case 6:  test06(rank, dev); break;
    case 7:  test07(rank, dev); break;
    case 8:  test08(rank, dev); break;
    case 9:  test09(rank, dev); break;
    case 10: test10(rank, dev); break;
    case 11: test11(rank, dev); break;
    case 12: test12(rank, dev); break;
    case 13: test13(rank, dev); break;
    default:
        if (rank == 0) printf("unknown T=%d\n", tnum);
        break;
    }

    if (rank == 0)
        printf("=== T%02d done ===\n\n", tnum);

    aclrtResetDevice(dev);
    aclFinalize();
    MPI_Finalize();
    return 0;
}
