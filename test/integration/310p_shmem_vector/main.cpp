#include <cstdio>
#include <cstdlib>
#include <vector>
#include <chrono>
#include <thread>
#include <mpi.h>
#include "acl/acl.h"
#include "smem.h"
#include "smem_shm.h"
#include "smem_shm_sdma_layout.h"
#include "../../../src/hybm/ops/hybm_kernel/hybm_sdma_contract.h"
#include <cstddef>
#include "aclrtlaunch_vectorQueues.h"

constexpr size_t kPoolBytes = 128 * 1024 * 1024;
constexpr size_t kStatusBytes = 16 * 128;
constexpr uint64_t kFlagBase = 1024 * 1024;
static int rankId;

static void Check(const char *api, int ret)
{
    if (ret == 0) {
        return;
    }
    fprintf(stderr, "ERROR rank=%d api=%s ret=%d\n", rankId, api, ret);
    MPI_Abort(MPI_COMM_WORLD, 1);
}

static uint64_t ReadCell(uint8_t *res, uint64_t offset)
{
    uint64_t value = 0;
    Check("read cell", aclrtMemcpy(&value, sizeof(value), res + offset, sizeof(value), ACL_MEMCPY_DEVICE_TO_HOST));
    return value;
}

static void CheckQueues(uint8_t *res, uint32_t mask, uint32_t rounds)
{
    for (uint32_t q = 0; q < SMEM_SHM_SDMA_QUEUE_COUNT; ++q) {
        const uint64_t expected = rankId == 0 && q < 15 && (mask & (1U << q)) ? 4ULL * rounds : 0;
        const auto tail = ReadCell(res, SMEM_SHM_SDMA_RS_REQ_TAIL + q * SMEM_SHM_SDMA_RS_CURSOR_STRIDE);
        const auto head = ReadCell(res, SMEM_SHM_SDMA_RS_REQ_HEAD + q * SMEM_SHM_SDMA_RS_CURSOR_STRIDE);
        Check("queue cursors", tail == expected && head == expected ? 0 : -1);
        printf("QUEUE rank=%d q=%u tail=%lu head=%lu expected=%lu\n", rankId, q, tail, head, expected);
        if (expected != 0) {
            const auto stamp = ReadCell(res, SMEM_SHM_SDMA_RS_QUIET + q * SMEM_SHM_SDMA_RS_QUIET_STRIDE +
                                                 (expected - 1) % SMEM_SHM_SDMA_RS_QUIET_SLOTS * 8);
            Check("completion sequence", stamp == expected ? 0 : -1);
        }
    }
}

static void CheckData(uint8_t *own, uint32_t mask, uint32_t rounds)
{
    std::vector<uint32_t> data(15 * 1024);
    Check("read data", aclrtMemcpy(data.data(), data.size() * 4, own, data.size() * 4, ACL_MEMCPY_DEVICE_TO_HOST));
    for (uint32_t block = 0; block < 15; ++block) {
        if ((mask & (1U << block)) == 0) {
            continue;
        }
        for (uint32_t word = 0; word < 16; ++word) {
            const auto expected = static_cast<uint32_t>((rankId == 0 ? 20000 : 10000) + block * 100 + word);
            Check("payload", data[block * 1024 + (rankId == 0 ? 128 : 64) + word] == expected ? 0 : -1);
            if (rankId == 1) {
                Check("fused payload", data[block * 1024 + 192 + word] == expected ? 0 : -1);
            }
        }
        if (rankId == 1) {
            Check("fused signal", ReadCell(own, kFlagBase + block * 128) == rounds ? 0 : -1);
            Check("standalone signal", ReadCell(own, kFlagBase + block * 128 + 64) == rounds ? 0 : -1);
        }
    }
}

static void CheckStatus(uint32_t *status, uint32_t mask)
{
    std::vector<uint32_t> host(kStatusBytes / 4);
    Check("read status", aclrtMemcpy(host.data(), kStatusBytes, status, kStatusBytes, ACL_MEMCPY_DEVICE_TO_HOST));
    for (uint32_t block = 0; block < 15; ++block) {
        auto *row = host.data() + block * 32;
        printf("BLOCK rank=%d block=%u ran=%x ctxblock=%u vector=%u mask=%x result=%u stage=%u\n", rankId, block,
               row[0], row[1], row[2], row[3], row[4], row[5]);
        Check("block ran", row[0] == 0xC04E1234 && row[1] == block ? 0 : -1);
        Check("core type", row[2] == (block >= 8) ? 0 : -1);
        Check("metadata and operations", row[3] == mask && row[4] == 0 ? 0 : -1);
    }
}

static uint8_t *InitializePool(int device, uint32_t mask, bool explicitMask, smem_shm_t &pool, uint8_t *&base)
{
    Check("smem_init", smem_init(0));
    smem_shm_config_t config{};
    Check("config init", smem_shm_config_init(&config));
    if (explicitMask) {
        config.sdmaProducerMask = mask;
    }
    config.startConfigStoreServer = rankId == 0;
    const char *url = getenv("STORE_URL");
    Check("shm init", smem_shm_init(url ? url : "tcp://127.0.0.1:29881", 2, rankId, device, &config));
    void *rawBase = nullptr;
    pool = smem_shm_create(0, 2, rankId, kPoolBytes, SMEMS_DATA_OP_SDMA, 0, &rawBase);
    base = static_cast<uint8_t *>(rawBase);
    Check("shm create", pool && base ? 0 : -1);
    const auto sym = smem_shm_get_symmetric_size(pool);
    auto *own = static_cast<uint8_t *>(base) + rankId * sym;
    Check("clear", aclrtMemset(own, kFlagBase + 4096, 0, kFlagBase + 4096));
    std::vector<uint32_t> input(15 * 1024);
    for (uint32_t block = 0; block < 15; ++block) {
        for (uint32_t word = 0; word < 16; ++word) {
            input[block * 1024 + word] = (rankId + 1) * 10000 + block * 100 + word;
        }
    }
    Check("initialize input",
          aclrtMemcpy(own, input.size() * 4, input.data(), input.size() * 4, ACL_MEMCPY_HOST_TO_DEVICE));
    return own;
}

