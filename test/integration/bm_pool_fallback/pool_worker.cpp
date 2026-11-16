#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <fstream>
#include <unistd.h>
#include "smem.h"
#include "smem_bm.h"

namespace {
int rankId;
int worldSize;

bool FinishTogether(const std::string &directory, const std::string &phase)
{
    std::ofstream(directory + "/" + phase + "-" + std::to_string(rankId)).put('1');
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < deadline) {
        bool ready = true;
        for (int rank = 0; rank < worldSize; ++rank) {
            ready = ready && access((directory + "/" + phase + "-" + std::to_string(rank)).c_str(), F_OK) == 0;
        }
        if (ready) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::fprintf(stderr, "rank %d: completion barrier timed out\n", rankId);
    return false;
}

int Verify(smem_bm_t pool, bool dram, uint64_t size)
{
    auto memoryType = dram ? SMEM_MEM_TYPE_HOST : SMEM_MEM_TYPE_DEVICE;
    auto writeType = dram ? SMEMB_COPY_H2GH : SMEMB_COPY_H2G;
    auto readType = dram ? SMEMB_COPY_GH2H : SMEMB_COPY_G2H;
    alignas(64) std::array<uint64_t, 32> expected{};
    expected.fill(0x1234000000000000ULL + rankId);
    for (int peer = 0; peer < worldSize; ++peer) {
        auto base = static_cast<uint8_t *>(smem_bm_ptr_by_mem_type(pool, memoryType, peer));
        if (base == nullptr) {
            std::fprintf(stderr, "rank %d: missing peer %d address\n", rankId, peer);
            return 1;
        }
        for (auto offset : {static_cast<uint64_t>(rankId * 256), size - 4096 + rankId * 256}) {
            smem_copy_params_t copy{};
            copy.src = expected.data();
            copy.dest = base + offset;
            copy.dataSize = sizeof(expected);
            int ret = smem_bm_copy(pool, &copy, writeType, 0);
            if (ret != 0) {
                std::fprintf(stderr, "rank %d: write peer %d offset %lu ret %d\n", rankId, peer, offset, ret);
                return 1;
            }
            smem_bm_wait(pool);
            alignas(64) std::array<uint64_t, 32> received{};
            copy.src = base + offset;
            copy.dest = received.data();
            ret = smem_bm_copy(pool, &copy, readType, 0);
            if (ret != 0 || received != expected) {
                std::fprintf(stderr, "rank %d: read peer %d offset %lu ret %d mismatch\n", rankId, peer, offset, ret);
                return 1;
            }
        }
    }
    std::printf("COPY_OK rank=%d peers=%d\n", rankId, worldSize);
    return 0;
}

smem_bm_t CreatePool(bool dram, uint64_t bytes)
{
    smem_bm_create_option_t options{};
    options.maxHbmSize = options.localHBMSize = dram ? 0 : bytes;
    options.maxDramSize = options.localDRAMSize = dram ? bytes : 0;
    options.dataOpType = dram ? SMEMB_DATA_OP_HOST_SHM : SMEMB_DATA_OP_SDMA;
    return smem_bm_create2(1, &options);
}

int Run(int argc, char **argv)
{
    if (argc != 9) {
        std::fprintf(stderr, "usage: worker rank world device store hbm|dram bytes success|reject workdir\n");
        return 2;
    }
    rankId = std::stoi(argv[1]);
    worldSize = std::stoi(argv[2]);
    int device = std::stoi(argv[3]);
    bool dram = std::strcmp(argv[5], "dram") == 0;
    uint64_t bytes = std::stoull(argv[6]);
    bool reject = std::strcmp(argv[7], "reject") == 0;
    smem_bm_config_t config{};
    smem_bm_config_init(&config);
    config.startConfigStoreServer = rankId == 0;
    config.rankId = rankId;
    config.autoRanking = false;
    config.initTimeout = config.createTimeout = config.controlOperationTimeout = 30;
    smem_set_log_level(1);
    int ret = smem_bm_init(argv[4], worldSize, device, &config);
    if (ret != 0) {
        std::fprintf(stderr, "rank %d: smem_bm_init ret %d\n", rankId, ret);
        return 1;
    }
    auto pool = CreatePool(dram, bytes);
    if (pool == nullptr) {
        std::fprintf(stderr, "rank %d: pool creation failed\n", rankId);
        return 1;
    }
    if (!FinishTogether(argv[8], "created")) {
        return 1;
    }
    ret = smem_bm_join(pool, 0);
    std::printf("JOIN_RESULT rank=%d ret=%d\n", rankId, ret);
    if (reject) {
        return FinishTogether(argv[8], "joined") ? 0 : 1;
    }
    if (ret != 0 || !FinishTogether(argv[8], "joined")) {
        return 1;
    }
    std::this_thread::sleep_for(std::chrono::seconds(1));
    if (Verify(pool, dram, bytes) != 0 || !FinishTogether(argv[8], "copied")) {
        return 1;
    }
    smem_bm_destroy(pool);
    smem_bm_uninit(0);
    return 0;
}
} // namespace

int main(int argc, char **argv)
{
    int result = Run(argc, argv);
    std::fflush(nullptr);
    // CANN在进程退出后回收失败join的资源；跳过全局析构，保留join的测试结果。
    _exit(result);
}
