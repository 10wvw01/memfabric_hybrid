/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */
#include <dlfcn.h>
#include <inttypes.h>
#include <limits.h>
#include <mpi.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <acl/acl.h>
#include <driver/ascend_hal.h>

#define MIB (1ULL << 20)
#define GIB (1ULL << 30)
#define REMOTE_BASE UINT64_C(0x280040000000)
#define LOW_BASE UINT64_C(0x12c200000000)
#define POOL_COUNT 3
#define PHYSICAL_BYTES (2 * MIB)
#define DATA_WORDS (PHYSICAL_BYTES / sizeof(uint32_t))

typedef drvError_t (*ReserveFn)(void **, size_t, size_t, void *, uint64_t);
typedef drvError_t (*FreeFn)(void *);
typedef struct {
    void *base;
    void *local;
    void *peer;
    aclrtDrvMemHandle owned;
    aclrtDrvMemHandle imported;
    int local_mapped;
    int peer_mapped;
} Pool;

static int rank_id, world_size, device_id, peer_device;
static void *hal_library;
static ReserveFn hal_reserve;
static FreeFn hal_free;

static int report(int ret, const char *operation)
{
    if (ret != 0) {
        fprintf(stderr, "[rank %d device %d] FAIL %s ret=%d\n", rank_id, device_id, operation, ret);
    }
    return ret != 0;
}

static int collective(int failed, const char *phase)
{
    int any_failed = 0;
    MPI_Allreduce(&failed, &any_failed, 1, MPI_INT, MPI_MAX, MPI_COMM_WORLD);
    if (any_failed && rank_id == 0) {
        fprintf(stderr, "[FAIL] phase=%s\n", phase);
    }
    return any_failed;
}

static int parse_device(const char *name, int fallback, int *result)
{
    const char *value = getenv(name);
    if (!value) {
        *result = fallback;
        return 0;
    }
    char *end = NULL;
    long parsed = strtol(value, &end, 10);
    if (!*value || *end || parsed < 0 || parsed > INT_MAX) {
        fprintf(stderr, "Invalid %s=%s\n", name, value);
        return 1;
    }
    *result = (int)parsed;
    return 0;
}

static int load_hal(void)
{
    const char *name = getenv("HAL_LIBRARY");
    hal_library = dlopen(name ? name : "libascend_hal.so", RTLD_NOW | RTLD_LOCAL);
    if (!hal_library) {
        fprintf(stderr, "dlopen HAL failed: %s\n", dlerror());
        return 1;
    }
    hal_reserve = (ReserveFn)dlsym(hal_library, "halMemAddressReserve");
    hal_free = (FreeFn)dlsym(hal_library, "halMemAddressFree");
    if (!hal_reserve || !hal_free) {
        fprintf(stderr, "HAL address reservation symbols are missing\n");
        return 1;
    }
    return 0;
}

static int reserve_address(int use_acl, void **base, size_t size, uint64_t address, uint64_t flags)
{
    *base = NULL;
    if (use_acl) {
        return aclrtReserveMemAddress(base, size, 0, (void *)(uintptr_t)address, flags);
    }
    return hal_reserve(base, size, 0, (void *)(uintptr_t)address, flags);
}

static int free_address(int use_acl, void *base)
{
    return use_acl ? aclrtReleaseMemAddress(base) : (int)hal_free(base);
}

/* Comparison results describe the installed driver. Release only addresses actually acquired. */
static int observe(int use_acl, uint64_t address, size_t size, uint64_t flags)
{
    void *base = NULL;
    int ret = reserve_address(use_acl, &base, size, address, flags);
    printf("OBSERVE api=%s address=0x%" PRIx64 " size=%zu flags=%" PRIu64 " ret=%d returned=%p\n",
           use_acl ? "ACL" : "HAL", address, size, flags, ret, base);
    if (ret == 0) {
        return report(free_address(use_acl, base), "release observation");
    }
    return 0;
}

