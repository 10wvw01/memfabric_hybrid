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
#include <algorithm>
#include <array>
#include <vector>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>
#include <cstring>
#include <string>
#include <cstdio>
#include "band_width_calculator.h"

const uint32_t POOL_ALIGN_SIZE = 2UL * 1024UL * 1024UL;  // AclVmm 段要求 2M 对齐
const uint16_t BARRIOR_PORT = 21666U;
constexpr int32_t DIRECTION_TYPE_NUM_MAX = static_cast<int32_t>(CopyType::ALL_DIRECTION);

using OptFunc = void (BandWidthCalculator::*)(BarrierUtil *, uint32_t, uint32_t, BwTestResult *);

static inline uint64_t TimeNs()
{
    struct timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000UL + static_cast<uint64_t>(ts.tv_nsec);
}

void GenerateData(void *ptr, int32_t rank, uint32_t len)
{
    if (ptr == nullptr) {
        return;
    }
    int32_t *arr = (int32_t *)ptr;
    static int32_t mod = INT16_MAX;
    int32_t base = rank;
    for (uint32_t i = 0; i < len / sizeof(int); i++) {
        base = (base * 23 + 17) % mod;
        if ((i + rank) % 3 == 0) {
            arr[i] = -base; // 构造三分之一的负数
        } else {
            arr[i] = base;
        }
    }
}

int64_t CheckData(void *base, void *ptr, uint32_t len)
{
    int64_t wrongNum = 0;
    if (base == nullptr || ptr == nullptr) {
        return wrongNum;
    }
    int32_t *arr1 = (int32_t *)base;
    int32_t *arr2 = (int32_t *)ptr;
    for (uint32_t i = 0; i < len / sizeof(int); i++) {
        if (arr1[i] != arr2[i]) {
            wrongNum++;
        }
    }
    return wrongNum;
}

std::string ReplacePort(const std::string &input, uint16_t newPort)
{
    size_t colonPos = input.rfind(':');
    if (colonPos == std::string::npos) {
        return input;
    }
    return input.substr(0, colonPos + 1) + std::to_string(newPort);
}

std::string CopyType2Str(CopyType type)
{
    switch (type) {
        case CopyType::HOST_TO_DEVICE:
            return "H2D";
        case CopyType::DEVICE_TO_HOST:
            return "D2H";
        case CopyType::HOST_TO_REMOTE_DEVICE:
            return "H2RD";
        case CopyType::HOST_TO_REMOTE_HOST:
            return "H2RH";
        case CopyType::DEVICE_TO_REMOTE_DEVICE:
            return "D2RD";
        case CopyType::DEVICE_TO_REMOTE_HOST:
            return "D2RH";
        case CopyType::REMOTE_HOST_TO_DEVICE:
            return "RH2D";
        case CopyType::REMOTE_HOST_TO_HOST:
            return "RH2H";
        case CopyType::REMOTE_DEVICE_TO_DEVICE:
            return "RD2D";
        case CopyType::REMOTE_DEVICE_TO_HOST:
            return "RD2H";
        default:
            return "UNKNOWN";
    }
    return "UNKNOWN";
}

const char *PoolNameOfDirection(CopyType type)
{
    // 目的池类型决定执行实体: DEVICE 池 -> HBM 实体(SDMA op 位), HOST 池 -> DRAM 实体(HOST_SHM)
    switch (type) {
        case CopyType::DEVICE_TO_HOST:
        case CopyType::HOST_TO_REMOTE_HOST:
        case CopyType::DEVICE_TO_REMOTE_HOST:
        case CopyType::REMOTE_HOST_TO_DEVICE:
        case CopyType::REMOTE_HOST_TO_HOST:
            return "DRAM";
        default:
            return "HBM";
    }
}

uint32_t GenRmtRankId(uint32_t rankId, uint32_t localRankSize, uint32_t worldSize)
{
    if (localRankSize == worldSize) {
        return (rankId + 1) % worldSize;
    } else {
        return (rankId + localRankSize) % worldSize;
    }
}

BandWidthCalculator::BandWidthCalculator(BwTestParam &param)
{
    cmdParam_ = param;
    if (param.localRankStart == 0) {
        isMaster_ = true;
    }
}

int32_t BandWidthCalculator::MultiProcessExecute()
{
    std::vector<pid_t> pids(cmdParam_.localRankSize);
    std::vector<std::array<int, 2>> pipes(cmdParam_.localRankSize);
    auto ret = ForkSubProcesses(pids, pipes);
    if (ret != 0) {
        return ret;
    }
    return CollectSubProcessResults(pids, pipes);
}

