#include <acl/acl.h>
#include <driver/ascend_hal.h>
#include <dlfcn.h>
#include <inttypes.h>
#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MIB (UINT64_C(1) << 20)
#define GIB (UINT64_C(1) << 30)
#define BASE UINT64_C(0x280040000000)
#define CHUNK (8 * MIB)
#define MAX_POOLS 3

#define HAL_APIS(X) \
    X(halMemAddressReserve) X(halMemAddressFree) X(halMemCreate) X(halMemRelease) \
    X(halMemMap) X(halMemUnmap) X(halMemSetAccess) X(halMemExportToShareableHandle) \
    X(halMemImportFromShareableHandle) X(halMemShareHandleSetAttribute) X(halMemExportToShareableHandleV2)
#define DECLARE(name) static __typeof__(&name) f_##name;
HAL_APIS(DECLARE)

typedef struct {
    void *base, *local, *peer;
    drv_mem_handle_t *owned, *imported;
    int local_mapped, peer_mapped;
} Pool;

static int rank_id, world, device, peer_device, driver_device;
static int used_fallback;
static size_t physical_bytes = 2 * MIB, window_bytes = GIB;
static void *library;

static int check(int ret, const char *api)
{
    if (ret) fprintf(stderr, "FAIL rank=%d device=%d api=%s ret=%d\n", rank_id, device, api, ret);
    return ret != 0;
}

static int all_failed(int failed, const char *phase)
{
    int result = 0;
    MPI_Allreduce(&failed, &result, 1, MPI_INT, MPI_MAX, MPI_COMM_WORLD);
    if (result && !rank_id) fprintf(stderr, "FAIL phase=%s\n", phase);
    return result;
}

static int parse_int(const char *name, int fallback, int maximum, int *value)
{
    const char *s = getenv(name);
    if (!s) { *value = fallback; return 0; }
    char *end = NULL;
    long n = strtol(s, &end, 10);
    if (!*s || *end || n < 0 || n > maximum) {
        fprintf(stderr, "Invalid %s=%s\n", name, s);
        return 1;
    }
    *value = (int)n;
    return 0;
}

static int load_hal(void)
{
    const char *path = getenv("HAL_LIBRARY");
    library = dlopen(path ? path : "libascend_hal.so", RTLD_NOW | RTLD_LOCAL);
    if (!library) { fprintf(stderr, "%s\n", dlerror()); return 1; }
#define LOAD(name) f_##name = dlsym(library, #name); \
    if (!f_##name) { fprintf(stderr, "Missing %s\n", #name); return 1; }
    HAL_APIS(LOAD)
#undef LOAD
    return 0;
}

static int run_probe(void)
{
    int failed = 0;
    const unsigned types[] = {MEM_HBM_TYPE, MEM_DDR_TYPE, MEM_P2P_HBM_TYPE, MEM_P2P_DDR_TYPE};
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); ++i) {
        struct drv_mem_prop prop = {MEM_DEV_SIDE, driver_device, 0, MEM_HUGE_PAGE_TYPE, types[i], 0};
        drv_mem_handle_t *handle = NULL;
        int ret = f_halMemCreate(&handle, 2 * MIB, &prop, 0);
        printf("OBSERVE create type=%u size=%" PRIu64 " driver_device=%d ret=%d\n",
               types[i], 2 * MIB, driver_device, ret);
        if (ret) {
            if (types[i] == MEM_DDR_TYPE || types[i] == MEM_P2P_DDR_TYPE) failed = 1;
            continue;
        }
        for (unsigned ht = MEM_HANDLE_TYPE_NONE; ht <= MEM_HANDLE_TYPE_FABRIC; ++ht) {
            struct MemShareHandle shared = {0};
            ret = f_halMemExportToShareableHandleV2(handle, ht, 0, &shared);
            printf("OBSERVE export_v2 mem_type=%u handle_type=%u ret=%d\n", types[i], ht, ret);
        }
        failed |= check(f_halMemRelease(handle), "halMemRelease(probe)");
    }
    return failed;
}

static int reserve_pool(Pool *pool, uint64_t requested)
{
    int ret = f_halMemAddressReserve(&pool->base, window_bytes, 0, (void *)(uintptr_t)requested,
                                    MEM_RSV_TYPE_REMOTE_MAP);
    printf("rank=%d reserve address=0x%" PRIx64 " size=%zu ret=%d\n", rank_id, requested, window_bytes, ret);
    if (ret) pool->base = NULL;
    int failed = check(ret, "halMemAddressReserve") || pool->base != (void *)(uintptr_t)requested;
    if (all_failed(failed, "reserve")) return 1;
    uint64_t address = (uintptr_t)pool->base, addresses[2];
    MPI_Allgather(&address, 1, MPI_UINT64_T, addresses, 1, MPI_UINT64_T, MPI_COMM_WORLD);
    if (all_failed(addresses[0] != addresses[1], "equal bases")) return 1;
    pool->local = (char *)pool->base + rank_id * (window_bytes / 2);
    pool->peer = (char *)pool->base + (1 - rank_id) * (window_bytes / 2);
    return 0;
}

