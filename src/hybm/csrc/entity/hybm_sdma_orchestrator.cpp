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
#define _GNU_SOURCE // dladdr(GNU 扩展, 安装前缀自动发现用; 构建系统可能已定义)
#endif
#include "hybm_sdma_orchestrator.h" // 本类声明(工作区布局常量/单例接口/契约头)

#include <climits>   // PATH_MAX(realpath 结果缓冲尺寸)
#include <cctype>    // isalnum(deviceSoName 白名单校验)
#include <cctype>    // isalnum(deviceSoName 白名单校验)
#include <cerrno>    // errno(锁文件打开失败诊断)
#include <chrono>    // chrono(监督线程退避)
#include <cstdio>    // snprintf(临时 json 路径构造)
#include <cstdlib>   // realpath 等 POSIX 原型
#include <cstring>   // strncpy(KFC 命令缓冲拷贝)
#include <dlfcn.h>   // dladdr(本 so 路径定位)
#include <fcntl.h>   // open(部署互斥锁文件)
#include <fstream>   // ifstream(launch json 的 kernelSo 字段提取)
#include <iterator>  // istreambuf_iterator(json 整文件读入)
#include <string>    // std::string(json 路径承载)
#include <unistd.h>  // access/getpid/sleep/unlink

#include <sys/file.h> // flock(部署进程间互斥)
#include <sys/stat.h> // stat(host 侧 kernel so 尺寸校验)

#include "dl_rt_api.h"     // RtStreamGetSqid(rt 私有符号, 经 libruntime 解析)
#include "mf_env_define.h" // env::MF_SDMA_ORCH_JSON/MF_SDMA_ORCH_DISABLE 读取

