/*
 * 15_peer_needed — 跨卡 IPC 是否需要 enable peer(干净对照)
 * Atlas 300I Duo (310P3, 4 卡 x 2 die = 8 dev), CANN 9.1.0, aarch64
 * MPI 双进程: rank0=exporter(dev A, A_DEV 默认 0)
 *             rank1=importer(dev B, B_DEV 默认 2)  <- 跨卡
 *
 * 目的: 干净回答"跨卡 IPC import 是否需要 enable peer"。
 *
 * MODE(环境变量, 默认 DEFAULT), 每次独立 mpirun:
 *   DEFAULT      ImportByKey(key, DEFAULT), 不做任何 enable              -> 507899
 *   ENABLE       rank1 单向 aclrtDeviceEnablePeerAccess(A_DEV) 再 import  -> 仍 507899
 *   ENABLE_R0    rank0 单向 aclrtDeviceEnablePeerAccess(B_DEV) 再 import  -> 仍 507899
 *   ENABLE_BOTH  双向 aclrtDeviceEnablePeerAccess 再 DEFAULT import       -> import 0 + D2D OK
 *   PEER         ImportByKey(key, ENABLE_PEER_ACCESS)                    -> import 0 + D2D OK
 *
 * 结论(源码级, devdrv_ctrl.c:4588):
 *   PCIe 跨卡 P2P 的硬件配置(devdrv_p2p_cfg_enable: IOMMU+TX ATU+P2P消息通道,
 *   双向一次配好)只在两个方向的软件 ref 都>0 时才下发
 *   ("Both directions are configured to take effect")。
 *   故单向 enable 必然不够; PEER flag 内部 EnableP2PWithOtherDevice 正是做了双向。
 *   HCCS 同卡不需要 ATU(devdrv_init_p2p_io_mem 对 HCCS return 0), 同卡 IPC 免 enable。
 * cleanup: ENABLE 模式做 DisablePeerAccess 拆 P2P(防 D 态挂死)。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <mpi.h>
#include "acl/acl.h"

#define IPC_KEY_LEN  65
#define MEM_SIZE     (16u * 1024u * 1024u)  /* 16 MB */
#define TAG          0x1

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
            printf("[15]   [verify] mismatch at word %zu: got 0x%08x, want 0x%08x\n",
                   i, buf[i], pat(rank, tag, i));
            return -1;
        }
    }
    return 0;
}

static int32_t env_dev(const char *name, int32_t dflt)
{
    const char *s = getenv(name);
    return s ? (int32_t)atoi(s) : dflt;
}