static int set_access(void *address)
{
    struct drv_mem_access_desc access = {0};
    access.type = MEM_ACCESS_TYPE_READWRITE;
    access.location.side = MEM_DEV_SIDE;
    access.location.id = driver_device;
    return check(f_halMemSetAccess(address, physical_bytes, &access, 1), "halMemSetAccess");
}

static int allocate_local(Pool *pool)
{
    struct drv_mem_prop prop = {MEM_DEV_SIDE, driver_device, 0, MEM_HUGE_PAGE_TYPE, MEM_P2P_DDR_TYPE, 0};
    int ret = f_halMemCreate(&pool->owned, physical_bytes, &prop, 0);
    printf("rank=%d create P2P_DDR bytes=%zu ret=%d\n", rank_id, physical_bytes, ret);
    if (ret == DRV_ERROR_OUT_OF_MEMORY) {
        used_fallback = 1;
        prop.mem_type = MEM_DDR_TYPE;
        ret = f_halMemCreate(&pool->owned, physical_bytes, &prop, 0);
        printf("rank=%d fallback DDR bytes=%zu ret=%d\n", rank_id, physical_bytes, ret);
    }
    if (check(ret, "halMemCreate")) return 1;
    ret = f_halMemMap(pool->local, physical_bytes, 0, pool->owned, 0);
    if (check(ret, "halMemMap(local)")) return 1;
    pool->local_mapped = 1;
    return set_access(pool->local);
}

static int share_pool(Pool *pool)
{
    uint64_t share = 0, shares[2];
    int ret = f_halMemExportToShareableHandle(pool->owned, MEM_HANDLE_TYPE_NONE, 0, &share);
    if (all_failed(check(ret, "halMemExportToShareableHandle"), "export")) return 1;
    struct ShareHandleAttr attr = {.enableFlag = SHR_HANDLE_NO_WLIST_ENABLE};
    ret = f_halMemShareHandleSetAttribute(share, SHR_HANDLE_ATTR_NO_WLIST_IN_SERVER, attr);
    if (all_failed(check(ret, "halMemShareHandleSetAttribute"), "share policy")) return 1;
    MPI_Allgather(&share, 1, MPI_UINT64_T, shares, 1, MPI_UINT64_T, MPI_COMM_WORLD);
    ret = aclrtSetDevice(peer_device);
    int failed = check(ret, "set exporter context");
    if (!failed) {
        ret = f_halMemImportFromShareableHandle(shares[1 - rank_id], driver_device, &pool->imported);
        failed |= check(ret, "halMemImportFromShareableHandle");
    }
    failed |= check(aclrtSetDevice(device), "restore local context");
    if (all_failed(failed, "import")) return 1;
    ret = f_halMemMap(pool->peer, physical_bytes, 0, pool->imported, 0);
    if (!ret) pool->peer_mapped = 1;
    if (all_failed(check(ret, "halMemMap(peer)"), "peer map")) return 1;
    return all_failed(set_access(pool->peer), "peer access");
}

static uint64_t value_at(size_t offset, size_t word, int source, int pool, int writeback)
{
    return ((offset / sizeof(uint64_t)) + word) ^ ((uint64_t)(source + 1) << 56) ^
           ((uint64_t)(pool + 1) << 48) ^ ((uint64_t)writeback << 63);
}

static int transfer_range(void *address, uint64_t *buffer, int write, int source, int pool, int writeback)
{
    for (size_t offset = 0; offset < physical_bytes; offset += CHUNK) {
        size_t size = physical_bytes - offset < CHUNK ? physical_bytes - offset : CHUNK;
        if (write) {
            for (size_t i = 0; i < size / sizeof(uint64_t); ++i) {
                buffer[i] = value_at(offset, i, source, pool, writeback);
            }
        }
        void *remote = (char *)address + offset;
        int ret = write ? aclrtMemcpy(remote, size, buffer, size, ACL_MEMCPY_HOST_TO_DEVICE) :
                          aclrtMemcpy(buffer, CHUNK, remote, size, ACL_MEMCPY_DEVICE_TO_HOST);
        if (check(ret, write ? "write range" : "read range")) return 1;
        if (!write) {
            for (size_t i = 0; i < size / sizeof(uint64_t); ++i) {
                if (buffer[i] != value_at(offset, i, source, pool, writeback)) {
                    fprintf(stderr, "FAIL rank=%d mismatch byte=%zu pool=%d writeback=%d\n",
                            rank_id, offset + i * sizeof(uint64_t), pool, writeback);
                    return 1;
                }
            }
        }
    }
    return 0;
}

static int check_data(Pool *pool, int index, uint64_t *buffer)
{
    int failed = transfer_range(pool->local, buffer, 1, rank_id, index, 0);
    if (all_failed(failed, "write local full range")) return 1;
    failed = transfer_range(pool->peer, buffer, 0, 1 - rank_id, index, 0);
    if (all_failed(failed, "verify peer full range")) return 1;
    failed = transfer_range(pool->peer, buffer, 1, rank_id, index, 1);
    if (all_failed(failed, "write peer full range")) return 1;
    failed = transfer_range(pool->local, buffer, 0, 1 - rank_id, index, 1);
    if (all_failed(failed, "verify local full range")) return 1;
    printf("rank=%d pool=%d full_range_bytes=%zu peer_read=PASS peer_writeback=PASS\n",
           rank_id, index, physical_bytes);
    return 0;
}