namespace ock { // MemFabric 总命名空间
namespace mf {  // hybm 内存池引擎子命名空间
namespace {     // 匿名命名空间(编译单元内私有常量)
/* ACL_STREAM_DEVICE_USE_ONLY 的值(acl_rt.h:0x20; CP 流关键 flag, X3/A.3) */
constexpr uint32_t HYBM_ACL_STREAM_DEVICE_USE_ONLY = 0x20U;       /* CP 流标志: SQ 注册进 CP1 trs 表 */
constexpr uint32_t HYBM_SDMA_ORCH_BLOCK_DIM = 1;                    /* 单 block(kernel 状态机单线程语义) */
constexpr uint32_t HYBM_SDMA_ORCH_LAUNCH_TIMEOUT_S = 25;            /* launch attr 超时, < AICPU watchdog 30s */
constexpr char HYBM_SDMA_ORCH_FUNC_NAME[] = "HybmSdmaOrchEpoch";       /* V5 epoch 消费 kernel 入口符号名 */
constexpr char HYBM_SDMA_ORCH_DEFAULT_JSON[] = "libmf_sdma_orch_v5.json"; /* CWD 缺省 json(v5) */
constexpr char HYBM_SDMA_ORCH_CUST_JSON[] = "libmf_sdma_orch.json";  /* CUST 迁移 json 名(与 launch json 同目录) */
constexpr char HYBM_SDMA_ORCH_CUST_SO[] = "libmf_sdma_orch.so";      /* host 侧 kernel so 名(CUST 迁移源) */
/* CP1 搜索路径(实测: 每卡独立设备 OS 文件系统, 各卡路径同名, 须在本卡设备上部署) */
constexpr char HYBM_SDMA_ORCH_DEV_SEARCH_DIR[] = "/usr/lib64/aicpu_kernels/0/aicpu_kernels_device";
constexpr char HYBM_SDMA_ORCH_CUST_DEV_DIR[] = "/home/CustAiCpuUser/lib"; /* CUST 迁移设备侧落盘目录 */
constexpr char HYBM_SDMA_ORCH_DEPLOY_LOCK[] = "/tmp/.mf_sdma_orch_deploy.lock"; /* 部署互斥锁(host 侧) */
constexpr uint32_t HYBM_SDMA_ORCH_DEPLOY_RETRY = 5;                  /* 复制/校验重试次数(迁移异步完成裕量) */
constexpr uint32_t HYBM_SDMA_ORCH_CMD_BUF_SIZE = 512;                /* KFC 命令缓冲尺寸(命令为受控常量拼接) */
/* KFC 命令退出码非 0 时 stream sync 的回传码(实测): 用于区分"文件缺失"与"通道异常" */
constexpr int32_t HYBM_SDMA_ORCH_KFC_CMD_EXIT_ERR = 507018;
} // namespace

SdmaOrchestrator &SdmaOrchestrator::GetInstance() // 单例入口
{
    static SdmaOrchestrator instance;  // C++11 魔法静态(线程安全一次性构造, 进程唯一)
    return instance;                  // 返回单例引用
}

bool SdmaOrchestrator::Disabled() noexcept // 读显式禁用环境变量
{
    const auto &val = env::MF_SDMA_ORCH_DISABLE; // 读环境变量(进程启动时已缓存为 inline 变量)
    return val == "1" || val == "on";            // 仅 "1"/"on" 视为禁用, 其余(含未设置)一律启用
}

Result SdmaOrchestrator::Acquire(uint16_t devId) // 引用计数+1(首次全量初始化)
{
    std::lock_guard<std::mutex> guard(mutex_);  // 加锁: 计数变更与初始化/清理互斥
    if (refCount_ > 0) {                        // 已初始化: 仅做计数累加与设备绑定校验
        if (devId != devId_) {                  // 同进程第二个池请求不同设备
            /* CP 流/工作区绑定单设备, 同进程多池须同设备 */
            // 根因日志: 绑定设备/新请求设备
            BM_LOG_ERROR("sdma orchestrator bound devId: " << devId_ << " new devId: " << devId);
            return BM_INVALID_PARAM; // 透传失败(根因已记录)
        }
        refCount_++;  // 计数 +1, 复用既有资源
        return BM_OK; // 复用成功
    }
    auto ret = InitOnce(devId); // 首次进入: 全量初始化(CP 流→工作区→kernel)
    if (ret != BM_OK) {         // 任一步失败
        Cleanup(); /* 半初始化资源回收, 保证可重试 */
        return ret; // 透传失败(根因已记录)
    }
    refCount_ = 1; // 初始化成功, 计数置 1
    ready_ = true; // 三步初始化完成, 对外声明就绪
    // 就绪信息日志: 设备/流/工作区
    BM_LOG_INFO("310P sdma orchestrator ready: devId: " << devId_ << " sqId: " << sqId_
                                                   << " streamId: " << streamId_ << " dieId: " << dieId_
                                                   << " workspace: 0x" << std::hex << workspace_.load());
    return BM_OK; // Acquire 成功
}

void SdmaOrchestrator::Release() // 引用计数-1(归零清理)
{
    std::lock_guard<std::mutex> guard(mutex_); // 与 Acquire 同一把锁串行
    if (refCount_ == 0) {                      // 当前未持有
        return;                                // 幂等返回(UT 覆盖此语义)
    }
    refCount_--;               // 计数 -1
    if (refCount_ == 0) {      // 归零: 本 entity 是最后持有者
        Cleanup();             // 逆序清理全部资源(CP 流/工作区/句柄复位)
        ready_ = false;        // 撤下就绪标志
    }
}

Result SdmaOrchestrator::InitOnce(uint16_t devId) // 全量初始化(锁内调用)
{
    devId_ = devId;                                                // 绑定设备(后续 Acquire 一致性校验用)
    dieId_ = static_cast<uint32_t>(devId) % 2U; /* CP1 内 HAL devId = die 号(A.3) */
    auto ret = InitCpStream(devId); // 第 1 步: 创建 CP 流并取 sqId/streamId
    if (ret != BM_OK) {             // 第 1 步失败
        return ret;                 // 透传失败(根因已记录)
    }
    ret = AllocWorkspace(); // 第 2 步: 分配并清零 4KB 协同工作区
    if (ret != BM_OK) {     // 第 2 步失败
        return ret;         // 透传失败(根因已记录)
    }
    ret = LoadOrchestratorKernel(); // 第 3 步: 加载 AICPU 编排 kernel 二进制
    if (ret != BM_OK) {             // 第 3 步失败
        return ret;                 // 透传失败(根因已记录)
    }
    return BM_OK; // 三步全过, 初始化成功(端到端验证由 08 示例阶段①逐字校验承担)
}

Result SdmaOrchestrator::InitCpStream(uint16_t devId) // 第1步: CP 流
{
    // 创建 CP 流: DEVICE_USE_ONLY 使其 SQ 注册进 CP1 进程 trs 表(AICPU kernel 可查可投的前提, X3/A.3)
    auto ret = DlAclApi::AclrtCreateStreamWithConfig(&cpStream_, 0, HYBM_ACL_STREAM_DEVICE_USE_ONLY);
    if (ret != BM_OK) { // CP 流创建失败
        BM_LOG_ERROR("create CP stream failed, ret: " << ret << " devId: " << devId); // 根因日志: ret/devId
        return ret;                                                                   // 透传失败(根因已记录)
    }
    ret = DlAclApi::AclrtStreamGetId(cpStream_, &streamId_); // 取 CP 流 id(SQE 的 rt_stream_id 字段)
    if (ret != BM_OK) {                                       // 取流 id 失败
        BM_LOG_ERROR("get CP streamId failed, ret: " << ret << " devId: " << devId); // 根因日志: ret/devId
        return ret;                                                                  // 透传失败(根因已记录)
    }
    ret = DlRtApi::RtStreamGetSqid(cpStream_, &sqId_); // 取 CP 流 SQ id(rt 私有接口, kernel 内活查询用)
    if (ret != BM_OK) {                                // 取 SQ id 失败
        BM_LOG_ERROR("rtStreamGetSqid failed, ret: " << ret << " devId: " << devId); // 根因日志: ret/devId
        return ret;                                                                  // 透传失败(根因已记录)
    }
    ret = DlAclApi::AclrtCreateStream(&launchStream_); // 另建普通流作编排 kernel 的提交流
    if (ret != BM_OK) {                                // 提交流创建失败
        // 根因日志: ret/devId(此处不提前 return, 统一走函数末尾)
        BM_LOG_ERROR("create launch stream failed, ret: " << ret << " devId: " << devId);
    }
    return ret; // 末步结果即函数结果(失败路径根因已记录)
}

Result SdmaOrchestrator::AllocWorkspace() // 第2步: 工作区
{
    void *ws = nullptr; // 工作区设备内存指针(待分配)
    auto ret = DlAclApi::AclrtMalloc(&ws, HYBM_SDMA_ORCH_WS_SIZE, // 分配 4KB 工作区(策略 HUGE_FIRST)
                                     ACL_MEM_MALLOC_HUGE_FIRST);
    if (ret != BM_OK) { // 分配失败
        BM_LOG_ERROR("alloc workspace failed, ret: " << ret << " devId: " << devId_
                     << " size: " << HYBM_SDMA_ORCH_WS_SIZE); // 根因日志: ret/devId/size
        return ret;                                                                        // 透传失败(根因已记录)
    }
    ret = DlAclApi::AclrtMemset(ws, HYBM_SDMA_ORCH_WS_SIZE, 0, HYBM_SDMA_ORCH_WS_SIZE); // 清零(邮箱槽初值必须为 0)
    if (ret != BM_OK) { // 清零失败
        BM_LOG_ERROR("memset workspace failed, ret: " << ret << " devId: " << devId_); // 根因日志: ret/devId
        (void)DlAclApi::AclrtFree(ws);                                                // 回滚: 释放刚分配的工作区
        return ret;                                                                   // 透传失败(根因已记录)
    }
    workspace_ = reinterpret_cast<uint64_t>(ws); // 记录设备地址(meta 填充/param 装配共用)
    return BM_OK;                                // 工作区就绪
}

/* 安装前缀自动发现: 本 so 所在目录(如 <prefix>/lib64)的同级 hybm/aicpu_kernel 下
 * 找 launch json(安装布局: so→<prefix>/lib64, json→<prefix>/hybm/aicpu_kernel)。
 * 命中返回绝对路径, 未命中返回空串(调用方继续回退 CWD)。 */
static std::string FindInstalledJson()
{
    Dl_info info = {}; // 地址→so 映射信息
    if (dladdr(reinterpret_cast<void *>(&SdmaOrchestrator::GetInstance), &info) == 0 || info.dli_fname == nullptr) {
        return ""; // 定位本 so 失败(静态链接等场景)
    }
    std::string soDir(info.dli_fname); // 本 so 路径
    auto pos = soDir.find_last_of('/'); // 最后一个目录分隔
    if (pos == std::string::npos) {
        return ""; // 无目录分量
    }
    soDir.resize(pos); // 本 so 所在目录
    char resolved[PATH_MAX] = {0}; // realpath 结果缓冲
    // 候选: <soDir>/../hybm/aicpu_kernel/<json>(lib64 与 hybm/aicpu_kernel 同级于安装前缀)
    std::string cand = soDir + "/../hybm/aicpu_kernel/" + HYBM_SDMA_ORCH_DEFAULT_JSON;
    if (realpath(cand.c_str(), resolved) != nullptr && access(resolved, R_OK) == 0) {
        return std::string(resolved); // 命中安装前缀
    }
    return ""; // 未命中(继续回退)
}

/* 部署互斥锁 RAII: 同 host 多进程(双 rank/多池)并发部署时串行化, 防复制竞态
 * (双进程同时 cat 同一目标可能产生拼接/校验误判); 锁文件打开失败不致命(单进程无影响) */
class DeployFlockGuard {
public:
    /* O_NOFOLLOW: 拒绝跟随预置符号链接(防锁文件劫持); O_CLOEXEC: 子进程不继承;
     * 0600: 属主读写(收紧权限, 免其他用户篡改) */
    DeployFlockGuard()
        : fd_(open(HYBM_SDMA_ORCH_DEPLOY_LOCK, O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600)) // 打开/创建锁文件
    {
        if (fd_ < 0) { // 打开失败(极罕见: /tmp 不可写)
            BM_LOG_INFO("deploy lock open failed, errno: " << errno); // 信息级(尽力而为, 不阻断)
            return; // 无锁继续
        }
        (void)flock(fd_, LOCK_EX); // 排它锁(阻塞等对端部署完成, 部署为秒级)
    }
    ~DeployFlockGuard()
    {
        if (fd_ >= 0) { // 持锁才需释放
            (void)flock(fd_, LOCK_UN); // 解锁
            (void)close(fd_);          // 关闭描述符
        }
    }
private:
    int fd_; // 锁文件描述符(-1 = 未持锁)
};

/* 取路径目录部分(最后一个 '/' 之前; 无 '/' 返回空串) */
static std::string DirOf(const std::string &path)
{
    auto pos = path.find_last_of('/'); // 最后一个目录分隔
    return (pos == std::string::npos) ? std::string() : path.substr(0, pos); // 前缀即目录
}

/* 从 launch json 提取 kernelSo 字段值(CP1 搜索的设备侧 so 名); 文件不可读或字段
 * 缺失时回退文件名推导(v4.json → v4.so, 与 CMake 产物命名约定一致) */
static std::string ExtractDeviceSoName(const std::string &jsonPath)
{
    std::ifstream f(jsonPath); // 打开 launch json(小文件)
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()); // 整读
    auto pos = content.find("\"kernelSo\""); // 定位 kernelSo 字段
    if (pos != std::string::npos) {           // 字段存在
        auto colon = content.find(':', pos);   // 字段值前的冒号
        auto s = (colon == std::string::npos) ? colon : content.find('"', colon); // 值起始引号
        auto e = (s == std::string::npos) ? s : content.find('"', s + 1);         // 值结束引号
        if (e != std::string::npos && e > s) { // 引号对完整
            return content.substr(s + 1, e - s - 1); // 字段值(设备侧 so 名)
        }
    }
    auto slash = jsonPath.find_last_of('/'); // 回退: 文件名推导
    std::string base = (slash == std::string::npos) ? jsonPath : jsonPath.substr(slash + 1); // 去目录
    if (base.size() > 5 && base.compare(base.size() - 5, 5, ".json") == 0) { // 以 .json 结尾
        base.resize(base.size() - 5); // 去扩展名
    }
    return base + ".so"; // v6 → v6.so(命名约定)
}

/* deviceSoName 白名单校验(仅 [A-Za-z0-9._-], 长度 1~64): 名字来自 launch json 的
 * kernelSo 字段, 拼入 KFC 命令串经设备 OS system() 执行——不做校验存在 shell
 * 注入风险(分号/管道/反引号); json 由本仓构建产出, 校验是纵深防御 */
static bool IsValidDeviceSoName(const std::string &name)
{
    if (name.empty() || name.size() > 64) { // 长度界
        return false;                       // 拒绝
    }
    for (char c : name) { // 逐字符白名单
        if (std::isalnum(static_cast<unsigned char>(c)) == 0 && c != '.' && c != '_' && c != '-') {
            return false; // 非白名单字符
        }
    }
    return true; // 合法
}

Result SdmaOrchestrator::LoadOrchestratorKernel() // 第3步: kernel 加载(设备侧部署探测 + 自动部署)
{
    std::string jsonPath = env::MF_SDMA_ORCH_JSON;  // ① 环境变量显式指定的 launch json 路径
    if (jsonPath.empty()) {                            // 未设置
        jsonPath = FindInstalledJson();                // ② 安装前缀自动发现(本 so 同级的 hybm/aicpu_kernel)
    }
    if (jsonPath.empty()) {                            // 未发现
        jsonPath = HYBM_SDMA_ORCH_DEFAULT_JSON;        // ③ CWD 缺省名 libmf_sdma_orch_v6.json
    }
    char resolved[PATH_MAX] = {0}; // realpath 结果缓冲(解析后的绝对路径)
    if (realpath(jsonPath.c_str(), resolved) == nullptr || access(resolved, R_OK) != 0) { // 路径不存在或不可读
        // 根因日志: json 路径 + 设置提示
        BM_LOG_ERROR("sdma orchestrator json not accessible: " << jsonPath
                                                                 << " (set MF_SDMA_ORCH_JSON?)");
        return BM_FILE_NOT_ACCESS; // 显式报错(kernel 未部署即不可用)
    }
    /* 注意: mode=0 加载不校验设备侧文件(dlopen 惰性到 launch), 须以 KFC 探测文件存在性为闸门 */
    const std::string devSoName = ExtractDeviceSoName(resolved); // 设备侧目标名(json kernelSo 字段)
    if (!IsValidDeviceSoName(devSoName)) {                       // 白名单校验(防 shell 注入)
        // 根因日志: 非法名字
        BM_LOG_ERROR("invalid device so name (whitelist [A-Za-z0-9._-]): " << devSoName
                     << " json: " << resolved);
        return BM_INVALID_PARAM; // 拒绝部署
    }
    const std::string dst = std::string(HYBM_SDMA_ORCH_DEV_SEARCH_DIR) + "/" + devSoName;
    const auto gate = KfcExec("test -f " + dst, true); // 设备侧文件存在性探测(文件缺失为首轮预期, 降级 INFO)
    if (gate != HYBM_SDMA_ORCH_KFC_CMD_EXIT_ERR && gate != BM_OK) { // 非"文件缺失"语义 = 通道异常
        // KFC 不可用(锁定环境): 按预部署假定直接加载, 文件若真缺失由数据面 wait 显式报错
        BM_LOG_ERROR("kfc channel unavailable (ret: " << gate << "), assume pre-deployed, dst: " << dst);
    } else if (gate == BM_OK) { // 设备侧已部署(预部署/同卡先启进程)
        auto ret = TryLoadKernel(resolved); // 直接加载
        if (ret != BM_OK) { // 已部署仍加载失败(json/环境问题)
            BM_LOG_ERROR("kernel load failed though deployed, ret: " << ret << " json: " << resolved); // 根因
        }
        return ret; // 透传(根因已记录)
    } else { // gate == 507018: 设备侧文件缺失 → 自动部署(迁移+复制+校验)后加载
        BM_LOG_INFO("kernel not on device, auto deploy, devId: " << devId_ << " dst: " << dst);
        return DeployOrchestratorKernel(resolved, dst);
    }
    auto ret = TryLoadKernel(resolved); // 通道异常分支的兜底加载(预部署假定)
    if (ret != BM_OK) {                 // 加载失败(json/环境问题, 与部署无关)
        BM_LOG_ERROR("kernel load failed, ret: " << ret << " json: " << resolved); // 根因: ret/json
    }
    return ret; // 透传(根因已记录)
}

Result SdmaOrchestrator::TryLoadKernel(const std::string &jsonPath) // 单次加载尝试(日志由调用方分层记录)
{
    aclrtBinaryLoadOption loadOption{}; // 二进制加载选项(仅一项: cpuKernelMode)
    loadOption.type = aclrtBinaryLoadOptionType::ACL_RT_BINARY_LOAD_OPT_CPU_KERNEL_MODE; // 选项类型 = CPU kernel 模式
    loadOption.value.cpuKernelMode = 0; /* cpuMode=0: 走 CP1 搜索路径(A.4-3) */
    aclrtBinaryLoadOptions loadOptions{}; // 选项数组容器
    loadOptions.options = &loadOption;    // 挂接单项选项
    loadOptions.numOpt = 1;               // 选项个数
    auto ret = DlAclApi::AclrtBinaryLoadFromFile(jsonPath.c_str(), &loadOptions, &binHandle_); // 按 json 加载
    if (ret != BM_OK) { // 加载失败(设备侧文件缺失或 json 问题)
        return ret;     // 透传(首次=INFO 转部署, 末次=ERROR, 均由调用方记录)
    }
    return DlAclApi::AclrtBinaryGetFunction(binHandle_, HYBM_SDMA_ORCH_FUNC_NAME, &funcHandle_); // 取入口句柄
}

Result SdmaOrchestrator::DeployOrchestratorKernel(const std::string &launchJsonPath, const std::string &dst) // 自动部署
{
    DeployFlockGuard lock; // host 侧进程间互斥(双 rank 并发部署防复制竞态)
    if (KfcExec("test -f " + dst, true) == BM_OK) { // 锁内复查: 对端进程可能刚部署完(缺失为预期, 降级 INFO)
        auto ret = TryLoadKernel(launchJsonPath); // 复用对端部署成果, 直接加载
        if (ret == BM_OK) { // 加载成功
            BM_LOG_INFO("kernel deployed by peer process, devId: " << devId_); // 免重复迁移
            return BM_OK; // 部署复用
        }
        BM_LOG_ERROR("kernel load failed though peer deployed, ret: " << ret << " json: " << launchJsonPath); // 根因
        return ret; // 透传失败(根因已记录)
    }
    const std::string dir = DirOf(launchJsonPath); // 三件套同目录(launch json 旁)
    uint64_t soSize = 0;                           // host 侧 kernel so 尺寸(完整性校验用)
    auto ret = MigrateViaCustChannel(dir, soSize); // CUST 通道迁移落盘
    if (ret != BM_OK) {                             // 迁移失败
        return ret;                                 // 透传失败(根因已记录)
    }
    const std::string devSo = dst.substr(dst.find_last_of('/') + 1); // 设备侧目标名(目标路径尾段)
    ret = CopyToSearchPath(devSo, soSize);          // 设备内复制 + 完整性校验
    if (ret != BM_OK) {                             // 复制/校验失败
        return ret;                                 // 透传失败(根因已记录)
    }
    ret = TryLoadKernel(launchJsonPath);            // 部署后加载
    if (ret != BM_OK) {                             // 仍失败(部署无法修复的 json/环境问题)
        BM_LOG_ERROR("kernel load still fails after deploy, ret: " << ret << " devId: " << devId_
                     << " json: " << launchJsonPath); // 根因日志: ret/devId/json
    }
    return ret; // 透传(根因已记录)
}

Result SdmaOrchestrator::MigrateViaCustChannel(const std::string &dir, uint64_t &soSize) // CUST 迁移落盘
{
    const std::string hostSo = dir + "/" + HYBM_SDMA_ORCH_CUST_SO; // host 侧 kernel so(与 launch json 同目录)
    struct stat st = {}; // 文件状态(取尺寸)
    if (stat(hostSo.c_str(), &st) != 0) { // so 缺失(安装布局不完整)
        BM_LOG_ERROR("cust kernel so missing, path: " << hostSo); // 根因日志: 缺失路径
        return BM_FILE_NOT_ACCESS; // 部署前提缺失
    }
    soSize = static_cast<uint64_t>(st.st_size); // 记录尺寸(设备侧完整性校验用)
    const std::string custJson = dir + "/" + HYBM_SDMA_ORCH_CUST_JSON; // CUST 迁移 json 路径
    char resolved[PATH_MAX] = {0}; // realpath 结果缓冲
    if (realpath(custJson.c_str(), resolved) == nullptr || access(resolved, R_OK) != 0) { // json 缺失
        BM_LOG_ERROR("cust json not accessible, path: " << custJson); // 根因日志: 缺失路径
        return BM_FILE_NOT_ACCESS; // 部署前提缺失
    }
    /* 迁移前清残留(防历史/对端残留与本轮迁移文件拼接; flock 已保证无并发迁移) */
    const std::string cleanCmd = "rm -f " + std::string(HYBM_SDMA_ORCH_CUST_DEV_DIR) + "/" +
                                 HYBM_SDMA_ORCH_CUST_SO + ".*"; // 清迁移目录残留命令
    if (KfcExec(cleanCmd) != BM_OK) { // 清理失败(目录异常)
        BM_LOG_ERROR("deploy clean residue failed, devId: " << devId_); // 根因: devId
        return BM_ERROR; // 部署失败
    }
    aclrtBinaryLoadOption loadOption{}; // mode=1: H2D 迁移通道(CUSTAICPUKernel 专用)
    loadOption.type = aclrtBinaryLoadOptionType::ACL_RT_BINARY_LOAD_OPT_CPU_KERNEL_MODE; // 选项类型 = CPU kernel 模式
    loadOption.value.cpuKernelMode = 1; // mode=1 本身不落盘, GetFunction 才触发设备写
    aclrtBinaryLoadOptions loadOptions{}; // 选项数组容器
    loadOptions.options = &loadOption;    // 挂接单项选项
    loadOptions.numOpt = 1;               // 选项个数
    aclrtBinHandle bh = nullptr; // 迁移句柄(成功后转成员保活)
    auto ret = DlAclApi::AclrtBinaryLoadFromFile(resolved, &loadOptions, &bh); // 按迁移 json 加载
    if (ret != BM_OK) { // 迁移加载失败
        BM_LOG_ERROR("cust migrate load failed, ret: " << ret << " json: " << resolved); // 根因: ret/json
        return ret; // 透传失败(根因已记录)
    }
    aclrtFuncHandle fh = nullptr; // 入口句柄(触发实际落盘)
    ret = DlAclApi::AclrtBinaryGetFunction(bh, HYBM_SDMA_ORCH_FUNC_NAME, &fh); // 取入口(触发设备写文件)
    if (ret != BM_OK) { // 返回值不可靠: 实测 CUST 模式下正确入口名也可能 107000 且文件照样落盘
        // 不作失败处理, 以复制+完整性校验为最终判据(落盘与否由 cat/verify 兜底)
        BM_LOG_INFO("cust migrate getfunc ret: " << ret << " (tolerated, verify later)");
    }
    custBinHandle_ = bh; // 保活: 持句柄至进程退出(迁移进程退出才触发清理链, 本进程即用户进程)
    BM_LOG_INFO("cust migrate done, devId: " << devId_ << " soSize: " << soSize); // 迁移完成
    return BM_OK; // 迁移就位(以复制+校验为最终判据)
}

/* 加载 KFC 通道入口: 写临时 json → mode=0 加载 → 取 op "K" 句柄 → 删临时文件
 * (json 须带 .json 扩展名——无扩展名实测加载 107000; mkstemp 模板须以 X 结尾
 * 与扩展名冲突, 故用 pid+计数排它创建; CWD 可能只读, 用 /tmp) */
Result SdmaOrchestrator::LoadKfcChannel(aclrtFuncHandle &fh)
{
    if (kfcFuncHandle_ != nullptr) { // 已缓存(首次装载后复用, 免每次部署重装)
        fh = kfcFuncHandle_;         // 直接复用
        return BM_OK;                // 缓存命中
    }
    fh = nullptr; // 出参先置空(失败时调用方见空句柄)
    char path[64] = {0}; // 临时 json 路径(唯一, 带扩展名)
    int fd = -1;         // 文件描述符
    for (uint32_t i = 0; i < 16 && fd < 0; i++) { // 计数上限 16(同名残留/并发冲突防御)
        (void)snprintf(path, sizeof(path), "/tmp/mf_kfc_%d_%u.json", static_cast<int>(getpid()), i);
        fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600); // 排它创建(防并发/残留复用)
    }
    if (fd < 0) { // 创建失败
        BM_LOG_ERROR("kfc tmpfile failed, errno: " << errno); // 根因日志: errno
        return BM_FILE_NOT_ACCESS; // 部署通道不可用
    }
    // 最小 KFC json(functionName=system, kernelSo 指向不存在文件触发 dlsym 回退, 同 tools/mini_kfc)
    static constexpr char KFC_JSON_CONTENT[] =
        "{\"K\":{\"opInfo\":{\"opKernelLib\":\"KFCKernel\",\"kernelSo\":\"kfc_min.so\","
        "\"functionName\":\"system\"}}}";
    const ssize_t wantLen = static_cast<ssize_t>(sizeof(KFC_JSON_CONTENT) - 1); // 期望写入长度
    if (write(fd, KFC_JSON_CONTENT, sizeof(KFC_JSON_CONTENT) - 1) != wantLen) {
        // 根因日志: 写入长度不符(磁盘满等)
        BM_LOG_ERROR("kfc tmpfile write failed, errno: " << errno << " json: " << path);
        (void)close(fd);   // 关闭描述符
        (void)unlink(path); // 清理半成品
        return BM_FILE_NOT_ACCESS; // 部署通道不可用
    }
    (void)close(fd); // 关闭(后续按路径加载)
    aclrtBinaryLoadOption loadOption{}; // mode=0: 不迁移, 直接按 json 路由
    loadOption.type = aclrtBinaryLoadOptionType::ACL_RT_BINARY_LOAD_OPT_CPU_KERNEL_MODE; // 选项类型 = CPU kernel 模式
    loadOption.value.cpuKernelMode = 0; // mode=0
    aclrtBinaryLoadOptions loadOptions{}; // 选项数组容器
    loadOptions.options = &loadOption;    // 挂接单项选项
    loadOptions.numOpt = 1;               // 选项个数
    aclrtBinHandle bh = nullptr; // KFC 二进制句柄
    auto ret = DlAclApi::AclrtBinaryLoadFromFile(path, &loadOptions, &bh); // 加载 KFC 路由
    (void)unlink(path); // json 已消费即删(不留临时文件)
    if (ret != BM_OK) { // 加载失败(KFC 通道不可用)
        BM_LOG_ERROR("kfc load failed, ret: " << ret << " json: " << path); // 根因: ret/json
        return ret; // 透传失败(根因已记录)
    }
    ret = DlAclApi::AclrtBinaryGetFunction(bh, "K", &fh); // 取 op "K" 的函数句柄
    if (ret == BM_OK) { // 装载成功
        kfcFuncHandle_ = fh; // 缓存句柄(部署链全程复用, 免每次部署重装)
    } else { // 取句柄失败
        BM_LOG_ERROR("kfc getfunc failed, ret: " << ret); // 根因日志: ret
    }
    return ret; // 通道装载结果(失败路径根因已记录)
}