int32_t BandWidthCalculator::ForkSubProcesses(std::vector<pid_t> &pids, std::vector<std::array<int, 2>> &pipes)
{
    for (uint32_t i = 0; i < cmdParam_.localRankSize; ++i) {
        if (pipe(pipes[i].data()) == -1) {
            LOG_ERROR("pipe failed, subProcess:" << i);
            return -1;
        }
        pids[i] = fork();
        if (pids[i] < 0) {
            LOG_ERROR("fork failed, pid:" << pids[i] << " subProcess:" << i);
            return -1;
        }
        if (pids[i] == 0) {
            close(pipes[i][0]);
            RunSubProcess(i, pipes[i][1]);
        }
        close(pipes[i][1]);
    }
    return 0;
}

void BandWidthCalculator::RunSubProcess(uint32_t idx, int32_t pipeFdWrite)
{
    LOG_INFO("subprocess (" << idx << ") start.");
    auto ret = Execute(idx + cmdParam_.deviceId, idx + cmdParam_.localRankStart, cmdParam_.localRankSize,
                       cmdParam_.worldRankSize, pipeFdWrite);
    LOG_INFO("subprocess (" << idx << ") exited" << (ret != 0 ? " with error" : "") << ".");
    std::exit(ret != 0 ? 1 : 0);
}

int32_t BandWidthCalculator::CollectSubProcessResults(std::vector<pid_t> &pids,
                                                      std::vector<std::array<int, 2>> &pipes)
{
    int status[RANK_SIZE_MAX];
    for (uint32_t i = 0; i < cmdParam_.localRankSize; ++i) {
        waitpid(pids[i], &status[i], 0);
        if (WIFEXITED(status[i]) && WEXITSTATUS(status[i]) != 0) {
            LOG_INFO("subprocess exit error: " << i);
        } else if (WIFSIGNALED(status[i])) {
            LOG_INFO("subprocess (" << i << ") terminated by signal: " << WTERMSIG(status[i]));
        }
    }
    std::vector<BwTestResult> resultVec;
    for (uint32_t i = 0; i < cmdParam_.localRankSize; ++i) {
        ReadPipeResult(pipes[i][0], resultVec);
        close(pipes[i][0]);
    }
    PrintResult(resultVec);
    return 0;
}

void BandWidthCalculator::ReadPipeResult(int32_t fd, std::vector<BwTestResult> &resultVec)
{
    BwTestResult results[DIRECTION_TYPE_NUM_MAX];
    auto len = sizeof(BwTestResult) * DIRECTION_TYPE_NUM_MAX;
    auto n = read(fd, &results, len);
    if (n == 0) {
        LOG_WARN("pipe " << fd << " no data received");
        return;
    }
    if (n < 0) {
        LOG_ERROR("pipe read failed, fd:" << fd << " errno:" << errno);
        return;
    }
    if (n < len) {
        LOG_WARN("pipe partial data received: " << n << " bytes, fd:" << fd);
        return;
    }
    for (int32_t j = 0; j < DIRECTION_TYPE_NUM_MAX; ++j) {
        if (results[j].flag >= 0) {
            resultVec.push_back(results[j]);
        }
    }
}

int32_t BandWidthCalculator::PreInit(uint32_t deviceId, BarrierUtil *&barrier, uint32_t rankId, uint32_t rkSize,
                                     aclrtStream *stream)
{
    CHECK_RET_ERR(barrier != nullptr, "barrier is not nullptr, rank:" << rankId);
    auto ret = aclInit(nullptr);
    CHECK_RET_ERR(ret, "acl init failed, ret:" << ret << " rank:" << rankId);
    ret = aclrtSetDevice(deviceId);
    CHECK_RET_ERR(ret, "acl set device failed, ret:" << ret << " deviceId:" << deviceId << " rank:" << rankId);

    aclrtStream ss = nullptr;
    ret = aclrtCreateStream(&ss);
    CHECK_RET_ERR(ret, "acl create stream failed, ret:" << ret << " rank:" << rankId);

    ret = smem_init(0);
    CHECK_RET_ERR(ret, "smem init failed, ret:" << ret << " rank:" << rankId);

    smem_set_log_level(2);

    smem_bm_config_t config;
    (void)smem_bm_config_init(&config);
    config.autoRanking = false;
    config.rankId = rankId;
    if (rankId != 0) {
        config.startConfigStoreServer = false;
    }
    ret = smem_bm_init(cmdParam_.ipPort.c_str(), rkSize, deviceId, &config);
    CHECK_RET_ERR(ret, "smem bm init failed, ret:" << ret << " rank:" << rankId);

    barrier = new (std::nothrow) BarrierUtil;
    CHECK_RET_ERR((barrier == nullptr), "malloc barrier failed, rank:" << rankId);
    ret = barrier->Init(deviceId, rankId, rkSize, ReplacePort(cmdParam_.ipPort, BARRIOR_PORT));
    CHECK_RET_ERR(ret, "barrier init failed, ret:" << ret << " rank:" << rankId << " " << errno);

    *stream = ss;
    LOG_INFO(" ==================== [TEST] init, rank:" << rankId << " devId:" << deviceId);
    return 0;
}

