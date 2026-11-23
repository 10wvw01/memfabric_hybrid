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
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>
#include "mf_scope_guard.h"
#include <set>
#include <string>
#include <unistd.h>

#include <sys/file.h>
#include <sys/stat.h>

#include "dl_rt_api.h"
#include "mf_env_define.h"

namespace ock {
namespace mf {
namespace {
/* ACL_STREAM_DEVICE_USE_ONLY 的值(acl_rt.h:0x20): SQ 注册进 CP1 trs 表(AICPU 可查可投的前提) */
constexpr uint32_t HYBM_ACL_STREAM_DEVICE_USE_ONLY = 0x20U;
constexpr uint32_t HYBM_SDMA_ORCH_BLOCK_DIM = 1;                 /* 单 block(kernel 状态机单线程语义) */
constexpr uint32_t HYBM_SDMA_ORCH_KFC_TIMEOUT_S = 25;            /* KFC 设备 shell 命令超时(部署链专用) */
constexpr char HYBM_SDMA_ORCH_FUNC_NAME[] = "HybmSdmaOrchEpoch"; /* epoch 消费 kernel 入口符号 */
constexpr char HYBM_SDMA_ORCH_DEFAULT_JSON[] = "libmf_sdma_orch_launch.json";
constexpr char HYBM_SDMA_ORCH_CUST_JSON[] = "libmf_sdma_orch.json"; /* CUST 迁移 json 名(与 launch json 同目录) */
constexpr char HYBM_SDMA_ORCH_CUST_SO[] = "libmf_sdma_orch.so";     /* host 侧 kernel so 名(CUST 迁移源) */
/* CP1 搜索路径(实测: 每卡独立设备 OS 文件系统, 须在本卡设备上部署) */
constexpr char HYBM_SDMA_ORCH_DEV_SEARCH_DIR[] = "/usr/lib64/aicpu_kernels/0/aicpu_kernels_device";
constexpr char HYBM_SDMA_ORCH_DEPLOY_LOCK[] = "/tmp/.mf_sdma_orch_deploy.lock"; /* 部署互斥锁(host 侧) */
constexpr uint32_t HYBM_SDMA_ORCH_CMD_BUF_SIZE = 512; /* KFC 命令缓冲尺寸(命令为受控常量拼接) */
/* AICPU任务异常也可能来自system丢失退出状态，须另行确认命令实际结果。 */
constexpr int32_t HYBM_SDMA_ORCH_KFC_RESULT_UNCONFIRMED = 507018;
class ServiceContextGuard {
public:
    explicit ServiceContextGuard(void *target)
    {
        if (target == nullptr)
            return;
        ret = DlAclApi::AclrtGetCurrentContext(&previous);
        if (ret == BM_OK)
            ret = DlAclApi::AclrtSetCurrentContext(target);
        if (ret != BM_OK)
            BM_LOG_ERROR("service context switch failed, ret: " << ret);
        restore = ret == BM_OK;
    }
    ~ServiceContextGuard()
    {
        Restore();
    }
    void Restore()
    {
        if (restore && DlAclApi::AclrtSetCurrentContext(previous) != BM_OK)
            BM_LOG_ERROR("restore caller context failed");
        restore = false;
    }
    Result ret = BM_OK;

private:
    void *previous = nullptr;
    bool restore = false;
};

std::string ServiceEnv(const char *name)
{
    const char *value = std::getenv(name);
    return value == nullptr ? std::string() : std::string(value);
}
} // namespace

SdmaOrchestrator &SdmaOrchestrator::GetInstance()
{
    // 设备故障时后台线程可能仍引用服务对象，保留到进程结束。
    static auto *instance = new SdmaOrchestrator;
    return *instance;
}

bool SdmaOrchestrator::Disabled() noexcept
{
    const auto &val = env::MF_SDMA_ORCH_DISABLE;
    return val == "1" || val == "on"; // 仅 "1"/"on" 视为禁用, 其余一律启用
}

Result SdmaOrchestrator::Acquire(uint16_t devId)
{
    std::lock_guard<std::mutex> guard(mutex_);
    if (HasUnconfirmedResources()) {
        BM_LOG_ERROR("previous SDMA service still owns device resources, devId: " << devId_);
        return BM_ERROR;
    }
    if (refCount_ > 0) {
        if (devId != devId_) { /* CP 流/工作区绑定单设备, 同进程多池须同设备 */
            BM_LOG_ERROR("sdma orchestrator bound devId: " << devId_ << " new devId: " << devId);
            return BM_INVALID_PARAM;
        }
        refCount_++;
        ready_ = true;
        return BM_OK;
    }
    void *callerContext = nullptr;
    auto ret = DlAclApi::AclrtGetCurrentContext(&callerContext);
    if (ret != BM_OK || callerContext == nullptr) {
        BM_LOG_ERROR("caller context unavailable, devId: " << devId << " ret: " << ret);
        return ret == BM_OK ? BM_NOT_INITIALIZED : ret;
    }
    auto restore = make_scope_guard(callerContext, [](void *ctx) {
        if (DlAclApi::AclrtSetCurrentContext(ctx) != BM_OK)
            BM_LOG_ERROR("restore caller context after acquire failed");
    });
    ret = InitOnce(devId);
    if (ret != BM_OK) {
        Cleanup(); /* 半初始化资源回收, 保证可重试 */
        return ret;
    }
    refCount_ = 1;
    ready_ = true;
    BM_LOG_INFO("310P sdma orchestrator ready: devId: " << devId_ << " sqId: " << sqId_ << " streamId: " << streamId_
                                                        << " dieId: " << dieId_ << " workspace: 0x" << std::hex
                                                        << workspace_.load());
    return BM_OK;
}

void SdmaOrchestrator::Release()
{
    std::lock_guard<std::mutex> guard(mutex_);
    if (refCount_ == 0) {
        return; // 幂等(UT 覆盖)
    }
    if (refCount_ == 1 && StopEpoch() != BM_OK)
        return;
    refCount_--;
    if (refCount_ == 0) {
        Cleanup();
        ready_ = false;
    }
}

Result SdmaOrchestrator::InitOnce(uint16_t devId)
{
    devId_ = devId;
    if (!ResolveSdmaOptions(ServiceEnv("MF_SDMA_ORCH_MODE"), ServiceEnv("MF_SDMA_ORCH_EPOCH_BUDGET_S"),
                            ServiceEnv("MF_SDMA_ORCH_HOST_LEASE_MS"), ServiceEnv("MF_SDMA_ORCH_STOP_TIMEOUT_MS"),
                            serviceOptions_)) {
        BM_LOG_ERROR("invalid SDMA service configuration, devId: " << devId);
        return BM_INVALID_PARAM;
    }
    auto contextRet = DlAclApi::AclrtCreateContext(&serviceContext_, devId);
    if (contextRet != BM_OK) {
        BM_LOG_ERROR("create service context failed, devId: " << devId << " ret: " << contextRet);
        return contextRet;
    }
    dieId_ = static_cast<uint32_t>(devId) % 2U; /* CP1 内 HAL devId = die 号(A.3) */
    auto ret = InitCpStream(devId);
    if (ret != BM_OK) {
        return ret;
    }
    ret = AllocWorkspace();
    if (ret != BM_OK) {
        return ret;
    }
    ret = InitControl();
    if (ret != BM_OK)
        return ret;
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
    ret = DlAclApi::AclrtCreateStream(&launchStream_); // 普通流(可提任务; CP_PROCESS_USE 流拒收本进程下发, 实测 507000)
    if (ret != BM_OK) {
        BM_LOG_ERROR("create launch stream failed, ret: " << ret << " devId: " << devId);
    }
    return ret;
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

/* 同 Host 的迁移和复制串行执行，避免多个进程混用 CUST 临时文件。 */
class DeployFlockGuard {
public:
    /* O_NOFOLLOW: 拒绝跟随预置符号链接(防锁文件劫持); O_CLOEXEC: 子进程不继承;
     * 0600: 属主读写 */
    DeployFlockGuard() : fd_(open(HYBM_SDMA_ORCH_DEPLOY_LOCK, O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600))
    {
        if (fd_ < 0) {
            BM_LOG_ERROR("deploy lock open failed, errno: " << errno);
            return;
        }
        if (flock(fd_, LOCK_EX) != 0) {
            BM_LOG_ERROR("deploy lock acquire failed, errno: " << errno);
            (void)close(fd_);
            fd_ = -1;
        }
    }
    bool Valid() const
    {
        return fd_ >= 0;
    }
    ~DeployFlockGuard()
    {
        if (fd_ >= 0) {
            (void)flock(fd_, LOCK_UN);
            (void)close(fd_);
        }
    }

private:
    int fd_; // -1 = 未持锁
};

/* 取路径目录部分(最后一个 '/' 之前; 无 '/' 返回空串) */
static std::string DirOf(const std::string &path)
{
    auto pos = path.find_last_of('/');
    return (pos == std::string::npos) ? std::string() : path.substr(0, pos);
}

/* 读取构建产物的 kernelSo，运行时替换为本次加载的独立文件名。 */
static std::string ExtractDeviceSoName(const std::string &content)
{
    auto pos = content.find("\"kernelSo\"");
    if (pos == std::string::npos) {
        return "";
    }
    auto colon = content.find(':', pos);
    auto start = (colon == std::string::npos) ? colon : content.find('"', colon);
    auto end = (start == std::string::npos) ? start : content.find('"', start + 1);
    return (end == std::string::npos) ? "" : content.substr(start + 1, end - start - 1);
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

static bool HasOrchestratorArtifacts(const std::filesystem::path &directory)
{
    const char *names[] = {HYBM_SDMA_ORCH_CUST_SO, HYBM_SDMA_ORCH_CUST_JSON, HYBM_SDMA_ORCH_DEFAULT_JSON};
    for (const auto *name : names) {
        const auto file = directory / name;
        std::error_code error;
        if (!std::filesystem::is_regular_file(file, error) || access(file.c_str(), R_OK) != 0) {
            return false;
        }
    }
    return true;
}

static Result FindOrchestratorJson(const std::string &directory, std::string &jsonPath)
{
    std::error_code error;
    const auto root = std::filesystem::canonical(directory, error);
    if (error || !std::filesystem::is_directory(root, error)) {
        BM_LOG_ERROR("invalid MF_SDMA_ORCH_DIR: " << directory << " error: " << error.message());
        return BM_FILE_NOT_ACCESS;
    }
    std::set<std::string> matches;
    // 子目录符号链接不递归跟随，避免循环和同一安装目录被重复发现。
    std::filesystem::recursive_directory_iterator current(root, error), end;
    for (; !error && current != end; current.increment(error)) {
        const auto &path = current->path();
        if (path.filename() == HYBM_SDMA_ORCH_DEFAULT_JSON && HasOrchestratorArtifacts(path.parent_path())) {
            matches.insert(path.string());
        }
    }
    if (error) {
        BM_LOG_ERROR("scan MF_SDMA_ORCH_DIR failed, directory: " << directory << " error: " << error.message());
        return BM_FILE_NOT_ACCESS;
    }
    if (matches.size() != 1) {
        BM_LOG_ERROR("MF_SDMA_ORCH_DIR requires one complete artifact directory, root: "
                     << directory << " matches: " << matches.size() << " required: " << HYBM_SDMA_ORCH_CUST_SO << ", "
                     << HYBM_SDMA_ORCH_CUST_JSON << ", " << HYBM_SDMA_ORCH_DEFAULT_JSON);
        for (const auto &match : matches) {
            BM_LOG_ERROR("SDMA artifact candidate: " << match);
        }
        return BM_FILE_NOT_ACCESS;
    }
    jsonPath = *matches.begin();
    BM_LOG_INFO("SDMA artifacts found under MF_SDMA_ORCH_DIR, launch json: " << jsonPath);
    return BM_OK;
}

Result SdmaOrchestrator::ResolveOrchestratorJson(const std::string &directory, std::string &jsonPath)
{
    jsonPath.clear();
    if (!directory.empty()) {
        return FindOrchestratorJson(directory, jsonPath);
    }
    std::string candidate = FindInstalledJson();
    if (candidate.empty()) {
        candidate = HYBM_SDMA_ORCH_DEFAULT_JSON;
    }
    char resolved[PATH_MAX] = {0};
    if (realpath(candidate.c_str(), resolved) == nullptr || access(resolved, R_OK) != 0) {
        BM_LOG_ERROR("sdma orchestrator json not accessible: " << candidate << " (set MF_SDMA_ORCH_DIR)");
        return BM_FILE_NOT_ACCESS;
    }
    jsonPath = resolved;
    return BM_OK;
}

Result SdmaOrchestrator::LoadOrchestratorKernel()
{
    std::string jsonPath;
    auto ret = ResolveOrchestratorJson(env::MF_SDMA_ORCH_DIR, jsonPath);
    if (ret != BM_OK) {
        return ret;
    }
    std::string runtimeJson;
    ret = PrepareRuntimeJson(jsonPath, runtimeJson);
    if (ret != BM_OK) {
        return ret;
    }
    ret = DeployOrchestratorKernel(DirOf(jsonPath), runtimeJson);
    (void)unlink(runtimeJson.c_str());
    return ret;
}

Result SdmaOrchestrator::PrepareRuntimeJson(const std::string &jsonPath, std::string &runtimeJson)
{
    std::ifstream input(jsonPath);
    if (!input) {
        BM_LOG_ERROR("open launch json failed, path: " << jsonPath);
        return BM_FILE_NOT_ACCESS;
    }
    std::string content((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    const auto sourceName = ExtractDeviceSoName(content);
    if (!IsValidDeviceSoName(sourceName) || content.find_last_of('}') == std::string::npos) {
        BM_LOG_ERROR("invalid launch json or kernelSo, path: " << jsonPath);
        return BM_INVALID_PARAM;
    }
    const auto field = content.find("\"kernelSo\"");
    const auto namePos = content.find('"', content.find(':', field)) + 1;
    hostPid_ = 0;
    const auto pidRet = DlAclApi::RtDeviceGetBareTgid(&hostPid_);
    if (pidRet != BM_OK || hostPid_ == 0) {
        BM_LOG_ERROR("resolve deployment host pid failed, ret: " << pidRet << " hostPid: " << hostPid_
                                                                 << " deviceId: " << devId_);
        hostPid_ = 0;
        return BM_DL_FUNCTION_FAILED;
    }
    BM_LOG_INFO("deployment pid mapping, localPid: " << getpid() << " hostPid: " << hostPid_);
    std::string path = "/tmp/libmf_sdma_orch_" + std::to_string(hostPid_) + "_XXXXXX.json";
    int fd = mkstemps(&path[0], 5);
    if (fd < 0) {
        BM_LOG_ERROR("create runtime launch json failed, errno: " << errno);
        return BM_FILE_NOT_ACCESS;
    }
    const auto base = path.substr(path.find_last_of('/') + 1);
    deviceSoName_ = base.substr(0, base.size() - 5) + ".so";
    content.replace(namePos, sourceName.size(), deviceSoName_);
    const auto end = content.find_last_of('}');
    content.insert(end, ",\"HybmSdmaVerifyDeploy\":{\"opInfo\":{\"opKernelLib\":\"KFCKernel\",\"kernelSo\":\"" +
                            deviceSoName_ + "\",\"functionName\":\"HybmSdmaVerifyDeploy\"}}");
    const bool written = write(fd, content.data(), content.size()) == static_cast<ssize_t>(content.size());
    const int closeRet = close(fd);
    if (!written || closeRet != 0) {
        BM_LOG_ERROR("write runtime launch json failed, path: " << path << " errno: " << errno);
        (void)unlink(path.c_str());
        return BM_FILE_NOT_ACCESS;
    }
    runtimeJson = path;
    return BM_OK;
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

Result SdmaOrchestrator::DeployOrchestratorKernel(const std::string &kernelDir, const std::string &launchJsonPath)
{
    DeployFlockGuard lock;
    if (!lock.Valid()) {
        return BM_ERROR;
    }
    const std::string dst = std::string(HYBM_SDMA_ORCH_DEV_SEARCH_DIR) + "/" + deviceSoName_;
    deployNonce_ = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    auto ret = BM_OK;
    BM_LOG_INFO("automatic deploy begin, devId: " << devId_ << " dst: " << dst);
    uint64_t soSize = 0;
    ret = MigrateViaCustChannel(kernelDir, soSize);
    if (ret != BM_OK) {
        return ret;
    }
    deviceSoOwned_ = true;
    ret = CopyToSearchPath(deviceSoName_, soSize);
    if (ret != BM_OK && ret != HYBM_SDMA_ORCH_KFC_RESULT_UNCONFIRMED) {
        return ret;
    }
    const bool resultUnconfirmed = ret == HYBM_SDMA_ORCH_KFC_RESULT_UNCONFIRMED;
    ret = TryLoadKernel(launchJsonPath);
    if (ret == BM_OK && resultUnconfirmed) {
        ret = VerifyDeployment(kernelDir + "/" + HYBM_SDMA_ORCH_CUST_SO);
    }
    if (ret != BM_OK) {
        BM_LOG_ERROR("deployment load or verification failed, devId: " << devId_ << " dst: " << dst << " ret: " << ret);
    } else {
        BM_LOG_INFO("kernel deployed to CP1 search path, devId: " << devId_ << " nonce: " << deployNonce_
                                                                  << " dst: " << dst);
    }
    return ret;
}

Result SdmaOrchestrator::MigrateViaCustChannel(const std::string &dir, uint64_t &soSize)
{
    const std::string hostSo = dir + "/" + HYBM_SDMA_ORCH_CUST_SO;
    struct stat st = {};
    if (stat(hostSo.c_str(), &st) != 0) {
        BM_LOG_ERROR("cust kernel so missing, path: " << hostSo);
        return BM_FILE_NOT_ACCESS;
    }
    soSize = static_cast<uint64_t>(st.st_size); // 设备侧完整性校验用
    const std::string custJson = dir + "/" + HYBM_SDMA_ORCH_CUST_JSON;
    char resolved[PATH_MAX] = {0};
    if (realpath(custJson.c_str(), resolved) == nullptr || access(resolved, R_OK) != 0) {
        BM_LOG_ERROR("cust json not accessible, path: " << custJson);
        return BM_FILE_NOT_ACCESS;
    }
    // CANN 会复用已迁移的文件；文件生命周期由 CANN 管理，后续初始化保留迁移源。
    aclrtBinaryLoadOption loadOption{};
    loadOption.type = aclrtBinaryLoadOptionType::ACL_RT_BINARY_LOAD_OPT_CPU_KERNEL_MODE;
    loadOption.value.cpuKernelMode = 1; /* mode=1: H2D 迁移通道(CUSTAICPUKernel 专用), 本身不落盘 */
    aclrtBinaryLoadOptions loadOptions{};
    loadOptions.options = &loadOption;
    loadOptions.numOpt = 1;
    aclrtBinHandle bh = nullptr;
    auto ret = DlAclApi::AclrtBinaryLoadFromFile(resolved, &loadOptions, &bh);
    if (ret != BM_OK) {
        BM_LOG_ERROR("cust migrate load failed, ret: " << ret << " json: " << resolved);
        return ret;
    }
    aclrtFuncHandle fh = nullptr;
    ret = DlAclApi::AclrtBinaryGetFunction(bh, HYBM_SDMA_ORCH_FUNC_NAME, &fh); // GetFunction 才触发设备写文件
    if (ret != BM_OK) {
        /* 返回值不可靠: 实测 CUST 模式下正确入口名也可能 107000 且文件照样落盘,
         * 不作失败处理, 以复制+完整性校验为最终判据 */
        BM_LOG_INFO("cust migrate getfunc ret: " << ret << " (tolerated, verify later)");
    }
    custBinHandle_ = bh; // 保活: 持句柄至进程退出(迁移进程退出才触发清理链, 本进程即用户进程)
    BM_LOG_INFO("cust migrate done, devId: " << devId_ << " soSize: " << soSize);
    return BM_OK;
}

/* 加载 KFC 通道入口: 写临时 json → mode=0 加载 → 取 op "K" 句柄 → 删临时文件
 * (json 须带 .json 扩展名——无扩展名实测加载 107000; mkstemp 模板须以 X 结尾
 * 与扩展名冲突, 故用 pid+计数排它创建; CWD 可能只读, 用 /tmp) */
Result SdmaOrchestrator::LoadKfcChannel(aclrtFuncHandle &fh)
{
    if (kfcFuncHandle_ != nullptr) { // 已缓存(首次装载后复用)
        fh = kfcFuncHandle_;
        return BM_OK;
    }
    fh = nullptr;
    char path[64] = {0};
    int fd = -1;
    for (uint32_t i = 0; i < 16 && fd < 0; i++) { // 计数上限 16(同名残留/并发冲突防御)
        (void)snprintf(path, sizeof(path), "/tmp/mf_kfc_%d_%u.json", static_cast<int>(getpid()), i);
        fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
    }
    if (fd < 0) {
        BM_LOG_ERROR("kfc tmpfile failed, errno: " << errno);
        return BM_FILE_NOT_ACCESS;
    }
    // 最小 KFC json(functionName=system, kernelSo 指向不存在文件触发 dlsym 回退)
    static constexpr char KFC_JSON_CONTENT[] =
        "{\"K\":{\"opInfo\":{\"opKernelLib\":\"KFCKernel\",\"kernelSo\":\"kfc_min.so\","
        "\"functionName\":\"system\"}}}";
    const ssize_t wantLen = static_cast<ssize_t>(sizeof(KFC_JSON_CONTENT) - 1);
    if (write(fd, KFC_JSON_CONTENT, sizeof(KFC_JSON_CONTENT) - 1) != wantLen) {
        BM_LOG_ERROR("kfc tmpfile write failed, errno: " << errno << " json: " << path);
        (void)close(fd);
        (void)unlink(path);
        return BM_FILE_NOT_ACCESS;
    }
    (void)close(fd);
    aclrtBinaryLoadOption loadOption{};
    loadOption.type = aclrtBinaryLoadOptionType::ACL_RT_BINARY_LOAD_OPT_CPU_KERNEL_MODE;
    loadOption.value.cpuKernelMode = 0;
    aclrtBinaryLoadOptions loadOptions{};
    loadOptions.options = &loadOption;
    loadOptions.numOpt = 1;
    aclrtBinHandle bh = nullptr;
    auto ret = DlAclApi::AclrtBinaryLoadFromFile(path, &loadOptions, &bh);
    (void)unlink(path); // json 已消费即删
    if (ret != BM_OK) {
        BM_LOG_ERROR("kfc load failed, ret: " << ret << " json: " << path);
        return ret;
    }
    ret = DlAclApi::AclrtBinaryGetFunction(bh, "K", &fh);
    if (ret == BM_OK) {
        kfcFuncHandle_ = fh; // 缓存句柄(部署链全程复用)
    } else {
        BM_LOG_ERROR("kfc getfunc failed, ret: " << ret);
    }
    return ret;
}

Result SdmaOrchestrator::KfcExec(const std::string &cmd)
{
    if (cmd.size() >= HYBM_SDMA_ORCH_CMD_BUF_SIZE) {
        BM_LOG_ERROR("kfc cmd too long, len: " << cmd.size());
        return BM_INVALID_PARAM;
    }
    aclrtFuncHandle fh = nullptr;
    auto ret = LoadKfcChannel(fh);
    if (ret != BM_OK) {
        return ret == HYBM_SDMA_ORCH_KFC_RESULT_UNCONFIRMED ? BM_ERROR : ret;
    }
    char buf[HYBM_SDMA_ORCH_CMD_BUF_SIZE] = {0};
    strncpy(buf, cmd.c_str(), sizeof(buf) - 1);
    aclrtLaunchKernelAttr attr{}; // 超时属性: 防设备 shell 挂死卡初始化
    attr.id = aclrtLaunchKernelAttrId::ACL_RT_LAUNCH_KERNEL_ATTR_TIMEOUT;
    attr.value.timeout = HYBM_SDMA_ORCH_KFC_TIMEOUT_S;
    aclrtLaunchKernelCfg cfg{};
    cfg.attrs = &attr;
    cfg.numAttrs = 1;
    void *commandStream = nullptr;
    ret = DlAclApi::AclrtCreateStream(&commandStream);
    if (ret != BM_OK) {
        BM_LOG_ERROR("create shell stream failed, ret: " << ret);
        return ret == HYBM_SDMA_ORCH_KFC_RESULT_UNCONFIRMED ? BM_ERROR : ret;
    }
    // 命令串作为 host args 透传 → 设备 OS 内 system() 执行
    ret = DlAclApi::AclrtLaunchKernelWithHostArgs(fh, 1, commandStream, &cfg, buf, strlen(buf) + 1, nullptr, 0);
    if (ret != BM_OK) {
        BM_LOG_ERROR("kfc launch failed, ret: " << ret << " cmd: " << cmd);
        (void)DlAclApi::AclrtDestroyStream(commandStream);
        return ret == HYBM_SDMA_ORCH_KFC_RESULT_UNCONFIRMED ? BM_ERROR : ret;
    }
    ret = DlAclApi::AclrtSynchronizeStream(commandStream);
    if (ret != BM_OK) { // 保留原始错误，507018交由调用方确认实际状态
        if (ret == HYBM_SDMA_ORCH_KFC_RESULT_UNCONFIRMED) {
            BM_LOG_INFO("shell result unconfirmed; postcondition required, sync: " << ret << " cmd: " << cmd);
        } else {
            BM_LOG_ERROR("kfc cmd failed, sync: " << ret << " cmd: " << cmd);
        }
    }
    (void)DlAclApi::AclrtDestroyStream(commandStream);
    return ret;
}

Result SdmaOrchestrator::CopyToSearchPath(const std::string &deviceSoName, uint64_t soSize)
{
    if (hostPid_ == 0) {
        BM_LOG_ERROR("deployment host pid unavailable, deviceId: " << devId_);
        return BM_INVALID_PARAM;
    }
    // CANN 的迁移目录使用宿主机 PID，容器内 PID 可能属于另一个命名空间。
    const std::string src = "/home/CustAiCpuUser/cust_aicpu_" + std::to_string(dieId_) + "_*_" +
                            std::to_string(hostPid_) + "/" + HYBM_SDMA_ORCH_CUST_SO;
    const std::string dst = std::string(HYBM_SDMA_ORCH_DEV_SEARCH_DIR) + "/" + deviceSoName;
    const std::string copyCmd = "cat " + src + " > " + dst + " && chmod 644 " + dst;
    const auto ret = KfcExec(copyCmd);
    BM_LOG_INFO("deployment copy submitted, devId: " << devId_ << " nonce: " << deployNonce_ << " bytes: " << soSize
                                                     << " shellRet: " << ret);
    return ret;
}

Result SdmaOrchestrator::VerifyDeployment(const std::string &hostSo)
{
    std::ifstream input(hostSo, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (input.bad() || bytes.empty()) {
        BM_LOG_ERROR("read deployment source failed, path: " << hostSo);
        return BM_FILE_NOT_ACCESS;
    }
    return CheckDeploymentResult(bytes.data(), bytes.size());
}

Result SdmaOrchestrator::CheckDeploymentResult(const void *bytes, size_t size, bool allowPresent)
{
    void *expected = nullptr;
    auto ret = DlAclApi::AclrtMalloc(&expected, size + 128, ACL_MEM_MALLOC_HUGE_FIRST);
    if (ret != BM_OK) {
        BM_LOG_ERROR("alloc verification buffer failed, ret: " << ret);
        return ret;
    }
    ScopeGuard<void *> release(expected, [](void *p) { (void)DlAclApi::AclrtFree(p); });
    if (size != 0) {
        ret = DlAclApi::AclrtMemcpy(expected, size, bytes, size, ACL_MEMCPY_HOST_TO_DEVICE);
    }
    if (ret != BM_OK) {
        BM_LOG_ERROR("upload verification bytes failed, ret: " << ret);
        return ret;
    }
    HybmSdmaDeployCheck check{};
    check.expectedAddr = reinterpret_cast<uint64_t>(expected);
    check.expectedSize = size;
    check.nonce = deployNonce_;
    check.resultAddr = reinterpret_cast<uint64_t>(expected) + ((size + 63) & ~size_t(63));
    const std::string target = std::string(HYBM_SDMA_ORCH_DEV_SEARCH_DIR) + "/" + deviceSoName_;
    if (target.size() >= sizeof(check.path)) {
        BM_LOG_ERROR("verification path too long, size: " << target.size());
        return BM_INVALID_PARAM;
    }
    memcpy(check.path, target.c_str(), target.size() + 1);
    ret = DlAclApi::AclrtMemset(reinterpret_cast<void *>(check.resultAddr), sizeof(HybmSdmaDeployResult), 0,
                                sizeof(HybmSdmaDeployResult));
    aclrtFuncHandle verify = nullptr;
    if (ret == BM_OK) {
        ret = DlAclApi::AclrtBinaryGetFunction(binHandle_, "HybmSdmaVerifyDeploy", &verify);
    }
    void *stream = nullptr;
    if (ret == BM_OK) {
        ret = DlAclApi::AclrtCreateStream(&stream);
    }
    if (ret != BM_OK) {
        BM_LOG_ERROR("prepare verifier failed, ret: " << ret);
        return ret;
    }
    ScopeGuard<void *> closeStream(stream, [](void *p) { (void)DlAclApi::AclrtDestroyStream(p); });
    ret = DlAclApi::AclrtLaunchKernelWithHostArgs(verify, 1, stream, nullptr, &check, sizeof(check), nullptr, 0);
    if (ret == BM_OK) {
        ret = DlAclApi::AclrtSynchronizeStream(stream);
    }
    HybmSdmaDeployResult result{};
    if (ret == BM_OK) {
        ret = DlAclApi::AclrtMemcpy(&result, sizeof(result), reinterpret_cast<void *>(check.resultAddr), sizeof(result),
                                    ACL_MEMCPY_DEVICE_TO_HOST);
    }
    const bool presentAllowed = allowPresent && size == 0 && result.status == 6;
    if (ret != BM_OK || result.nonce != check.nonce || (result.status != 0 && !presentAllowed) ||
        result.compared != size) {
        BM_LOG_ERROR("deployment postcondition failed, ret: " << ret << " status: " << result.status
                                                              << " compared: " << result.compared
                                                              << " expected: " << size << " nonce: " << result.nonce);
        return BM_ERROR;
    }
    const char *message = allowPresent ? "cleanup verifier ready, nonce: "
                          : size       ? "deployed SO byte-for-byte verified, nonce: "
                                       : "deployment files absence verified, nonce: ";
    BM_LOG_INFO(message << check.nonce << " bytes: " << result.compared << " path: " << target);
    return BM_OK;
}

Result SdmaOrchestrator::ConfigureOrch(uint64_t ownSegGva, uint64_t physSize, uint64_t symSize, uint32_t ownRank,
                                       uint64_t poolBaseGva, uint32_t rankCount)
{
    std::lock_guard<std::mutex> guard(mutex_);
    ServiceContextGuard context(serviceContext_);
    if (context.ret != BM_OK)
        return context.ret;
    if (configured_) { /* 约束: 双环一次绑定, 每进程单 SDMA 池 */
        BM_LOG_ERROR("sdma orch already configured, second pool unsupported, devId: " << devId_);
        return BM_INVALID_PARAM;
    }
    if (symSize == 0 || (symSize & (symSize - 1)) != 0) { // 2 的幂(对端段基址掩码算术依赖)
        BM_LOG_ERROR("symSize not power of two, symSize: " << symSize);
        return BM_INVALID_PARAM;
    }
    if (rankCount == 0 || rankCount > SMEM_SHM_SDMA_MAX_RANKS || ownRank >= rankCount) {
        BM_LOG_ERROR("invalid orch ranks, rank: " << ownRank << " rankCount: " << rankCount);
        return BM_INVALID_PARAM;
    }

    if (poolBaseGva == 0 || ownSegGva < poolBaseGva) { // 池窗口基址(对端 rank 路由原点)
        BM_LOG_ERROR("orch pool base invalid, poolBaseGva: 0x" << std::hex << poolBaseGva << std::dec
                                                               << " ownSegGva: 0x" << std::hex << ownSegGva);
        return BM_INVALID_PARAM;
    }
    const uint64_t resBase = ownSegGva + physSize - SMEM_SHM_SDMA_RESERVED;
    // 保留区整体清零(双环/游标/戳/停止标志从零起步)
    auto ret =
        DlAclApi::AclrtMemset(reinterpret_cast<void *>(resBase), SMEM_SHM_SDMA_RESERVED, 0, SMEM_SHM_SDMA_RESERVED);
    if (ret != BM_OK) {
        BM_LOG_ERROR("memset orch reserved failed, ret: " << ret << " resBase: 0x" << std::hex << resBase
                                                          << " size: " << SMEM_SHM_SDMA_RESERVED);
        return ret;
    }
    ret = DlAclApi::AclrtMemset(reinterpret_cast<void *>(workspace_.load()), HYBM_SDMA_ORCH_WS_SIZE, 0,
                                HYBM_SDMA_ORCH_WS_SIZE);
    if (ret != BM_OK) {
        BM_LOG_ERROR("clear service workspace failed, devId: " << devId_ << " ret: " << ret);
        return ret;
    }
    {
        std::lock_guard<std::mutex> lock(workerMutex_);
        cachedResult_ = {};
    }
    epochParam_ = HybmSdmaOrchParam{};
    epochParam_.devId = dieId_; /* CP1 内 HAL/SMMU 均用 die 号(A.3); dev0/dev1 下与 host 设备号同值 */
    epochParam_.sqId = sqId_;
    epochParam_.streamId = streamId_;
    const uint32_t budgetS = serviceOptions_.epochSeconds; /* 常驻服务或显式有限轮次 */
    epochParam_.epochBudgetS = budgetS;
    epochParam_.hostLeaseMs = serviceOptions_.leaseMs;
    epochParam_.stopTimeoutMs = serviceOptions_.stopMs;
    epochParam_.instance = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    boundReservation_ = resBase;
    epochParam_.resBase = resBase; /* 本端保留区基址(v29: 三请求环/完成戳/暂存均由此定位) */
    const uint64_t wsBase = workspace_.load();
    /* 停止标志/overflow/结果区放工作区而非段内: 销毁序安全(段先销毁也不影响停止语义) */
    epochParam_.stopAddr = wsBase + HYBM_SDMA_ORCH_WS_STOP_OFF;
    epochParam_.overflowAddr = wsBase + HYBM_SDMA_ORCH_WS_OVF_OFF;
    epochParam_.resultAddr = wsBase + HYBM_SDMA_ORCH_WS_RESULT_OFF;
    epochParam_.symSize = symSize;
    epochParam_.physSize = physSize;
    epochParam_.rank = ownRank; /* v29: 请求环/到达环路由(本端视角) */
    epochParam_.rankCount = rankCount;
    epochParam_.poolBaseGva = poolBaseGva; /* v29: dstSeg → 目的 rank 的换算原点(路由复核) */
    /* 首轮内联下发: 失败立即报错(池创建失败, 调用方可重试)——监督线程只负责后续重拉,
     * 不得吞掉首轮失败造成池看似就绪实则无消费者的静默半死 */
    ret = DlAclApi::AclrtCreateStream(&epochStream_);
    if (ret != BM_OK) {
        BM_LOG_ERROR("create epoch stream failed, ret: " << ret << " devId: " << devId_);
        return ret;
    }
    releaseConfirmed_ = false;
    ret = StartServiceWorker();
    if (ret != BM_OK)
        return ret;
    configured_ = true;
    ready_ = true;
    BM_LOG_INFO("sdma orch configured, devId: "
                << devId_ << " epochBudgetS: "
                << (budgetS == HYBM_SDMA_ORCH_EPOCH_BUDGET_RESIDENT ? std::string("resident") : std::to_string(budgetS))
                << " resBase: 0x" << std::hex << epochParam_.resBase << " rank: " << std::dec << epochParam_.rank
                << " poolBaseGva: 0x" << std::hex << epochParam_.poolBaseGva
                << " peerRings: " << SMEM_SHM_SDMA_MAX_PEERS << " arrivalDepth: " << SMEM_SHM_SDMA_RS_PEER_SLOTS);
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
    if (controlHost_ != nullptr) {
        void *buffer = static_cast<char *>(controlHost_) + 128;
        std::memset(buffer, 0, sizeof(HybmSdmaOrchResult));
        ret = CopyControl(epochParam_.resultAddr, buffer, sizeof(HybmSdmaOrchResult), ACL_MEMCPY_HOST_TO_DEVICE);
        if (ret != BM_OK)
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
    // 服务自行判断时间及停止条件，runtime任务超时保持为0。
    attr.value.timeout = HYBM_SDMA_ORCH_LAUNCH_TIMEOUT_NEVER_S;
    aclrtLaunchKernelCfg cfg{};
    cfg.attrs = &attr;
    cfg.numAttrs = 1;
    // 单轮下发(异步提交, kernel 跑满预算/停止标志/错误退出; 由监督线程 sync)
    ret =
        DlAclApi::AclrtLaunchKernelWithConfig(funcHandle_, HYBM_SDMA_ORCH_BLOCK_DIM, stream, &cfg, argsHandle, nullptr);
    if (ret != BM_OK) {
        BM_LOG_ERROR("LaunchKernelWithConfig(epoch) failed, ret: " << ret << " sqId: " << sqId_);
    }
    return ret;
}

Result SdmaOrchestrator::InitControl()
{
    auto ret = DlAclApi::AclrtCreateStream(&controlStream_);
    if (ret == BM_OK)
        ret = DlAclApi::AclrtMallocHost(&controlHost_, 256);
    if (ret != BM_OK)
        BM_LOG_ERROR("allocate SDMA control resources failed, devId: " << devId_ << " ret: " << ret);
    else
        std::memset(controlHost_, 0, 256);
    return ret;
}

Result SdmaOrchestrator::CopyControl(uint64_t device, void *host, size_t bytes, uint32_t kind)
{
    if (controlPending_) {
        auto ret = DlAclApi::AclrtSynchronizeStreamWithTimeout(controlStream_, 250);
        if (ret != BM_OK) {
            BM_LOG_ERROR("previous control copy pending, devId: " << devId_ << " ret: " << ret);
            return ret;
        }
        controlPending_ = false;
    }
    void *dev = reinterpret_cast<void *>(device);
    auto ret = DlAclApi::AclrtMemcpyAsync(kind == ACL_MEMCPY_HOST_TO_DEVICE ? dev : host, bytes,
                                          kind == ACL_MEMCPY_HOST_TO_DEVICE ? host : dev, bytes, kind, controlStream_);
    if (ret == BM_OK) {
        controlPending_ = true;
        ret = DlAclApi::AclrtSynchronizeStreamWithTimeout(controlStream_, 250);
        if (ret == BM_OK)
            controlPending_ = false;
    }
    if (ret != BM_OK)
        BM_LOG_ERROR("SDMA control copy failed, devId: " << devId_ << " ret: " << ret);
    return ret;
}

Result SdmaOrchestrator::ReadServiceResult(HybmSdmaOrchResult &result)
{
    void *buffer = static_cast<char *>(controlHost_) + 128;
    auto ret = CopyControl(epochParam_.resultAddr, buffer, sizeof(result), ACL_MEMCPY_DEVICE_TO_HOST);
    if (ret == BM_OK)
        std::memcpy(&result, buffer, sizeof(result));
    return ret;
}

Result SdmaOrchestrator::StartServiceWorker()
{
    stopFlag_ = false;
    startResult_ = BM_NOT_INITIALIZED;
    workerDone_ = false;
    try {
        supervisor_ = std::thread(&SdmaOrchestrator::SupervisorLoop, this);
    } catch (const std::system_error &e) {
        workerDone_ = true;
        releaseConfirmed_ = true;
        BM_LOG_ERROR("start SDMA service thread failed, errno: " << e.code().value());
        return BM_ERROR;
    }
    std::unique_lock<std::mutex> lock(workerMutex_);
    if (!workerChanged_.wait_for(lock, std::chrono::seconds(2),
                                 [this] { return startResult_.load() != BM_NOT_INITIALIZED || workerDone_.load(); })) {
        stopFlag_ = true;
        BM_LOG_ERROR("SDMA service startup timeout, devId: " << devId_);
        return BM_TIMEOUT;
    }
    return startResult_.load();
}

Result SdmaOrchestrator::RunService()
{
    auto ret = DlAclApi::AclrtSetCurrentContext(serviceContext_);
    if (ret != BM_OK) {
        BM_LOG_ERROR("bind SDMA service context failed, devId: " << devId_ << " ret: " << ret);
        return ret;
    }
    if (!epochLaunched_) {
        ret = LaunchEpoch(epochStream_);
        if (ret != BM_OK)
            return ret;
        epochLaunched_ = true;
    }
    return PollService();
}

Result SdmaOrchestrator::UpdateServiceControl(bool &stopWritten, std::chrono::steady_clock::time_point &deadline,
                                              std::chrono::steady_clock::time_point &nextHeartbeat)
{
    const auto now = std::chrono::steady_clock::now();
    if (stopFlag_.load() && !stopWritten) {
        auto *stop = reinterpret_cast<uint64_t *>(static_cast<char *>(controlHost_) + 64);
        *stop = 1;
        auto ret = CopyControl(epochParam_.stopAddr, stop, sizeof(*stop), ACL_MEMCPY_HOST_TO_DEVICE);
        if (ret != BM_OK)
            return ret;
        stopWritten = true;
        deadline = now + std::chrono::milliseconds(serviceOptions_.stopMs + 500);
    }
    if (!stopWritten && now >= nextHeartbeat) {
        auto *heartbeat = static_cast<uint64_t *>(controlHost_);
        ++*heartbeat;
        auto ret = CopyControl(workspace_.load() + HYBM_SDMA_ORCH_WS_HEARTBEAT_OFF, heartbeat, sizeof(*heartbeat),
                               ACL_MEMCPY_HOST_TO_DEVICE);
        if (ret != BM_OK)
            return ret;
        nextHeartbeat = now + std::chrono::seconds(1);
    }
    return BM_OK;
}

Result SdmaOrchestrator::HandleServiceResult(const HybmSdmaOrchResult &observed, bool &finished)
{
    HybmSdmaOrchResult r = observed;
    finished = false;
    if (r.magic == 0)
        return BM_OK;
    if (r.magic != HYBM_SDMA_ORCH_RESULT_MAGIC || r.instance != epochParam_.instance) {
        BM_LOG_ERROR("SDMA service protocol mismatch, magic: " << r.magic << " devId: " << devId_);
        return BM_ERROR;
    }
    {
        std::lock_guard<std::mutex> lock(workerMutex_);
        cachedResult_ = r;
    }
    if (startResult_ == BM_NOT_INITIALIZED && r.serviceState == SMEM_SHM_SDMA_RUNNING) {
        startResult_ = BM_OK;
        workerChanged_.notify_all();
    }
    if (r.exitReason == HYBM_SDMA_ORCH_EXIT_RUNNING)
        return BM_OK;
    auto ret = DlAclApi::AclrtSynchronizeStreamWithTimeout(epochStream_, static_cast<int32_t>(serviceOptions_.stopMs));
    if (ret != BM_OK) {
        BM_LOG_ERROR("service task completion unconfirmed, ret: " << ret << " devId: " << devId_);
        return ret;
    }
    // 退出标志可早于排空完成被读到，stream结束后重新读取完整终态。
    ret = ReadServiceResult(r);
    if (ret != BM_OK)
        return ret;
    if (r.magic != HYBM_SDMA_ORCH_RESULT_MAGIC || r.instance != epochParam_.instance) {
        BM_LOG_ERROR("completed SDMA service protocol mismatch, devId: " << devId_);
        return BM_ERROR;
    }
    {
        std::lock_guard<std::mutex> lock(workerMutex_);
        cachedResult_ = r;
    }
    if (r.exitReason == HYBM_SDMA_ORCH_EXIT_DEADLINE)
        return LaunchEpoch(epochStream_);
    releaseConfirmed_ = r.releaseConfirmed != 0;
    finished = true;
    if (r.exitReason == HYBM_SDMA_ORCH_EXIT_ERROR || !releaseConfirmed_) {
        BM_LOG_ERROR("SDMA service failed, ret: " << r.mainRet << " devId: " << devId_
                                                  << " releaseConfirmed: " << r.releaseConfirmed);
        return BM_ERROR;
    }
    return BM_OK;
}

Result SdmaOrchestrator::PollService()
{
    auto nextHeartbeat = std::chrono::steady_clock::now();
    auto deadline = std::chrono::steady_clock::time_point::max();
    bool stopWritten = false;
    for (;;) {
        auto ret = UpdateServiceControl(stopWritten, deadline, nextHeartbeat);
        if (ret != BM_OK)
            return ret;
        HybmSdmaOrchResult result{};
        ret = ReadServiceResult(result);
        if (ret != BM_OK)
            return ret;
        bool finished = false;
        ret = HandleServiceResult(result, finished);
        if (ret != BM_OK || finished)
            return ret;
        if (std::chrono::steady_clock::now() >= deadline) {
            BM_LOG_ERROR("SDMA service stop timeout, devId: " << devId_);
            return BM_TIMEOUT;
        }
        std::unique_lock<std::mutex> lock(workerMutex_);
        workerChanged_.wait_for(lock, std::chrono::milliseconds(100),
                                [this, stopWritten] { return stopFlag_.load() && !stopWritten; });
    }
}

void SdmaOrchestrator::SupervisorLoop()
{
    auto done = make_scope_guard(this, [](SdmaOrchestrator *self) {
        self->ready_ = false;
        {
            std::lock_guard<std::mutex> lock(self->workerMutex_);
            self->workerDone_ = true;
        }
        self->workerChanged_.notify_all();
    });
    workerResult_ = RunService();
    if (workerResult_ != BM_OK)
        RecordHostFailure(1384);
    if (startResult_ == BM_NOT_INITIALIZED)
        startResult_ = workerResult_.load();
}

Result SdmaOrchestrator::DestroyEpochResources()
{
    if (epochStream_ == nullptr) {
        return BM_OK;
    }
    auto ret = DlAclApi::AclrtSynchronizeStreamWithTimeout(epochStream_, static_cast<int32_t>(serviceOptions_.stopMs));
    if (ret != BM_OK) {
        BM_LOG_ERROR("epoch completion unconfirmed, retaining stream, ret: " << ret << " sqId: " << sqId_);
        return ret;
    }
    ret = DlAclApi::AclrtDestroyStream(epochStream_);
    if (ret != BM_OK) {
        BM_LOG_ERROR("destroy epoch stream failed, ret: " << ret << " sqId: " << sqId_);
        return ret;
    }
    epochStream_ = nullptr;
    return BM_OK;
}

Result SdmaOrchestrator::GetServiceStatus(HybmSdmaOrchResult &result)
{
    std::lock_guard<std::mutex> lock(workerMutex_);
    result = cachedResult_;
    if (result.magic != HYBM_SDMA_ORCH_RESULT_MAGIC) {
        BM_LOG_ERROR("SDMA service status unavailable, devId: " << devId_);
        return BM_NOT_INITIALIZED;
    }
    return BM_OK;
}

void SdmaOrchestrator::RecordHostFailure(uint32_t reason)
{
    std::lock_guard<std::mutex> lock(workerMutex_);
    if (cachedResult_.magic == HYBM_SDMA_ORCH_RESULT_MAGIC && cachedResult_.serviceState != SMEM_SHM_SDMA_FAILED) {
        cachedResult_.serviceState = SMEM_SHM_SDMA_FAILED;
        cachedResult_.mainRet = reason;
        cachedResult_.releaseConfirmed = 0;
    }
}

Result SdmaOrchestrator::WaitServiceWorker()
{
    if (!supervisor_.joinable())
        return BM_OK;
    std::unique_lock<std::mutex> lock(workerMutex_);
    if (!workerChanged_.wait_for(lock, std::chrono::milliseconds(serviceOptions_.stopMs + 1000),
                                 [this] { return workerDone_.load(); })) {
        ready_ = false;
        lock.unlock();
        RecordHostFailure(1383);
        BM_LOG_ERROR("SDMA worker still active, retaining resources, devId: " << devId_);
        return BM_TIMEOUT;
    }
    lock.unlock();
    supervisor_.join();
    return BM_OK;
}

Result SdmaOrchestrator::RetryServiceStop()
{
    workerDone_ = false;
    try {
        supervisor_ = std::thread(&SdmaOrchestrator::SupervisorLoop, this);
    } catch (const std::system_error &e) {
        workerDone_ = true;
        BM_LOG_ERROR("retry SDMA stop thread failed, errno: " << e.code().value());
        return BM_ERROR;
    }
    return WaitServiceWorker();
}

Result SdmaOrchestrator::StopEpoch()
{
    std::lock_guard<std::mutex> stopLock(stopMutex_);
    stopFlag_ = true;
    workerChanged_.notify_all();
    const bool retry =
        workerDone_.load() && epochLaunched_.load() && !releaseConfirmed_.load() && controlHost_ != nullptr;
    auto workerRet = WaitServiceWorker();
    if (workerRet != BM_OK)
        return workerRet;
    if (retry) {
        workerRet = RetryServiceStop();
        if (workerRet != BM_OK)
            return workerRet;
    }
    ServiceContextGuard context(serviceContext_);
    if (context.ret != BM_OK)
        return context.ret;
    if (controlPending_) {
        auto ret = DlAclApi::AclrtSynchronizeStreamWithTimeout(controlStream_, 250);
        if (ret != BM_OK) {
            BM_LOG_ERROR("control stream completion unconfirmed, devId: " << devId_ << " ret: " << ret);
            return ret;
        }
        controlPending_ = false;
    }
    if (epochLaunched_ && !releaseConfirmed_) {
        BM_LOG_ERROR("SDMA resource references unconfirmed, devId: " << devId_ << " ret: " << workerResult_.load());
        return workerResult_ == BM_OK ? BM_ERROR : workerResult_.load();
    }
    auto ret = DestroyEpochResources();
    if (ret == BM_OK) {
        epochLaunched_ = false;
        configured_ = false;
        boundReservation_ = 0;
    }
    return ret;
}

void SdmaOrchestrator::Cleanup()
{
    if (StopEpoch() != BM_OK) {
        ready_ = false;
        return;
    }
    ServiceContextGuard context(serviceContext_);
    if (context.ret != BM_OK)
        return;
    configured_ = false; // 复位(允许后续重配置)
    if (deviceSoOwned_ && launchStream_ != nullptr) {
        const std::string dst = std::string(HYBM_SDMA_ORCH_DEV_SEARCH_DIR) + "/" + deviceSoName_;
        // 首次校验提交失败时，SO 可能尚未真正加载；删除前先执行入口，保留可用的校验代码。
        auto ret = CheckDeploymentResult(nullptr, 0, true);
        if (ret == BM_OK) {
            ret = KfcExec("rm -f " + dst);
        }
        if (ret == BM_OK || ret == HYBM_SDMA_ORCH_KFC_RESULT_UNCONFIRMED) {
            ret = CheckDeploymentResult(nullptr, 0);
        }
        if (ret == BM_OK) {
            BM_LOG_INFO("automatic deploy file removed, devId: " << devId_ << " dst: " << dst);
        } else {
            BM_LOG_ERROR("remove deployed kernel failed, devId: " << devId_ << " dst: " << dst << " ret: " << ret);
        }
    }
    deviceSoOwned_ = false;
    deployNonce_ = 0;
    deviceSoName_.clear();
    hostPid_ = 0;
    if (cpStream_ != nullptr) {
        (void)DlAclApi::AclrtDestroyStream(cpStream_); // SQ 随流注销
        cpStream_ = nullptr;
    }
    if (launchStream_ != nullptr) {
        (void)DlAclApi::AclrtDestroyStream(launchStream_);
        launchStream_ = nullptr;
    }
    if (workspace_ != 0) {
        (void)DlAclApi::AclrtFree(reinterpret_cast<void *>(workspace_.load()));
        workspace_ = 0;
    }
    if (controlStream_ != nullptr) {
        (void)DlAclApi::AclrtDestroyStream(controlStream_);
        controlStream_ = nullptr;
    }
    if (controlHost_ != nullptr) {
        (void)DlAclApi::AclrtFreeHost(controlHost_);
        controlHost_ = nullptr;
    }
    controlPending_ = false;
    // 先恢复调用线程，再销毁服务Context。
    context.Restore();
    if (serviceContext_ != nullptr) {
        auto ret = DlAclApi::AclrtDestroyContext(serviceContext_);
        if (ret != BM_OK)
            BM_LOG_ERROR("destroy service context failed, devId: " << devId_ << " ret: " << ret);
        else
            serviceContext_ = nullptr;
    }
    /* binHandle_ 无对应卸载接口(DlAclApi 未封装), 随进程生命周期释放 */
    binHandle_ = nullptr;
    funcHandle_ = nullptr;
    custBinHandle_ = nullptr;
    kfcFuncHandle_ = nullptr;
    refCount_ = 0;
    sqId_ = 0;
    streamId_ = 0;
    dieId_ = 0;
}
} // namespace mf
} // namespace ock
