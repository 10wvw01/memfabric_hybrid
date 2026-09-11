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
#include <iostream>
#include <string>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <regex>
#include "band_width_calculator.h"

static const uint64_t DEFAULT_HBM_POOL_SIZE = 2ULL << 30;  // 2GB
static const uint64_t DEFAULT_DRAM_POOL_SIZE = 2ULL << 30; // 2GB
static const uint64_t POOL_ALIGN_SIZE = 2ULL * 1024ULL * 1024ULL; // AclVmm 段要求 2M 对齐

void PrintHelp()
{
    std::cout << "usage: bm_perf_benchmark_310p [-h | --help]\n"
                 "               <command> [<option>] [<args>]\n\n"
                 "310P(Ascend310P3) dual-pool BM bandwidth test (HBM AclVmm + DRAM HOST_SHM).\n"
                 "-h, --help                  Displays help information.\n"
                 "-bw, bandwidth              Performs a bandwidth test.\n"
                 "    -t,  --type             Indicates the data copy type.\n"
                 "(h2d,d2h,h2rd,h2rh,d2rd,d2rh,rh2d,rh2h,rd2d,rd2h,all).\n"
                 "    -s,  --size             Specifies block size (default 1024).\n"
                 "    -cc, --copy-count       Specifies copy count (default 1000).\n"
                 "    -bs, --batch-size       Specifies block count (default 1).\n"
                 "    -d,  --device           Specifies starting device ID (default 0).\n"
                 "    -ws, --world-size       Specifies the total number of NPUs (default 2).\n"
                 "    -lrs, --local-rank-size Specifies the number of local NPUs (default 2).\n"
                 "    -rs, --rank-start       Specifies the starting rankId (default 0).\n"
                 "    -ip, --ip               Specifies the IP address and port (e.g., tcp://<ip>:<port>).\n"
                 "    -hbm-size, --hbm-size   Specifies HBM pool size in bytes (default 2GB).\n"
                 "    -dram-size, --dram-size Specifies DRAM pool size in bytes (default 2GB).\n"
                 "    -hm, --host-mem         Local host buffer type: pinned|pageable (default pinned).\n"
              << std::endl;
}

[[noreturn]] void ExitWithError(const std::string &msg)
{
    std::cerr << "Error: " << msg << std::endl;
    std::exit(1);
}

CopyType ParseType(const std::string &arg)
{
    if (arg == "h2d") {
        return CopyType::HOST_TO_DEVICE;
    }
    if (arg == "d2h") {
        return CopyType::DEVICE_TO_HOST;
    }
    if (arg == "h2rd") {
        return CopyType::HOST_TO_REMOTE_DEVICE;
    }
    if (arg == "h2rh") {
        return CopyType::HOST_TO_REMOTE_HOST;
    }
    if (arg == "d2rh") {
        return CopyType::DEVICE_TO_REMOTE_HOST;
    }
    if (arg == "d2rd") {
        return CopyType::DEVICE_TO_REMOTE_DEVICE;
    }
    if (arg == "rh2d") {
        return CopyType::REMOTE_HOST_TO_DEVICE;
    }
    if (arg == "rh2h") {
        return CopyType::REMOTE_HOST_TO_HOST;
    }
    if (arg == "rd2d") {
        return CopyType::REMOTE_DEVICE_TO_DEVICE;
    }
    if (arg == "rd2h") {
        return CopyType::REMOTE_DEVICE_TO_HOST;
    }
    if (arg == "all") {
        return CopyType::ALL_DIRECTION;
    }
    ExitWithError("invalid type: " + arg +
                  " (must be one of h2d/d2h/h2rd/h2rh/d2rh/d2rd/rh2d/rh2h/rd2d/rd2h/all)");
}

uint64_t ParseUint64(const std::string &arg, const std::string &field)
{
    try {
        size_t pos = 0;
        uint64_t val = std::stoull(arg, &pos, 0);
        if (pos != arg.size()) {
            ExitWithError("invalid numeric value for " + field + ": " + arg);
        }
        return val;
    } catch (...) {
        ExitWithError("invalid numeric value for " + field + ": " + arg);
    }
}

uint32_t ParseUint32(const std::string &arg, const std::string &field)
{
    uint64_t val = ParseUint64(arg, field);
    if (val > UINT32_MAX) {
        ExitWithError("value too large for " + field + ": " + arg);
    }
    return static_cast<uint32_t>(val);
}

void ParseIp(const std::string &arg)
{
    std::regex ipPattern(R"(^tcp://([0-9]+\.[0-9]+\.[0-9]+\.[0-9]+):([0-9]{1,5})$)");
    if (!std::regex_match(arg, ipPattern)) {
        ExitWithError("invalid IP address and port format: " + arg + " (must be tcp://<ip>:<port>)");
    }
}

void CheckPoolSize(const std::string &field, uint64_t size)
{
    if (size == 0 || size % POOL_ALIGN_SIZE != 0) {
        ExitWithError(field + " must be a non-zero multiple of 2MB: " + std::to_string(size));
    }
}

