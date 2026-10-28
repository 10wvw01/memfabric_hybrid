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

/* hybm_sdma_orchestrator.cpp — 310P 设备发起 SDMA 的 AICPU 编排资源管理(实现)
 * 机制档案见 docs/310p/机制档案.md(SQE 配方/CP 流/工作区/通知链)。
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE // dladdr(GNU 扩展, 构建系统可能已定义)
#endif
#include "hybm_sdma_orchestrator.h"

#include <cctype>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <iterator>
#include <string>
#include <unistd.h>

#include <signal.h>
#include <sys/wait.h>

#include "dl_rt_api.h"
#include "mf_env_define.h"

namespace ock {
namespace mf {
namespace {
/* ACL_STREAM_DEVICE_USE_ONLY 的值(acl_rt.h:0x20): SQ 注册进 CP1 trs 表(AICPU 可查可投的前提) */
constexpr uint32_t HYBM_ACL_STREAM_DEVICE_USE_ONLY = 0x20U;
constexpr uint32_t HYBM_SDMA_ORCH_BLOCK_DIM = 1;       /* 单 block(kernel 状态机单线程语义) */
constexpr char HYBM_SDMA_ORCH_FUNC_NAME[] = "HybmSdmaOrchEpoch"; /* epoch 消费 kernel 入口符号 */
/* CWD 缺省 json 名随 KVER 生成(主构建经 CMake 注入 MF_SDMA_ORCH_KVER, 与设备侧文件名同步;
 * 独立编译场景无注入时用兜底值, 须与 hybm_kernel/CMakeLists 的 KVER 手动同步) */
#ifndef MF_SDMA_ORCH_KVER
#define MF_SDMA_ORCH_KVER 28
#endif
#define HYBM_SDMA_KVER_STR_(x) #x
#define HYBM_SDMA_KVER_STR(x) HYBM_SDMA_KVER_STR_(x)
constexpr char HYBM_SDMA_ORCH_DEFAULT_JSON[] = "libmf_sdma_orch_v" HYBM_SDMA_KVER_STR(MF_SDMA_ORCH_KVER) ".json";
/* CP1 搜索路径(实测: 每卡独立设备 OS 文件系统, 须在本卡设备上部署) */
constexpr char HYBM_SDMA_ORCH_DEV_SEARCH_DIR[] = "/usr/lib64/aicpu_kernels/0/aicpu_kernels_device";
constexpr uint32_t HYBM_SDMA_ORCH_SUPERVISOR_MAX_ERR = 16; /* 监督线程连续失败上限(放弃后数据面停在背压) */

/* 解析每轮时长(秒): MF_SDMA_ORCH_EPOCH_BUDGET_S 显式指定;
 * 非法/未设 → 默认 15s; 4294967295 → 永不退出的专用值(诊断模式, 会阻塞全设备同步) */
uint32_t ResolveEpochBudgetS()
{
    const std::string &val = env::MF_SDMA_ORCH_EPOCH_BUDGET_S;
    if (val.empty()) {
        return HYBM_SDMA_ORCH_EPOCH_TIME_BUDGET_S;
    }
    errno = 0;
    char *end = nullptr;
    const unsigned long parsed = std::strtoul(val.c_str(), &end, 10);
    if (errno != 0 || end == nullptr || *end != '\0' || parsed > 0xFFFFFFFFUL) {
        BM_LOG_WARN("invalid MF_SDMA_ORCH_EPOCH_BUDGET_S: " << val << ", fallback to default "
                    << HYBM_SDMA_ORCH_EPOCH_TIME_BUDGET_S << "s");
        return HYBM_SDMA_ORCH_EPOCH_TIME_BUDGET_S;
    }
    if (parsed == 0) {
        return HYBM_SDMA_ORCH_EPOCH_TIME_BUDGET_S; /* 0 语义 = 默认(kernel 契约) */
    }
    return static_cast<uint32_t>(parsed);
}
} // namespace

SdmaOrchestrator &SdmaOrchestrator::GetInstance()
{
    static SdmaOrchestrator instance; // C++11 线程安全单例
    return instance;
}

bool SdmaOrchestrator::Disabled() noexcept
{
    const auto &val = env::MF_SDMA_ORCH_DISABLE;
    return val == "1" || val == "on"; // 仅 "1"/"on" 视为禁用, 其余一律启用
}