static int duplicate_and_reuse(void)
{
    void *base = NULL, *duplicate = NULL;
    int ret = hal_reserve(&base, GIB, 0, (void *)(uintptr_t)REMOTE_BASE, MEM_RSV_TYPE_REMOTE_MAP);
    if (report(ret, "HAL 1GiB remote reserve")) return 1;
    int failed = base != (void *)(uintptr_t)REMOTE_BASE;
    ret = hal_reserve(&duplicate, GIB, 0, base, MEM_RSV_TYPE_REMOTE_MAP);
    printf("CHECK duplicate reserve ret=%d\n", ret);
    if (ret == 0) {
        fprintf(stderr, "Duplicate address reservation unexpectedly succeeded\n");
        failed = 1;
        if (duplicate != base) failed |= report(hal_free(duplicate), "release duplicate");
    }
    failed |= report(hal_free(base), "release original");
    if (failed) return failed;
    ret = hal_reserve(&base, GIB, 0, (void *)(uintptr_t)REMOTE_BASE, MEM_RSV_TYPE_REMOTE_MAP);
    if (report(ret, "reserve after release")) return 1;
    failed = base != (void *)(uintptr_t)REMOTE_BASE;
    failed |= report(hal_free(base), "release reused address");
    printf("CHECK release_and_reuse=%s\n", failed ? "FAIL" : "PASS");
    return failed;
}

static int run_probe(void)
{
    const uint64_t addresses[] = {0, LOW_BASE, UINT64_C(0x12da80000000), REMOTE_BASE};
    const size_t sizes[] = {32 * MIB, GIB, 4 * GIB};
    int failed = 0;
    for (size_t a = 0; a < sizeof(addresses) / sizeof(addresses[0]); ++a) {
        for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); ++s) {
            failed |= observe(1, addresses[a], sizes[s], 0);
        }
    }
    failed |= observe(0, REMOTE_BASE, GIB, 0);
    const size_t remote_sizes[] = {2 * MIB, 32 * MIB, 64 * MIB, GIB};
    for (size_t s = 0; s < sizeof(remote_sizes) / sizeof(remote_sizes[0]); ++s) {
        failed |= observe(0, REMOTE_BASE, remote_sizes[s], MEM_RSV_TYPE_REMOTE_MAP);
    }
    failed |= duplicate_and_reuse();
    return failed;
}

static int check_topology(void)
{
    int physical = -1, devices[2] = {-1, -1};
    int failed = report(aclrtGetPhyDevIdByLogicDevId(device_id, &physical), "get physical device");
    MPI_Allgather(&physical, 1, MPI_INT, devices, 1, MPI_INT, MPI_COMM_WORLD);
    if (devices[0] < 0 || devices[1] < 0 || devices[0] == devices[1] || devices[0] / 2 != devices[1] / 2) {
        fprintf(stderr, "Expected one Duo card, got physical devices %d,%d\n", devices[0], devices[1]);
        failed = 1;
    }
    printf("[rank %d] user_device=%d physical_device=%d\n", rank_id, device_id, physical);
    return collective(failed, "same-card topology");
}

static int reserve_pool(Pool *pool, int use_acl, uint64_t requested)
{
    void *base = NULL;
    int ret = reserve_address(use_acl, &base, GIB, requested, use_acl ? 0 : MEM_RSV_TYPE_REMOTE_MAP);
    printf("[rank %d] reserve api=%s address=0x%" PRIx64 " size=%" PRIu64 " flags=%u ret=%d\n",
           rank_id, use_acl ? "ACL" : "HAL", requested, (uint64_t)GIB,
           use_acl ? 0 : MEM_RSV_TYPE_REMOTE_MAP, ret);
    if (ret == 0) pool->base = base;
    int failed = report(ret, "exact window reservation") || base != (void *)(uintptr_t)requested;
    if (collective(failed, "exact window reservation")) return 1;
    uint64_t own = (uintptr_t)base, all[2];
    MPI_Allgather(&own, 1, MPI_UINT64_T, all, 1, MPI_UINT64_T, MPI_COMM_WORLD);
    return collective(all[0] != all[1], "equal window bases");
}

static int allocate_local(Pool *pool)
{
    aclrtPhysicalMemProp prop = {0};
    prop.handleType = ACL_MEM_HANDLE_TYPE_NONE;
    prop.allocationType = ACL_MEM_ALLOCATION_TYPE_PINNED;
    prop.memAttr = ACL_MEM_P2P_HUGE;
    prop.location.id = device_id;
    prop.location.type = ACL_MEM_LOCATION_TYPE_DEVICE;
    aclrtDrvMemHandle owned = NULL;
    int ret = aclrtMallocPhysical(&owned, PHYSICAL_BYTES, &prop, 0);
    if (report(ret, "ACL physical allocation")) return 1;
    pool->owned = owned;
    pool->local = (char *)pool->base + rank_id * (GIB / 2);
    pool->peer = (char *)pool->base + (1 - rank_id) * (GIB / 2);
    ret = aclrtMapMem(pool->local, PHYSICAL_BYTES, 0, pool->owned, 0);
    if (report(ret, "ACL local map")) return 1;
    pool->local_mapped = 1;
    aclrtMemAccessDesc access = {0};
    access.flags = ACL_RT_MEM_ACCESS_FLAGS_READWRITE;
    access.location.type = ACL_MEM_LOCATION_TYPE_DEVICE;
    access.location.id = device_id;
    return report(aclrtMemSetAccess(pool->local, PHYSICAL_BYTES, &access, 1), "ACL local access");
}