void CheckCmdBw(BwTestParam &opts)
{
    if (opts.worldRankSize == 0 || opts.localRankSize == 0) {
        ExitWithError("world-size and local-rank-size must be greater than 0");
    }
    if (opts.worldRankSize < opts.localRankSize) {
        ExitWithError("world-size must be greater than or equal to local-rank-size");
    }
    if (opts.worldRankSize != (opts.worldRankSize & (~(opts.worldRankSize - 1)))) {
        ExitWithError("world-size must be the power of 2");
    }
    if (opts.worldRankSize > RANK_SIZE_MAX) {
        ExitWithError("world-size must be less than or equal to " + std::to_string(RANK_SIZE_MAX));
    }
    if (opts.copyCount == 0) {
        ExitWithError("copy-count must be greater than 0");
    }
    if (opts.copySize == 0) {
        ExitWithError("size must be greater than 0");
    }
    if (opts.batchSize == 0) {
        ExitWithError("batch-size must be greater than 0");
    }
    CheckPoolSize("hbm-size", opts.hbmPoolSize);
    CheckPoolSize("dram-size", opts.dramPoolSize);
    uint64_t len = opts.batchSize * opts.copySize;
    if (len > opts.hbmPoolSize || len > opts.dramPoolSize) {
        ExitWithError("batch-size * size (" + std::to_string(len) +
                      ") exceeds pool size, hbmPoolSize: " + std::to_string(opts.hbmPoolSize) +
                      " dramPoolSize: " + std::to_string(opts.dramPoolSize));
    }
}

void ParseHostMem(const std::string &arg, BwTestParam &opts)
{
    if (arg == "pinned") {
        opts.hostMemPinned = true;
        return;
    }
    if (arg == "pageable") {
        opts.hostMemPinned = false;
        return;
    }
    ExitWithError("invalid host-mem type: " + arg + " (must be pinned or pageable)");
}

void ParseCmdBw(int argc, char *argv[], BwTestParam &opts)
{
    bool hasType = false;

    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "-t" || arg == "--type") {
            if (i + 1 >= argc) {
                ExitWithError("missing value for " + arg);
            }
            opts.type = ParseType(argv[++i]);
            hasType = true;
        } else if (arg == "-s" || arg == "--size") {
            if (i + 1 >= argc) {
                ExitWithError("missing value for " + arg);
            }
            opts.copySize = ParseUint64(argv[++i], "size");
        } else if (arg == "-cc" || arg == "--copy-count") {
            if (i + 1 >= argc) {
                ExitWithError("missing value for " + arg);
            }
            opts.copyCount = ParseUint64(argv[++i], "copy-count");
        } else if (arg == "-bs" || arg == "--batch-size") {
            if (i + 1 >= argc) {
                ExitWithError("missing value for " + arg);
            }
            opts.batchSize = ParseUint64(argv[++i], "batch-size");
        } else if (arg == "-d" || arg == "--device") {
            if (i + 1 >= argc) {
                ExitWithError("missing value for " + arg);
            }
            opts.deviceId = ParseUint32(argv[++i], "device");
        } else if (arg == "-ws" || arg == "--world-size") {
            if (i + 1 >= argc) {
                ExitWithError("missing value for " + arg);
            }
            opts.worldRankSize = ParseUint32(argv[++i], "world-size");
        } else if (arg == "-lrs" || arg == "--local-rank-size") {
            if (i + 1 >= argc) {
                ExitWithError("missing value for " + arg);
            }
            opts.localRankSize = ParseUint32(argv[++i], "local-rank-size");
        } else if (arg == "-rs" || arg == "--rank-start") {
            if (i + 1 >= argc) {
                ExitWithError("missing value for " + arg);
            }
            opts.localRankStart = ParseUint32(argv[++i], "rank-start");
        } else if (arg == "-ip" || arg == "--ip") {
            if (i + 1 >= argc) {
                ExitWithError("missing value for " + arg);
            }
            opts.ipPort = argv[++i];
            ParseIp(opts.ipPort);
        } else if (arg == "-hbm-size" || arg == "--hbm-size") {
            if (i + 1 >= argc) {
                ExitWithError("missing value for " + arg);
            }
            opts.hbmPoolSize = ParseUint64(argv[++i], "hbm-size");
        } else if (arg == "-dram-size" || arg == "--dram-size") {
            if (i + 1 >= argc) {
                ExitWithError("missing value for " + arg);
            }
            opts.dramPoolSize = ParseUint64(argv[++i], "dram-size");
        } else if (arg == "-hm" || arg == "--host-mem") {
            if (i + 1 >= argc) {
                ExitWithError("missing value for " + arg);
            }
            ParseHostMem(argv[++i], opts);
        } else {
            ExitWithError("unknown option: " + arg);
        }
    }

    if (!hasType) {
        ExitWithError("missing required option: -t / --type");
    }

    CheckCmdBw(opts);
}

void RunBwTest(BwTestParam &opts)
{
    BandWidthCalculator bwCalculator(opts);
    auto ret = bwCalculator.MultiProcessExecute();
    if (ret != 0) {
        LOG_ERROR("bandwidth test failed.");
    }
}

int main(int32_t argc, char *argv[])
{
    if (argc < 2) {
        PrintHelp();
        return 0;
    }

    std::string cmd = argv[1];
    if (cmd == "-h" || cmd == "--help") {
        PrintHelp();
        return 0;
    } else if (cmd == "-bw" || cmd == "--bandwidth") {
        BwTestParam opts;
        opts.hbmPoolSize = DEFAULT_HBM_POOL_SIZE;
        opts.dramPoolSize = DEFAULT_DRAM_POOL_SIZE;
        ParseCmdBw(argc, argv, opts);
        RunBwTest(opts);
    } else {
        ExitWithError("unknown command: " + cmd);
    }

    LOG_INFO("main process exited.");
    return 0;
}