void BandWidthCalculator::FinalizeAll(uint32_t deviceId, BarrierUtil *&barrier, aclrtStream *stream)
{
    if (barrier != nullptr) {
        delete barrier;
        barrier = nullptr;
    }
    smem_bm_uninit(0);
    if (stream != nullptr) {
        aclrtDestroyStream(stream);
    }
    aclrtResetDevice(deviceId);
    aclFinalize();
}

int32_t BandWidthCalculator::CreateAndJoinPools(uint32_t rankId)
{
    // HBM 池: SDMA op 位 -> 310P AclVmm 段 + aclrtMemcpy 算子(DRAM 参数必须为 0)
    hbmHandle_ = smem_bm_create(0, 0, SMEMB_DATA_OP_SDMA, 0, cmdParam_.hbmPoolSize, 0);
    CHECK_RET_ERR((hbmHandle_ == nullptr), "smem_bm_create(HBM) failed, rank:" << rankId
                                                                          << " hbmPoolSize:" << cmdParam_.hbmPoolSize);
    auto ret = smem_bm_join(hbmHandle_, 0);
    CHECK_RET_ERR(ret, "smem_bm_join(HBM) failed, ret:" << ret << " rank:" << rankId);

    // DRAM 池: HOST_SHM, 禁止与 HBM 同实体(smem_bm 层强校验), 必须独立 entity
    dramHandle_ = smem_bm_create(1, 0, SMEMB_DATA_OP_HOST_SHM, cmdParam_.dramPoolSize, 0, 0);
    CHECK_RET_ERR((dramHandle_ == nullptr), "smem_bm_create(DRAM) failed, rank:" << rankId
                                                                           << " dramPoolSize:" << cmdParam_.dramPoolSize);
    ret = smem_bm_join(dramHandle_, 0);
    CHECK_RET_ERR(ret, "smem_bm_join(DRAM) failed, ret:" << ret << " rank:" << rankId);
    LOG_INFO("smem_bm_join(HBM+DRAM) ok, rank:" << rankId);
    return 0;
}

void BandWidthCalculator::DestroyPools()
{
    // 逆序销毁: DRAM 后建先毁(对齐 07 示例)
    if (dramHandle_ != nullptr) {
        smem_bm_destroy(dramHandle_);
        dramHandle_ = nullptr;
    }
    if (hbmHandle_ != nullptr) {
        smem_bm_destroy(hbmHandle_);
        hbmHandle_ = nullptr;
    }
}

smem_bm_t BandWidthCalculator::HandleForRmt(smem_bm_mem_type rmtMemType) const
{
    return (rmtMemType == SMEM_MEM_TYPE_DEVICE) ? hbmHandle_ : dramHandle_;
}

int32_t BandWidthCalculator::PrepareLocalMem(uint32_t rankId)
{
    uint64_t len = cmdParam_.batchSize * cmdParam_.copySize;
    void *dataPtr = malloc(len);
    CHECK_RET_ERR((dataPtr == nullptr), "malloc pattern buffer failed, len:" << len << " rank:" << rankId);
    GenerateData(dataPtr, static_cast<int32_t>(rankId), len);

    // 注: 裸 host/device 指针无需 register_user_mem —— 各方向 copyType 显式钉住 local 侧
    // 内存类型(TransToHybmDirection), 且 AclVmm 段不支持 RegisterMemory(对齐 07 示例不注册)
    auto ret = PrepareLocalHostMem(dataPtr, len);
    if (ret == 0) {
        ret = PrepareLocalDeviceMem(dataPtr, len);
    }
    if (ret == 0) {
        ret = PrefillDramPool(dataPtr, len, rankId);
    }
    free(dataPtr);
    return ret;
}

int32_t BandWidthCalculator::PrepareLocalHostMem(const void *dataPtr, uint64_t len)
{
    auto ret = posix_memalign(&localDram_, 4096, len);
    CHECK_RET_ERR(ret, "malloc local dram failed, len:" << len);
    ret = aclrtMemcpy(localDram_, len, dataPtr, len, ACL_MEMCPY_HOST_TO_HOST);
    CHECK_RET_ERR((ret != 0), "memcpy pattern to local dram failed, ret:" << ret << " len:" << len);
    return 0;
}

int32_t BandWidthCalculator::PrepareLocalDeviceMem(const void *dataPtr, uint64_t len)
{
    auto ret = aclrtMalloc(&localHbm_, len, ACL_MEM_MALLOC_HUGE_FIRST);
    CHECK_RET_ERR((ret != 0 || localHbm_ == nullptr), "malloc local hbm failed, ret:" << ret << " len:" << len);
    ret = aclrtMemcpy(localHbm_, len, dataPtr, len, ACL_MEMCPY_HOST_TO_DEVICE);
    CHECK_RET_ERR((ret != 0), "memcpy pattern to local hbm failed, ret:" << ret << " len:" << len);
    return 0;
}