static int share_and_map(Pool *pool)
{
    uint64_t handle = 0, handles[2];
    int ret = aclrtMemExportToShareableHandle(pool->owned, ACL_MEM_HANDLE_TYPE_NONE,
                                             ACL_RT_VMM_EXPORT_FLAG_DISABLE_PID_VALIDATION, &handle);
    if (collective(report(ret, "ACL export"), "export handles")) return 1;
    MPI_Allgather(&handle, 1, MPI_UINT64_T, handles, 1, MPI_UINT64_T, MPI_COMM_WORLD);
    aclrtDrvMemHandle imported = NULL;
    ret = aclrtSetDevice(peer_device);
    if (ret == 0) {
        ret = aclrtMemImportFromShareableHandle(handles[1 - rank_id], device_id, &imported);
        if (ret == 0) pool->imported = imported;
    }
    int failed = report(ret, "ACL import in exporter context");
    failed |= report(aclrtSetDevice(device_id), "restore local context");
    if (collective(failed, "import handles")) return 1;
    ret = aclrtMapMem(pool->peer, PHYSICAL_BYTES, 0, pool->imported, 0);
    if (ret == 0) pool->peer_mapped = 1;
    if (collective(report(ret, "ACL peer map"), "map peers")) return 1;
    aclrtMemAccessDesc access = {0};
    access.flags = ACL_RT_MEM_ACCESS_FLAGS_READWRITE;
    access.location.type = ACL_MEM_LOCATION_TYPE_DEVICE;
    access.location.id = device_id;
    ret = aclrtMemSetAccess(pool->peer, PHYSICAL_BYTES, &access, 1);
    return collective(report(ret, "ACL peer access"), "peer access");
}

static uint32_t pattern(size_t index, int pool, int source_rank, int writeback)
{
    return (uint32_t)index ^ ((uint32_t)(pool + 1) << 24) ^ ((uint32_t)source_rank << 20) ^
           (writeback ? UINT32_C(0x80000000) : 0);
}

static void fill(uint32_t *buffer, int pool, int source_rank, int writeback)
{
    for (size_t i = 0; i < DATA_WORDS; ++i) buffer[i] = pattern(i, pool, source_rank, writeback);
}

static int verify(const uint32_t *buffer, int pool, int source_rank, int writeback)
{
    for (size_t i = 0; i < DATA_WORDS; ++i) {
        uint32_t expected = pattern(i, pool, source_rank, writeback);
        if (buffer[i] != expected) {
            fprintf(stderr, "[rank %d] pool=%d word=%zu actual=%08x expected=%08x\n",
                    rank_id, pool, i, buffer[i], expected);
            return 1;
        }
    }
    return 0;
}

static int check_data(Pool *pool, int index, uint32_t *input, uint32_t *output)
{
    fill(input, index, rank_id, 0);
    int ret = aclrtMemcpy(pool->local, PHYSICAL_BYTES, input, PHYSICAL_BYTES, ACL_MEMCPY_HOST_TO_DEVICE);
    if (collective(report(ret, "write local pattern"), "write local patterns")) return 1;
    ret = aclrtMemcpy(output, PHYSICAL_BYTES, pool->peer, PHYSICAL_BYTES, ACL_MEMCPY_DEVICE_TO_HOST);
    int failed = report(ret, "read peer pattern");
    if (!failed) failed = verify(output, index, 1 - rank_id, 0);
    if (collective(failed, "verify peer patterns")) return 1;
    fill(input, index, rank_id, 1);
    ret = aclrtMemcpy(pool->peer, PHYSICAL_BYTES, input, PHYSICAL_BYTES, ACL_MEMCPY_HOST_TO_DEVICE);
    if (collective(report(ret, "write peer pattern"), "write peer patterns")) return 1;
    ret = aclrtMemcpy(output, PHYSICAL_BYTES, pool->local, PHYSICAL_BYTES, ACL_MEMCPY_DEVICE_TO_HOST);
    failed = report(ret, "read local writeback");
    if (!failed) failed = verify(output, index, 1 - rank_id, 1);
    if (collective(failed, "verify writebacks")) return 1;
    printf("[rank %d] pool=%d 2MiB peer_read=PASS peer_writeback=PASS\n", rank_id, index);
    return 0;
}