int main(int argc, char *argv[])
{
    MPI_Init(&argc, &argv);
    int rank = 0, nproc = 0;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &nproc);
    if (nproc != 2) {
        if (rank == 0)
            printf("[15] FATAL: need exactly 2 MPI procs, got %d\n", nproc);
        MPI_Finalize();
        return 1;
    }

    const char *role = (rank == 0) ? "r0/exporter" : "r1/importer";
    int32_t dev = (rank == 0) ? env_dev("A_DEV", 0) : env_dev("B_DEV", 2);
    const char *mode = getenv("MODE");
    if (!mode || !*mode)
        mode = "DEFAULT";

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("[15] === 15_peer_needed: MODE=%s, rank%d %s dev%d ===\n", mode, rank, role, dev);

    aclrtStream stream = NULL;
    void *da = NULL, *p = NULL, *loc = NULL;
    uint32_t *hbuf = NULL, *vbuf = NULL;
    char key1[IPC_KEY_LEN] = {0};
    int32_t ret = 0;
    int init_ok = 0, dev_ok = 0, exp_ok = 0, imp_ok = 0, d2d_ok = 0, peer_en = 0, peer_en0 = 0;
    int fatal = 0;
    size_t words = MEM_SIZE / sizeof(uint32_t);

    ret = aclInit(NULL);
    if (ret != ACL_ERROR_NONE) {
        printf("[15] init: %s aclInit FAIL ret=%d\n", role, ret);
        fatal = 1;
    } else {
        init_ok = 1;
        ret = aclrtSetDevice(dev);
        if (ret != ACL_ERROR_NONE) {
            printf("[15] init: %s aclrtSetDevice(%d) FAIL ret=%d\n", role, dev, ret);
            fatal = 1;
        }
    }
    if (!fatal) {
        dev_ok = 1;
        if (rank == 1) {
            ret = aclrtCreateStream(&stream);
            if (ret != ACL_ERROR_NONE) {
                printf("[15] init: r1 aclrtCreateStream FAIL ret=%d\n", ret);
                fatal = 1;
            }
            if (!fatal) {
                ret = aclrtMalloc((void **)&loc, MEM_SIZE, ACL_MEM_MALLOC_HUGE_FIRST);
                if (ret != ACL_ERROR_NONE) {
                    printf("[15] init: r1 aclrtMalloc(loc 16MB device) FAIL ret=%d\n", ret);
                    fatal = 1;
                }
            }
        }
        if (!fatal) {
            hbuf = (uint32_t *)malloc(MEM_SIZE);
            vbuf = (uint32_t *)malloc(MEM_SIZE);
            if (!hbuf || !vbuf) {
                printf("[15] init: %s host malloc FAIL\n", role);
                fatal = 1;
            }
        }
    }
    if (!fatal)
        printf("[15] init: %s aclInit/SetDevice(%d)%s OK\n", role, dev,
               (rank == 1) ? "/CreateStream" : "");
    {
        int st = fatal;
        MPI_Allreduce(MPI_IN_PLACE, &st, 1, MPI_INT, MPI_MAX, MPI_COMM_WORLD);
        fatal = st;
    }

    /* phase 1: exporter malloc + H2D pattern + export key + send */
    if (!fatal) {
        if (rank == 0) {
            int ok = 0;
            ret = aclrtMalloc(&da, MEM_SIZE, ACL_MEM_MALLOC_HUGE_FIRST_P2P);
            if (ret == ACL_ERROR_NONE) {
                fill_pat(hbuf, words, 0, TAG);
                ret = aclrtMemcpy(da, MEM_SIZE, hbuf, MEM_SIZE, ACL_MEMCPY_HOST_TO_DEVICE);
            }
            if (ret == ACL_ERROR_NONE)
                ret = aclrtIpcMemGetExportKey(da, MEM_SIZE, key1, IPC_KEY_LEN,
                                              ACL_RT_IPC_MEM_EXPORT_FLAG_DISABLE_PID_VALIDATION);
            if (ret == ACL_ERROR_NONE) {
                exp_ok = 1;
                if (MPI_Send(key1, IPC_KEY_LEN, MPI_CHAR, 1, 100, MPI_COMM_WORLD) == MPI_SUCCESS)
                    ok = 1;
            }
            printf("[15] step 1 (r0): malloc P2P 16MB + H2D pattern + export key + MPI-send %s (ret=%d)\n",
                   ok ? "OK" : "FAIL", ret);
            if (!ok)
                fatal = 1;
        } else {
            MPI_Recv(key1, IPC_KEY_LEN, MPI_CHAR, 0, 100, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        }
    }
    {
        int st = fatal;
        MPI_Allreduce(MPI_IN_PLACE, &st, 1, MPI_INT, MPI_MAX, MPI_COMM_WORLD);
        fatal = st;
    }

    /* phase 1.5: ENABLE_BOTH/ENABLE_R0 P2P setup (R0 = exporter-side only) */
    if (!fatal && (strcmp(mode, "ENABLE_BOTH") == 0 || strcmp(mode, "ENABLE_R0") == 0)) {
        if (rank == 0) {
            int32_t peer = env_dev("B_DEV", 2);
            int32_t er = aclrtDeviceEnablePeerAccess(peer, 0);
            printf("[15] step 1.5 (r0): EnablePeerAccess(%d) ret=%d\n", peer, er);
            if (er == ACL_ERROR_NONE)
                peer_en0 = 1;
        } else if (strcmp(mode, "ENABLE_BOTH") == 0) {
            int32_t peer = env_dev("A_DEV", 0);
            int32_t er = aclrtDeviceEnablePeerAccess(peer, 0);
            printf("[15] step 1.5 (r1): EnablePeerAccess(%d) ret=%d\n", peer, er);
            if (er == ACL_ERROR_NONE)
                peer_en = 1;
        }
        MPI_Barrier(MPI_COMM_WORLD);
        int res_p[2] = { peer_en0, peer_en };
        MPI_Allreduce(MPI_IN_PLACE, res_p, 2, MPI_INT, MPI_MAX, MPI_COMM_WORLD);
        peer_en0 = res_p[0];
        peer_en = res_p[1];
    }

    /* phase 2: importer import by MODE + async D2D verify */
    if (!fatal && rank == 1) {
        uint64_t flag = ACL_RT_IPC_MEM_IMPORT_FLAG_DEFAULT;
        if (strcmp(mode, "PEER") == 0)
            flag = ACL_RT_IPC_MEM_IMPORT_FLAG_ENABLE_PEER_ACCESS;
        if (strcmp(mode, "ENABLE") == 0) {
            int32_t peer = env_dev("A_DEV", 0);
            int32_t er = aclrtDeviceEnablePeerAccess(peer, 0);
            printf("[15] step 2 (r1): EnablePeerAccess(%d) ret=%d\n", peer, er);
            if (er == ACL_ERROR_NONE)
                peer_en = 1;
        }
        int32_t ir = aclrtIpcMemImportByKey(&p, key1, flag);
        printf("[15] step 2 (r1): import MODE=%s flag=0x%lx ret=%d\n",
               mode, (unsigned long)flag, ir);
        if (ir == ACL_ERROR_NONE) {
            imp_ok = 1;
            int32_t r2 = aclrtMemcpyAsync(loc, MEM_SIZE, p, MEM_SIZE,
                                          ACL_MEMCPY_DEVICE_TO_DEVICE, stream);
            if (r2 == ACL_ERROR_NONE)
                r2 = aclrtSynchronizeStream(stream);
            if (r2 == ACL_ERROR_NONE) {
                memset(vbuf, 0, MEM_SIZE);
                r2 = aclrtMemcpy(vbuf, MEM_SIZE, loc, MEM_SIZE, ACL_MEMCPY_DEVICE_TO_HOST);
                if (r2 == ACL_ERROR_NONE)
                    d2d_ok = (check_pat(vbuf, words, 0, TAG) == 0);
            }
            printf("[15] step 2 (r1): async D2D read + D2H verify %s (ret=%d)\n",
                   d2d_ok ? "OK" : "FAIL", r2);
        }
    }
    {
        int res[4] = { imp_ok, d2d_ok, peer_en, peer_en0 };
        MPI_Allreduce(MPI_IN_PLACE, res, 4, MPI_INT, MPI_MAX, MPI_COMM_WORLD);
        imp_ok = res[0];
        d2d_ok = res[1];
        peer_en = res[2];
        peer_en0 = res[3];
    }

    /* cleanup */
    if (rank == 1) {
        if (imp_ok) {
            int32_t cr = aclrtIpcMemClose(key1);
            printf("[15] cleanup (r1): IpcMemClose(key1) ret=%d\n", cr);
        }
        if (peer_en) {
            int32_t peer = env_dev("A_DEV", 0);
            int32_t dr = aclrtDeviceDisablePeerAccess(peer);
            printf("[15] cleanup (r1): DisablePeerAccess(%d) ret=%d\n", peer, dr);
        }
    } else {
        if (exp_ok) {
            int32_t cr = aclrtIpcMemClose(key1);
            printf("[15] cleanup (r0): IpcMemClose(key1) ret=%d\n", cr);
        }
        if (peer_en0) {
            int32_t bdev = env_dev("B_DEV", 2);
            int32_t dr = aclrtDeviceDisablePeerAccess(bdev);
            printf("[15] cleanup (r0): DisablePeerAccess(%d) ret=%d\n", bdev, dr);
        }
    }
    MPI_Barrier(MPI_COMM_WORLD);
    if (rank == 0 && da)
        aclrtFree(da);
    if (rank == 1 && loc)
        aclrtFree(loc);
    if (stream)
        aclrtDestroyStream(stream);
    free(hbuf);
    free(vbuf);
    if (dev_ok)
        aclrtResetDevice(dev);
    if (init_ok)
        aclFinalize();

    /* RESULT */
    {
        int pass = 0;
        const char *verdict = "";
        if (strcmp(mode, "DEFAULT") == 0) {
            pass = !imp_ok;
            verdict = pass ? "符合预期: 裸 DEFAULT 跨卡 507899(需 PEER flag)"
                           : "★推翻: 裸 DEFAULT 跨卡可用";
        } else if (strcmp(mode, "ENABLE") == 0) {
            pass = !imp_ok;
            if (pass)
                verdict = peer_en
                    ? "符合预期: 显式 EnablePeerAccess 不够, DEFAULT flag 跨卡仍 507899"
                    : "符合预期(弱): EnablePeerAccess 本身失败, DEFAULT 跨卡 507899, 无法证明 enable 够不够";
            else
                verdict = "★推翻: 显式 enable 让 DEFAULT import 可用";
        } else if (strcmp(mode, "ENABLE_R0") == 0) {
            pass = imp_ok && d2d_ok;
            if (pass)
                verdict = "★方向确认: 仅 exporter 端 enable(dev0 调 EnablePeerAccess(2)) 即可让 DEFAULT import 成功";
            else
                verdict = "仅 exporter 端 enable 不够, 需要双向(或 importer 端方向)";
        } else if (strcmp(mode, "ENABLE_BOTH") == 0) {
            pass = imp_ok && d2d_ok;
            if (!imp_ok && !peer_en0 && !peer_en)
                verdict = "无法判定: 双向 EnablePeerAccess 均失败";
            else if (pass)
                verdict = "★重要发现: 双向 EnablePeerAccess + DEFAULT flag 跨卡可用, 说明 PEER flag 内部核心就是双向 P2P";
            else
                verdict = "符合预期: 双向 EnablePeerAccess 仍不够, PEER flag 还做了更多";
        } else { /* PEER */
            pass = imp_ok && d2d_ok;
            verdict = pass ? "符合预期: PEER flag import 是跨卡唯一可行方式"
                           : "不符合预期: PEER flag 不可用";
        }
        printf("[15] RESULT: MODE=%-7s import=%-10s d2d=%-5s -> %s\n",
               mode, imp_ok ? "OK" : "FAIL", d2d_ok ? "OK" : "-", verdict);
        fflush(stdout);
        if (fatal || !pass)
            fatal = 1;
    }

    MPI_Finalize();
    return fatal ? 1 : 0;
}