int32_t BandWidthCalculator::PrefillDramPool(const void *dataPtr, uint64_t len, uint32_t rankId)
{
    // HOST_SHM 池为真实 host 内存, own 槽位直填本校验 pattern;
    // HBM 池 VMM VA 为设备地址, 严禁 CPU 直接解引用(上游 F14)
    void *gva = smem_bm_ptr_by_mem_type(dramHandle_, SMEM_MEM_TYPE_HOST, rankId);
    CHECK_RET_ERR((gva == nullptr), "get own DRAM pool gva failed, rank:" << rankId);
    memcpy(gva, dataPtr, len);
    CHECK_RET_ERR((memcmp(gva, dataPtr, len) != 0), "prefill own DRAM pool check failed, len:" << len);
    return 0;
}

int32_t BandWidthCalculator::PrepareCopyParam(smem_bm_mem_type localMemType, smem_bm_mem_type rmtMemType,
                                              uint32_t gvaRankId, smem_bm_t handle, BatchCopyParam &param)
{
    if (rmtMemType != SMEM_MEM_TYPE_DEVICE && rmtMemType != SMEM_MEM_TYPE_HOST) {
        LOG_ERROR("rmtMemType type error, localMemType:" << localMemType << " rmtMemType:" << rmtMemType);
        return -1;
    }
    void *localPtr = nullptr;
    if (localMemType == SMEM_MEM_TYPE_LOCAL_DEVICE) {
        localPtr = localHbm_;
    } else if (localMemType == SMEM_MEM_TYPE_LOCAL_HOST) {
        localPtr = localDram_;
    }
    CHECK_RET_ERR((localPtr == nullptr), "localPtr is nullptr, localMemType:" << localMemType);
    uint64_t gva = (uint64_t)smem_bm_ptr_by_mem_type(handle, rmtMemType, gvaRankId);
    CHECK_RET_ERR((gva == 0), "get pool gva failed, rmtMemType:" << rmtMemType << " gvaRankId:" << gvaRankId);
    uint64_t offset = 0;
    for (uint32_t i = 0; i < param.batchSize; i++) {
        param.localAddrs[i] = reinterpret_cast<void *>((uintptr_t)localPtr + i * cmdParam_.copySize);
        param.globalAddrs[i] = reinterpret_cast<void *>(gva + offset);
        param.dataSizes[i] = cmdParam_.copySize;
        offset += cmdParam_.copySize;
    }
    return 0;
}

int64_t BandWidthCalculator::CheckCopyResult(smem_bm_mem_type localMemType, smem_bm_mem_type rmtMemType,
                                              uint32_t gvaRankId, smem_bm_t handle)
{
    if (rmtMemType != SMEM_MEM_TYPE_DEVICE && rmtMemType != SMEM_MEM_TYPE_HOST) {
        LOG_ERROR("rmtMemType type error, localMemType:" << localMemType << " rmtMemType:" << rmtMemType);
        return -1;
    }
    int ret = 0;
    uint64_t len = cmdParam_.copySize * cmdParam_.batchSize;
    void *localPtr = malloc(len);
    CHECK_RET_ERR((localPtr == nullptr), "malloc failed, len:" << len);
    if (localMemType == SMEM_MEM_TYPE_LOCAL_DEVICE) {
        ret = aclrtMemcpy(localPtr, len, localHbm_, len, ACL_MEMCPY_DEVICE_TO_HOST);
    } else if (localMemType == SMEM_MEM_TYPE_LOCAL_HOST) {
        ret = aclrtMemcpy(localPtr, len, localDram_, len, ACL_MEMCPY_HOST_TO_HOST);
    }
    CHECK_RET_ERR((ret != 0), "memcpy local failed, ret:" << ret << " len:" << len);

    void *rmtPtr = malloc(len);
    CHECK_RET_ERR((rmtPtr == nullptr), "malloc rmt failed, len:" << len);
    void *gva = smem_bm_ptr_by_mem_type(handle, rmtMemType, gvaRankId);
    CHECK_RET_ERR((gva == nullptr), "get pool gva failed, rmtMemType:" << rmtMemType << " gvaRankId:" << gvaRankId);
    smem_copy_params params{};
    params.src = gva;
    params.dest = rmtPtr;
    params.dataSize = len;
    ret = smem_bm_copy(handle, &params, SMEMB_COPY_G2H, 0);
    CHECK_RET_ERR((ret != 0), "g2h copy failed, ret:" << ret << " rmtMemType:" << rmtMemType);
    auto wrongNum = CheckData(localPtr, rmtPtr, len);
    if (wrongNum != 0) {
        LOG_ERROR("check data failed, localMemType:" << localMemType << " rmtMemType:" << rmtMemType
                                                     << " wrongNum:" << wrongNum);
    }
    free(localPtr);
    free(rmtPtr);
    return wrongNum;
}