Result SdmaOrchestrator::KfcExec(const std::string &cmd, bool probe) // KFC 通道执行一条设备 OS 命令
{
    if (cmd.size() >= HYBM_SDMA_ORCH_CMD_BUF_SIZE) { // 命令超长(受控常量拼接, 理论不达)
        BM_LOG_ERROR("kfc cmd too long, len: " << cmd.size()); // 根因日志: 长度
        return BM_INVALID_PARAM; // 拒绝执行
    }
    aclrtFuncHandle fh = nullptr; // KFC 入口句柄
    auto ret = LoadKfcChannel(fh); // 装载通道(临时 json→加载→句柄)
    if (ret != BM_OK) {            // 通道装载失败
        return ret;                // 透传失败(根因已记录)
    }
    char buf[HYBM_SDMA_ORCH_CMD_BUF_SIZE] = {0}; // 命令缓冲(host args 透传)
    strncpy(buf, cmd.c_str(), sizeof(buf) - 1); // 拷贝命令(末字节保持 0 终止)
    aclrtLaunchKernelAttr attr{}; // launch 属性(仅一项: 超时, 防设备 shell 挂死卡初始化)
    attr.id = aclrtLaunchKernelAttrId::ACL_RT_LAUNCH_KERNEL_ATTR_TIMEOUT; // 属性类型 = 超时
    attr.value.timeout = HYBM_SDMA_ORCH_LAUNCH_TIMEOUT_S; // 25s(与编排 launch 一致)
    aclrtLaunchKernelCfg cfg{}; // launch 配置(挂属性数组)
    cfg.attrs = &attr;          // 属性数组首址
    cfg.numAttrs = 1;           // 属性个数
    // 提交 KFC 命令(命令串作为 host args 透传 → 设备 OS 内 system() 执行)
    ret = DlAclApi::AclrtLaunchKernelWithHostArgs(fh, 1, launchStream_, &cfg, buf, strlen(buf) + 1, nullptr, 0);
    if (ret != BM_OK) { // 提交失败
        BM_LOG_ERROR("kfc launch failed, ret: " << ret << " cmd: " << cmd); // 根因: ret/cmd
        return ret; // 透传失败(根因已记录)
    }
    ret = DlAclApi::AclrtSynchronizeStream(launchStream_); // 同步取回命令结果
    if (ret != BM_OK) { // sync 非 0 = 命令退出码非 0(或流错误)
        if (probe && ret == HYBM_SDMA_ORCH_KFC_CMD_EXIT_ERR) { // 探测命令遇"文件缺失"退出码 = 首轮预期情况
            BM_LOG_INFO("kfc probe miss (expected on first run), sync: " << ret << " cmd: " << cmd); // 信息级免误导
        } else {
            BM_LOG_ERROR("kfc cmd failed, sync: " << ret << " cmd: " << cmd); // 根因: sync/cmd
        }
    }
    return ret; // 命令结果(失败路径根因已记录)
}

