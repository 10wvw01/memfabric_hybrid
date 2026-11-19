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
constexpr uint32_t HYBM_SDMA_ORCH_BLOCK_DIM = 1;       /* 单 block(kernel 状态机单线程语义) */
constexpr char HYBM_SDMA_ORCH_DEFAULT_JSON[] = "libmf_sdma_orch_launch.json";
constexpr char HYBM_SDMA_ORCH_KERNEL_SO[] = "libmf_sdma_orch.so"; /* host 侧 kernel so 名 */
constexpr char HYBM_SDMA_ORCH_KERNEL_JSON[] = "libmf_sdma_orch.json"; /* kernel 配套 op json(产物三件套之一) */
/* pypto 机制: CP1 启动时预加载的 CANN extend 库, 提供在 CP1 进程内加载任意 so 的 loader.
 * 不依赖设备搜索路径文件与 KFC shell(2026-11-18 起 KFC shell 平台级失效). */
constexpr char HYBM_SDMA_ORCH_EXTEND_SO[] = "libaicpu_extend_kernels.so";
constexpr char HYBM_SDMA_ORCH_EXTEND_INIT_FUNC[] = "DynTileFwkKernelServerInit"; /* loader 入口 */
constexpr char HYBM_SDMA_ORCH_EXTEND_EXEC_FUNC[] = "DynTileFwkKernelServer";     /* 函数表分发入口 */
constexpr uint32_t HYBM_SDMA_ORCH_EXTEND_KERNEL_TYPE = 5;      /* AICPU_KFC */
constexpr uint32_t HYBM_SDMA_ORCH_EXTEND_LAUNCH_FLAG = 0x100U; /* RT_KERNEL_USE_SPECIAL_TIMEOUT */
constexpr uint16_t HYBM_SDMA_ORCH_EXTEND_TIMEOUT_NEVER = 0xFFFFU; /* args.timeout: 0xFFFF=永不超时 */
constexpr uint32_t HYBM_SDMA_ORCH_SUPERVISOR_MAX_ERR = 16; /* 监督线程连续失败上限(放弃后数据面停在背压) */

/* ---- extend lib loader 的 ABI 镜像 ----
 * loader 在 CP1 内读 DeviceArgs.aicpuSoBin/aicpuSoLen/deviceId(偏移 96/104/112)取 so 字节;
 * DeviceKernelArgs.cfgdata(偏移 40)承载本端参数指针. 布局与 pypto device_runner 一致. */