Result SdmaOrchestrator::Acquire(uint16_t devId)
{
    std::lock_guard<std::mutex> guard(mutex_);
    if (refCount_ > 0) {
        if (devId != devId_) { /* CP 流/工作区绑定单设备, 同进程多池须同设备 */
            BM_LOG_ERROR("sdma orchestrator bound devId: " << devId_ << " new devId: " << devId);
            return BM_INVALID_PARAM;
        }
        refCount_++;
        return BM_OK;
    }
    auto ret = InitOnce(devId);
    if (ret != BM_OK) {
        Cleanup(); /* 半初始化资源回收, 保证可重试 */
        return ret;
    }
    refCount_ = 1;
    ready_ = true;
    BM_LOG_INFO("310P sdma orchestrator ready: devId: " << devId_ << " sqId: " << sqId_
                                                   << " streamId: " << streamId_ << " dieId: " << dieId_
                                                   << " workspace: 0x" << std::hex << workspace_.load());
    return BM_OK;
}

void SdmaOrchestrator::Release()
{
    std::lock_guard<std::mutex> guard(mutex_);
    if (refCount_ == 0) {
        return; // 幂等(UT 覆盖)
    }
    refCount_--;
    if (refCount_ == 0) {
        Cleanup();
        ready_ = false;
    }
}

Result SdmaOrchestrator::InitOnce(uint16_t devId)
{
    devId_ = devId;
    dieId_ = static_cast<uint32_t>(devId) % 2U; /* CP1 内 HAL devId = die 号(A.3) */
    auto ret = InitCpStream(devId);
    if (ret != BM_OK) {
        return ret;
    }
    ret = AllocWorkspace();
    if (ret != BM_OK) {
        return ret;
    }
    ret = LoadOrchestratorKernel();
    if (ret != BM_OK) {
        return ret;
    }
    return BM_OK;
}

Result SdmaOrchestrator::InitCpStream(uint16_t devId)
{
    // 创建 CP 流: DEVICE_USE_ONLY 使其 SQ 注册进 CP1 进程 trs 表(X3/A.3)
    auto ret = DlAclApi::AclrtCreateStreamWithConfig(&cpStream_, 0, HYBM_ACL_STREAM_DEVICE_USE_ONLY);
    if (ret != BM_OK) {
        BM_LOG_ERROR("create CP stream failed, ret: " << ret << " devId: " << devId);
        return ret;
    }
    ret = DlAclApi::AclrtStreamGetId(cpStream_, &streamId_); // SQE 的 rt_stream_id 字段
    if (ret != BM_OK) {
        BM_LOG_ERROR("get CP streamId failed, ret: " << ret << " devId: " << devId);
        return ret;
    }
    ret = DlRtApi::RtStreamGetSqid(cpStream_, &sqId_); // rt 私有接口, kernel 内活查询用
    if (ret != BM_OK) {
        BM_LOG_ERROR("rtStreamGetSqid failed, ret: " << ret << " devId: " << devId);
        return ret;
    }
    return BM_OK;
}

Result SdmaOrchestrator::AllocWorkspace()
{
    void *ws = nullptr;
    auto ret = DlAclApi::AclrtMalloc(&ws, HYBM_SDMA_ORCH_WS_SIZE, ACL_MEM_MALLOC_HUGE_FIRST);
    if (ret != BM_OK) {
        BM_LOG_ERROR("alloc workspace failed, ret: " << ret << " devId: " << devId_
                     << " size: " << HYBM_SDMA_ORCH_WS_SIZE);
        return ret;
    }
    ret = DlAclApi::AclrtMemset(ws, HYBM_SDMA_ORCH_WS_SIZE, 0, HYBM_SDMA_ORCH_WS_SIZE); // 邮箱槽初值必须为 0
    if (ret != BM_OK) {
        BM_LOG_ERROR("memset workspace failed, ret: " << ret << " devId: " << devId_);
        (void)DlAclApi::AclrtFree(ws);
        return ret;
    }
    workspace_ = reinterpret_cast<uint64_t>(ws);
    return BM_OK;
}

/* 安装前缀自动发现: 本 so 所在目录(如 <prefix>/lib64)的同级 hybm/aicpu_kernel 下
 * 找 launch json(so→<prefix>/lib64, json→<prefix>/hybm/aicpu_kernel)。
 * 命中返回绝对路径, 未命中返回空串(调用方继续回退 CWD)。 */