Result SdmaOrchestrator::CopyToSearchPath(const std::string &deviceSoName, uint64_t soSize) // 复制+校验
{
    // 复制源: 迁移落盘文件(裸 glob——不匹配时 cat 快速失败, 禁用可展开为空的 $() 形态防 stdin 挂死;
    // 残留已在迁移前清除, 此处 glob 只命中本轮迁移的单个文件)
    const std::string src = std::string(HYBM_SDMA_ORCH_CUST_DEV_DIR) + "/" + HYBM_SDMA_ORCH_CUST_SO + ".*";
    const std::string dst = std::string(HYBM_SDMA_ORCH_DEV_SEARCH_DIR) + "/" + deviceSoName; // CP1 搜索路径目标
    const std::string copyCmd = "cat " + src + " > " + dst + " && chmod 644 " + dst; // 复制+授权
    /* 完整性校验: 入口符号存在 + 精确尺寸一致(防截断/拼接; 设备侧工具极简——实测可用
     * grep/awk/顶层管道, wc/stat/md5sum/cmp 均缺失, 故用 ls -l 第 5 列取尺寸) */
    const std::string verifyCmd = "grep -q " + std::string(HYBM_SDMA_ORCH_FUNC_NAME) + " " + dst +
                                  " && ls -l " + dst + " | awk '{print $5}' | grep -qx " +
                                  std::to_string(soSize); // 符号+尺寸双校验
    for (uint32_t i = 0; i < HYBM_SDMA_ORCH_DEPLOY_RETRY; i++) { // 重试(为迁移异步完成留裕量)
        if (KfcExec(copyCmd) != BM_OK) { // 复制失败(迁移文件可能尚未就位)
            sleep(1); // 等迁移完成
            continue; // 重试
        }
        if (KfcExec(verifyCmd) == BM_OK) { // 校验通过
            BM_LOG_INFO("kernel deployed to CP1 search path, devId: " << devId_ << " dst: " << dst);
            return BM_OK; // 部署就位
        }
        sleep(1); // 校验失败等一拍再重试(重 cat 幂等)
    }
    BM_LOG_ERROR("deploy copy/verify failed after retries, devId: " << devId_ << " dst: " << dst
                 << " expectedSize: " << soSize); // 根因: devId/dst/期望尺寸
    return BM_ERROR; // 部署失败
}