struct ExtendDeviceKernelArgs {
    int64_t *ctrlFlowCache{nullptr};
    int64_t *inputs{nullptr};
    int64_t *outputs{nullptr};
    int64_t *workspace{nullptr};
    int64_t *tilingdata{nullptr};
    int64_t *cfgdata{nullptr};
    int64_t *commContexts{nullptr};
    void *costmodeldata{nullptr};
    void *aicoreModel{nullptr};
    uint64_t taskWastTime{0};
    uint64_t maxDynamicAssembleOutcastMem{0};
    uint64_t maxDynamicCellMatchTableMem{0};
    uint8_t machineConfig{0};
    uint8_t pad[3]{0};
    uint8_t subMachineConfig[4]{0}; /* ToSubMachineConfig{ProfConfig{uint32}} */
    uint32_t runMode{0};
    uint32_t p1{0};
    uint64_t globalRound{0};
    uint64_t ctrlBlockNum{0};
};
struct ExtendDeviceArgs {
    uint32_t nrAic{0};
    uint32_t nrAiv{0};
    uint32_t nrAicpu{0};
    uint32_t nrValidAic{0};
    uint64_t opaque{0};
    uint64_t devQueueAddr{0};
    uint64_t sharedBuffer{0};
    uint64_t coreRegAddr{0};
    uint64_t corePmuRegAddr{0};
    uint64_t corePmuAddr{0};
    uint64_t pmuEventAddr{0};
    uint64_t taskType : 4;
    uint64_t machineConfig : 8;
    uint64_t taskId : 52;
    uint64_t taskData{0};
    uint64_t taskWastTime{0};
    uint64_t aicpuSoBin{0};
    uint64_t aicpuSoLen{0};
    uint64_t deviceId{0};
    uint64_t runtimeDataRingBufferAddr{0};
    uint64_t dynamicCellMatchAddr{0};
    uint64_t dynamicCellMatchCapacity{0};
    uint32_t hostPid{0};
    uint32_t scheCpuNum{0};
    uint32_t enableCtrl : 2;
    uint32_t validGetPgMask : 30;
    uint64_t aicpuPerfAddr{0};
    uint64_t devDfxArgAddr{0};
    uint32_t maxAicpuNum{0};
    uint32_t launchSchedAicpuNum{7};
    bool launchSchedSameCluster{true};
    bool enableVFFusion{false};
    bool enableEslModel{false};
    bool hasAicpuTask{false};
    bool commByReg{false};
    uint32_t archInfo{2002}; /* DAV_2002 */
    uint32_t profValue{0};
};
struct ExtendArgs {
    ExtendDeviceKernelArgs kArgs;
    char kernelName[32];
    char soName[32];
    char opName[32];
};
struct ExtendRtArgs {
    void *args;
    void *hostInputInfoPtr;
    void *kernelOffsetInfoPtr;
    uint32_t argsSize;
    uint16_t hostInputInfoNum;
    uint16_t kernelOffsetInfoNum;
    uint32_t soNameAddrOffset;
    uint32_t kernelNameAddrOffset;
    uint8_t isNoNeedH2DCopy;
    uint16_t timeout;
    uint8_t reserved;
};
static_assert(offsetof(ExtendDeviceArgs, aicpuSoBin) == 96, "aicpuSoBin offset");
static_assert(offsetof(ExtendDeviceArgs, aicpuSoLen) == 104, "aicpuSoLen offset");
static_assert(offsetof(ExtendDeviceArgs, deviceId) == 112, "deviceId offset");
static_assert(offsetof(ExtendDeviceKernelArgs, cfgdata) == 40, "cfgdata offset");
static_assert(offsetof(ExtendArgs, kernelName) == 128, "kernelName offset");
static_assert(offsetof(ExtendArgs, soName) == 160, "soName offset");
static_assert(sizeof(ExtendArgs) == 224, "ExtendArgs size");

using RtAicpuLaunchFn = int (*)(uint32_t, const char *, uint32_t, const ExtendRtArgs *, void *, const void *,
                                uint32_t);

/* halMemCtl 私有接口(从 ascend_hal_define.h 复制最小集, 用于查 AICore 控制寄存器基址)。
 * 注意: 返回的 ptr 是设备侧 AICPU 进程的 VA, host 不能 dereference, 只能 H2D 传给
 * AICPU kernel(volatile 读写目标核 SPR)。 */
constexpr uint32_t HYBM_ADDR_MAP_TYPE_REG_AIC_CTRL = 2;
struct HybmAddrMapInPara { uint32_t addr_type; uint32_t devid; };
struct HybmAddrMapOutPara { uint64_t ptr; uint64_t len; };
typedef int (*halMemCtl_t)(int type, void *in, size_t inSize, void *out, size_t *outSizeRet);
constexpr char HYBM_HAL_LIB_PATH[] = "/usr/local/Ascend/driver/lib64/driver/libascend_hal.so";

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
    QueryAicoreRegs(devId); /* 寄存器同步查询: 失败不报错, 只降级 commByReg_=false 走 GM */
    return BM_OK;
}

/* 寄存器同步唤醒(commByReg)查询: 借鉴 pypto commByReg 配方, host 经 halMemCtl 查
 * AICore 控制寄存器基址(需驱动侧寄存器映射改动生效), 查到后 regBase 数组(10 核 × 2MB
 * stride)H2D 到设备内存, AICPU kernel 空闲时经此 volatile 读 COND(SPR 0x4C8)。
 * 门控(默认关): MF_SDMA_ORCH_COMM_BY_REG=1 才尝试开启; MF_SDMA_ORCH_FORCE_GM=1 强制
 * 走 GM(诊断开关, 优先级高)。任何失败路径只置 commByReg_=false 降级 GM(零功能损失)。 */