static std::string FindInstalledJson()
{
    Dl_info info = {};
    if (dladdr(reinterpret_cast<void *>(&SdmaOrchestrator::GetInstance), &info) == 0 || info.dli_fname == nullptr) {
        return "";
    }
    std::string soDir(info.dli_fname);
    auto pos = soDir.find_last_of('/');
    if (pos == std::string::npos) {
        return "";
    }
    soDir.resize(pos);
    char resolved[PATH_MAX] = {0};
    std::string cand = soDir + "/../hybm/aicpu_kernel/" + HYBM_SDMA_ORCH_DEFAULT_JSON;
    if (realpath(cand.c_str(), resolved) != nullptr && access(resolved, R_OK) == 0) {
        return std::string(resolved);
    }
    return "";
}

/* 取路径目录部分(最后一个 '/' 之前; 无 '/' 返回空串) */
static std::string DirOf(const std::string &path)
{
    auto pos = path.find_last_of('/');
    return (pos == std::string::npos) ? std::string() : path.substr(0, pos);
}

/* 从 launch json 提取 kernelSo 字段值(CP1 搜索的设备侧 so 名); 字段缺失时回退
 * 文件名推导(v6.json → v6.so, 与 CMake 产物命名约定一致) */
static std::string ExtractDeviceSoName(const std::string &jsonPath)
{
    std::ifstream f(jsonPath);
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    auto pos = content.find("\"kernelSo\"");
    if (pos != std::string::npos) {
        auto colon = content.find(':', pos);
        auto s = (colon == std::string::npos) ? colon : content.find('"', colon);
        auto e = (s == std::string::npos) ? s : content.find('"', s + 1);
        if (e != std::string::npos && e > s) {
            return content.substr(s + 1, e - s - 1);
        }
    }
    auto slash = jsonPath.find_last_of('/');
    std::string base = (slash == std::string::npos) ? jsonPath : jsonPath.substr(slash + 1);
    if (base.size() > 5 && base.compare(base.size() - 5, 5, ".json") == 0) {
        base.resize(base.size() - 5);
    }
    return base + ".so";
}

/* deviceSoName 白名单校验(仅 [A-Za-z0-9._-], 长度 1~64): 名字拼入 KFC 命令串经
 * 设备 OS system() 执行——不做校验存在 shell 注入风险; json 由本仓构建产出,
 * 校验是纵深防御 */
static bool IsValidDeviceSoName(const std::string &name)
{
    if (name.empty() || name.size() > 64) {
        return false;
    }
    for (char c : name) {
        if (std::isalnum(static_cast<unsigned char>(c)) == 0 && c != '.' && c != '_' && c != '-') {
            return false;
        }
    }
    return true;
}

Result SdmaOrchestrator::LoadOrchestratorKernel()
{
    std::string jsonPath = env::MF_SDMA_ORCH_JSON; // ① 环境变量显式指定
    if (jsonPath.empty()) {
        jsonPath = FindInstalledJson();            // ② 安装前缀自动发现
    }
    if (jsonPath.empty()) {
        jsonPath = HYBM_SDMA_ORCH_DEFAULT_JSON;     // ③ CWD 缺省名
    }
    char resolved[PATH_MAX] = {0};
    if (realpath(jsonPath.c_str(), resolved) == nullptr || access(resolved, R_OK) != 0) {
        BM_LOG_ERROR("sdma orchestrator json not accessible: " << jsonPath
                                                                << " (set MF_SDMA_ORCH_JSON?)");
        return BM_FILE_NOT_ACCESS;
    }
    /* mode=0 加载不校验设备侧文件(dlopen 惰性到 launch), 须以 KFC 探测文件存在性为闸门 */
    const std::string devSoName = ExtractDeviceSoName(resolved);
    if (!IsValidDeviceSoName(devSoName)) {
        BM_LOG_ERROR("invalid device so name (whitelist [A-Za-z0-9._-]): " << devSoName
                     << " json: " << resolved);
        return BM_INVALID_PARAM;
    }
    const std::string dst = std::string(HYBM_SDMA_ORCH_DEV_SEARCH_DIR) + "/" + devSoName;
    /* 2026-10-18 实测(310P3+CANN9.1): torch_npu 宿主进程内执行一条 KFC system() 命令——
     * 即使是首轮 test -f 探测的预期"文件缺失"退出码——即令后续全部 launch(含普通 torch
     * 算子)持续 507018/aicpu 异常, 时间/新流均不可恢复, 仅新进程可行; 纯 ACL 进程不受影响。
     * 因此探测+迁移+复制+校验必须全部在 fork+exec 的独立助手进程内执行, 本进程零 KFC 命令。
     * 助手缺失或失败时明确报错, 不回退进程内部署、不"假定已部署"(锁定环境两路径均不可信)。 */
    bool helperRan = false;
    auto ret = RunDeployHelper(resolved, dst, helperRan);
    if (!helperRan) {
        BM_LOG_ERROR("deploy helper not found (MF_SDMA_ORCH_DEPLOY_HELPER or libmf_hybm_core.so 同目录/"
                     "../bin/), refusing in-process KFC deploy in torch host, dst: " << dst);
        return BM_FILE_NOT_ACCESS;
    }
    if (ret != BM_OK) {
        return ret; // RunDeployHelper 已分层记录失败原因
    }
    ret = TryLoadKernel(resolved);
    if (ret != BM_OK) {
        BM_LOG_ERROR("kernel load failed after helper deploy, ret: " << ret << " json: " << resolved);
    }
    return ret;
}