Result SdmaOrchestrator::ConfigureOrch(uint64_t ownSegGva, uint64_t physSize, uint64_t symSize) // 保留区初始化
{
    std::lock_guard<std::mutex> guard(mutex_); // 与 Acquire/Release 串行
    if (configured_) {                         // 二次配置(第二池)
        // V5 约束: 双环一次绑定, 每进程单 SDMA 池
        BM_LOG_ERROR("sdma orch already configured, second pool unsupported in V5, devId: " << devId_);
        return BM_INVALID_PARAM; // 显式拒绝(根因已记录)
    }
    if (symSize == 0 || (symSize & (symSize - 1)) != 0) { // 2 的幂校验(对端段基址掩码算术依赖)
        // 根因日志: symSize
        BM_LOG_ERROR("symSize not power of two, symSize: " << symSize);
        return BM_INVALID_PARAM; // 显式拒绝
    }
    const uint64_t resBase = ownSegGva + physSize - SMEM_SHM_SDMA_RESERVED; // 保留区基址
    // 保留区整体清零(双环/游标/戳/停止标志从零起步)
    auto ret = DlAclApi::AclrtMemset(reinterpret_cast<void *>(resBase), SMEM_SHM_SDMA_RESERVED, 0,
                                     SMEM_SHM_SDMA_RESERVED);
    if (ret != BM_OK) { // 清零失败(段映射异常)
        BM_LOG_ERROR("memset orch reserved failed, ret: " << ret << " resBase: 0x" << std::hex << resBase
                                                          << " size: " << SMEM_SHM_SDMA_RESERVED);
        return ret; // 透传失败(根因已记录)
    }
    epochParam_ = HybmSdmaOrchParam{};                    // 参数清零后逐字段装配
    epochParam_.devId = devId_;                           // 设备号
    epochParam_.sqId = sqId_;                             // CP 流 SQ id
    epochParam_.streamId = streamId_;                     // CP 流 id
    epochParam_.reqBase = resBase + SMEM_SHM_SDMA_RS_REQ_RING;     // 请求环基址
    epochParam_.quietBase = resBase + SMEM_SHM_SDMA_RS_QUIET;      // 完成戳基址
    const uint64_t wsBase = workspace_.load(); // 工作区基址快照(锁内)
    epochParam_.stopAddr = wsBase + HYBM_SDMA_ORCH_WS_STOP_OFF; // 停止标志(工作区自有: 销毁序安全)
    epochParam_.overflowAddr = wsBase + HYBM_SDMA_ORCH_WS_OVF_OFF;   // overflow(工作区)
    epochParam_.resultAddr = wsBase + HYBM_SDMA_ORCH_WS_RESULT_OFF; // 结果区(工作区)
    epochParam_.symSize = symSize;                        // 对称段大小(掩码算术)
    epochParam_.physSize = physSize;                      // 段物理大小
    stopFlag_ = false;                                    // 停止标志复位
    try {                                                 // thread 构造可能抛 system_error(资源不足)
        supervisor_ = std::thread(&SdmaOrchestrator::SupervisorLoop, this); // 监督线程启动
    } catch (const std::system_error &e) {                // 启动失败
        // 根因日志: errno/devId(未置 configured_, 状态一致可重试)
        BM_LOG_ERROR("start supervisor thread failed, errno: " << e.code().value() << " devId: " << devId_);
        return BM_ERROR;                                  // 透传失败
    }
    configured_ = true;                                   // 绑定完成(线程启动成功后置位)
    BM_LOG_INFO("sdma orch configured, devId: " << devId_ << " reqBase: 0x" << std::hex << epochParam_.reqBase
              << " arrivalDepth: " << SMEM_SHM_SDMA_RS_ARR_SLOTS);
    return BM_OK;                                         // 配置完成
}