void SdmaOrchestrator::QueryAicoreRegs(uint16_t devId)
{
    const auto &forceGm = env::MF_SDMA_ORCH_FORCE_GM;
    if (!forceGm.empty()) { /* 诊断开关: 强制 GM, 优先级最高 */
        BM_LOG_WARN("commByReg: forced GM by MF_SDMA_ORCH_FORCE_GM (diagnostic override)");
        commByReg_ = false;
        return;
    }
    const auto &enable = env::MF_SDMA_ORCH_COMM_BY_REG;
    if (enable != "1" && enable != "on") { /* 默认关: 显式 opt-in 才尝试 */
        commByReg_ = false;
        return;
    }
    void *h = dlopen(HYBM_HAL_LIB_PATH, RTLD_NOW);
    if (h == nullptr) {
        BM_LOG_WARN("commByReg: dlopen libascend_hal.so failed, fallback to GM: " << dlerror());
        commByReg_ = false;
        return;
    }
    auto halMemCtl = (halMemCtl_t)dlsym(h, "halMemCtl");
    if (halMemCtl == nullptr) {
        BM_LOG_WARN("commByReg: dlsym halMemCtl failed, fallback to GM: " << dlerror());
        dlclose(h);
        commByReg_ = false;
        return;
    }
    HybmAddrMapInPara in = {HYBM_ADDR_MAP_TYPE_REG_AIC_CTRL, static_cast<uint32_t>(devId)};
    HybmAddrMapOutPara out = {0, 0};
    int ret = halMemCtl(0, &in, sizeof(in), &out, nullptr);
    if (ret != 0 || out.ptr == 0) {
        BM_LOG_WARN("commByReg: halMemCtl(AIC_CTRL) failed ret:" << ret
                    << ", fallback to GM (driver reg-map change not installed?)");
        dlclose(h);
        commByReg_ = false;
        return;
    }
    BM_LOG_INFO("commByReg: halMemCtl OK, base=0x" << std::hex << out.ptr << " len="
                << (out.len / (1024 * 1024)) << "MB");
    dlclose(h);
    /* H2D regBase 数组(10 core × 2MB stride): AICPU 侧经此 volatile 读各核 COND */
    uint64_t regBaseHost[REG_BASE_CORES] = {0};
    for (uint32_t i = 0; i < REG_BASE_CORES; i++) {
        regBaseHost[i] = out.ptr + static_cast<uint64_t>(i) * REG_CORE_STRIDE;
    }
    void *devRegBase = nullptr;
    auto aclRet = DlAclApi::AclrtMalloc(&devRegBase, sizeof(regBaseHost), ACL_MEM_MALLOC_HUGE_FIRST);
    if (aclRet != BM_OK) {
        BM_LOG_WARN("commByReg: aclrtMalloc regBase failed, fallback to GM: " << aclRet);
        commByReg_ = false;
        return;
    }
    aclRet = DlAclApi::AclrtMemcpy(devRegBase, sizeof(regBaseHost), regBaseHost, sizeof(regBaseHost),
                                   ACL_MEMCPY_HOST_TO_DEVICE);
    if (aclRet != BM_OK) {
        BM_LOG_WARN("commByReg: aclrtMemcpy regBase failed, fallback to GM: " << aclRet);
        (void)DlAclApi::AclrtFree(devRegBase);
        commByReg_ = false;
        return;
    }
    regBaseDevAddr_ = reinterpret_cast<uint64_t>(devRegBase);
    commByReg_ = true;
    BM_LOG_INFO("commByReg: enabled, regBase devAddr=0x" << std::hex << regBaseDevAddr_
                << " cores=" << std::dec << REG_BASE_CORES);
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

/* 取路径目录部分(最后一个 '/' 之前; 无 '/' 返回空串) */
static std::string DirOf(const std::string &path)
{
    auto pos = path.find_last_of('/');
    return (pos == std::string::npos) ? std::string() : path.substr(0, pos);
}

/* 产物目录完整性: so + kernel op json + launch json 三件套齐全且可读 */
static bool HasOrchestratorArtifacts(const std::filesystem::path &directory)
{
    const char *names[] = {HYBM_SDMA_ORCH_KERNEL_SO, HYBM_SDMA_ORCH_KERNEL_JSON, HYBM_SDMA_ORCH_DEFAULT_JSON};
    for (const auto *name : names) {
        const auto file = directory / name;
        std::error_code error;
        if (!std::filesystem::is_regular_file(file, error) || access(file.c_str(), R_OK) != 0) {
            return false;
        }
    }
    return true;
}

/* 在 MF_SDMA_ORCH_DIR 下递归找完整的产物目录, 必须恰好命中一套(0 套或多套都判失败, 避免选错版本) */
static Result FindOrchestratorJson(const std::string &directory, std::string &jsonPath)
{
    std::error_code error;
    const auto root = std::filesystem::canonical(directory, error);
    if (error || !std::filesystem::is_directory(root, error)) {
        BM_LOG_ERROR("invalid MF_SDMA_ORCH_DIR: " << directory << " error: " << error.message());
        return BM_FILE_NOT_ACCESS;
    }
    std::set<std::string> matches;
    // 子目录符号链接不递归跟随, 避免循环和同一安装目录被重复发现。
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
        BM_LOG_ERROR("MF_SDMA_ORCH_DIR requires one complete artifact directory, root: " << directory
                     << " matches: " << matches.size() << " required: " << HYBM_SDMA_ORCH_KERNEL_SO << ", "
                     << HYBM_SDMA_ORCH_KERNEL_JSON << ", " << HYBM_SDMA_ORCH_DEFAULT_JSON);
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
    std::string candidate = FindInstalledJson();   // ① 安装前缀自动发现
    if (candidate.empty()) {
        candidate = HYBM_SDMA_ORCH_DEFAULT_JSON;   // ② CWD 缺省名
    }
    char resolved[PATH_MAX] = {0};
    if (realpath(candidate.c_str(), resolved) == nullptr || access(resolved, R_OK) != 0) {
        BM_LOG_ERROR("sdma orchestrator json not accessible: " << candidate
                     << " (set MF_SDMA_ORCH_DIR)");
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
    /* json 仅用于定位 kernel 目录(产物三件套同目录); 部署走 CP1 extend lib loader, 不消费 json 内容 */
    return DeployOrchestratorKernel(DirOf(jsonPath));
}

Result SdmaOrchestrator::DeployOrchestratorKernel(const std::string &kernelDir)
{
    const std::string hostSo = kernelDir + "/" + HYBM_SDMA_ORCH_KERNEL_SO;
    std::ifstream input(hostSo, std::ios::binary);
    if (!input) {
        BM_LOG_ERROR("sdma orch kernel so not accessible, path: " << hostSo);
        return BM_FILE_NOT_ACCESS;
    }
    std::vector<char> soBytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (soBytes.empty()) {
        BM_LOG_ERROR("sdma orch kernel so empty, path: " << hostSo);
        return BM_FILE_NOT_ACCESS;
    }
    /* 1) so 字节 H2D: loader 经 DeviceArgs.aicpuSoBin 读取, 在 CP1 进程内 memfd 加载 */
    if (DlAclApi::AclrtMalloc(&devSo_, soBytes.size(), ACL_MEM_MALLOC_HUGE_FIRST) != BM_OK ||
        DlAclApi::AclrtMemcpy(devSo_, soBytes.size(), soBytes.data(), soBytes.size(),
                              ACL_MEMCPY_HOST_TO_DEVICE) != BM_OK) {
        BM_LOG_ERROR("upload kernel so failed, devId: " << devId_ << " size: " << soBytes.size());
        return BM_ERROR;
    }
    /* 2) DeviceArgs H2D: loader 只读 aicpuSoBin/aicpuSoLen/deviceId(偏移 96/104/112) */
    ExtendDeviceArgs da{};
    da.deviceId = devId_;
    da.archInfo = 2002; /* DAV_2002 */
    da.nrAic = 10;
    da.nrValidAic = 8;
    da.nrAicpu = 5;
    da.maxAicpuNum = 15;
    da.scheCpuNum = 3;
    da.hostPid = static_cast<uint32_t>(getpid());
    da.aicpuSoBin = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(devSo_));
    da.aicpuSoLen = soBytes.size();
    if (DlAclApi::AclrtMalloc(&devArgs_, sizeof(da), ACL_MEM_MALLOC_HUGE_FIRST) != BM_OK ||
        DlAclApi::AclrtMemcpy(devArgs_, sizeof(da), &da, sizeof(da), ACL_MEMCPY_HOST_TO_DEVICE) != BM_OK) {
        BM_LOG_ERROR("upload DeviceArgs failed, devId: " << devId_);
        return BM_ERROR;
    }
    /* 3) rtAicpuKernelLaunchExWithArgs(libruntime.so): 与 pypto 相同的 KFC 下发接口。
     * 优先从已加载符号解析(进程通常已由 ascendcl 带入 libruntime, 不依赖 LD_LIBRARY_PATH
     * 里有 CANN lib64——新机器上更稳); 都取不到再显式 dlopen */
    if (rtLaunch_ == nullptr) {
        rtLaunch_ = dlsym(RTLD_DEFAULT, "rtAicpuKernelLaunchExWithArgs");
    }
    if (rtLaunch_ == nullptr) {
        runtimeLib_ = dlopen("libruntime.so", RTLD_NOW | RTLD_GLOBAL);
        if (runtimeLib_ == nullptr) {
            BM_LOG_ERROR("dlopen libruntime.so failed: " << dlerror());
            return BM_ERROR;
        }
        rtLaunch_ = dlsym(runtimeLib_, "rtAicpuKernelLaunchExWithArgs");
        if (rtLaunch_ == nullptr) {
            BM_LOG_ERROR("dlsym rtAicpuKernelLaunchExWithArgs failed: " << dlerror());
            return BM_ERROR;
        }
    }
    /* 4) 启动 CP1 内 loader: extend lib 在 CP1 进程内 memfd 加载本 so 并填函数表 */
    ExtendArgs initArgs{};
    initArgs.kArgs.cfgdata = reinterpret_cast<int64_t *>(devArgs_);
    (void)snprintf(initArgs.kernelName, sizeof(initArgs.kernelName), "%s", HYBM_SDMA_ORCH_EXTEND_INIT_FUNC);
    (void)snprintf(initArgs.soName, sizeof(initArgs.soName), "%s", HYBM_SDMA_ORCH_EXTEND_SO);
    ExtendRtArgs rtInit{};
    rtInit.args = &initArgs;
    rtInit.argsSize = sizeof(initArgs);
    rtInit.kernelNameAddrOffset = offsetof(ExtendArgs, kernelName);
    rtInit.soNameAddrOffset = offsetof(ExtendArgs, soName);
    auto launch = reinterpret_cast<RtAicpuLaunchFn>(rtLaunch_);
    int ret = launch(HYBM_SDMA_ORCH_EXTEND_KERNEL_TYPE, "AST_DYN_AICPU", 1, &rtInit, nullptr, launchStream_, 0);
    if (ret != 0) {
        BM_LOG_ERROR("extend loader launch failed, ret: " << ret << " devId: " << devId_);
        return BM_ERROR;
    }
    ret = DlAclApi::AclrtSynchronizeStream(launchStream_);
    if (ret != BM_OK) {
        BM_LOG_ERROR("extend loader sync failed, ret: " << ret << " devId: " << devId_);
        return BM_ERROR;
    }
    /* 5) epoch 参数设备缓冲(每轮 H2D, kernel 只读) */
    if (DlAclApi::AclrtMalloc(&devParam_, sizeof(HybmSdmaOrchParam), ACL_MEM_MALLOC_HUGE_FIRST) != BM_OK) {
        BM_LOG_ERROR("alloc epoch param buffer failed, devId: " << devId_);
        return BM_ERROR;
    }
    extendLoaderReady_ = true;
    BM_LOG_INFO("kernel deployed to CP1 via extend loader, devId: " << devId_ << " soSize: " << soBytes.size());
    return BM_OK;
}

Result SdmaOrchestrator::ConfigureOrch(uint64_t ownSegGva, uint64_t physSize, uint64_t symSize, uint32_t ownRank,
                                       uint64_t poolBaseGva)
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
    if (ownRank >= SMEM_SHM_SDMA_MAX_RANKS) { // 到达环按 MAX_RANKS-1 设计, 超限无环可路由
        BM_LOG_ERROR("orch rank exceeds layout limit, rank: " << ownRank
                                                              << " maxRanks: " << SMEM_SHM_SDMA_MAX_RANKS);
        return BM_INVALID_PARAM;
    }
    if (poolBaseGva == 0 || ownSegGva < poolBaseGva) { // 池窗口基址(对端 rank 路由原点)
        BM_LOG_ERROR("orch pool base invalid, poolBaseGva: 0x" << std::hex << poolBaseGva << std::dec
                                                               << " ownSegGva: 0x" << std::hex << ownSegGva);
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
    epochParam_.resBase = resBase; /* 本端保留区基址(v29: 三请求环/完成戳/暂存均由此定位) */
    const uint64_t wsBase = workspace_.load();
    /* 停止标志/overflow/结果区放工作区而非段内: 销毁序安全(段先销毁也不影响停止语义) */
    epochParam_.stopAddr = wsBase + HYBM_SDMA_ORCH_WS_STOP_OFF;
    epochParam_.overflowAddr = wsBase + HYBM_SDMA_ORCH_WS_OVF_OFF;
    epochParam_.resultAddr = wsBase + HYBM_SDMA_ORCH_WS_RESULT_OFF;
    epochParam_.symSize = symSize;
    epochParam_.physSize = physSize;
    epochParam_.rank = ownRank;             /* v29: 请求环/到达环路由(本端视角) */
    epochParam_.rsv = 0;                   /* 填充显式置 0 */
    epochParam_.poolBaseGva = poolBaseGva; /* v29: dstSeg → 目的 rank 的换算原点(路由复核) */
    /* 寄存器同步唤醒透传: commByReg 标志 + regBase 设备地址(默认 0 = GM 轮询) */
    epochParam_.commByReg = commByReg_ ? 1 : 0;
    epochParam_.regBaseAddr = regBaseDevAddr_; /* rsv2 已随整体清零置 0 */
    if (commByReg_) {
        /* 保留区标志槽写 1(上面 memset 已清零; AICore submit 路径读此槽决定是否 set_cond 唤醒) */
        uint8_t regFlag = 1;
        ret = DlAclApi::AclrtMemcpy(reinterpret_cast<void *>(resBase + SMEM_SHM_SDMA_RS_COMM_BY_REG),
                                    sizeof(regFlag), &regFlag, sizeof(regFlag), ACL_MEMCPY_HOST_TO_DEVICE);
        if (ret != BM_OK) { /* 标志槽写失败不致命: AICore 不 set_cond, AICPU 走兜底强制扫描 */
            BM_LOG_WARN("commByReg: write reserved flag failed, ret: " << ret);
        } else {
            BM_LOG_INFO("commByReg: reserved flag written (offset 0x" << std::hex
                        << SMEM_SHM_SDMA_RS_COMM_BY_REG << ")");
        }
    }
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
    epochLaunched_ = true;
    // 首轮 kernel 写入协议标识和 RUNNING 状态后才允许建池成功。
    // 最多轮询 200 次、每次间隔 10 ms，及时拒绝旧 kernel、异常退出或未启动的情况。
    HybmSdmaOrchResult capability{};
    for (uint32_t attempt = 0; attempt < 200; ++attempt) {
        ret = DlAclApi::AclrtMemcpy(&capability, sizeof(capability),
            reinterpret_cast<void *>(epochParam_.resultAddr), sizeof(capability), ACL_MEMCPY_DEVICE_TO_HOST);
        if (ret != BM_OK || capability.magic != 0) { break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (ret != BM_OK || capability.magic != HYBM_SDMA_ORCH_RESULT_MAGIC ||
        capability.exitReason != HYBM_SDMA_ORCH_EXIT_RUNNING) {
        BM_LOG_ERROR("SDMA kernel protocol handshake failed, magic: " << capability.magic
            << " expected: " << HYBM_SDMA_ORCH_RESULT_MAGIC << " ret: " << ret << " devId: " << devId_);
        StopEpoch();
        return BM_ERROR;
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
              << " resBase: 0x" << std::hex << epochParam_.resBase << " rank: " << std::dec
              << epochParam_.rank << " poolBaseGva: 0x" << std::hex << epochParam_.poolBaseGva
              << " peerRings: " << SMEM_SHM_SDMA_MAX_PEERS
              << " arrivalDepth: " << SMEM_SHM_SDMA_RS_PEER_SLOTS);
    return BM_OK;
}

Result SdmaOrchestrator::LaunchEpoch(void *stream)
{
    if (!extendLoaderReady_ || rtLaunch_ == nullptr || devParam_ == nullptr || stream == nullptr) {
        BM_LOG_ERROR("sdma orch launch precond failed, loaderReady: " << extendLoaderReady_
                   << " rtLaunch null: " << (rtLaunch_ == nullptr)
                   << " devParam null: " << (devParam_ == nullptr)
                   << " stream null: " << (stream == nullptr));
        return BM_NOT_INITIALIZED;
    }
    /* 每轮参数 H2D(loader 通道的 DeviceKernelArgs.cfgdata 指向它, kernel 侧只读) */
    auto ret = DlAclApi::AclrtMemcpy(devParam_, sizeof(HybmSdmaOrchParam), &epochParam_,
                                     sizeof(HybmSdmaOrchParam), ACL_MEMCPY_HOST_TO_DEVICE);
    if (ret != BM_OK) {
        BM_LOG_ERROR("upload epoch param failed, ret: " << ret << " sqId: " << sqId_);
        return ret;
    }
    /* KFC 方式下发 extend lib 的 DynTileFwkKernelServer: CP1 内按函数表分发到本 so 的
     * 同名导出(经偏移 40 的 cfgdata 取参数指针后转调 HybmSdmaOrchEpoch) */
    ExtendArgs execArgs{};
    execArgs.kArgs.cfgdata = reinterpret_cast<int64_t *>(devParam_);
    (void)snprintf(execArgs.kernelName, sizeof(execArgs.kernelName), "%s", HYBM_SDMA_ORCH_EXTEND_EXEC_FUNC);
    (void)snprintf(execArgs.soName, sizeof(execArgs.soName), "%s", HYBM_SDMA_ORCH_EXTEND_SO);
    ExtendRtArgs rtExec{};
    rtExec.args = &execArgs;
    rtExec.argsSize = sizeof(execArgs);
    rtExec.kernelNameAddrOffset = offsetof(ExtendArgs, kernelName);
    rtExec.soNameAddrOffset = offsetof(ExtendArgs, soName);
    /* 永不超时: timeout=0xFFFF + flag 含 RT_KERNEL_USE_SPECIAL_TIMEOUT(0x100) ——与
     * pypto 相同的约定; 每轮时长由 kernel 读报时计数自限, 不经 runtime 超时 */
    rtExec.timeout = HYBM_SDMA_ORCH_EXTEND_TIMEOUT_NEVER;
    auto launch = reinterpret_cast<RtAicpuLaunchFn>(rtLaunch_);
    const int retLaunch = launch(HYBM_SDMA_ORCH_EXTEND_KERNEL_TYPE, "AST_DYN_AICPU",
                                 HYBM_SDMA_ORCH_BLOCK_DIM, &rtExec, nullptr, stream,
                                 HYBM_SDMA_ORCH_EXTEND_LAUNCH_FLAG);
    if (retLaunch != 0) {
        BM_LOG_ERROR("epoch launch via extend loader failed, ret: " << retLaunch << " sqId: " << sqId_);
        return BM_ERROR;
    }
    return BM_OK;
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
    if (launchStream_ != nullptr) {
        (void)DlAclApi::AclrtDestroyStream(launchStream_);
        launchStream_ = nullptr;
    }
    if (workspace_ != 0) {
        (void)DlAclApi::AclrtFree(reinterpret_cast<void *>(workspace_.load()));
        workspace_ = 0;
    }
    /* extend loader 缓冲释放: loader 侧 so 已在 CP1 进程内 memfd 加载, 此处仅释放 host
     * 侧 H2D 副本(设备侧加载物随 CP1 进程生命周期, 无卸载接口——与 pypto 相同取舍) */
    if (devParam_ != nullptr) {
        (void)DlAclApi::AclrtFree(devParam_);
        devParam_ = nullptr;
    }
    if (devArgs_ != nullptr) {
        (void)DlAclApi::AclrtFree(devArgs_);
        devArgs_ = nullptr;
    }
    if (devSo_ != nullptr) {
        (void)DlAclApi::AclrtFree(devSo_);
        devSo_ = nullptr;
    }
    extendLoaderReady_ = false;
    rtLaunch_ = nullptr;  // libruntime.so 的符号缓存随句柄失效重取
    if (runtimeLib_ != nullptr) {
        (void)dlclose(runtimeLib_);
        runtimeLib_ = nullptr;
    }
    if (regBaseDevAddr_ != 0) { /* 寄存器同步 regBase 数组释放 */
        (void)DlAclApi::AclrtFree(reinterpret_cast<void *>(regBaseDevAddr_));
        regBaseDevAddr_ = 0;
    }
    commByReg_ = false;
    refCount_ = 0;
    sqId_ = 0;
    streamId_ = 0;
    dieId_ = 0;
}
} // namespace mf
} // namespace ock
