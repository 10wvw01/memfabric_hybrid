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

/* hybm_overlap_prep.cpp — 310P AICore×AICPU 协同模式传输准备(实现)
 * 机制依据: 310p-aicore-aicpu-开发进展.md 附录 A(SQE 配方/CP 流/工作区/冒烟)
 * 与 P0 三实验的实测定论(档案见 310p-aicore-aicpu-开发进展.md)。
 */
#include "hybm_overlap_prep.h" // 本类声明(工作区布局常量/单例接口/契约头)

#include <climits>   // PATH_MAX(realpath 结果缓冲尺寸)
#include <cstdlib>   // realpath 等 POSIX 原型
#include <cstring>   // 内存原语(结果读回缓冲初始化等)
#include <string>    // std::string(json 路径承载)
#include <unistd.h>  // access 路径可达性检查

#include "dl_rt_api.h"     // RtStreamGetSqid(rt 私有符号, 经 libruntime 解析)
#include "mf_env_define.h" // env::MF_310P_OVERLAP_JSON/MF_310P_SHM_OVERLAP 读取

namespace ock { // MemFabric 总命名空间
namespace mf {  // hybm 内存池引擎子命名空间
namespace {     // 匿名命名空间(编译单元内私有常量)
/* ACL_STREAM_DEVICE_USE_ONLY 的值(acl_rt.h:0x20; CP 流关键 flag, X3/A.3) */
constexpr uint32_t HYBM_ACL_STREAM_DEVICE_USE_ONLY = 0x20U;       /* CP 流标志: SQ 注册进 CP1 trs 表 */
constexpr uint32_t HYBM_OVERLAP_BLOCK_DIM = 1;                    /* 单 block(kernel 状态机单线程语义) */
constexpr uint32_t HYBM_OVERLAP_LAUNCH_TIMEOUT_S = 25;            /* launch attr 超时, < AICPU watchdog 30s */
constexpr uint32_t HYBM_OVERLAP_SMOKE_WORDS = 256;                /* 冒烟数据量(256 字 = 1KB) */
constexpr char HYBM_OVERLAP_FUNC_NAME[] = "HybmOverlapRun";       /* 编排 kernel 入口符号名 */
constexpr char HYBM_OVERLAP_DEFAULT_JSON[] = "libmf_overlap_v2.json"; /* CWD 缺省 json(v2 契约) */
constexpr uint64_t HYBM_OVERLAP_RESULT_MAGIC = 0xA1C0DE01ULL;         /* kernel 存活魔数(契约头同值) */
} // namespace

HybmOverlapPrep &HybmOverlapPrep::GetInstance() // 单例入口
{
    static HybmOverlapPrep instance;  // C++11 魔法静态(线程安全一次性构造, 进程唯一)
    return instance;                  // 返回单例引用
}

bool HybmOverlapPrep::SwitchOn() noexcept // 读开关环境变量
{
    const auto &val = env::MF_310P_SHM_OVERLAP;  // 读环境变量(进程启动时已缓存为 inline 变量)
    return val == "1" || val == "on";            // 仅 "1"/"on" 视为开, 其余(含未设置)一律为关
}

Result HybmOverlapPrep::Acquire(uint16_t devId) // 引用计数+1(首次全量初始化)
{
    std::lock_guard<std::mutex> guard(mutex_);  // 加锁: 计数变更与初始化/清理互斥
    if (refCount_ > 0) {                        // 已初始化: 仅做计数累加与设备绑定校验
        if (devId != devId_) {                  // 同进程第二个池请求不同设备
            /* CP 流/工作区绑定单设备, 同进程多池须同设备 */
            // 根因日志: 绑定设备/新请求设备
            BM_LOG_ERROR("overlap prep bound devId: " << devId_ << " new devId: " << devId);
            return BM_INVALID_PARAM; // 透传失败(根因已记录)
        }
        refCount_++;  // 计数 +1, 复用既有资源
        return BM_OK; // 复用成功
    }
    auto ret = InitOnce(devId); // 首次进入: 全量初始化(CP 流→工作区→kernel→冒烟)
    if (ret != BM_OK) {         // 任一步失败
        Cleanup(); /* 半初始化资源回收, 保证可重试 */
        return ret; // 透传失败(根因已记录)
    }
    refCount_ = 1; // 初始化成功, 计数置 1
    ready_ = true; // 冒烟通过, 对外声明就绪
    // 就绪信息日志: 设备/流/工作区
    BM_LOG_INFO("310P overlap prep ready: devId: " << devId_ << " sqId: " << sqId_
                                                   << " streamId: " << streamId_ << " dieId: " << dieId_
                                                   << " workspace: 0x" << std::hex << workspace_);
    return BM_OK; // Acquire 成功
}

void HybmOverlapPrep::Release() // 引用计数-1(归零清理)
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

Result HybmOverlapPrep::InitOnce(uint16_t devId) // 全量初始化(锁内调用)
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
    ret = SmokeTest(); // 第 4 步: 冒烟自证(不过即整体失败, 不静默)
    if (ret != BM_OK) { // 第 4 步失败
        return ret;     // 透传失败(根因已记录)
    }
    return BM_OK; // 四步全过, 初始化成功
}

Result HybmOverlapPrep::InitCpStream(uint16_t devId) // 第1步: CP 流
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

Result HybmOverlapPrep::AllocWorkspace() // 第2步: 工作区
{
    void *ws = nullptr; // 工作区设备内存指针(待分配)
    auto ret = DlAclApi::AclrtMalloc(&ws, HYBM_OVERLAP_WS_SIZE, ACL_MEM_MALLOC_HUGE_FIRST); // 分配 4KB(策略 HUGE_FIRST)
    if (ret != BM_OK) { // 分配失败
        BM_LOG_ERROR("alloc workspace failed, ret: " << ret << " devId: " << devId_
                                                     << " size: " << HYBM_OVERLAP_WS_SIZE); // 根因日志: ret/devId/size
        return ret;                                                                        // 透传失败(根因已记录)
    }
    ret = DlAclApi::AclrtMemset(ws, HYBM_OVERLAP_WS_SIZE, 0, HYBM_OVERLAP_WS_SIZE); // 清零(邮箱槽初值必须为 0)
    if (ret != BM_OK) { // 清零失败
        BM_LOG_ERROR("memset workspace failed, ret: " << ret << " devId: " << devId_); // 根因日志: ret/devId
        (void)DlAclApi::AclrtFree(ws);                                                // 回滚: 释放刚分配的工作区
        return ret;                                                                   // 透传失败(根因已记录)
    }
    workspace_ = reinterpret_cast<uint64_t>(ws); // 记录设备地址(meta 填充/param 装配共用)
    return BM_OK;                                // 工作区就绪
}

Result HybmOverlapPrep::LoadOrchestratorKernel() // 第3步: kernel 加载
{
    std::string jsonPath = env::MF_310P_OVERLAP_JSON; // 优先取环境变量指定的 launch json 路径
    if (jsonPath.empty()) {                           // 环境变量未设置
        jsonPath = HYBM_OVERLAP_DEFAULT_JSON;         // 回退 CWD 缺省名 libmf_overlap_v1.json
    }
    char resolved[PATH_MAX] = {0}; // realpath 结果缓冲(解析后的绝对路径)
    if (realpath(jsonPath.c_str(), resolved) == nullptr || access(resolved, R_OK) != 0) { // 路径不存在或不可读
        // 根因日志: json 路径 + 设置提示
        BM_LOG_ERROR("overlap kernel json not accessible: " << jsonPath
                                                            << " (set MF_310P_OVERLAP_JSON?)");
        return BM_FILE_NOT_ACCESS; // 显式报错(kernel 未部署即不可用)
    }
    aclrtBinaryLoadOption loadOption{}; // 二进制加载选项(仅一项: cpuKernelMode)
    loadOption.type = aclrtBinaryLoadOptionType::ACL_RT_BINARY_LOAD_OPT_CPU_KERNEL_MODE; // 选项类型 = CPU kernel 模式
    loadOption.value.cpuKernelMode = 0; /* cpuMode=0: 走 CP1 搜索路径(A.4-3) */
    aclrtBinaryLoadOptions loadOptions{}; // 选项数组容器
    loadOptions.options = &loadOption;    // 挂接单项选项
    loadOptions.numOpt = 1;               // 选项个数
    auto ret = DlAclApi::AclrtBinaryLoadFromFile(resolved, &loadOptions, &binHandle_); // 按 json 加载二进制(带出句柄)
    if (ret != BM_OK) { // 加载失败
        BM_LOG_ERROR("BinaryLoadFromFile failed, ret: " << ret << " json: " << resolved); // 根因日志: ret/json 绝对路径
        return ret;                                                                      // 透传失败(根因已记录)
    }
    ret = DlAclApi::AclrtBinaryGetFunction(binHandle_, HYBM_OVERLAP_FUNC_NAME, &funcHandle_); // 取入口函数句柄
    if (ret != BM_OK) { // 取函数句柄失败
        // 根因日志: ret/函数名
        BM_LOG_ERROR("BinaryGetFunction failed, ret: " << ret << " func: " << HYBM_OVERLAP_FUNC_NAME);
    }
    return ret; // 末步结果即函数结果(失败路径根因已记录)
}

Result HybmOverlapPrep::LaunchOrchestrator(const HybmOverlapParam &param) // 提交编排 kernel
{
    if (funcHandle_ == nullptr || launchStream_ == nullptr) { // 前置: kernel 句柄与提交流任一缺失
        /* 内部前置: kernel 句柄与提交流就绪即可(冒烟在 ready_ 置位前发生,
         * ready_ 是"初始化含冒烟全部完成"的对外语义, 不作为内部守卫) */
        // 根因日志: 前置条件状态
        BM_LOG_ERROR("overlap launch precond failed, funcHandle null: " << (funcHandle_ == nullptr)
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
    HybmOverlapParam p = param; /* Append 接口非 const, 本地拷贝(120B) */
    aclrtParamHandle paramHandle = nullptr; // 参数块句柄(Append 输出, 本函数不消费)
    ret = DlAclApi::AclrtKernelArgsAppend(argsHandle, &p, sizeof(p), &paramHandle); // 追加 120B 入参块
    if (ret == BM_OK) {                     // 追加成功后
        ret = DlAclApi::AclrtKernelArgsFinalize(argsHandle); // 封包(此后不可再 Append)
    }
    if (ret != BM_OK) { // 追加或封包失败
        BM_LOG_ERROR("KernelArgs Append/Finalize failed, ret: " << ret << " sqId: " << sqId_); // 根因日志: ret/sqId
        return ret;                                                                          // 透传失败(根因已记录)
    }
    aclrtLaunchKernelAttr attr{}; // launch 属性(仅一项: 超时)
    attr.id = aclrtLaunchKernelAttrId::ACL_RT_LAUNCH_KERNEL_ATTR_TIMEOUT; // 属性类型 = 超时
    attr.value.timeout = HYBM_OVERLAP_LAUNCH_TIMEOUT_S; // 25s, 须 < AICPU watchdog 30s
    aclrtLaunchKernelCfg cfg{}; // launch 配置(挂属性数组)
    cfg.attrs = &attr;          // 属性数组首址
    cfg.numAttrs = 1;           // 属性个数
    // 提交编排 kernel(blockDim=1, 带超时属性, 异步不等待)
    ret = DlAclApi::AclrtLaunchKernelWithConfig(funcHandle_, HYBM_OVERLAP_BLOCK_DIM, launchStream_, &cfg,
                                                argsHandle, nullptr);
    if (ret != BM_OK) { // 提交失败
        // 根因日志: ret/sqId/mode
        BM_LOG_ERROR("LaunchKernelWithConfig failed, ret: " << ret << " sqId: " << sqId_
                                                            << " mode: " << param.submitMode);
    }
    return ret; // 提交结果(失败路径根因已记录)
}

Result HybmOverlapPrep::WaitOrchestrator() // 同步编排 kernel
{
    auto ret = DlAclApi::AclrtSynchronizeStream(launchStream_); // 同步提交流(kernel 完成即返回
    if (ret != BM_OK) {                                          // 同步失败
        BM_LOG_ERROR("SynchronizeStream(launch) failed, ret: " << ret << " sqId: " << sqId_); // 根因日志: ret/sqId
    }
    return ret; // 同步结果(失败路径根因已记录)
}

/* 冒烟结果校验(E0a 六判据中除数据逐字比对外全部 + 数据首尾抽样) */
static Result CheckSmokeResult(const HybmOverlapResult &r, uint64_t flagVal, const uint32_t *dst,
                               uint32_t words)
{
    // 四项硬判据: magic 存活 / mainRet=0 / 阶段=完成 / head 前进(TSFW 消费证据)
    if (r.magic != HYBM_OVERLAP_RESULT_MAGIC || r.mainRet != 0 || r.stage != STAGE_DONE ||
        r.headAfter == r.sqTail) { // 判据 4: head 未前进 = SQE 未被消费
        BM_LOG_ERROR("smoke result bad: magic: 0x" << std::hex << r.magic << std::dec
                                                   << " mainRet: " << r.mainRet << " stage: " << r.stage
                                                   << " headBefore: " << r.sqTail
                                                   << " headAfter: " << r.headAfter); // 根因日志: 四项判据值
        return BM_ERROR;                                                          // 判据不满足即冒烟失败
    }
    if (flagVal != HYBM_OVERLAP_SMOKE_SENTINEL) { // 标志值必须等于预置哨兵
        BM_LOG_ERROR("smoke flag mismatch: 0x" << std::hex << flagVal); // 根因日志: 实得标志值
        return BM_ERROR;                                                // 标志错误 = 搬运链路不通
    }
    for (uint32_t i = 0; i < words; i += (words / 8U + 1U)) { /* 首尾+等间隔抽样 8 点 */
        if (dst[i] != (0x5AU << 24) + i) { // 抽样点须命中写入的 pattern
            // 根因日志: 错点位置与值
            BM_LOG_ERROR("smoke data mismatch at idx: " << i << " val: 0x" << std::hex << dst[i]);
            return BM_ERROR; // 数据错误 = 搬运内容不正确
        }
    }
    return BM_OK; // 全部判据通过
}

/* 冒烟数据准备: dst/flag/result 清零 + pattern/哨兵/邮箱写入工作区与 src */
static Result PrepareSmokeData(void *src, void *dst, void *flag, uint32_t words)
{
    static uint32_t pattern[512]; /* 冒烟数据 pattern(0x5A<<24|idx), 静态免大栈 */
    for (uint32_t i = 0; i < words; i++) { // 逐字生成 pattern
        pattern[i] = (0x5AU << 24) + i; /* 与 CheckSmokeResult 抽样基准一致 */
    }
    auto ret = DlAclApi::AclrtMemset(dst, words * 4, 0, words * 4); // 清零目的区(防旧数据误判)
    ret = (ret == BM_OK) ? DlAclApi::AclrtMemset(flag, 8, 0, 8) : ret; // 清零标志位(初值必须 0)
    if (ret == BM_OK) { // 以上清零均成功
        // 计算 result 槽地址(工作区基址 + 1032 偏移)
        auto *wsResult = reinterpret_cast<void *>(HybmOverlapPrep::GetInstance().GetWorkspace() +
                                                  HYBM_OVERLAP_WS_RESULT_OFF);
        ret = DlAclApi::AclrtMemset(wsResult, 64, 0, 64); // 清零 kernel result 槽
    }
    // pattern 灌入源缓冲(H2D, 1KB)
    ret = (ret == BM_OK) ? DlAclApi::AclrtMemcpy(src, words * 4, pattern, words * 4, ACL_MEMCPY_HOST_TO_DEVICE)
                         : ret;
    if (ret != BM_OK) { // 任一准备步骤失败
        return ret;     // 透传失败(根因已记录)
    }
    const uint64_t ws = HybmOverlapPrep::GetInstance().GetWorkspace(); // 工作区基址(单例查询)
    uint64_t sent = HYBM_OVERLAP_SMOKE_SENTINEL;                       // 哨兵值(栈上暂存待拷入)
    uint64_t mb[2] = {reinterpret_cast<uint64_t>(src), words};         // 邮箱描述符 {src 地址, 字数}
    // 哨兵写入工作区 +1024 槽(SQE 对的标志源, H2D 8B)
    ret = DlAclApi::AclrtMemcpy(reinterpret_cast<void *>(ws + HYBM_OVERLAP_WS_SENTINEL_OFF), 8, &sent, 8,
                                ACL_MEMCPY_HOST_TO_DEVICE);
    // 邮箱描述符写入工作区 +0 槽(AICPU 轮询目标, H2D 16B), 结果即函数结果
    return (ret == BM_OK) ? DlAclApi::AclrtMemcpy(reinterpret_cast<void *>(ws + HYBM_OVERLAP_WS_MAILBOX_OFF),
                                                   16, mb, 16, ACL_MEMCPY_HOST_TO_DEVICE)
                          : ret;
}

/* 冒烟结果读回与判定: 返回 BM_OK 即冒烟通过(D2) */
static Result RunSmokeAndCheck(const HybmOverlapParam &p, void *dst, void *flag, uint32_t words)
{
    auto &prep = HybmOverlapPrep::GetInstance(); // 单例引用(launch/wait/工作区查询均经它)
    auto ret = prep.LaunchOrchestrator(p);       // 提交编排 kernel(mode13 单发自证)
    ret = (ret == BM_OK) ? prep.WaitOrchestrator() : ret; // 同步等待 kernel 完成
    if (ret != BM_OK) { // 提交或同步失败
        return ret;     // 透传失败(根因已记录)
    }
    const uint64_t ws = prep.GetWorkspace(); // 工作区基址(读回 result 用)
    HybmOverlapResult r = {};                // kernel 结果读回缓冲(零初始化)
    uint64_t flagVal = 0;                    // 标志读回值(初值 0)
    static uint32_t dstHost[512]; /* 读回缓冲(静态免大栈, 冒烟单线程串行) */
    // D2H: kernel result 64B
    ret = DlAclApi::AclrtMemcpy(&r, sizeof(r), reinterpret_cast<void *>(ws + HYBM_OVERLAP_WS_RESULT_OFF),
                                sizeof(r), ACL_MEMCPY_DEVICE_TO_HOST);
    ret = (ret == BM_OK) ? DlAclApi::AclrtMemcpy(&flagVal, 8, flag, 8, ACL_MEMCPY_DEVICE_TO_HOST) : ret; // D2H: 标志 8B
    // D2H: 目的数据 1KB
    ret = (ret == BM_OK) ? DlAclApi::AclrtMemcpy(dstHost, words * 4, dst, words * 4, ACL_MEMCPY_DEVICE_TO_HOST)
                         : ret;
    if (ret != BM_OK) { // 任一读回失败
        BM_LOG_ERROR("smoke readback failed, ret: " << ret); // 根因日志: ret
        return ret;                                          // 透传失败(根因已记录)
    }
    return CheckSmokeResult(r, flagVal, dstHost, words); // 综合判定(四判据+哨兵+抽样)
}

Result HybmOverlapPrep::SmokeTest() // 第4步: 冒烟自证
{
    void *src = nullptr;             // 源缓冲(pattern 出发地)
    void *dst = nullptr;             // 目的缓冲(SQE 数据搬运目的地)
    void *flag = nullptr;            // 标志缓冲(SQE 对第二支的写入目标)
    const uint32_t words = HYBM_OVERLAP_SMOKE_WORDS; // 冒烟数据量 256 字(1KB)
    auto ret = DlAclApi::AclrtMalloc(&src, words * 4, ACL_MEM_MALLOC_HUGE_FIRST); // 分配源缓冲
    ret = (ret == BM_OK) ? DlAclApi::AclrtMalloc(&dst, words * 4, ACL_MEM_MALLOC_HUGE_FIRST) : ret; // 分配目的缓冲
    ret = (ret == BM_OK) ? DlAclApi::AclrtMalloc(&flag, 8, ACL_MEM_MALLOC_HUGE_FIRST) : ret; // 分配标志缓冲
    if (ret != BM_OK) { // 任一分配失败
        BM_LOG_ERROR("smoke alloc failed, ret: " << ret << " devId: " << devId_); // 根因日志: ret/devId
        return ret;                                                               // 透传失败(根因已记录)
    }
    HybmOverlapParam p = {}; // kernel 入参(逐字段装配, 见下)
    p.devId = dieId_;        // CP1 内 HAL devId = die 号
    p.sqId = sqId_;          // CP 流 SQ id(kernel 内活查询)
    p.streamId = streamId_;  // CP 流 id(SQE 的 rt_stream_id)
    p.submitMode = SUBMIT_MODE_PIPELINE; // 冒烟走流水模式, chunks=1 即单发自证
    p.chunks = 1;                        // 单发自证(邮箱槽 0)
    p.dstAddr = reinterpret_cast<uint64_t>(dst); // 数据目的地址(独立 dst 缓冲)
    p.overflowAddr = workspace_ + HYBM_OVERLAP_WS_OVF_OFF; // SQE overflow 目标槽
    p.flagSentinelAddr = workspace_ + HYBM_OVERLAP_WS_SENTINEL_OFF; // 标志源 = 工作区哨兵槽
    p.flagDoneAddr = reinterpret_cast<uint64_t>(flag); // 标志目的 = 独立 flag 缓冲
    p.resultAddr = workspace_ + HYBM_OVERLAP_WS_RESULT_OFF; // 结果写回槽
    p.mailboxAddr = workspace_ + HYBM_OVERLAP_WS_MAILBOX_OFF; /* 本端 AICore 写(ring) */
    Result check = BM_ERROR; // 判定结果初值(悲观)
    ret = PrepareSmokeData(src, dst, flag, words); // 准备 pattern/哨兵/邮箱
    if (ret == BM_OK) { // 准备成功
        check = RunSmokeAndCheck(p, dst, flag, words); // 提交+读回+判定
        /* 复位冒烟占用的工作区槽位(mailbox/result), 生产可见干净初态 */
        // 清 mailbox 槽(生产可见干净初态)
        (void)DlAclApi::AclrtMemset(reinterpret_cast<void *>(workspace_ + HYBM_OVERLAP_WS_MAILBOX_OFF), 16, 0, 16);
        // 清 result 槽(同上)
        (void)DlAclApi::AclrtMemset(reinterpret_cast<void *>(workspace_ + HYBM_OVERLAP_WS_RESULT_OFF), 64, 0, 64);
    } else { // 准备失败
        BM_LOG_ERROR("smoke prepare failed, ret: " << ret << " devId: " << devId_); // 根因日志: ret/devId
    }
    (void)DlAclApi::AclrtFree(src);  // 释放冒烟源缓冲
    (void)DlAclApi::AclrtFree(dst);  // 释放冒烟目的缓冲
    (void)DlAclApi::AclrtFree(flag); // 释放冒烟标志缓冲
    return (ret == BM_OK) ? check : ret; // 准备失败透传 ret, 否则返回判定结果
}

void HybmOverlapPrep::Cleanup() // 逆序清理(锁内, 归零时)
{
    if (cpStream_ != nullptr) { // CP 流存在
        (void)DlAclApi::AclrtDestroyStream(cpStream_); // 销毁 CP 流(SQ 随流注销)
        cpStream_ = nullptr; // 置空防重复销毁
    }
    if (launchStream_ != nullptr) { // 提交流存在
        (void)DlAclApi::AclrtDestroyStream(launchStream_); // 销毁提交流
        launchStream_ = nullptr; // 置空防重复销毁
    }
    if (workspace_ != 0) { // 工作区已分配
        (void)DlAclApi::AclrtFree(reinterpret_cast<void *>(workspace_)); // 释放 4KB 工作区
        workspace_ = 0;   // 清零标记未分配
    }
    /* binHandle_ 无对应卸载接口(DlAclApi 未封装), 随进程生命周期释放 */
    binHandle_ = nullptr;  // 仅清指针(资源随进程释放)
    funcHandle_ = nullptr; // 仅清指针(同上)
    refCount_ = 0;         // 计数归零
    sqId_ = 0;             // SQ id 复位
    streamId_ = 0;         // 流 id 复位
    dieId_ = 0;            // die 号复位
}
} // namespace mf
} // namespace ock