Result SdmaOrchestrator::LaunchEpoch() // 提交一纪 epoch kernel(监督线程内)
{
    if (funcHandle_ == nullptr || launchStream_ == nullptr) { // 前置: kernel 句柄与提交流任一缺失
        /* 守卫直接检查将使用的句柄与流本身(不依赖 ready_ 对外语义), 缺一即拒 */
        // 根因日志: 前置条件状态
        BM_LOG_ERROR("sdma orch launch precond failed, funcHandle null: " << (funcHandle_ == nullptr)
                                                                        << " stream null: "
                                                                        << (launchStream_ == nullptr));
        return BM_NOT_INITIALIZED; // 未初始化即拒绝提交
    }
    aclrtArgsHandle argsHandle = nullptr; // kernel 参数包句柄
    auto ret = DlAclApi::AclrtKernelArgsInit(funcHandle_, &argsHandle); // 参数包初始化
    if (ret != BM_OK) { // 参数包初始化失败
        BM_LOG_ERROR("KernelArgsInit failed, ret: " << ret << " sqId: " << sqId_); // 根因日志: ret/sqId
        return ret;                                                               // 透传失败(根因已记录)
    }
    HybmSdmaOrchParam p = epochParam_; /* Append 接口非 const, 本地拷贝(72B) */
    aclrtParamHandle paramHandle = nullptr; // 参数块句柄(Append 输出, 本函数不消费)
    ret = DlAclApi::AclrtKernelArgsAppend(argsHandle, &p, sizeof(p), &paramHandle); // 追加 72B 入参块
    if (ret == BM_OK) {                     // 追加成功后
        ret = DlAclApi::AclrtKernelArgsFinalize(argsHandle); // 封包(此后不可再 Append)
    }
    if (ret != BM_OK) { // 追加或封包失败
        BM_LOG_ERROR("KernelArgs Append/Finalize failed, ret: " << ret << " sqId: " << sqId_); // 根因日志: ret/sqId
        return ret;                                                                          // 透传失败(根因已记录)
    }
    aclrtLaunchKernelAttr attr{}; // launch 属性(仅一项: 超时)
    attr.id = aclrtLaunchKernelAttrId::ACL_RT_LAUNCH_KERNEL_ATTR_TIMEOUT; // 属性类型 = 超时
    attr.value.timeout = HYBM_SDMA_ORCH_LAUNCH_TIMEOUT_S; // 25s, 须 < AICPU watchdog 30s
    aclrtLaunchKernelCfg cfg{}; // launch 配置(挂属性数组)
    cfg.attrs = &attr;          // 属性数组首址
    cfg.numAttrs = 1;           // 属性个数
    // 提交 epoch kernel(blockDim=1, 带超时属性, 异步不等待; 监督线程稍后 sync)
    ret = DlAclApi::AclrtLaunchKernelWithConfig(funcHandle_, HYBM_SDMA_ORCH_BLOCK_DIM, launchStream_, &cfg,
                                                argsHandle, nullptr);
    if (ret != BM_OK) { // 提交失败
        BM_LOG_ERROR("LaunchKernelWithConfig(epoch) failed, ret: " << ret << " sqId: " << sqId_); // 根因
    }
    return ret; // 提交结果(失败路径根因已记录)
}

