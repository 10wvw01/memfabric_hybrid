/*
 * 310p_hcomm_demo — 严格复现 hcomm Ascend910 平台的 IPC 实现
 *
 * 拓扑: 单个 socket 上的 2 张 Duo 卡 = 4 个 die
 *   Socket 0: card0 (dev0, dev1) + card1 (dev2, dev3)
 *   rank0=dev0, rank1=dev1, rank2=dev2, rank3=dev3
 *   die pair: (0,1), (2,3)
 *   跨卡: 0↔2, 0↔3, 1↔2, 1↔3 (PCIe)
 *
 * 严格匹配 hcomm Ascend910 (legacy) 的 IPC 实现:
 *   1. 内存分配: aclrtMalloc(ACL_MEM_MALLOC_HUGE_FIRST) — 注意: 不是 P2P
 *   2. 页对齐: aclrtPointerGetAttributes → 向下对齐到 pageSize
 *   3. Export: aclrtIpcMemGetExportKey(aligned_ptr, aligned_size, key, 65, 0UL)
 *   4. PID 白名单: aclrtDeviceGetBareTgid → aclrtIpcMemSetImportPid
 *   5. 交换: MPI_Allgather (key + offset + bare_pid)
 *   6. Import: aclrtIpcMemImportByKey(&ptr, key, 0UL) — flag=0 (DEFAULT)
 *   7. 偏移恢复: ptr = imported_ptr + offset
 *   8. 传输: aclrtMemcpyAsync(D2D)
 *   9. Cleanup: aclrtIpcMemClose → aclrtFree → aclrtResetDevice
 *      注意: 不需要 DisablePeerAccess (flag=0 不建立 P2P)
 *
 * hcomm 源码参考:
 *   adapter_rts.cc:1244 — hrtIpcSetMemoryName: aclrtIpcMemGetExportKey(..., 0UL)
 *   adapter_rts.cc:1324 — hrtIpcOpenMemory: aclrtIpcMemImportByKey(..., 0UL)
 *   adapter_rts.cc:1357 — hrtIpcSetMemoryPid: aclrtIpcMemSetImportPid
 *   adapter_rts.cc:1408 — hrtDevMemAlignWithPage
 *   adapter_rts.cc:2114 — hrtDeviceGetBareTgid: aclrtDeviceGetBareTgid
 *   mem_name_repository.cc:41-163 — SetIpcMem (对齐 + export + PID)
 *   mem_name_repository.cc:165-228 — OpenIpcMem (import + offset)
 *   local_ipc_rma_buffer_impl.cc:107-123 — Grant (bare PID 白名单)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <mpi.h>
#include "acl/acl.h"

#define N               4
#define IPC_KEY_LEN     65
#define SLAB_SIZE       (16u * 1024u * 1024u)   /* 16 MB */
#define SCRATCH_SIZE    (16u * 1024u * 1024u)   /* 16 MB */

#define TAG_CREATE      0xA0
#define TAG_WRITE_BASE  0xB0

/* rank → device 映射: 单个 socket 上的 2 张 Duo 卡 */
static const int32_t dev_map[N] = {0, 1, 2, 3};

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

static int check_pat(const uint32_t *buf, size_t words, uint32_t rank, uint32_t tag, int who)
{
    for (size_t i = 0; i < words; i++) {
        if (buf[i] != pat(rank, tag, i)) {
            printf("[hcomm] r%d [verify] mismatch at word %zu: got 0x%08x, want 0x%08x\n",
                   who, i, buf[i], pat(rank, tag, i));
            return -1;
        }
    }
    return 0;
}