static void ObserveEpoch(aclrtStream stream, uint8_t *workspace)
{
    aclrtEvent finish = nullptr;
    Check("create finish event", aclrtCreateEvent(&finish));
    Check("record finish event", aclrtRecordEvent(finish, stream));
    uint64_t previous = 0;
    unsigned resets = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    aclrtEventRecordedStatus state = ACL_EVENT_RECORDED_STATUS_NOT_READY;
    while (state != ACL_EVENT_RECORDED_STATUS_COMPLETE) {
        Check("query finish", aclrtQueryEventStatus(finish, &state));
        const uint64_t current = ReadCell(workspace, offsetof(ock::mf::HybmSdmaOrchResult, consumed)) & UINT32_MAX;
        if (current < previous) {
            ++resets;
        }
        previous = current;
        Check("epoch test deadline", std::chrono::steady_clock::now() < deadline ? 0 : -1);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    printf("EPOCH rank=%d observed_resets=%u\n", rankId, resets);
    if (rankId == 0) {
        Check("epoch restarted while producing", resets > 0 ? 0 : -1);
    }
    aclrtDestroyEvent(finish);
}

static void WaitForKernel(aclrtStream stream, uint32_t *status, uint8_t *res)
{
    const auto syncRet = aclrtSynchronizeStreamWithTimeout(stream, 30000);
    if (syncRet != 0) {
        std::vector<uint32_t> partial(kStatusBytes / 4);
        aclrtMemcpy(partial.data(), kStatusBytes, status, kStatusBytes, ACL_MEMCPY_DEVICE_TO_HOST);
        for (unsigned b = 0; b < 15; ++b) {
            auto *r = partial.data() + b * 32;
            printf("PARTIAL rank=%d block=%u ran=%x ctxblock=%u vector=%u mask=%x code=%u stage=%u\n", rankId, b, r[0],
                   r[1], r[2], r[3], r[4], r[5]);
        }
        for (unsigned q = 0; q < 15; ++q) {
            printf("PARTIAL_QUEUE rank=%d q=%u tail=%lu head=%lu\n", rankId, q,
                   ReadCell(res, SMEM_SHM_SDMA_RS_REQ_TAIL + q * 64),
                   ReadCell(res, SMEM_SHM_SDMA_RS_REQ_HEAD + q * 64));
        }
    }
    Check("sync", syncRet);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IOLBF, 0);
    Check("MPI_Init", MPI_Init(&argc, &argv));
    MPI_Comm_rank(MPI_COMM_WORLD, &rankId);
    int world = 0;
    MPI_Comm_size(MPI_COMM_WORLD, &world);
    Check("world size", world == 2 ? 0 : -1);
    const uint32_t mask = argc > 1 ? strtoul(argv[1], nullptr, 0) : 0xFF;
    const uint32_t rounds = argc > 2 ? strtoul(argv[2], nullptr, 0) : 600;
    Check("mask", SmemShmSdmaProducerMaskValid(mask) && rounds > 0 ? 0 : -1);
    const int baseDevice = getenv("TEST_DEVICE_BASE") ? atoi(getenv("TEST_DEVICE_BASE")) : 4;
    const int device = baseDevice + rankId;
    Check("aclInit", aclInit(nullptr));
    Check("set device", aclrtSetDevice(device));
    for (auto attr : {ACL_DEV_ATTR_AICORE_CORE_NUM, ACL_DEV_ATTR_VECTOR_CORE_NUM}) {
        int64_t value = 0;
        Check("device info", aclrtGetDeviceInfo(device, attr, &value));
        printf("DEVICE rank=%d dev=%d attr=%d count=%ld\n", rankId, device, int(attr), value);
    }
    smem_shm_t pool = nullptr;
    uint8_t *base = nullptr;
    auto *own = InitializePool(device, mask, argc > 1, pool, base);
    auto *res = own + kPoolBytes - SMEM_SHM_SDMA_RESERVED;
    uint32_t *status = nullptr;
    Check("status alloc", aclrtMalloc(reinterpret_cast<void **>(&status), kStatusBytes, ACL_MEM_MALLOC_HUGE_FIRST));
    Check("status clear", aclrtMemset(status, kStatusBytes, 0, kStatusBytes));
    aclrtStream stream = nullptr;
    Check("stream", aclrtCreateStream(&stream));
    Check("prepare", MPI_Barrier(MPI_COMM_WORLD));
    Check("launch", ACLRT_LAUNCH_KERNEL(vectorQueues)(15, stream, base, status, mask, rounds));
    if (getenv("CHECK_EPOCH")) {
        ObserveEpoch(stream, static_cast<uint8_t *>(smem_shm_sdma_get_workspace(pool)));
    }
    WaitForKernel(stream, status, res);
    Check("complete", MPI_Barrier(MPI_COMM_WORLD));
    CheckStatus(status, mask);
    CheckData(own, mask, rounds);
    CheckQueues(res, mask, rounds);
    printf("PASS rank=%d mask=0x%x rounds=%u\n", rankId, mask, rounds);
    aclrtFree(status);
    aclrtDestroyStream(stream);
    smem_shm_destroy(pool, 0);
    smem_shm_uninit(0);
    smem_uninit();
    aclrtResetDevice(device);
    aclFinalize();
    MPI_Finalize();
    return 0;
}