Result SdmaOrchestrator::TryLoadKernel(const std::string &jsonPath)
{
    aclrtBinaryLoadOption loadOption{};
    loadOption.type = aclrtBinaryLoadOptionType::ACL_RT_BINARY_LOAD_OPT_CPU_KERNEL_MODE;
    loadOption.value.cpuKernelMode = 0; /* mode=0: 走 CP1 搜索路径(A.4-3) */
    aclrtBinaryLoadOptions loadOptions{};
    loadOptions.options = &loadOption;
    loadOptions.numOpt = 1;
    auto ret = DlAclApi::AclrtBinaryLoadFromFile(jsonPath.c_str(), &loadOptions, &binHandle_);
    if (ret != BM_OK) { // 设备侧文件缺失或 json 问题(日志由调用方分层记录)
        return ret;
    }
    return DlAclApi::AclrtBinaryGetFunction(binHandle_, HYBM_SDMA_ORCH_FUNC_NAME, &funcHandle_);
}

/* fork+exec 部署助手查找: 显式 env → 本 so 同目录(安装包 lib64) → 前缀 bin/。
 * 找不到返回空串(调用方明确报错, 不回退进程内 KFC 部署)。 */
static std::string FindDeployHelper()
{
    const char *env = getenv("MF_SDMA_ORCH_DEPLOY_HELPER");
    if (env != nullptr && *env != '\0' && access(env, X_OK) == 0) {
        return std::string(env);
    }
    Dl_info info = {};
    if (dladdr(reinterpret_cast<void *>(&SdmaOrchestrator::GetInstance), &info) == 0 || info.dli_fname == nullptr) {
        return "";
    }
    std::string soDir(info.dli_fname);
    auto pos = soDir.find_last_of('/');
    if (pos == std::string::npos) {
        return "";
    }
    soDir.resize(pos);
    for (const auto &cand : {soDir + "/orch_deploy_helper", soDir + "/../bin/orch_deploy_helper"}) {
        if (access(cand.c_str(), X_OK) == 0) {
            return cand;
        }
    }
    return "";
}

/* fork+exec orch_deploy_helper: 探测+CUST 迁移+复制+校验全部在子进程执行(新 ACL
 * 上下文, 非零退出码无碍), 本进程不执行任何 KFC 命令——torch_npu 宿主一条非零
 * 退出 KFC 命令即永久打死设备上下文(2026-10-18 实测, 310P3+CANN9.1)。
 * 退出码 0/3=已部署/本就在, 2=部署失败, 1/127=助手自身故障。 */