void BandWidthCalculator::PrintResult(std::vector<BwTestResult> &results)
{
    if (results.empty()) {
        LOG_INFO("No bandwidth test results available.");
        return;
    }

    std::vector<BwTestResult> sortedResults = results;
    std::sort(sortedResults.begin(), sortedResults.end(), [](const BwTestResult &a, const BwTestResult &b) {
        if (a.flag != b.flag) {
            return a.flag < b.flag;
        }
        return a.devId < b.devId;
    });

    std::cout << std::endl
              << "  copy_size:" << results[0].copySize << "  copy_count:" << results[0].copyCount
              << "  batch_size:" << results[0].batchSize << std::endl;
    std::cout << "----------------------------------------------------------"
                 "Band Width Test(310P dual pool)----------------------------------------------------------\n";
    const int32_t DIGIT_WIDTH = 10;
    std::cout << std::left << std::setw(DIGIT_WIDTH) << "Type" << std::setw(6) << "Pool" << std::setw(DIGIT_WIDTH)
              << "NPU" << std::setw(DIGIT_WIDTH) << "Rank" << std::setw(20) << "TotalSize(KB)" << std::setw(20)
              << "TotalTime(us)" << std::setw(20) << "AvgTime(us)" << std::setw(20) << "BW(GB/s)"
              << std::setw(DIGIT_WIDTH) << "WrongBytes(B)" << std::endl;

    for (const auto &r : sortedResults) {
        if (r.flag < 0) {
            continue;
        }
        uint64_t totalKB = r.copySize * r.copyCount * r.batchSize / 1024;
        uint64_t avgTimeUs = r.totalTimeUs / r.copyCount;
        auto bandwidthGBps = totalKB * 1024.0 / r.totalTimeUs / 1000.0;

        std::cout << std::left << std::setw(DIGIT_WIDTH) << r.typeName << std::setw(6)
                  << PoolNameOfDirection(static_cast<CopyType>(r.flag)) << std::setw(DIGIT_WIDTH) << r.devId
                  << std::setw(DIGIT_WIDTH) << r.rankId << std::setw(20) << totalKB << std::setw(20) << r.totalTimeUs
                  << std::setw(20) << std::fixed << std::setprecision(0) << avgTimeUs << std::setw(20) << std::fixed
                  << std::setprecision(3) << bandwidthGBps << std::setw(DIGIT_WIDTH) << r.wrongNum << std::endl;
    }
    std::cout << std::endl;
}

void BandWidthCalculator::SendResult(BwTestResult *results, int32_t pipeFdWrite)
{
    if (pipeFdWrite == 0) {
        return;
    }
    auto len = sizeof(BwTestResult) * DIRECTION_TYPE_NUM_MAX;
    auto ret = write(pipeFdWrite, results, len);
    if (ret != len) {
        LOG_ERROR("pipe write failed, errno:" << errno);
    }
    close(pipeFdWrite);
}

int32_t BandWidthCalculator::Execute(uint32_t deviceId, uint32_t rankId, uint32_t localRankNum, uint32_t rkSize,
                                     int32_t pipeFdWrite)
{
    aclrtStream stream;
    BarrierUtil *barrier = nullptr;
    auto ret = PreInit(deviceId, barrier, rankId, rkSize, &stream);
    CHECK_RET_ERR(ret, "pre init failed, ret:" << ret << " rank:" << rankId);

    ret = CreateAndJoinPools(rankId); // 根因错误已记录
    if (ret == 0) {
        ret = barrier->Barrier();
        if (ret != 0) {
            LOG_ERROR("barrier failed after pools join, ret:" << ret << " rank:" << rankId);
        }
    }
    if (ret == 0) {
        ret = PrepareLocalMem(rankId); // 根因错误已记录
    }
    if (ret == 0) {
        ret = barrier->Barrier();
        if (ret != 0) {
            LOG_ERROR("barrier failed after prepare local mem, ret:" << ret << " rank:" << rankId);
        }
    }
    if (ret != 0) {
        DestroyPools();
        FinalizeAll(deviceId, barrier, &stream);
        return -1;
    }
    LOG_INFO(" ==================== [TEST] bm init ok, rank:" << rankId);

    BwTestResult testResults[DIRECTION_TYPE_NUM_MAX];
    for (auto i = 0; i < DIRECTION_TYPE_NUM_MAX; ++i) {
        testResults[i].flag = -1;
        testResults[i].devId = deviceId;
        testResults[i].rankId = rankId;
    }
    ret = BandWidthCalculation(barrier, rankId, GenRmtRankId(rankId, localRankNum, cmdParam_.worldRankSize),
                               testResults);
    if (ret == 0) {
        SendResult(testResults, pipeFdWrite);
    }
    DestroyPools();
    FinalizeAll(deviceId, barrier, &stream);
    return 0;
}

