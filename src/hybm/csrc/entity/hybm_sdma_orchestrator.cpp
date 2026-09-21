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
#include <fstream>
#include <iterator>
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
constexpr uint32_t HYBM_SDMA_ORCH_BLOCK_DIM = 1;       /* 单 block(kernel 状态机单线程语义) */
constexpr uint32_t HYBM_SDMA_ORCH_LAUNCH_TIMEOUT_S = 25; /* launch attr 超时, 须 < AICPU watchdog 30s */
constexpr char HYBM_SDMA_ORCH_FUNC_NAME[] = "HybmSdmaOrchEpoch"; /* epoch 消费 kernel 入口符号 */
constexpr char HYBM_SDMA_ORCH_DEFAULT_JSON[] = "libmf_sdma_orch_v6.json"; /* CWD 缺省 json(与 KVER 同步) */
constexpr char HYBM_SDMA_ORCH_CUST_JSON[] = "libmf_sdma_orch.json";  /* CUST 迁移 json 名(与 launch json 同目录) */
constexpr char HYBM_SDMA_ORCH_CUST_SO[] = "libmf_sdma_orch.so";      /* host 侧 kernel so 名(CUST 迁移源) */
/* CP1 搜索路径(实测: 每卡独立设备 OS 文件系统, 须在本卡设备上部署) */
constexpr char HYBM_SDMA_ORCH_DEV_SEARCH_DIR[] = "/usr/lib64/aicpu_kernels/0/aicpu_kernels_device";
constexpr char HYBM_SDMA_ORCH_CUST_DEV_DIR[] = "/home/CustAiCpuUser/lib"; /* CUST 迁移设备侧落盘目录 */
constexpr char HYBM_SDMA_ORCH_DEPLOY_LOCK[] = "/tmp/.mf_sdma_orch_deploy.lock"; /* 部署互斥锁(host 侧) */
constexpr uint32_t HYBM_SDMA_ORCH_DEPLOY_RETRY = 5;   /* 复制/校验重试次数(迁移异步完成裕量) */
constexpr uint32_t HYBM_SDMA_ORCH_CMD_BUF_SIZE = 512; /* KFC 命令缓冲尺寸(命令为受控常量拼接) */
/* KFC 命令退出码非 0 时 stream sync 的回传码(实测): 用于区分"文件缺失"与"通道异常" */
constexpr int32_t HYBM_SDMA_ORCH_KFC_CMD_EXIT_ERR = 507018;
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
    ret = DlAclApi::AclrtCreateStream(&launchStream_); // 普通流作编排 kernel 的提交流
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

/* 部署互斥锁 RAII: 同 host 多进程(双 rank/多池)并发部署时串行化, 防复制竞态;
 * 锁文件打开失败不致命(单进程无影响) */