Result SdmaOrchestrator::RunDeployHelper(const std::string &launchJsonPath, const std::string &dst,
                                         bool &helperRan)
{
    helperRan = false;
    const std::string helper = FindDeployHelper();
    if (helper.empty()) {
        return BM_FILE_NOT_ACCESS;
    }
    /* fork 前备好全部参数: 子进程内仅做 async-signal-safe 的 exec */
    char devStr[16] = {0};
    (void)snprintf(devStr, sizeof(devStr), "%u", devId_);
    const char *helperArgv[] = {helper.c_str(), devStr, launchJsonPath.c_str(), nullptr};
    const pid_t pid = fork();
    if (pid < 0) {
        BM_LOG_ERROR("deploy helper fork failed, errno: " << errno);
        return BM_ERROR;
    }
    if (pid == 0) { // 子进程: 立即 exec(不触碰父进程任何 ACL/线程状态)
        (void)execv(helper.c_str(), const_cast<char **>(helperArgv));
        _exit(127); // exec 失败
    }
    int status = 0;
    for (int waited = 0; waited < 1800; ++waited) { // 上限 180s(迁移秒级, 重试含 sleep)
        const pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            helperRan = true;
            if (WIFEXITED(status)) {
                const int rc = WEXITSTATUS(status);
                BM_LOG_INFO("deploy helper rc: " << rc << " dst: " << dst);
                return (rc == 0 || rc == 3) ? BM_OK : BM_ERROR;
            }
            BM_LOG_ERROR("deploy helper terminated abnormally, status: " << status);
            return BM_ERROR;
        }
        if (r < 0) {
            BM_LOG_ERROR("deploy helper waitpid failed, errno: " << errno);
            return BM_ERROR;
        }
        usleep(100000); // 100ms 轮询
    }
    BM_LOG_ERROR("deploy helper timeout (180s), killing");
    (void)kill(pid, SIGKILL);
    (void)waitpid(pid, &status, 0);
    return BM_ERROR;
}

Result SdmaOrchestrator::ConfigureOrch(uint64_t ownSegGva, uint64_t physSize, uint64_t symSize)
{
    std::lock_guard<std::mutex> guard(mutex_);
    if (configured_) { /* 约束: 双环一次绑定, 每进程单 SDMA 池 */
        BM_LOG_ERROR("sdma orch already configured, second pool unsupported, devId: " << devId_);
        return BM_INVALID_PARAM;
    }
    if (symSize == 0 || (symSize & (symSize - 1)) != 0) { // 2 的幂(对端段基址掩码算术依赖)
        BM_LOG_ERROR("symSize not power of two, symSize: " << symSize);
        return BM_INVALID_PARAM;
    }
    const uint64_t resBase = ownSegGva + physSize - SMEM_SHM_SDMA_RESERVED;
    // 保留区整体清零(双环/游标/戳/停止标志从零起步)
    auto ret = DlAclApi::AclrtMemset(reinterpret_cast<void *>(resBase), SMEM_SHM_SDMA_RESERVED, 0,
                                     SMEM_SHM_SDMA_RESERVED);
    if (ret != BM_OK) {
        BM_LOG_ERROR("memset orch reserved failed, ret: " << ret << " resBase: 0x" << std::hex << resBase
                                                          << " size: " << SMEM_SHM_SDMA_RESERVED);
        return ret;
    }
    epochParam_ = HybmSdmaOrchParam{};
    epochParam_.devId = dieId_; /* CP1 内 HAL/SMMU 均用 die 号(A.3); dev0/dev1 下与 host 设备号同值 */
    epochParam_.sqId = sqId_;
    epochParam_.streamId = streamId_;
    const uint32_t budgetS = ResolveEpochBudgetS(); /* 每轮时长(默认 15s)或永不退出的专用值(诊断) */
    epochParam_.epochBudgetS = budgetS;
    epochParam_.reqBase = resBase + SMEM_SHM_SDMA_RS_REQ_RING;
    epochParam_.quietBase = resBase + SMEM_SHM_SDMA_RS_QUIET;
    const uint64_t wsBase = workspace_.load();
    /* 停止标志/overflow/结果区放工作区而非段内: 销毁序安全(段先销毁也不影响停止语义) */
    epochParam_.stopAddr = wsBase + HYBM_SDMA_ORCH_WS_STOP_OFF;
    epochParam_.overflowAddr = wsBase + HYBM_SDMA_ORCH_WS_OVF_OFF;
    epochParam_.resultAddr = wsBase + HYBM_SDMA_ORCH_WS_RESULT_OFF;
    epochParam_.symSize = symSize;
    epochParam_.physSize = physSize;
    /* 首轮内联下发: 失败立即报错(池创建失败, 调用方可重试)——监督线程只负责后续重拉,
     * 不得吞掉首轮失败造成池看似就绪实则无消费者的静默半死 */
    ret = DlAclApi::AclrtCreateStream(&epochStream_);
    if (ret != BM_OK) {
        BM_LOG_ERROR("create epoch stream failed, ret: " << ret << " devId: " << devId_);
        return ret;
    }
    ret = LaunchEpoch(epochStream_);
    if (ret != BM_OK) {
        BM_LOG_ERROR("first epoch launch failed, ret: " << ret << " sqId: " << sqId_ << " devId: " << devId_);
        DestroyEpochResources();
        return ret;
    }
    configured_ = true; // 首轮已在设备运行
    epochLaunched_ = true;
    if (budgetS != HYBM_SDMA_ORCH_EPOCH_BUDGET_RESIDENT) {
        try {
            stopFlag_ = false;
            supervisor_ = std::thread(&SdmaOrchestrator::SupervisorLoop, this);
        } catch (const std::system_error &e) { // 资源不足: 停掉首轮保持状态一致(未置 configured_ 语义可重试)
            BM_LOG_ERROR("start supervisor thread failed, errno: " << e.code().value() << " devId: " << devId_);
            StopEpoch();
            configured_ = false;
            epochLaunched_ = false;
            return BM_ERROR;
        }
    }
    BM_LOG_INFO("sdma orch configured, devId: " << devId_ << " epochBudgetS: "
              << (budgetS == HYBM_SDMA_ORCH_EPOCH_BUDGET_RESIDENT ? std::string("resident")
                                                                   : std::to_string(budgetS))
              << " reqBase: 0x" << std::hex << epochParam_.reqBase
              << " arrivalDepth: " << SMEM_SHM_SDMA_RS_ARR_SLOTS);
    return BM_OK;
}