int32_t BandWidthCalculator::BandWidthCalculation(BarrierUtil *barrier, uint32_t rankId, uint32_t remoteRankId,
                                                  BwTestResult *results)
{
    static const OptFunc OPT_FUNC_MAP[DIRECTION_TYPE_NUM_MAX] = {
        &BandWidthCalculator::H2D,  &BandWidthCalculator::D2H,  &BandWidthCalculator::H2RD, &BandWidthCalculator::H2RH,
        &BandWidthCalculator::D2RD, &BandWidthCalculator::D2RH, &BandWidthCalculator::RH2D, &BandWidthCalculator::RH2H,
        &BandWidthCalculator::RD2D, &BandWidthCalculator::RD2H,
    };
    if (static_cast<int32_t>(cmdParam_.type) > DIRECTION_TYPE_NUM_MAX || static_cast<int32_t>(cmdParam_.type) < 0) {
        LOG_ERROR("copy type error" << static_cast<int32_t>(cmdParam_.type));
        return -1;
    }
    if (cmdParam_.type != CopyType::ALL_DIRECTION) {
        if (!isMaster_) {
            barrier->Barrier();
        }
        (this->*OPT_FUNC_MAP[static_cast<int32_t>(cmdParam_.type)])(barrier, rankId, remoteRankId, results);
        if (isMaster_) {
            barrier->Barrier();
        }
        return 0;
    }
    for (auto i = 0; i < DIRECTION_TYPE_NUM_MAX; ++i) {
        if (!isMaster_) {
            barrier->Barrier();
        }
        (this->*OPT_FUNC_MAP[i])(barrier, rankId, remoteRankId, results);
        if (isMaster_) {
            barrier->Barrier();
        }
    }
    return 0;
}

void BandWidthCalculator::BatchCopyPut(smem_bm_mem_type localMemType, smem_bm_mem_type rmtMemType, uint32_t gvaRankId,
                                       smem_bm_t handle, CopyType type, BwTestResult &result)
{
    void *localAddrs[cmdParam_.batchSize];
    void *globalAddrs[cmdParam_.batchSize];
    uint64_t sizeList[cmdParam_.batchSize];
    BatchCopyParam tmpParam = {(void **)localAddrs, globalAddrs, sizeList, static_cast<uint32_t>(cmdParam_.batchSize)};
    int ret = PrepareCopyParam(localMemType, rmtMemType, gvaRankId, handle, tmpParam);
    CHECK_RET_VOID(ret, "prepare local failed, ret:" << ret << " rank:" << gvaRankId);

    smem_batch_copy_params param = {};
    param.sources = tmpParam.localAddrs;
    param.destinations = tmpParam.globalAddrs;
    param.dataSizes = tmpParam.dataSizes;
    param.batchSize = tmpParam.batchSize;

    smem_bm_copy_type copyType = SMEMB_COPY_L2G;
    if (localMemType == SMEM_MEM_TYPE_LOCAL_DEVICE) {
        copyType = SMEMB_COPY_L2G;
    } else if (localMemType == SMEM_MEM_TYPE_LOCAL_HOST) {
        copyType = SMEMB_COPY_H2G;
    } else {
        LOG_ERROR("localMemType type error, localMemType:" << localMemType << " rmtMemType:" << rmtMemType);
        return;
    }
    uint64_t startTime = TimeNs();
    for (uint64_t i = 0; i < cmdParam_.copyCount; i++) {
        ret = smem_bm_copy_batch(handle, &param, copyType, 0);
        CHECK_RET_VOID(ret, "copy local to pool failed, ret:" << ret << " rank:" << gvaRankId);
    }
    uint64_t totalTimeUs = (TimeNs() - startTime) / 1000;
    auto direction = CopyType2Str(type);
    result.wrongNum = CheckCopyResult(localMemType, rmtMemType, gvaRankId, handle);
    if (result.wrongNum == -1) {
        LOG_ERROR(direction << " check data failed, rank:" << gvaRankId);
        return;
    } else if (result.wrongNum != 0) {
        LOG_WARN(direction << " check data wrong num:" << result.wrongNum << ", rank:" << gvaRankId);
    }
    result.totalTimeUs = totalTimeUs;
    result.copySize = cmdParam_.copySize;
    result.batchSize = cmdParam_.batchSize;
    result.copyCount = cmdParam_.copyCount;
    result.flag = static_cast<int32_t>(type);
    std::fill_n(result.typeName, sizeof(result.typeName), 0);
    std::copy_n(direction.begin(), std::min(direction.size(), sizeof(result.typeName) - 1), result.typeName);
    LOG_INFO(direction << " finished. rank: " << gvaRankId << ", flag:" << result.flag);
}