void SdmaOrchestrator::SupervisorLoop() // 监督线程主循环(epoch 续命直至停止)
{
    /* ACL 上下文为线程局部: 子线程须先绑定设备(否则 launch 报 107002 上下文无效) */
    if (DlAclApi::AclrtSetDevice(devId_) != BM_OK) { // 绑定失败
        BM_LOG_ERROR("supervisor set device failed, devId: " << devId_); // 根因日志: devId
        return; // 放弃续命
    }
    uint32_t errors = 0; // 连续失败计数
    while (!stopFlag_.load()) { // 停止标志轮询
        auto ret = LaunchEpoch(); // 提交一纪
        if (ret != BM_OK) {       // 提交失败
            BM_LOG_ERROR("epoch launch failed, ret: " << ret << " sqId: " << sqId_); // 根因
            if (++errors > 16) {  // 连续 16 次失败
                BM_LOG_ERROR("epoch launch failed 16 times, supervisor exit, sqId: " << sqId_); // 根因
                return;           // 放弃续命(数据面将停在背压/超时)
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100)); // 100ms 退避
            continue;             // 重试
        }
        ret = DlAclApi::AclrtSynchronizeStream(launchStream_); // 等 epoch 结束(时限/停止/错误)
        if (ret != BM_OK) {       // sync 异常(watchdog 杀/错误退出)
            HybmSdmaOrchResult r = {}; // 结果快照
            (void)DlAclApi::AclrtMemcpy(&r, sizeof(r), reinterpret_cast<void *>(workspace_.load()), sizeof(r),
                                         ACL_MEMCPY_DEVICE_TO_HOST); // D2H 诊断
            if (stopFlag_.load()) { // 停止窗口内的失败(段先于编排器销毁, kernel 读段被杀——预期自愈)
                BM_LOG_INFO("epoch faulted during shutdown (expected, self-healed), ret: " << ret); // 信息级
                continue;         // 下一纪读到停止标志即净退
            }
            // 根因日志: ret/mainRet/exitReason/consumed
            BM_LOG_ERROR("epoch sync failed, ret: " << ret << " mainRet: " << r.mainRet
                         << " exitReason: " << r.exitReason << " consumed: " << r.consumed);
            if (++errors > 16) {  // 连续 16 次失败
                return;           // 放弃续命
            }
            continue;             // 续命重试(瞬态错误可恢复)
        }
        errors = 0;               // 成功一纪, 失败计数清零
    }
}

