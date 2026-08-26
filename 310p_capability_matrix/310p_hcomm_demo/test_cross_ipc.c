/*
 * test_cross_ipc.c — 2-rank cross-card IPC flag comparison on 310P Duo
 *
 * rank0 = dev0, rank1 = dev2 (cross-card)
 *
 * Test A: export=DISABLE_PID_VALIDATION, bare PID whitelist, import=DEFAULT(0UL)
 * Test B: export=DISABLE_PID_VALIDATION, bare PID whitelist, import=PEER_ACCESS(1UL)
 *
 * For each: record import ret, whether D2D works.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <mpi.h>
#include "acl/acl.h"

#define N               2
#define IPC_KEY_LEN     65
#define SLAB_SIZE       (4u * 1024u * 1024u)
#define SCRATCH_SIZE    (4u * 1024u * 1024u)
#define WARM_MALLOC     (4u * 1024u)

#define TAG_CREATE      0xC0
#define TAG_WRITE       0xC1

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
            printf("[xipc] r%d mismatch at word %zu: got 0x%08x, want 0x%08x\n",
                   rank, i, buf[i], pat(rank, tag, i));
            return -1;
        }
    }
    return 0;
}

static void do_d2d_test(void *ipc_va, void *slab, void *scratch,
                        uint32_t *hbuf, uint32_t *vbuf,
                        size_t words, size_t slab_size, size_t scratch_size,
                        aclrtStream stream, int rank, int peer,
                        const char *label, int *d2d_ok)
{
    int32_t ret;
    *d2d_ok = 0;

    fill_pat(hbuf, words, (uint32_t)rank, TAG_WRITE);
    ret = aclrtMemcpy(scratch, scratch_size, hbuf, slab_size, ACL_MEMCPY_HOST_TO_DEVICE);
    if (ret != ACL_ERROR_NONE) {
        printf("[xipc] r%d %s: H2D prep FAIL ret=%d\n", rank, label, ret);
        return;
    }

    ret = aclrtMemcpyAsync(ipc_va, slab_size, scratch, slab_size,
                           ACL_MEMCPY_DEVICE_TO_DEVICE, stream);
    if (ret == ACL_ERROR_NONE)
        ret = aclrtSynchronizeStream(stream);
    *d2d_ok = (ret == ACL_ERROR_NONE);
    printf("[xipc] r%d %s: D2D write %s (ret=%d)\n", rank, label, *d2d_ok ? "OK" : "FAIL", ret);

    int d2d_map[N];
    MPI_Allgather(d2d_ok, 1, MPI_INT, d2d_map, 1, MPI_INT, MPI_COMM_WORLD);
    if (d2d_map[peer]) {
        memset(vbuf, 0, slab_size);
        ret = aclrtMemcpy(vbuf, slab_size, slab, slab_size, ACL_MEMCPY_DEVICE_TO_HOST);
        if (ret == ACL_ERROR_NONE) {
            int vok = (check_pat(vbuf, words, (uint32_t)peer, TAG_WRITE) == 0);
            printf("[xipc] r%d %s: verify data from peer: %s\n", rank, label, vok ? "OK" : "FAIL");
        } else {
            printf("[xipc] r%d %s: D2H verify FAIL ret=%d\n", rank, label, ret);
        }
    } else {
        printf("[xipc] r%d %s: peer D2D did not deliver, skip verify\n", rank, label);
    }
}

int main(int argc, char *argv[])
{
    MPI_Init(&argc, &argv);
    int rank = 0, nproc = 0;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &nproc);
    if (nproc != N) {
        if (rank == 0)
            printf("[xipc] FATAL: need %d MPI procs, got %d\n", N, nproc);
        MPI_Finalize();
        return 1;
    }

    int32_t dev = (rank == 0) ? 0 : 2;
    int peer_dev = (rank == 0) ? 2 : 0;
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("[xipc] === cross-card IPC flag test: rank%d = dev%d (peer dev%d) ===\n", rank, dev, peer_dev);

    int32_t ret = 0;
    int init_ok = 0, dev_ok = 0;
    aclrtStream stream = NULL;
    void *slab = NULL, *scratch = NULL, *warm = NULL;
    uint32_t *hbuf = NULL, *vbuf = NULL;
    char mykey[IPC_KEY_LEN] = {0};
    char keys[N][IPC_KEY_LEN];
    size_t words = SLAB_SIZE / sizeof(uint32_t);
    int fatal = 0;

    memset(keys, 0, sizeof(keys));

    ret = aclInit(NULL);
    if (ret != ACL_ERROR_NONE) {
        printf("[xipc] r%d aclInit FAIL ret=%d\n", rank, ret);
        fatal = 1;
    } else {
        init_ok = 1;
        ret = aclrtSetDevice(dev);
        if (ret != ACL_ERROR_NONE) {
            printf("[xipc] r%d SetDevice(%d) FAIL ret=%d\n", rank, dev, ret);
            fatal = 1;
        }
    }

    if (!fatal) {
        dev_ok = 1;
        aclrtMalloc(&warm, WARM_MALLOC, ACL_MEM_MALLOC_HUGE_FIRST);
        ret = aclrtCreateStream(&stream);
        if (ret != ACL_ERROR_NONE) {
            printf("[xipc] r%d CreateStream FAIL ret=%d\n", rank, ret);
            fatal = 1;
        }
    }
    if (!fatal) {
        ret = aclrtMalloc(&scratch, SCRATCH_SIZE, ACL_MEM_MALLOC_HUGE_FIRST);
        if (ret != ACL_ERROR_NONE) {
            printf("[xipc] r%d malloc scratch FAIL ret=%d\n", rank, ret);
            fatal = 1;
        }
    }
    if (!fatal) {
        hbuf = (uint32_t *)malloc(SLAB_SIZE);
        vbuf = (uint32_t *)malloc(SLAB_SIZE);
        if (!hbuf || !vbuf) {
            printf("[xipc] r%d host malloc FAIL\n", rank);
            fatal = 1;
        }
    }
    if (!fatal) {
        ret = aclrtMalloc(&slab, SLAB_SIZE, ACL_MEM_MALLOC_HUGE_FIRST_P2P);
        if (ret != ACL_ERROR_NONE) {
            printf("[xipc] r%d malloc slab FAIL ret=%d\n", rank, ret);
            fatal = 1;
        }
    }

    if (!fatal) {
        fill_pat(hbuf, words, (uint32_t)rank, TAG_CREATE);
        ret = aclrtMemcpy(slab, SLAB_SIZE, hbuf, SLAB_SIZE, ACL_MEMCPY_HOST_TO_DEVICE);
        if (ret != ACL_ERROR_NONE) {
            printf("[xipc] r%d H2D FAIL ret=%d\n", rank, ret);
            fatal = 1;
        }
    }

    if (!fatal) {
        ret = aclrtIpcMemGetExportKey(slab, SLAB_SIZE, mykey, IPC_KEY_LEN,
                                      ACL_RT_IPC_MEM_EXPORT_FLAG_DISABLE_PID_VALIDATION);
        if (ret != ACL_ERROR_NONE) {
            printf("[xipc] r%d export key FAIL ret=%d\n", rank, ret);
            fatal = 1;
        }
    }

    int32_t my_bare_pid = 0;
    if (!fatal) {
        ret = aclrtDeviceGetBareTgid(&my_bare_pid);
        if (ret != ACL_ERROR_NONE) {
            printf("[xipc] r%d GetBareTgid FAIL ret=%d, fallback getpid\n", rank, ret);
            my_bare_pid = (int32_t)getpid();
        }
    }

    int32_t all_bare_pids[N];
    MPI_Allgather(&my_bare_pid, 1, MPI_INT32_T, all_bare_pids, 1, MPI_INT32_T, MPI_COMM_WORLD);

    if (!fatal) {
        int peer = 1 - rank;
        ret = aclrtIpcMemSetImportPid(mykey, &all_bare_pids[peer], 1);
        if (ret != ACL_ERROR_NONE) {
            printf("[xipc] r%d SetImportPid(peer bare=%d) FAIL ret=%d\n", rank, all_bare_pids[peer], ret);
            fatal = 1;
        } else {
            printf("[xipc] r%d SetImportPid(peer bare=%d) OK\n", rank, all_bare_pids[peer]);
        }
    }

    MPI_Allgather(mykey, IPC_KEY_LEN, MPI_CHAR,
                  &keys[0][0], IPC_KEY_LEN, MPI_CHAR, MPI_COMM_WORLD);
    MPI_Barrier(MPI_COMM_WORLD);

    int peer = 1 - rank;

    /* ======== Test A: DEFAULT flag (0UL) ======== */
    printf("[xipc] r%d ===== Test A: import with DEFAULT flag (0UL) =====\n", rank);
    void *pv_default = NULL;
    int32_t ret_default = aclrtIpcMemImportByKey(&pv_default, keys[peer], 0UL);
    int ok_default = (ret_default == ACL_ERROR_NONE);
    printf("[xipc] r%d Test A: import DEFAULT ret=%d %s va=%p\n",
           rank, ret_default, ok_default ? "OK" : "FAIL", pv_default);

    int d2d_default_ok = 0;
    if (ok_default) {
        do_d2d_test(pv_default, slab, scratch, hbuf, vbuf,
                    words, SLAB_SIZE, SCRATCH_SIZE, stream, rank, peer,
                    "Test A", &d2d_default_ok);
        MPI_Barrier(MPI_COMM_WORLD);
        aclrtIpcMemClose(keys[peer]);
        MPI_Barrier(MPI_COMM_WORLD);
    } else {
        MPI_Barrier(MPI_COMM_WORLD);
        MPI_Barrier(MPI_COMM_WORLD);
    }

    /* ======== Test B: PEER_ACCESS flag (1UL) ======== */
    printf("[xipc] r%d ===== Test B: import with PEER_ACCESS flag (1UL) =====\n", rank);
    void *pv_peer = NULL;
    int32_t ret_peer = aclrtIpcMemImportByKey(&pv_peer, keys[peer],
                                               ACL_RT_IPC_MEM_IMPORT_FLAG_ENABLE_PEER_ACCESS);
    int ok_peer = (ret_peer == ACL_ERROR_NONE);
    printf("[xipc] r%d Test B: import PEER_ACCESS ret=%d %s va=%p\n",
           rank, ret_peer, ok_peer ? "OK" : "FAIL", pv_peer);

    int d2d_peer_ok = 0;
    if (ok_peer) {
        do_d2d_test(pv_peer, slab, scratch, hbuf, vbuf,
                    words, SLAB_SIZE, SCRATCH_SIZE, stream, rank, peer,
                    "Test B", &d2d_peer_ok);
        MPI_Barrier(MPI_COMM_WORLD);
        aclrtIpcMemClose(keys[peer]);
        MPI_Barrier(MPI_COMM_WORLD);
        aclrtDeviceDisablePeerAccess(peer_dev);
        printf("[xipc] r%d Test B: DisablePeerAccess(dev%d) done\n", rank, peer_dev);
    } else {
        MPI_Barrier(MPI_COMM_WORLD);
        MPI_Barrier(MPI_COMM_WORLD);
    }

    /* ======== Summary ======== */
    int local_results[4] = { ret_default, ok_default, ret_peer, ok_peer };
    int global_results[N * 4];
    MPI_Allgather(local_results, 4, MPI_INT, global_results, 4, MPI_INT, MPI_COMM_WORLD);
    MPI_Barrier(MPI_COMM_WORLD);

    if (rank == 0) {
        printf("\n");
        printf("[xipc] ============================================\n");
        printf("[xipc]  SUMMARY: cross-card IPC flag comparison\n");
        printf("[xipc]  rank0=dev0, rank1=dev2 (310P Duo)\n");
        printf("[xipc]  export: DISABLE_PID_VALIDATION + bare PID whitelist\n");
        printf("[xipc] --------------------------------------------\n");
        for (int r = 0; r < N; r++) {
            int r_ret_def = global_results[r * 4 + 0];
            int r_ok_def  = global_results[r * 4 + 1];
            int r_ret_pe  = global_results[r * 4 + 2];
            int r_ok_pe   = global_results[r * 4 + 3];
            printf("[xipc]  rank%d: DEFAULT    import ret=%d  %s\n", r, r_ret_def, r_ok_def ? "PASS" : "FAIL");
            printf("[xipc]  rank%d: PEER_ACCESS import ret=%d  %s\n", r, r_ret_pe, r_ok_pe ? "PASS" : "FAIL");
        }
        printf("[xipc] --------------------------------------------\n");
        printf("[xipc]  Conclusion:\n");
        if (global_results[1] && global_results[5])
            printf("[xipc]    DEFAULT flag SUPPORTS cross-card IPC import\n");
        else if (!global_results[1] && !global_results[5])
            printf("[xipc]    DEFAULT flag does NOT support cross-card IPC import\n");
        else
            printf("[xipc]    DEFAULT flag behavior is ASYMMETRIC across ranks\n");

        if (global_results[3] && global_results[7])
            printf("[xipc]    PEER_ACCESS flag SUPPORTS cross-card IPC import\n");
        else
            printf("[xipc]    PEER_ACCESS flag does NOT support cross-card IPC import\n");
        printf("[xipc] ============================================\n");
    }
    MPI_Barrier(MPI_COMM_WORLD);

    /* cleanup */
    aclrtIpcMemClose(mykey);
    MPI_Barrier(MPI_COMM_WORLD);
    if (slab) aclrtFree(slab);
    if (scratch) aclrtFree(scratch);
    if (warm) aclrtFree(warm);
    if (stream) aclrtDestroyStream(stream);
    free(hbuf);
    free(vbuf);
    if (dev_ok) aclrtResetDevice(dev);
    if (init_ok) aclFinalize();

    MPI_Finalize();
    return 0;
}
