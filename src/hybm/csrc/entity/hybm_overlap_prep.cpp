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
 * 机制依据: 310p-aicore-aicpu-开发计划.md 附录 A(SQE 配方/CP 流/工作区/冒烟)
 * 与 P0 三实验(310p-capability_matrix/17_aicore_aicpu_overlap/)的实测定论。
 */
#include "hybm_overlap_prep.h"

#include <climits>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

#include "dl_rt_api.h"
#include "mf_env_define.h"

namespace ock {
namespace mf {
namespace {
/* ACL_STREAM_DEVICE_USE_ONLY 的值(acl_rt.h:0x20; CP 流关键 flag, X3/A.3) */
constexpr uint32_t HYBM_ACL_STREAM_DEVICE_USE_ONLY = 0x20U;
constexpr uint32_t HYBM_OVERLAP_BLOCK_DIM = 1;         /* 单 block(kernel 状态机单线程语义) */
constexpr uint32_t HYBM_OVERLAP_LAUNCH_TIMEOUT_S = 25; /* launch attr 超时, < AICPU watchdog 30s */
constexpr uint32_t HYBM_OVERLAP_SMOKE_WORDS = 256;     /* 冒烟数据量(256 字 = 1KB) */
constexpr char HYBM_OVERLAP_FUNC_NAME[] = "HybmOverlapRun";
constexpr char HYBM_OVERLAP_DEFAULT_JSON[] = "libmf_overlap_v1.json"; /* CWD 缺省 json */
constexpr uint64_t HYBM_OVERLAP_RESULT_MAGIC = 0xA1C0DE01ULL;         /* kernel 存活魔数(契约头同值) */
} // namespace

HybmOverlapPrep &HybmOverlapPrep::GetInstance()
{
    static HybmOverlapPrep instance;
    return instance;
}

bool HybmOverlapPrep::SwitchOn() noexcept
{
    const auto &val = env::MF_310P_SHM_OVERLAP;
    return val == "1" || val == "on";
}

Result HybmOverlapPrep::Acquire(uint16_t devId)
{
    std::lock_guard<std::mutex> guard(mutex_);
    if (refCount_ > 0) {
        if (devId != devId_) {
            /* CP 流/工作区绑定单设备, 同进程多池须同设备 */
            BM_LOG_ERROR("overlap prep bound devId: " << devId_ << " new devId: " << devId);
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
    BM_LOG_INFO("310P overlap prep ready: devId: " << devId_ << " sqId: " << sqId_
                                                   << " streamId: " << streamId_ << " dieId: " << dieId_
                                                   << " workspace: 0x" << std::hex << workspace_);
    return BM_OK;
}

void HybmOverlapPrep::Release()
{
    std::lock_guard<std::mutex> guard(mutex_);
    if (refCount_ == 0) {
        return;
    }
    refCount_--;
    if (refCount_ == 0) {
        Cleanup();
        ready_ = false;
    }
}

Result HybmOverlapPrep::InitOnce(uint16_t devId)
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
    ret = SmokeTest();
    if (ret != BM_OK) {
        return ret;
    }
    return BM_OK;
}

Result HybmOverlapPrep::InitCpStream(uint16_t devId)
{
    auto ret = DlAclApi::AclrtCreateStreamWithConfig(&cpStream_, 0, HYBM_ACL_STREAM_DEVICE_USE_ONLY);
    if (ret != BM_OK) {
        BM_LOG_ERROR("create CP stream failed, ret: " << ret << " devId: " << devId);
        return ret;
    }
    ret = DlAclApi::AclrtStreamGetId(cpStream_, &streamId_);
    if (ret != BM_OK) {
        BM_LOG_ERROR("get CP streamId failed, ret: " << ret << " devId: " << devId);
        return ret;
    }
    ret = DlRtApi::RtStreamGetSqid(cpStream_, &sqId_);
    if (ret != BM_OK) {
        BM_LOG_ERROR("rtStreamGetSqid failed, ret: " << ret << " devId: " << devId);
        return ret;
    }
    ret = DlAclApi::AclrtCreateStream(&launchStream_);
    if (ret != BM_OK) {
        BM_LOG_ERROR("create launch stream failed, ret: " << ret << " devId: " << devId);
    }
    return ret;
}

Result HybmOverlapPrep::AllocWorkspace()
{
    void *ws = nullptr;
    auto ret = DlAclApi::AclrtMalloc(&ws, HYBM_OVERLAP_WS_SIZE, ACL_MEM_MALLOC_HUGE_FIRST);
    if (ret != BM_OK) {
        BM_LOG_ERROR("alloc workspace failed, ret: " << ret << " devId: " << devId_
                                                     << " size: " << HYBM_OVERLAP_WS_SIZE);
        return ret;
    }
    ret = DlAclApi::AclrtMemset(ws, HYBM_OVERLAP_WS_SIZE, 0, HYBM_OVERLAP_WS_SIZE);
    if (ret != BM_OK) {
        BM_LOG_ERROR("memset workspace failed, ret: " << ret << " devId: " << devId_);
        (void)DlAclApi::AclrtFree(ws);
        return ret;
    }
    workspace_ = reinterpret_cast<uint64_t>(ws);
    return BM_OK;
}

Result HybmOverlapPrep::LoadOrchestratorKernel()
{
    std::string jsonPath = env::MF_310P_OVERLAP_JSON;
    if (jsonPath.empty()) {
        jsonPath = HYBM_OVERLAP_DEFAULT_JSON;
    }
    char resolved[PATH_MAX] = {0};
    if (realpath(jsonPath.c_str(), resolved) == nullptr || access(resolved, R_OK) != 0) {
        BM_LOG_ERROR("overlap kernel json not accessible: " << jsonPath
                                                            << " (set MF_310P_OVERLAP_JSON?)");
        return BM_FILE_NOT_ACCESS;
    }
    aclrtBinaryLoadOption loadOption{};
    loadOption.type = aclrtBinaryLoadOptionType::ACL_RT_BINARY_LOAD_OPT_CPU_KERNEL_MODE;
    loadOption.value.cpuKernelMode = 0; /* cpuMode=0: 走 CP1 搜索路径(A.4-3) */
    aclrtBinaryLoadOptions loadOptions{};
    loadOptions.options = &loadOption;
    loadOptions.numOpt = 1;
    auto ret = DlAclApi::AclrtBinaryLoadFromFile(resolved, &loadOptions, &binHandle_);
    if (ret != BM_OK) {
        BM_LOG_ERROR("BinaryLoadFromFile failed, ret: " << ret << " json: " << resolved);
        return ret;
    }
    ret = DlAclApi::AclrtBinaryGetFunction(binHandle_, HYBM_OVERLAP_FUNC_NAME, &funcHandle_);
    if (ret != BM_OK) {
        BM_LOG_ERROR("BinaryGetFunction failed, ret: " << ret << " func: " << HYBM_OVERLAP_FUNC_NAME);
    }
    return ret;
}

Result HybmOverlapPrep::LaunchOrchestrator(const HybmOverlapParam &param)
{
    if (funcHandle_ == nullptr || launchStream_ == nullptr) {
        /* 内部前置: kernel 句柄与提交流就绪即可(冒烟在 ready_ 置位前发生,
         * ready_ 是"初始化含冒烟全部完成"的对外语义, 不作为内部守卫) */
        BM_LOG_ERROR("overlap launch precond failed, funcHandle null: " << (funcHandle_ == nullptr)
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
    HybmOverlapParam p = param; /* Append 接口非 const, 本地拷贝(120B) */
    aclrtParamHandle paramHandle = nullptr;
    ret = DlAclApi::AclrtKernelArgsAppend(argsHandle, &p, sizeof(p), &paramHandle);
    if (ret == BM_OK) {
        ret = DlAclApi::AclrtKernelArgsFinalize(argsHandle);
    }
    if (ret != BM_OK) {
        BM_LOG_ERROR("KernelArgs Append/Finalize failed, ret: " << ret << " sqId: " << sqId_);
        return ret;
    }
    aclrtLaunchKernelAttr attr{};
    attr.id = aclrtLaunchKernelAttrId::ACL_RT_LAUNCH_KERNEL_ATTR_TIMEOUT;
    attr.value.timeout = HYBM_OVERLAP_LAUNCH_TIMEOUT_S;
    aclrtLaunchKernelCfg cfg{};
    cfg.attrs = &attr;
    cfg.numAttrs = 1;
    ret = DlAclApi::AclrtLaunchKernelWithConfig(funcHandle_, HYBM_OVERLAP_BLOCK_DIM, launchStream_, &cfg,
                                                argsHandle, nullptr);
    if (ret != BM_OK) {
        BM_LOG_ERROR("LaunchKernelWithConfig failed, ret: " << ret << " sqId: " << sqId_
                                                            << " mode: " << param.submitMode);
    }
    return ret;
}

Result HybmOverlapPrep::WaitOrchestrator()
{
    auto ret = DlAclApi::AclrtSynchronizeStream(launchStream_);
    if (ret != BM_OK) {
        BM_LOG_ERROR("SynchronizeStream(launch) failed, ret: " << ret << " sqId: " << sqId_);
    }
    return ret;
}

/* 冒烟结果校验(E0a 六判据中除数据逐字比对外全部 + 数据首尾抽样) */
static Result CheckSmokeResult(const HybmOverlapResult &r, uint64_t flagVal, const uint32_t *dst,
                               uint32_t words)
{
    if (r.magic != HYBM_OVERLAP_RESULT_MAGIC || r.mainRet != 0 || r.stage != STAGE_DONE ||
        r.headAfter == r.sqTail) {
        BM_LOG_ERROR("smoke result bad: magic: 0x" << std::hex << r.magic << std::dec
                                                   << " mainRet: " << r.mainRet << " stage: " << r.stage
                                                   << " headBefore: " << r.sqTail
                                                   << " headAfter: " << r.headAfter);
        return BM_ERROR;
    }
    if (flagVal != HYBM_OVERLAP_SMOKE_SENTINEL) {
        BM_LOG_ERROR("smoke flag mismatch: 0x" << std::hex << flagVal);
        return BM_ERROR;
    }
    for (uint32_t i = 0; i < words; i += (words / 8U + 1U)) { /* 首尾+等间隔抽样 8 点 */
        if (dst[i] != (0x5AU << 24) + i) {
            BM_LOG_ERROR("smoke data mismatch at idx: " << i << " val: 0x" << std::hex << dst[i]);
            return BM_ERROR;
        }
    }
    return BM_OK;
}

/* 冒烟数据准备: dst/flag/result 清零 + pattern/哨兵/邮箱写入工作区与 src */
static Result PrepareSmokeData(void *src, void *dst, void *flag, uint32_t words)
{
    static uint32_t pattern[512]; /* 冒烟数据 pattern(0x5A<<24|idx), 静态免大栈 */
    for (uint32_t i = 0; i < words; i++) {
        pattern[i] = (0x5AU << 24) + i; /* 与 CheckSmokeResult 抽样基准一致 */
    }
    auto ret = DlAclApi::AclrtMemset(dst, words * 4, 0, words * 4);
    ret = (ret == BM_OK) ? DlAclApi::AclrtMemset(flag, 8, 0, 8) : ret;
    if (ret == BM_OK) {
        auto *wsResult = reinterpret_cast<void *>(HybmOverlapPrep::GetInstance().GetWorkspace() +
                                                  HYBM_OVERLAP_WS_RESULT_OFF);
        ret = DlAclApi::AclrtMemset(wsResult, 64, 0, 64);
    }
    ret = (ret == BM_OK) ? DlAclApi::AclrtMemcpy(src, words * 4, pattern, words * 4, ACL_MEMCPY_HOST_TO_DEVICE)
                         : ret;
    if (ret != BM_OK) {
        return ret;
    }
    const uint64_t ws = HybmOverlapPrep::GetInstance().GetWorkspace();
    uint64_t sent = HYBM_OVERLAP_SMOKE_SENTINEL;
    uint64_t mb[2] = {reinterpret_cast<uint64_t>(src), words};
    ret = DlAclApi::AclrtMemcpy(reinterpret_cast<void *>(ws + HYBM_OVERLAP_WS_SENTINEL_OFF), 8, &sent, 8,
                                ACL_MEMCPY_HOST_TO_DEVICE);
    return (ret == BM_OK) ? DlAclApi::AclrtMemcpy(reinterpret_cast<void *>(ws + HYBM_OVERLAP_WS_MAILBOX_OFF),
                                                   16, mb, 16, ACL_MEMCPY_HOST_TO_DEVICE)
                          : ret;
}

/* 冒烟结果读回与判定: 返回 BM_OK 即冒烟通过(D2) */
static Result RunSmokeAndCheck(const HybmOverlapParam &p, void *dst, void *flag, uint32_t words)
{
    auto &prep = HybmOverlapPrep::GetInstance();
    auto ret = prep.LaunchOrchestrator(p);
    ret = (ret == BM_OK) ? prep.WaitOrchestrator() : ret;
    if (ret != BM_OK) {
        return ret;
    }
    const uint64_t ws = prep.GetWorkspace();
    HybmOverlapResult r = {};
    uint64_t flagVal = 0;
    static uint32_t dstHost[512]; /* 读回缓冲(静态免大栈, 冒烟单线程串行) */
    ret = DlAclApi::AclrtMemcpy(&r, sizeof(r), reinterpret_cast<void *>(ws + HYBM_OVERLAP_WS_RESULT_OFF),
                                sizeof(r), ACL_MEMCPY_DEVICE_TO_HOST);
    ret = (ret == BM_OK) ? DlAclApi::AclrtMemcpy(&flagVal, 8, flag, 8, ACL_MEMCPY_DEVICE_TO_HOST) : ret;
    ret = (ret == BM_OK) ? DlAclApi::AclrtMemcpy(dstHost, words * 4, dst, words * 4, ACL_MEMCPY_DEVICE_TO_HOST)
                         : ret;
    if (ret != BM_OK) {
        BM_LOG_ERROR("smoke readback failed, ret: " << ret);
        return ret;
    }
    return CheckSmokeResult(r, flagVal, dstHost, words);
}

Result HybmOverlapPrep::SmokeTest()
{
    void *src = nullptr;
    void *dst = nullptr;
    void *flag = nullptr;
    const uint32_t words = HYBM_OVERLAP_SMOKE_WORDS;
    auto ret = DlAclApi::AclrtMalloc(&src, words * 4, ACL_MEM_MALLOC_HUGE_FIRST);
    ret = (ret == BM_OK) ? DlAclApi::AclrtMalloc(&dst, words * 4, ACL_MEM_MALLOC_HUGE_FIRST) : ret;
    ret = (ret == BM_OK) ? DlAclApi::AclrtMalloc(&flag, 8, ACL_MEM_MALLOC_HUGE_FIRST) : ret;
    if (ret != BM_OK) {
        BM_LOG_ERROR("smoke alloc failed, ret: " << ret << " devId: " << devId_);
        return ret;
    }
    HybmOverlapParam p = {};
    p.devId = dieId_;
    p.sqId = sqId_;
    p.streamId = streamId_;
    p.submitMode = SUBMIT_MODE_CHAIN;
    p.dstAddr = reinterpret_cast<uint64_t>(dst);
    p.overflowAddr = workspace_ + HYBM_OVERLAP_WS_OVF_OFF;
    p.flagSentinelAddr = workspace_ + HYBM_OVERLAP_WS_SENTINEL_OFF;
    p.flagDoneAddr = reinterpret_cast<uint64_t>(flag);
    p.resultAddr = workspace_ + HYBM_OVERLAP_WS_RESULT_OFF;
    p.reserved[1] = 0x8U; /* doorbell 强制走 HAL(A.4, 与 P0 探针一致) */
    p.mailboxAddr = workspace_ + HYBM_OVERLAP_WS_MAILBOX_OFF;
    p.role = 0;
    Result check = BM_ERROR;
    ret = PrepareSmokeData(src, dst, flag, words);
    if (ret == BM_OK) {
        check = RunSmokeAndCheck(p, dst, flag, words);
        /* 复位冒烟占用的工作区槽位(mailbox/result), 生产可见干净初态 */
        (void)DlAclApi::AclrtMemset(reinterpret_cast<void *>(workspace_ + HYBM_OVERLAP_WS_MAILBOX_OFF), 16, 0, 16);
        (void)DlAclApi::AclrtMemset(reinterpret_cast<void *>(workspace_ + HYBM_OVERLAP_WS_RESULT_OFF), 64, 0, 64);
    } else {
        BM_LOG_ERROR("smoke prepare failed, ret: " << ret << " devId: " << devId_);
    }
    (void)DlAclApi::AclrtFree(src);
    (void)DlAclApi::AclrtFree(dst);
    (void)DlAclApi::AclrtFree(flag);
    return (ret == BM_OK) ? check : ret;
}

void HybmOverlapPrep::Cleanup()
{
    if (cpStream_ != nullptr) {
        (void)DlAclApi::AclrtDestroyStream(cpStream_);
        cpStream_ = nullptr;
    }
    if (launchStream_ != nullptr) {
        (void)DlAclApi::AclrtDestroyStream(launchStream_);
        launchStream_ = nullptr;
    }
    if (workspace_ != 0) {
        (void)DlAclApi::AclrtFree(reinterpret_cast<void *>(workspace_));
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