Result SdmaOrchestrator::LaunchEpoch(void *stream)
{
    if (funcHandle_ == nullptr || stream == nullptr) { // 直接检查将使用的句柄与流
        BM_LOG_ERROR("sdma orch launch precond failed, funcHandle null: " << (funcHandle_ == nullptr)
                                                                        << " stream null: " << (stream == nullptr));
        return BM_NOT_INITIALIZED;
    }
    aclrtArgsHandle argsHandle = nullptr;
    auto ret = DlAclApi::AclrtKernelArgsInit(funcHandle_, &argsHandle);
    if (ret != BM_OK) {
        BM_LOG_ERROR("KernelArgsInit failed, ret: " << ret << " sqId: " << sqId_);
        return ret;
    }
    HybmSdmaOrchParam p = epochParam_; /* Append 接口非 const, 本地拷贝 */
    aclrtParamHandle paramHandle = nullptr;
    ret = DlAclApi::AclrtKernelArgsAppend(argsHandle, &p, sizeof(p), &paramHandle);
    if (ret == BM_OK) {
        ret = DlAclApi::AclrtKernelArgsFinalize(argsHandle); // 封包(此后不可再 Append)
    }
    if (ret != BM_OK) {
        BM_LOG_ERROR("KernelArgs Append/Finalize failed, ret: " << ret << " sqId: " << sqId_);
        return ret;
    }
    aclrtLaunchKernelAttr attr{};
    attr.id = aclrtLaunchKernelAttrId::ACL_RT_LAUNCH_KERNEL_ATTR_TIMEOUT;
    /* 0=永不超时: 每轮时长由 kernel 读报时计数自限, 不经 attr; 任何有限 attr 值到达即把
     * 任务标记为 507017 超时(host 侧语义)导致监督线程的 sync 提前返回; attr 不可省略
     * (numAttrs=0 被 runtime 拒绝 107000, 缺省值为默认超时 ≈6.4s 同样误杀) */
    attr.value.timeout = HYBM_SDMA_ORCH_LAUNCH_TIMEOUT_NEVER_S;
    aclrtLaunchKernelCfg cfg{};
    cfg.attrs = &attr;
    cfg.numAttrs = 1;
    // 单轮下发(异步提交, kernel 跑满预算/停止标志/错误退出; 由监督线程 sync)
    ret = DlAclApi::AclrtLaunchKernelWithConfig(funcHandle_, HYBM_SDMA_ORCH_BLOCK_DIM, stream, &cfg,
                                                argsHandle, nullptr);
    if (ret != BM_OK) {
        BM_LOG_ERROR("LaunchKernelWithConfig(epoch) failed, ret: " << ret << " sqId: " << sqId_);
    }
    return ret;
}