void BandWidthCalculator::BatchCopyGet(smem_bm_mem_type localMemType, smem_bm_mem_type rmtMemType, uint32_t gvaRankId,
                                       smem_bm_t handle, CopyType type, BwTestResult &result)
{
    void *localAddrs[cmdParam_.batchSize];
    void *globalAddrs[cmdParam_.batchSize];
    uint64_t sizeList[cmdParam_.batchSize];
    BatchCopyParam tmpParam = {(void **)localAddrs, globalAddrs, sizeList, static_cast<uint32_t>(cmdParam_.batchSize)};
    int ret = PrepareCopyParam(localMemType, rmtMemType, gvaRankId, handle, tmpParam);
    CHECK_RET_VOID(ret, "prepare local failed, ret:" << ret << " rank:" << gvaRankId);

    smem_batch_copy_params param = {};
    param.sources = tmpParam.globalAddrs;
    param.destinations = tmpParam.localAddrs;
    param.dataSizes = tmpParam.dataSizes;
    param.batchSize = tmpParam.batchSize;

    smem_bm_copy_type copyType = SMEMB_COPY_G2L;
    if (localMemType == SMEM_MEM_TYPE_LOCAL_DEVICE) {
        copyType = SMEMB_COPY_G2L;
    } else if (localMemType == SMEM_MEM_TYPE_LOCAL_HOST) {
        copyType = SMEMB_COPY_G2H;
    } else {
        LOG_ERROR("localMemType type error, localMemType:" << localMemType << " rmtMemType:" << rmtMemType);
        return;
    }
    uint64_t startTime = TimeNs();
    for (uint64_t i = 0; i < cmdParam_.copyCount; i++) {
        ret = smem_bm_copy_batch(handle, &param, copyType, 0);
        CHECK_RET_VOID(ret, "copy pool to local failed, ret:" << ret << " rank:" << gvaRankId);
    }
    uint64_t totalTimeUs = (TimeNs() - startTime) / 1000;
    auto direction = CopyType2Str(type);
    result.wrongNum = CheckCopyResult(localMemType, rmtMemType, gvaRankId, handle);
    if (result.wrongNum == -1) {
        LOG_ERROR(direction << " check data failed, rank:" << gvaRankId);
        return;
    } else if (result.wrongNum != 0) {
        LOG_ERROR(direction << " check data wrong num:" << result.wrongNum << ", rank:" << gvaRankId);
    }
    result.totalTimeUs = totalTimeUs;
    result.copySize = cmdParam_.copySize;
    result.batchSize = cmdParam_.batchSize;
    result.copyCount = cmdParam_.copyCount;
    result.flag = static_cast<int32_t>(type);
    std::fill_n(result.typeName, sizeof(result.typeName), 0);
    std::copy_n(direction.begin(), std::min(direction.size(), sizeof(result.typeName) - 1), result.typeName);
    LOG_INFO(direction << " finished. rank: " << gvaRankId << ", flag:" << result.flag);
}

void BandWidthCalculator::H2D(BarrierUtil *barrier, uint32_t rankId, uint32_t remoteRankId, BwTestResult *results)
{
    BatchCopyPut(SMEM_MEM_TYPE_LOCAL_HOST, SMEM_MEM_TYPE_DEVICE, rankId, HandleForRmt(SMEM_MEM_TYPE_DEVICE),
                 CopyType::HOST_TO_DEVICE, results[static_cast<int32_t>(CopyType::HOST_TO_DEVICE)]);
    auto ret = barrier->Barrier();
    CHECK_RET_VOID(ret, "barrier failed after H2D, ret:" << ret << " rank:" << rankId);
}

void BandWidthCalculator::D2H(BarrierUtil *barrier, uint32_t rankId, uint32_t remoteRankId, BwTestResult *results)
{
    BatchCopyPut(SMEM_MEM_TYPE_LOCAL_DEVICE, SMEM_MEM_TYPE_HOST, rankId, HandleForRmt(SMEM_MEM_TYPE_HOST),
                 CopyType::DEVICE_TO_HOST, results[static_cast<int32_t>(CopyType::DEVICE_TO_HOST)]);
    auto ret = barrier->Barrier();
    CHECK_RET_VOID(ret, "barrier failed after D2H, ret:" << ret << " rank:" << rankId);
}

void BandWidthCalculator::H2RD(BarrierUtil *barrier, uint32_t rankId, uint32_t remoteRankId, BwTestResult *results)
{
    BatchCopyPut(SMEM_MEM_TYPE_LOCAL_HOST, SMEM_MEM_TYPE_DEVICE, remoteRankId, HandleForRmt(SMEM_MEM_TYPE_DEVICE),
                 CopyType::HOST_TO_REMOTE_DEVICE, results[static_cast<int32_t>(CopyType::HOST_TO_REMOTE_DEVICE)]);
    auto ret = barrier->Barrier();
    CHECK_RET_VOID(ret, "barrier failed after H2RD, ret:" << ret << " rank:" << rankId);
}