void SdmaOrchestrator::StopSupervisor() // 停止监督线程(写设备停止标志 + join)
{
    if (!supervisor_.joinable()) { // 线程未启动
        return;                    // 无需停止
    }
    stopFlag_ = true;              // host 侧停止标志
    if (configured_ && epochParam_.stopAddr != 0) { // 设备侧停止标志已装配
        // 写非零值(epoch kernel ≤100µs 轮询间隔内退出; memset 字节语义, 非零即停)
        (void)DlAclApi::AclrtMemset(reinterpret_cast<void *>(epochParam_.stopAddr), 8, 1, 8);
    }
    supervisor_.join();            // 等线程退出(≤ 一个轮询间隔 + sync 返回)
}

void SdmaOrchestrator::Cleanup() // 逆序清理(锁内, 归零时)
{
    StopSupervisor(); // 先停监督线程(其使用 launchStream_, 必须先于流销毁)
    configured_ = false; // 配置状态复位(允许后续重配置)
    if (cpStream_ != nullptr) { // CP 流存在
        (void)DlAclApi::AclrtDestroyStream(cpStream_); // 销毁 CP 流(SQ 随流注销)
        cpStream_ = nullptr; // 置空防重复销毁
    }
    if (launchStream_ != nullptr) { // 提交流存在
        (void)DlAclApi::AclrtDestroyStream(launchStream_); // 销毁提交流
        launchStream_ = nullptr; // 置空防重复销毁
    }
    if (workspace_ != 0) { // 工作区已分配
        (void)DlAclApi::AclrtFree(reinterpret_cast<void *>(workspace_.load())); // 释放 4KB 工作区
        workspace_ = 0;   // 清零标记未分配
    }
    /* binHandle_ 无对应卸载接口(DlAclApi 未封装), 随进程生命周期释放 */
    binHandle_ = nullptr;  // 仅清指针(资源随进程释放)
    funcHandle_ = nullptr; // 仅清指针(同上)
    custBinHandle_ = nullptr; // 仅清指针(CUST 迁移句柄, 保活语义至进程退出)
    refCount_ = 0;         // 计数归零
    sqId_ = 0;             // SQ id 复位
    streamId_ = 0;         // 流 id 复位
    dieId_ = 0;            // die 号复位
}
} // namespace mf
} // namespace ock