void SdmaOrchestrator::SupervisorLoop()
{
    /* ACL 上下文为线程局部: 子线程须先绑定设备(否则 launch 报 107002 上下文无效) */
    if (DlAclApi::AclrtSetDevice(devId_) != BM_OK) {
        BM_LOG_ERROR("supervisor set device failed, devId: " << devId_);
        return; // 放弃重拉(数据面将停在背压/超时, 日志可见)
    }
    const uint64_t hbAddr = workspace_.load() + HYBM_SDMA_ORCH_WS_HEARTBEAT_OFF;
    const uint64_t wsBase = workspace_.load();
    uint32_t errors = 0; // 连续失败计数(成功即清零)
    uint64_t heartbeat = 0;
    auto tEpoch = std::chrono::steady_clock::now(); /* 首轮起点(耗时观测) */
    while (!stopFlag_.load()) {
        /* 心跳: 每 100ms 以同步 H2D 写递增计数(同步调用返回即完成——
         * 不给任何全设备同步留 pending 任务, 这是心跳方案的关键约束) */
        std::this_thread::sleep_for(std::chrono::milliseconds(1000 / HYBM_SDMA_ORCH_HEARTBEAT_HZ));
        if (stopFlag_.load()) {
            break;
        }
        heartbeat++;
        if (DlAclApi::AclrtMemcpy(reinterpret_cast<void *>(hbAddr), sizeof(uint64_t), &heartbeat,
                                  sizeof(uint64_t), ACL_MEMCPY_HOST_TO_DEVICE) != BM_OK) {
            BM_LOG_ERROR("write heartbeat failed, hbAddr: 0x" << std::hex << hbAddr << " devId: " << devId_);
        }
        /* 轮询结果区: exitReason 离开 RUNNING 即当前轮已退出(v11 起 kernel 入口
         * 写 RUNNING+刷出, 终值=退出路径; 同步 D2H 读, 不留 pending 任务) */
        HybmSdmaOrchResult r = {};
        if (DlAclApi::AclrtMemcpy(&r, sizeof(r), reinterpret_cast<void *>(wsBase), sizeof(r),
                                  ACL_MEMCPY_DEVICE_TO_HOST) != BM_OK) {
            BM_LOG_ERROR("poll result failed, devId: " << devId_);
            if (++errors > HYBM_SDMA_ORCH_SUPERVISOR_MAX_ERR) {
                BM_LOG_ERROR("poll failed " << HYBM_SDMA_ORCH_SUPERVISOR_MAX_ERR
                             << " times consecutively, supervisor exit, sqId: " << sqId_);
                return;
            }
            continue;
        }
        if (r.exitReason == HYBM_SDMA_ORCH_EXIT_RUNNING) {
            continue; // 当前轮仍在跑
        }
        /* 当前轮已退出: 耗时观测(≈预算=心跳生效; 持续远超说明异常, 影响同步等待上界) */
        const auto epochMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - tEpoch).count();
        if (r.exitReason == HYBM_SDMA_ORCH_EXIT_ERROR) {
            BM_LOG_ERROR("epoch exited with error, mainRet: " << r.mainRet << " consumed: " << r.consumed
                         << " durationMs: " << epochMs << " sqId: " << sqId_);
            if (++errors > HYBM_SDMA_ORCH_SUPERVISOR_MAX_ERR) {
                BM_LOG_ERROR("epoch errored " << HYBM_SDMA_ORCH_SUPERVISOR_MAX_ERR
                             << " times consecutively, supervisor exit, sqId: " << sqId_);
                return; // epochStream_ 留存, StopEpoch 兜底销毁
            }
        } else if (epochMs > static_cast<int64_t>(epochParam_.epochBudgetS) * 1500) {
            BM_LOG_WARN("epoch overran budget, durationMs: " << epochMs << " budgetS: "
                        << epochParam_.epochBudgetS << " exitReason: " << r.exitReason
                        << " sqId: " << sqId_);
            errors = 0;
        } else {
            errors = 0;
        }
        if (stopFlag_.load()) { // 重拉前复查(与 StopEpoch 写停止标志的竞态收口)
            break;
        }
        /* 心跳清零 + 重拉(环+游标语义, 间隙零丢失) */
        heartbeat = 0;
        const uint64_t zero = 0;
        (void)DlAclApi::AclrtMemcpy(reinterpret_cast<void *>(hbAddr), sizeof(uint64_t), &zero,
                                    sizeof(uint64_t), ACL_MEMCPY_HOST_TO_DEVICE);
        if (LaunchEpoch(epochStream_) != BM_OK) {
            BM_LOG_ERROR("epoch relaunch failed, sqId: " << sqId_);
            if (++errors > HYBM_SDMA_ORCH_SUPERVISOR_MAX_ERR) {
                BM_LOG_ERROR("epoch relaunch failed " << HYBM_SDMA_ORCH_SUPERVISOR_MAX_ERR
                             << " times consecutively, supervisor exit, sqId: " << sqId_);
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        tEpoch = std::chrono::steady_clock::now(); // 新一轮起点
        errors = 0;
    }
}

void SdmaOrchestrator::DestroyEpochResources()
{
    if (epochStream_ == nullptr) {
        return; // 幂等
    }
    /* 同步确认 kernel 退出后销毁(停止标志已生效时 ≤1ms; 监督线程异常退出场景的兜底) */
    (void)DlAclApi::AclrtSynchronizeStream(epochStream_);
    (void)DlAclApi::AclrtDestroyStream(epochStream_);
    epochStream_ = nullptr;
}

void SdmaOrchestrator::StopEpoch()
{
    if (!epochLaunched_ || epochParam_.stopAddr == 0) {
        return; // 未运行或已停止(幂等)
    }
    epochLaunched_ = false;
    // 先置线程退出标志再写停止标志: 竞态窗口内多拉的一轮读到停止标志即自退(≤1ms)
    stopFlag_ = true;
    // 写非零停止标志: kernel ≤100µs 轮询间隔内自退(memset 字节语义, 非零即停)
    auto ret = DlAclApi::AclrtMemset(reinterpret_cast<void *>(epochParam_.stopAddr), 8, 1, 8);
    if (ret != BM_OK) {
        BM_LOG_ERROR("write stop flag failed, ret: " << ret << " sqId: " << sqId_ << " devId: " << devId_);
    }
    if (supervisor_.joinable()) { // 分轮模式: 停监督线程(其发现当前轮退出后见标志退出)
        supervisor_.join();
    }
    /* 主上下文读结果区诊断(工作区为主上下文分配, D2H 无跨上下文问题) */
    HybmSdmaOrchResult r = {};
    (void)DlAclApi::AclrtMemcpy(&r, sizeof(r), reinterpret_cast<void *>(workspace_.load()), sizeof(r),
                                 ACL_MEMCPY_DEVICE_TO_HOST);
    /* epoch 流/上下文 teardown(切上下文同步销毁后还原) */
    DestroyEpochResources();
    if (ret != BM_OK || r.exitReason == HYBM_SDMA_ORCH_EXIT_ERROR) {
        /* 停止窗口内段可能先于编排器销毁, kernel 读段报错属预期; 其余为设备侧异常 */
        BM_LOG_ERROR("epoch stop abnormal, ret: " << ret << " mainRet: " << r.mainRet
                     << " exitReason: " << r.exitReason << " consumed: " << r.consumed
                     << " sqId: " << sqId_ << " devId: " << devId_);
        return;
    }
    BM_LOG_INFO("epoch stopped, exitReason: " << r.exitReason << " consumed: " << r.consumed
              << " sqId: " << sqId_);
}

void SdmaOrchestrator::Cleanup()
{
    StopEpoch(); // 先停监督线程与当前轮(占用 epochStream_, 必须先于流销毁)
    DestroyEpochResources(); // 兜底(StopEpoch 早退路径下防泄漏, 幂等)
    configured_ = false; // 复位(允许后续重配置)
    if (cpStream_ != nullptr) {
        (void)DlAclApi::AclrtDestroyStream(cpStream_); // SQ 随流注销
        cpStream_ = nullptr;
    }
    if (workspace_ != 0) {
        (void)DlAclApi::AclrtFree(reinterpret_cast<void *>(workspace_.load()));
        workspace_ = 0;
    }
    /* binHandle_ 无对应卸载接口(DlAclApi 未封装), 随进程生命周期释放 */
    binHandle_ = nullptr;
    funcHandle_ = nullptr;
    refCount_ = 0;
    sqId_ = 0;
    streamId_ = 0;
    dieId_ = 0;
}
} // namespace mf
} // namespace ock