static int cleanup_pool(Pool *pool, int use_acl)
{
    int failed = 0;
    if (pool->peer_mapped) failed |= report(aclrtUnmapMem(pool->peer), "unmap peer");
    if (pool->imported) failed |= report(aclrtFreePhysical(pool->imported), "free imported handle");
    /* Both importers finish before either owner releases the shared physical allocation. */
    MPI_Barrier(MPI_COMM_WORLD);
    if (pool->local_mapped) failed |= report(aclrtUnmapMem(pool->local), "unmap local");
    if (pool->owned) failed |= report(aclrtFreePhysical(pool->owned), "free owned handle");
    if (pool->base) failed |= report(free_address(use_acl, pool->base), "release window");
    memset(pool, 0, sizeof(*pool));
    return failed;
}

static int run_pools(int use_acl)
{
    if (check_topology()) return 1;
    uint64_t start = use_acl ? LOW_BASE : REMOTE_BASE;
    Pool pools[POOL_COUNT] = {0};
    uint32_t *input = malloc(PHYSICAL_BYTES), *output = malloc(PHYSICAL_BYTES);
    int failed = collective(!input || !output, "allocate host buffers");
    for (int i = 0; i < POOL_COUNT && !failed; ++i) {
        failed = reserve_pool(&pools[i], use_acl, start + (uint64_t)i * GIB);
        if (!failed) failed = collective(allocate_local(&pools[i]), "allocate local slices");
        if (!failed) failed = share_and_map(&pools[i]);
        if (!failed) failed = check_data(&pools[i], i, input, output);
    }
    int cleanup_failed = 0;
    for (int i = POOL_COUNT - 1; i >= 0; --i) cleanup_failed |= cleanup_pool(&pools[i], use_acl);
    failed |= collective(cleanup_failed, "pool cleanup");
    if (!failed) {
        Pool reused = {0};
        failed = reserve_pool(&reused, use_acl, start);
        failed |= collective(cleanup_pool(&reused, use_acl), "reuse cleanup");
        if (!failed) printf("[rank %d] first_window_reused=PASS\n", rank_id);
    }
    free(input);
    free(output);
    return failed;
}

static int run_case(const char *mode)
{
    if (strcmp(mode, "probe") == 0 && world_size == 1) return run_probe();
    if (strcmp(mode, "pools") == 0 && world_size == 2) return run_pools(0);
    if (strcmp(mode, "acl-pools") == 0 && world_size == 2) return run_pools(1);
    fprintf(stderr, "Use one MPI rank for probe, two ranks for pools or acl-pools\n");
    return 1;
}

int main(int argc, char **argv)
{
    MPI_Init(&argc, &argv);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank_id);
    MPI_Comm_size(MPI_COMM_WORLD, &world_size);
    setbuf(stdout, NULL);
    const char *mode = argc == 2 ? argv[1] : "";
    int a = 0, b = 1;
    int failed = parse_device("A_DEV", 0, &a) | parse_device("B_DEV", 1, &b);
    device_id = rank_id == 0 ? a : b;
    peer_device = rank_id == 0 ? b : a;
    int ret = aclInit(NULL);
    int initialized = ret == ACL_SUCCESS, device_set = 0;
    failed |= report(ret, "initialize ACL");
    if (!failed) {
        ret = aclrtSetDevice(device_id);
        device_set = ret == ACL_SUCCESS;
        failed = report(ret, "set local device");
    }
    failed |= load_hal();
    if (!collective(failed, "initialize")) {
        printf("[rank %d] soc=%s mode=%s device=%d remote_flag=%u\n",
               rank_id, aclrtGetSocName(), mode, device_id, MEM_RSV_TYPE_REMOTE_MAP);
        failed = run_case(mode);
    } else {
        failed = 1;
    }
    if (device_set) failed |= report(aclrtResetDevice(device_id), "reset device");
    if (initialized) failed |= report(aclFinalize(), "finalize ACL");
    if (hal_library) dlclose(hal_library);
    failed = collective(failed, "final result");
    if (rank_id == 0) printf("RESULT 17_va_reservation mode=%s %s\n", mode, failed ? "FAIL" : "PASS");
    MPI_Finalize();
    return failed ? 1 : 0;
}