void BandWidthCalculator::H2RH(BarrierUtil *barrier, uint32_t rankId, uint32_t remoteRankId, BwTestResult *results)
{
    BatchCopyPut(SMEM_MEM_TYPE_LOCAL_HOST, SMEM_MEM_TYPE_HOST, remoteRankId, HandleForRmt(SMEM_MEM_TYPE_HOST),
                 CopyType::HOST_TO_REMOTE_HOST, results[static_cast<int32_t>(CopyType::HOST_TO_REMOTE_HOST)]);
    auto ret = barrier->Barrier();
    CHECK_RET_VOID(ret, "barrier failed after H2RH, ret:" << ret << " rank:" << rankId);
}

void BandWidthCalculator::D2RD(BarrierUtil *barrier, uint32_t rankId, uint32_t remoteRankId, BwTestResult *results)
{
    BatchCopyPut(SMEM_MEM_TYPE_LOCAL_DEVICE, SMEM_MEM_TYPE_DEVICE, remoteRankId, HandleForRmt(SMEM_MEM_TYPE_DEVICE),
                 CopyType::DEVICE_TO_REMOTE_DEVICE, results[static_cast<int32_t>(CopyType::DEVICE_TO_REMOTE_DEVICE)]);
    auto ret = barrier->Barrier();
    CHECK_RET_VOID(ret, "barrier failed after D2RD, ret:" << ret << " rank:" << rankId);
}

void BandWidthCalculator::D2RH(BarrierUtil *barrier, uint32_t rankId, uint32_t remoteRankId, BwTestResult *results)
{
    BatchCopyPut(SMEM_MEM_TYPE_LOCAL_DEVICE, SMEM_MEM_TYPE_HOST, remoteRankId, HandleForRmt(SMEM_MEM_TYPE_HOST),
                 CopyType::DEVICE_TO_REMOTE_HOST, results[static_cast<int32_t>(CopyType::DEVICE_TO_REMOTE_HOST)]);
    auto ret = barrier->Barrier();
    CHECK_RET_VOID(ret, "barrier failed after D2RH, ret:" << ret << " rank:" << rankId);
}

void BandWidthCalculator::RH2D(BarrierUtil *barrier, uint32_t rankId, uint32_t remoteRankId, BwTestResult *results)
{
    BatchCopyGet(SMEM_MEM_TYPE_LOCAL_DEVICE, SMEM_MEM_TYPE_HOST, remoteRankId, HandleForRmt(SMEM_MEM_TYPE_HOST),
                 CopyType::REMOTE_HOST_TO_DEVICE, results[static_cast<int32_t>(CopyType::REMOTE_HOST_TO_DEVICE)]);
    auto ret = barrier->Barrier();
    CHECK_RET_VOID(ret, "barrier failed after RH2D, ret:" << ret << " rank:" << rankId);
}

void BandWidthCalculator::RH2H(BarrierUtil *barrier, uint32_t rankId, uint32_t remoteRankId, BwTestResult *results)
{
    BatchCopyGet(SMEM_MEM_TYPE_LOCAL_HOST, SMEM_MEM_TYPE_HOST, remoteRankId, HandleForRmt(SMEM_MEM_TYPE_HOST),
                 CopyType::REMOTE_HOST_TO_HOST, results[static_cast<int32_t>(CopyType::REMOTE_HOST_TO_HOST)]);
    auto ret = barrier->Barrier();
    CHECK_RET_VOID(ret, "barrier failed after RH2H, ret:" << ret << " rank:" << rankId);
}

void BandWidthCalculator::RD2D(BarrierUtil *barrier, uint32_t rankId, uint32_t remoteRankId, BwTestResult *results)
{
    BatchCopyGet(SMEM_MEM_TYPE_LOCAL_DEVICE, SMEM_MEM_TYPE_DEVICE, remoteRankId, HandleForRmt(SMEM_MEM_TYPE_DEVICE),
                 CopyType::REMOTE_DEVICE_TO_DEVICE, results[static_cast<int32_t>(CopyType::REMOTE_DEVICE_TO_DEVICE)]);
    auto ret = barrier->Barrier();
    CHECK_RET_VOID(ret, "barrier failed after RD2D, ret:" << ret << " rank:" << rankId);
}

void BandWidthCalculator::RD2H(BarrierUtil *barrier, uint32_t rankId, uint32_t remoteRankId, BwTestResult *results)
{
    BatchCopyGet(SMEM_MEM_TYPE_LOCAL_HOST, SMEM_MEM_TYPE_DEVICE, remoteRankId, HandleForRmt(SMEM_MEM_TYPE_DEVICE),
                 CopyType::REMOTE_DEVICE_TO_HOST, results[static_cast<int32_t>(CopyType::REMOTE_DEVICE_TO_HOST)]);
    auto ret = barrier->Barrier();
    CHECK_RET_VOID(ret, "barrier failed after RD2H, ret:" << ret << " rank:" << rankId);
}