static int cleanup_pool(Pool *pool)
{
    int failed = 0;
    if (pool->peer_mapped) failed |= check(f_halMemUnmap(pool->peer), "unmap peer");
    if (pool->imported) failed |= check(f_halMemRelease(pool->imported), "release imported");
    MPI_Barrier(MPI_COMM_WORLD);
    if (pool->local_mapped) failed |= check(f_halMemUnmap(pool->local), "unmap local");
    if (pool->owned) failed |= check(f_halMemRelease(pool->owned), "release owned");
    if (pool->base) failed |= check(f_halMemAddressFree(pool->base), "free address");
    memset(pool, 0, sizeof(*pool));
    return failed;
}

static int run_pools(int large)
{
    int physical = -1, ids[2];
    int failed = check(aclrtGetPhyDevIdByLogicDevId(device, &physical), "physical device");
    MPI_Allgather(&physical, 1, MPI_INT, ids, 1, MPI_INT, MPI_COMM_WORLD);
    failed |= ids[0] < 0 || ids[1] < 0 || ids[0] == ids[1] || ids[0] / 2 != ids[1] / 2;
    if (all_failed(failed, "same Duo")) return 1;
    int gib = 8, require_fallback = 0;
    failed = parse_int("POOL_GIB", 8, 16, &gib) | parse_int("EXPECT_FALLBACK", 0, 1, &require_fallback);
    if (large && gib <= 4) { fprintf(stderr, "POOL_GIB must exceed 4\n"); failed = 1; }
    if (all_failed(failed, "pool size")) return 1;
    if (large) { physical_bytes = (size_t)gib * GIB; window_bytes = 2 * physical_bytes; }
    Pool pools[MAX_POOLS] = {0};
    uint64_t *buffer = malloc(CHUNK);
    failed = all_failed(buffer == NULL, "host buffer");
    int count = large ? 1 : MAX_POOLS;
    for (int i = 0; i < count && !failed; ++i) {
        failed = reserve_pool(&pools[i], BASE + (uint64_t)i * window_bytes);
        if (!failed) failed = all_failed(allocate_local(&pools[i]), "allocate local");
        if (!failed) failed = share_pool(&pools[i]);
        if (!failed) failed = check_data(&pools[i], i, buffer);
    }
    if (!failed && large && require_fallback) failed = all_failed(!used_fallback, "fallback exercised on both ranks");
    int clean_failed = 0;
    for (int i = count - 1; i >= 0; --i) clean_failed |= cleanup_pool(&pools[i]);
    failed |= all_failed(clean_failed, "cleanup");
    if (!failed) {
        Pool reused = {0};
        failed = reserve_pool(&reused, BASE);
        failed |= all_failed(cleanup_pool(&reused), "reuse cleanup");
        if (!failed) printf("rank=%d window_reused=PASS fallback=%d\n", rank_id, used_fallback);
    }
    free(buffer);
    return failed;
}

int main(int argc, char **argv)
{
    MPI_Init(&argc, &argv);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank_id);
    MPI_Comm_size(MPI_COMM_WORLD, &world);
    setbuf(stdout, NULL);
    const char *mode = argc == 2 ? argv[1] : "";
    int probe = strcmp(mode, "probe") == 0, large = strcmp(mode, "large-pools") == 0;
    int failed = (!probe && !large && strcmp(mode, "pools")) || world != (probe ? 1 : 2);
    int a = 0, b = 1;
    failed |= parse_int("A_DEV", 0, INT32_MAX, &a) | parse_int("B_DEV", 1, INT32_MAX, &b);
    device = rank_id ? b : a;
    peer_device = rank_id ? a : b;
    int initialized = 0, device_set = 0;
    if (!failed) { int ret = aclInit(NULL); failed = check(ret, "aclInit"); initialized = !ret; }
    if (!failed) { int ret = aclrtSetDevice(device); failed = check(ret, "aclrtSetDevice"); device_set = !ret; }
    if (!failed) failed = check(aclrtGetLogicDevIdByUserDevId(device, &driver_device), "driver device id");
    if (!failed) failed = load_hal();
    if (!all_failed(failed, "initialize")) {
        printf("rank=%d device=%d driver_device=%d mode=%s\n", rank_id, device, driver_device, mode);
        failed = probe ? run_probe() : run_pools(large);
    } else { failed = 1; }
    if (device_set) failed |= check(aclrtResetDevice(device), "aclrtResetDevice");
    if (initialized) failed |= check(aclFinalize(), "aclFinalize");
    if (library) dlclose(library);
    failed = all_failed(failed, "final result");
    if (!rank_id) printf("RESULT 17_hal_vmm mode=%s %s\n", mode, failed ? "FAIL" : "PASS");
    MPI_Finalize();
    return failed ? 1 : 0;
}