static int is_die_pair(int i, int j)
{
    return i != j && (dev_map[i] / 2) == (dev_map[j] / 2);
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

int main(int argc, char *argv[])
{
    MPI_Init(&argc, &argv);
    int rank = 0, nproc = 0;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &nproc);
    if (nproc != N) {
        if (rank == 0)
            printf("[hcomm] FATAL: need exactly %d MPI procs, got %d\n", N, nproc);
        MPI_Finalize();
        return 1;
    }

    int32_t dev = dev_map[rank];
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("[hcomm] === hcomm-style IPC: 4 NPU (2 Duo cards, 1 socket), rank%d = dev%d ===\n",
           rank, dev);

    aclrtStream stream = NULL;
    void *slab = NULL, *scratch = NULL;
    void *aligned_ptr = NULL;
    size_t aligned_size = 0;
    uint32_t *hbuf = NULL, *vbuf = NULL;
    char mykey[IPC_KEY_LEN] = {0};
    char keys[N][IPC_KEY_LEN];
    void *ipc_va[N];
    int imp_ok[N];
    int imp_ret[N];
    int fatal = 0, join_ok = 0, write_ok = 0;
    int32_t ret = 0;
    int init_ok = 0, dev_ok = 0;
    size_t words = SLAB_SIZE / sizeof(uint32_t);

    memset(keys, 0, sizeof(keys));
    for (int i = 0; i < N; i++) { ipc_va[i] = NULL; imp_ok[i] = 0; imp_ret[i] = -999; }

    /* ================================================================
     * Phase 1: init + 内存分配
     * hcomm: hrtMalloc → aclrtMallocWithCfg(ACL_MEM_TYPE_HIGH_BAND_WIDTH | ACL_MEM_MALLOC_HUGE_FIRST)
     * 简化: aclrtMalloc(ACL_MEM_MALLOC_HUGE_FIRST) — 注意: 不是 P2P
     * ================================================================ */
    ret = aclInit(NULL);
    if (ret != ACL_ERROR_NONE) {
        printf("[hcomm] r%d init: aclInit FAIL ret=%d\n", rank, ret);
        BAIL(fatal);
    } else {
        init_ok = 1;
        ret = aclrtSetDevice(dev);
        if (ret != ACL_ERROR_NONE) {
            printf("[hcomm] r%d init: aclrtSetDevice(%d) FAIL ret=%d\n", rank, dev, ret);
            BAIL(fatal);
        }
    }
    if (!fatal) {
        dev_ok = 1;
        ret = aclrtCreateStream(&stream);
        if (ret != ACL_ERROR_NONE) {
            printf("[hcomm] r%d init: aclrtCreateStream FAIL ret=%d\n", rank, ret);
            BAIL(fatal);
        }
    }
    if (!fatal) {
        ret = aclrtMalloc(&scratch, SCRATCH_SIZE, ACL_MEM_MALLOC_HUGE_FIRST);
        if (ret != ACL_ERROR_NONE) {
            printf("[hcomm] r%d init: malloc scratch FAIL ret=%d\n", rank, ret);
            BAIL(fatal);
        }
    }
    if (!fatal) {
        hbuf = (uint32_t *)malloc(SLAB_SIZE);
        vbuf = (uint32_t *)malloc(SLAB_SIZE);
        if (!hbuf || !vbuf) {
            printf("[hcomm] r%d init: host malloc FAIL\n", rank);
            BAIL(fatal);
        }
    }
    if (!fatal) {
        /* hcomm 使用 ACL_MEM_MALLOC_HUGE_FIRST (不是 P2P) */
        ret = aclrtMalloc(&slab, SLAB_SIZE, ACL_MEM_MALLOC_HUGE_FIRST);
        if (ret != ACL_ERROR_NONE) {
            printf("[hcomm] r%d create: malloc slab FAIL ret=%d\n", rank, ret);
            BAIL(fatal);
        }
        printf("[hcomm] r%d create: slab @ %p (ACL_MEM_MALLOC_HUGE_FIRST, not P2P)\n", rank, slab);
    }

    /* ================================================================
     * Phase 2: 页对齐 (hcomm: hrtDevMemAlignWithPage)
     * hcomm 源码: adapter_rts.cc:1408-1461
     *   pageSize = aclrtPointerGetAttributes(ptr)->pageSize
     *   aligned_ptr = ptr & ~(pageSize - 1)
     *   aligned_size = size + offset
     * ================================================================ */
    uint64_t my_offset = 0;
    if (!fatal) {
        aclrtPtrAttributes attrs;
        memset(&attrs, 0, sizeof(attrs));
        ret = aclrtPointerGetAttributes(slab, &attrs);
        if (ret != ACL_ERROR_NONE) {
            printf("[hcomm] r%d align: PointerGetAttributes FAIL ret=%d\n", rank, ret);
            BAIL(fatal);
        } else {
            uint32_t page_size = attrs.pageSize;
            printf("[hcomm] r%d align: slab=%p pageSize=%u\n", rank, slab, page_size);
            if (page_size > 0) {
                uint64_t raw = (uint64_t)(uintptr_t)slab;
                uint64_t mask = ~((uint64_t)page_size - 1);
                aligned_ptr = (void *)(uintptr_t)(raw & mask);
                my_offset = raw - (uint64_t)(uintptr_t)aligned_ptr;
                aligned_size = SLAB_SIZE + my_offset;
                printf("[hcomm] r%d align: aligned_ptr=%p aligned_size=%zu offset=%lu\n",
                       rank, aligned_ptr, aligned_size, (unsigned long)my_offset);
            } else {
                aligned_ptr = slab;
                aligned_size = SLAB_SIZE;
                my_offset = 0;
                printf("[hcomm] r%d align: pageSize=0, no alignment needed\n", rank);
            }
        }
    }

    /* ================================================================
     * Phase 3: H2D 写入 pattern
     * ================================================================ */
    if (!fatal) {
        fill_pat(hbuf, words, (uint32_t)rank, TAG_CREATE);
        ret = aclrtMemcpy(slab, SLAB_SIZE, hbuf, SLAB_SIZE, ACL_MEMCPY_HOST_TO_DEVICE);
        if (ret != ACL_ERROR_NONE) {
            printf("[hcomm] r%d create: H2D pattern FAIL ret=%d\n", rank, ret);
            BAIL(fatal);
        }
    }

    /* ================================================================
     * Phase 4: Export (hcomm: hrtIpcSetMemoryName)
     * hcomm 源码: adapter_rts.cc:1244-1271
     *   aclrtIpcMemGetExportKey(ptr, ptrMaxLen, name, nameMaxLen, 0UL)
     *   ★★★ export flag = 0UL (ACL_RT_IPC_MEM_EXPORT_FLAG_DEFAULT) ★★★
     * ================================================================ */
    if (!fatal) {
        ret = aclrtIpcMemGetExportKey(aligned_ptr, aligned_size, mykey, IPC_KEY_LEN, 0UL);
        if (ret != ACL_ERROR_NONE) {
            printf("[hcomm] r%d create: export key (flag=0 DEFAULT) FAIL ret=%d\n", rank, ret);
            BAIL(fatal);
        }
        printf("[hcomm] r%d create: key exported (flag=0 DEFAULT)\n", rank);
    }

    /* ================================================================
     * Phase 5: 获取 bare PID + 设置 PID 白名单
     * hcomm 源码: adapter_rts.cc:2114-2137
     *   aclrtDeviceGetBareTgid(&pid)
     * hcomm 源码: local_ipc_rma_buffer_impl.cc:107-123
     *   aclrtIpcMemSetImportPid(name, &peerPid, 1)
     * ================================================================ */
    int32_t my_bare_pid = 0;
    ret = aclrtDeviceGetBareTgid(&my_bare_pid);
    if (ret != ACL_ERROR_NONE) {
        printf("[hcomm] r%d: aclrtDeviceGetBareTgid FAIL ret=%d, fallback to getpid\n", rank, ret);
        my_bare_pid = (int32_t)getpid();
    }
    printf("[hcomm] r%d: bare_pid=%d (getpid=%d)\n", rank, my_bare_pid, (int)getpid());

    int32_t all_bare_pids[N];
    MPI_Allgather(&my_bare_pid, 1, MPI_INT32_T, all_bare_pids, 1, MPI_INT32_T, MPI_COMM_WORLD);

    if (!fatal) {
        for (int p = 0; p < N; p++) {
            if (p != rank) {
                ret = aclrtIpcMemSetImportPid(mykey, &all_bare_pids[p], 1);
                if (ret != ACL_ERROR_NONE) {
                    printf("[hcomm] r%d: SetImportPid for p%d (bare_pid=%d) ret=%d\n",
                           rank, p, all_bare_pids[p], ret);
                }
            }
        }
        printf("[hcomm] r%d: %d peer bare PIDs whitelisted\n", rank, N - 1);
    }
    sync_state(&fatal);

    /* ================================================================
     * Phase 6: 交换 keys + offsets
     * hcomm 通过 Socket 交换, 这里用 MPI_Allgather 替代
     * 交换内容: key(65B) + offset(8B)
     * ================================================================ */
    uint64_t all_offsets[N];
    if (!fatal) {
        MPI_Allgather(mykey, IPC_KEY_LEN, MPI_CHAR,
                      &keys[0][0], IPC_KEY_LEN, MPI_CHAR, MPI_COMM_WORLD);
        MPI_Allgather(&my_offset, 1, MPI_UINT64_T, all_offsets, 1, MPI_UINT64_T, MPI_COMM_WORLD);
        printf("[hcomm] r%d exchange: allgathered %d keys and offsets\n", rank, N);
    }
    sync_state(&fatal);

    /* ================================================================
     * Phase 7: Import (hcomm: hrtIpcOpenMemory → MemNameRepository::OpenIpcMem)
     * hcomm 源码: adapter_rts.cc:1318-1350
     *   aclrtIpcMemImportByKey(ptr, name, 0UL)
     *   ★★★ import flag = 0UL (ACL_RT_IPC_MEM_IMPORT_FLAG_DEFAULT) ★★★
     * hcomm 源码: mem_name_repository.cc:225
     *   *ptr += offset  // 加上原始偏移
     * ================================================================ */
    if (!fatal) {
        printf("[hcomm] r%d join: import all peers (ALL flag=0 DEFAULT) begins\n", rank);
        for (int token = 0; token < N; token++) {
            MPI_Barrier(MPI_COMM_WORLD);
            if (token == rank) {
                for (int p = 0; p < N; p++) {
                    if (p == rank) continue;
                    void *pv = NULL;
                    int32_t r = aclrtIpcMemImportByKey(&pv, keys[p], 0UL);
                    imp_ok[p] = (r == ACL_ERROR_NONE);
                    imp_ret[p] = (int)r;
                    if (imp_ok[p])
                        ipc_va[p] = (void *)((uintptr_t)pv + all_offsets[p]);
                    printf("[hcomm]   r%d(dev%d) import p%d(dev%d) [%s, flag=0]: %s (ret=%d) va=%p\n",
                           rank, dev, p, dev_map[p],
                           is_die_pair(rank, p) ? "die-pair" : "cross-card",
                           imp_ok[p] ? "OK" : "FAIL", r, pv);
                }
                int joined = 0;
                for (int p = 0; p < N; p++)
                    if (p != rank && imp_ok[p]) joined++;
                join_ok = (joined == N - 1);
                printf("[hcomm] r%d joined %d/%d peers\n", rank, joined, N - 1);
            }
            MPI_Barrier(MPI_COMM_WORLD);
        }
    }
    sync_state(&fatal);

    /* ---- Link Matrix ---- */
    {
        int link_ok[N][N];
        memset(link_ok, 0, sizeof(link_ok));
        for (int p = 0; p < N; p++)
            if (p != rank) link_ok[rank][p] = imp_ok[p] ? 1 : 0;

        int global_links[N][N];
        MPI_Allreduce(link_ok, global_links, N * N, MPI_INT, MPI_MAX, MPI_COMM_WORLD);

        if (rank == 0) {
            int total_ok = 0, total_fail = 0;
            int die_ok = 0, die_fail = 0;
            int cross_ok = 0, cross_fail = 0;
            printf("[hcomm] === Link Matrix (row=importer, col=exporter) ===\n");
            printf("[hcomm]          ");
            for (int c = 0; c < N; c++) printf("  p%d(d%d)   ", c, dev_map[c]);
            printf("\n");
            for (int r = 0; r < N; r++) {
                printf("[hcomm] r%d(d%d)  ", r, dev_map[r]);
                for (int c = 0; c < N; c++) {
                    if (r == c) {
                        printf("   ---    ");
                    } else {
                        int ok = global_links[r][c];
                        printf("   %s     ", ok ? "OK" : "FAIL");
                        total_ok += ok;
                        total_fail += !ok;
                        if (is_die_pair(r, c)) {
                            die_ok += ok; die_fail += !ok;
                        } else {
                            cross_ok += ok; cross_fail += !ok;
                        }
                    }
                }
                printf("\n");
            }
            printf("[hcomm] === Summary ===\n");
            printf("[hcomm] Total:      %d OK + %d FAIL = %d (expect %d)\n",
                   total_ok, total_fail, total_ok + total_fail, N * (N - 1));
            printf("[hcomm] Die pair:   %d OK + %d FAIL = %d\n", die_ok, die_fail, 4);
            printf("[hcomm] Cross-card: %d OK + %d FAIL = %d\n", cross_ok, cross_fail, 8);
        }
    }

    /* ================================================================
     * Phase 8: D2D write + verify (ring: r writes to (r+1)%N)
     * hcomm 源码: transport_ipc_mem.cc:400-419
     *   dispatcher->MemcpyAsync(dst, len, src, len, D2D, stream, remoteRank, LINK_HCCS)
     *   → aclrtMemcpyAsync(D2D)
     * ================================================================ */
    if (!fatal) {
        int w_dst = (rank + 1) % N;
        int w_me = (rank + N - 1) % N;

        fill_pat(hbuf, words, (uint32_t)rank, (uint32_t)(TAG_WRITE_BASE + rank));
        ret = aclrtMemcpy(scratch, SCRATCH_SIZE, hbuf, SLAB_SIZE, ACL_MEMCPY_HOST_TO_DEVICE);
        if (ret != ACL_ERROR_NONE) {
            printf("[hcomm] r%d write: H2D FAIL ret=%d\n", rank, ret);
            BAIL(fatal);
        }
        if (!fatal) {
            int delivered = 0;
            if (imp_ok[w_dst]) {
                ret = aclrtMemcpyAsync(ipc_va[w_dst], SLAB_SIZE, scratch, SLAB_SIZE,
                                       ACL_MEMCPY_DEVICE_TO_DEVICE, stream);
                if (ret == ACL_ERROR_NONE)
                    ret = aclrtSynchronizeStream(stream);
                delivered = (ret == ACL_ERROR_NONE);
                printf("[hcomm] r%d write D2D 16MB -> r%d(dev%d): %s (ret=%d)\n",
                       rank, w_dst, dev_map[w_dst], delivered ? "OK" : "FAIL", ret);
            } else {
                printf("[hcomm] r%d write D2D -> r%d(dev%d): SKIP (import failed, ret=%d)\n",
                       rank, w_dst, dev_map[w_dst], imp_ret[w_dst]);
            }
            int dmap[N];
            MPI_Allgather(&delivered, 1, MPI_INT, dmap, 1, MPI_INT, MPI_COMM_WORLD);
            if (dmap[w_me]) {
                memset(vbuf, 0, SLAB_SIZE);
                ret = aclrtMemcpy(vbuf, SLAB_SIZE, slab, SLAB_SIZE, ACL_MEMCPY_DEVICE_TO_HOST);
                if (ret == ACL_ERROR_NONE) {
                    int ok = (check_pat(vbuf, words, (uint32_t)w_me,
                                        (uint32_t)(TAG_WRITE_BASE + w_me), rank) == 0);
                    write_ok = ok;
                    printf("[hcomm] r%d verify (writer r%d): %s\n", rank, w_me, ok ? "OK" : "FAIL");
                    if (!ok) g_fail = 1;
                } else {
                    printf("[hcomm] r%d verify: D2H FAIL ret=%d\n", rank, ret);
                    g_fail = 1;
                }
            } else {
                printf("[hcomm] r%d verify: writer r%d did not deliver, skip\n", rank, w_me);
                g_fail = 1;
            }
        }
    }
    sync_state(&fatal);

    /* ---- RESULT ---- */
    {
        int res[3];
        res[0] = g_fail;
        res[1] = !join_ok;
        res[2] = !write_ok;
        MPI_Allreduce(MPI_IN_PLACE, res, 3, MPI_INT, MPI_MAX, MPI_COMM_WORLD);
        g_fail = res[0];
        int join_fail = res[1], write_fail = res[2];
        printf("[hcomm] r%d RESULT: %s (join:%s write:%s)\n", rank,
               g_fail ? "FAIL" : "PASS",
               join_fail ? "FAIL" : "OK",
               write_fail ? "FAIL" : "OK");
        fflush(stdout);
    }

    /* ================================================================
     * Phase 9: cleanup
     * hcomm: aclrtIpcMemClose → aclrtFree → aclrtResetDevice
     * 注意: 不需要 DisablePeerAccess (flag=0 不建立 P2P)
     * ================================================================ */
    if (init_ok) {
        aclrtIpcMemClose(mykey);
        for (int p = 0; p < N; p++) {
            if (p != rank && imp_ok[p]) {
                aclrtIpcMemClose(keys[p]);
            }
        }
    }
    MPI_Barrier(MPI_COMM_WORLD);
    if (slab) aclrtFree(slab);
    if (scratch) aclrtFree(scratch);
    if (stream) aclrtDestroyStream(stream);
    free(hbuf);
    free(vbuf);
    if (dev_ok) aclrtResetDevice(dev);
    if (init_ok) aclFinalize();

    MPI_Finalize();
    return g_fail ? 1 : 0;
}