class DeployFlockGuard {
public:
    /* O_NOFOLLOW: 拒绝跟随预置符号链接(防锁文件劫持); O_CLOEXEC: 子进程不继承;
     * 0600: 属主读写 */
    DeployFlockGuard()
        : fd_(open(HYBM_SDMA_ORCH_DEPLOY_LOCK, O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600))
    {
        if (fd_ < 0) {
            BM_LOG_INFO("deploy lock open failed, errno: " << errno); // 尽力而为, 不阻断
            return;
        }
        (void)flock(fd_, LOCK_EX); // 排它锁(阻塞等对端部署完成, 部署为秒级)
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
    const auto gate = KfcExec("test -f " + dst, true); // 探测(文件缺失为首轮预期, 降级 INFO)
    if (gate != HYBM_SDMA_ORCH_KFC_CMD_EXIT_ERR && gate != BM_OK) {
        // KFC 不可用(锁定环境): 按预部署假定直接加载, 文件若真缺失由数据面 wait 显式报错
        BM_LOG_ERROR("kfc channel unavailable (ret: " << gate << "), assume pre-deployed, dst: " << dst);
    } else if (gate == BM_OK) { // 设备侧已部署(预部署/同卡先启进程)
        auto ret = TryLoadKernel(resolved);
        if (ret != BM_OK) {
            BM_LOG_ERROR("kernel load failed though deployed, ret: " << ret << " json: " << resolved);
        }
        return ret;
    } else { // 507018: 设备侧文件缺失 → 自动部署(迁移+复制+校验)后加载
        BM_LOG_INFO("kernel not on device, auto deploy, devId: " << devId_ << " dst: " << dst);
        return DeployOrchestratorKernel(resolved, dst);
    }
    auto ret = TryLoadKernel(resolved); // 通道异常分支的兜底加载(预部署假定)
    if (ret != BM_OK) {
        BM_LOG_ERROR("kernel load failed, ret: " << ret << " json: " << resolved);
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

Result SdmaOrchestrator::DeployOrchestratorKernel(const std::string &launchJsonPath, const std::string &dst)
{
    DeployFlockGuard lock; // host 侧进程间互斥
    if (KfcExec("test -f " + dst, true) == BM_OK) { // 锁内复查: 对端进程可能刚部署完
        auto ret = TryLoadKernel(launchJsonPath);
        if (ret == BM_OK) {
            BM_LOG_INFO("kernel deployed by peer process, devId: " << devId_);
            return BM_OK;
        }
        BM_LOG_ERROR("kernel load failed though peer deployed, ret: " << ret << " json: " << launchJsonPath);
        return ret;
    }
    const std::string dir = DirOf(launchJsonPath); // 三件套同目录(launch json 旁)
    uint64_t soSize = 0;
    auto ret = MigrateViaCustChannel(dir, soSize);
    if (ret != BM_OK) {
        return ret;
    }
    const std::string devSo = dst.substr(dst.find_last_of('/') + 1);
    ret = CopyToSearchPath(devSo, soSize);
    if (ret != BM_OK) {
        return ret;
    }
    ret = TryLoadKernel(launchJsonPath);
    if (ret != BM_OK) {
        BM_LOG_ERROR("kernel load still fails after deploy, ret: " << ret << " devId: " << devId_
                     << " json: " << launchJsonPath);
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
    /* 迁移前清残留(防历史/对端残留与本轮迁移文件拼接; flock 已保证无并发迁移) */
    const std::string cleanCmd = "rm -f " + std::string(HYBM_SDMA_ORCH_CUST_DEV_DIR) + "/" +
                                 HYBM_SDMA_ORCH_CUST_SO + ".*";
    if (KfcExec(cleanCmd) != BM_OK) {
        BM_LOG_ERROR("deploy clean residue failed, devId: " << devId_);
        return BM_ERROR;
    }
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

Result SdmaOrchestrator::KfcExec(const std::string &cmd, bool probe)
{
    if (cmd.size() >= HYBM_SDMA_ORCH_CMD_BUF_SIZE) {
        BM_LOG_ERROR("kfc cmd too long, len: " << cmd.size());
        return BM_INVALID_PARAM;
    }
    aclrtFuncHandle fh = nullptr;
    auto ret = LoadKfcChannel(fh);
    if (ret != BM_OK) {
        return ret;
    }
    char buf[HYBM_SDMA_ORCH_CMD_BUF_SIZE] = {0};
    strncpy(buf, cmd.c_str(), sizeof(buf) - 1);
    aclrtLaunchKernelAttr attr{}; // 超时属性: 防设备 shell 挂死卡初始化
    attr.id = aclrtLaunchKernelAttrId::ACL_RT_LAUNCH_KERNEL_ATTR_TIMEOUT;
    attr.value.timeout = HYBM_SDMA_ORCH_LAUNCH_TIMEOUT_S;
    aclrtLaunchKernelCfg cfg{};
    cfg.attrs = &attr;
    cfg.numAttrs = 1;
    // 命令串作为 host args 透传 → 设备 OS 内 system() 执行
    ret = DlAclApi::AclrtLaunchKernelWithHostArgs(fh, 1, launchStream_, &cfg, buf, strlen(buf) + 1, nullptr, 0);
    if (ret != BM_OK) {
        BM_LOG_ERROR("kfc launch failed, ret: " << ret << " cmd: " << cmd);
        return ret;
    }
    ret = DlAclApi::AclrtSynchronizeStream(launchStream_);
    if (ret != BM_OK) { // sync 非 0 = 命令退出码非 0(或流错误)
        if (probe && ret == HYBM_SDMA_ORCH_KFC_CMD_EXIT_ERR) {
            BM_LOG_INFO("kfc probe miss (expected on first run), sync: " << ret << " cmd: " << cmd);
        } else {
            BM_LOG_ERROR("kfc cmd failed, sync: " << ret << " cmd: " << cmd);
        }
    }
    return ret;
}

Result SdmaOrchestrator::CopyToSearchPath(const std::string &deviceSoName, uint64_t soSize)
{
    // 复制源: 迁移落盘文件(裸 glob——不匹配时 cat 快速失败, 禁用可展开为空的 $() 形态防 stdin 挂死)
    const std::string src = std::string(HYBM_SDMA_ORCH_CUST_DEV_DIR) + "/" + HYBM_SDMA_ORCH_CUST_SO + ".*";
    const std::string dst = std::string(HYBM_SDMA_ORCH_DEV_SEARCH_DIR) + "/" + deviceSoName;
    const std::string copyCmd = "cat " + src + " > " + dst + " && chmod 644 " + dst;
    /* 完整性校验: 入口符号存在 + 精确尺寸一致(防截断/拼接; 设备侧工具极简——实测可用
     * grep/awk/顶层管道, wc/stat/md5sum/cmp 均缺失, 故用 ls -l 第 5 列取尺寸) */
    const std::string verifyCmd = "grep -q " + std::string(HYBM_SDMA_ORCH_FUNC_NAME) + " " + dst +
                                  " && ls -l " + dst + " | awk '{print $5}' | grep -qx " +
                                  std::to_string(soSize);
    for (uint32_t i = 0; i < HYBM_SDMA_ORCH_DEPLOY_RETRY; i++) { // 重试为迁移异步完成留裕量
        if (KfcExec(copyCmd) != BM_OK) {
            sleep(1);
            continue;
        }
        if (KfcExec(verifyCmd) == BM_OK) {
            BM_LOG_INFO("kernel deployed to CP1 search path, devId: " << devId_ << " dst: " << dst);
            return BM_OK;
        }
        sleep(1); // 校验失败等一拍再重试(重 cat 幂等)
    }
    BM_LOG_ERROR("deploy copy/verify failed after retries, devId: " << devId_ << " dst: " << dst
                 << " expectedSize: " << soSize);
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
    epochParam_.devId = devId_;
    epochParam_.sqId = sqId_;
    epochParam_.streamId = streamId_;
    epochParam_.reqBase = resBase + SMEM_SHM_SDMA_RS_REQ_RING;
    epochParam_.quietBase = resBase + SMEM_SHM_SDMA_RS_QUIET;
    const uint64_t wsBase = workspace_.load();
    /* 停止标志/overflow/结果区放工作区而非段内: 销毁序安全(段先销毁也不影响停止语义) */
    epochParam_.stopAddr = wsBase + HYBM_SDMA_ORCH_WS_STOP_OFF;
    epochParam_.overflowAddr = wsBase + HYBM_SDMA_ORCH_WS_OVF_OFF;
    epochParam_.resultAddr = wsBase + HYBM_SDMA_ORCH_WS_RESULT_OFF;
    epochParam_.symSize = symSize;
    epochParam_.physSize = physSize;
    stopFlag_ = false;
    try {
        supervisor_ = std::thread(&SdmaOrchestrator::SupervisorLoop, this);
    } catch (const std::system_error &e) { // 资源不足; 未置 configured_, 状态一致可重试
        BM_LOG_ERROR("start supervisor thread failed, errno: " << e.code().value() << " devId: " << devId_);
        return BM_ERROR;
    }
    configured_ = true; // 线程启动成功后置位
    BM_LOG_INFO("sdma orch configured, devId: " << devId_ << " reqBase: 0x" << std::hex << epochParam_.reqBase
              << " arrivalDepth: " << SMEM_SHM_SDMA_RS_ARR_SLOTS);
    return BM_OK;
}

Result SdmaOrchestrator::LaunchEpoch()
{
    if (funcHandle_ == nullptr || launchStream_ == nullptr) { // 直接检查将使用的句柄与流
        BM_LOG_ERROR("sdma orch launch precond failed, funcHandle null: " << (funcHandle_ == nullptr)
                                                                        << " stream null: "
                                                                        << (launchStream_ == nullptr));
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
    attr.value.timeout = HYBM_SDMA_ORCH_LAUNCH_TIMEOUT_S; // 25s, 须 < watchdog 30s
    aclrtLaunchKernelCfg cfg{};
    cfg.attrs = &attr;
    cfg.numAttrs = 1;
    // 提交 epoch kernel(异步不等待, 监督线程稍后 sync)
    ret = DlAclApi::AclrtLaunchKernelWithConfig(funcHandle_, HYBM_SDMA_ORCH_BLOCK_DIM, launchStream_, &cfg,
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
        return; // 放弃续命
    }
    uint32_t errors = 0; // 连续失败计数
    while (!stopFlag_.load()) {
        auto ret = LaunchEpoch();
        if (ret != BM_OK) {
            BM_LOG_ERROR("epoch launch failed, ret: " << ret << " sqId: " << sqId_);
            if (++errors > 16) { // 连续 16 次失败即放弃(数据面将停在背压/超时)
                BM_LOG_ERROR("epoch launch failed 16 times, supervisor exit, sqId: " << sqId_);
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        ret = DlAclApi::AclrtSynchronizeStream(launchStream_);
        if (ret != BM_OK) { // sync 异常(watchdog 杀/错误退出)
            HybmSdmaOrchResult r = {};
            (void)DlAclApi::AclrtMemcpy(&r, sizeof(r), reinterpret_cast<void *>(workspace_.load()), sizeof(r),
                                         ACL_MEMCPY_DEVICE_TO_HOST); // D2H 诊断
            if (stopFlag_.load()) { // 停止窗口内的失败: 段先于编排器销毁, kernel 读段被杀——预期自愈
                BM_LOG_INFO("epoch faulted during shutdown (expected, self-healed), ret: " << ret);
                continue; // 下一纪读到停止标志即净退
            }
            BM_LOG_ERROR("epoch sync failed, ret: " << ret << " mainRet: " << r.mainRet
                         << " exitReason: " << r.exitReason << " consumed: " << r.consumed);
            if (++errors > 16) {
                return;
            }
            continue; // 瞬态错误可恢复
        }
        errors = 0;
    }
}

void SdmaOrchestrator::StopSupervisor()
{
    if (!supervisor_.joinable()) {
        return;
    }
    stopFlag_ = true;
    if (configured_ && epochParam_.stopAddr != 0) {
        // 写非零值: kernel ≤100µs 轮询间隔内退出(memset 字节语义, 非零即停)
        (void)DlAclApi::AclrtMemset(reinterpret_cast<void *>(epochParam_.stopAddr), 8, 1, 8);
    }
    supervisor_.join(); // ≤ 一个轮询间隔 + sync 返回
}

void SdmaOrchestrator::Cleanup()
{
    StopSupervisor(); // 先停监督线程(其使用 launchStream_, 必须先于流销毁)
    configured_ = false; // 复位(允许后续重配置)
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
    /* binHandle_ 无对应卸载接口(DlAclApi 未封装), 随进程生命周期释放 */
    binHandle_ = nullptr;
    funcHandle_ = nullptr;
    custBinHandle_ = nullptr;
    refCount_ = 0;
    sqId_ = 0;
    streamId_ = 0;
    dieId_ = 0;
}
} // namespace mf
} // namespace ock
