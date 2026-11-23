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

// 310P SDMA服务使用独立Context；后台线程更新健康序号并监测服务状态。
// 停止时确认请求和SDMA均结束后释放资源，失败时保留所属对象。
#ifndef MEM_FABRIC_HYBRID_HYBM_SDMA_ORCH_PREP_H
#define MEM_FABRIC_HYBRID_HYBM_SDMA_ORCH_PREP_H

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <condition_variable>
#include "hybm_sdma_service_options.h"

#include "dl_acl_api.h"
#include "hybm_common_include.h"
#include "hybm_sdma_contract.h"

namespace ock {
namespace mf {
/* 工作区布局(哨兵退役——戳 SQE 的源改为到达邮件像首字段) */
constexpr uint64_t HYBM_SDMA_ORCH_WS_SIZE = 4096;    /* 工作区总尺寸 */
constexpr uint64_t HYBM_SDMA_ORCH_WS_RESULT_OFF = 0; /* epoch 结果区 64B(诊断) */
constexpr uint64_t HYBM_SDMA_ORCH_WS_OVF_OFF = 64;   /* SQE overflow 目标 64B */
constexpr uint64_t HYBM_SDMA_ORCH_WS_STOP_OFF = 128; /* 停止标志 8B(工作区自有, 销毁序安全) */

// kernel自行处理停止及超时，runtime的任务超时属性保持为0。
constexpr uint32_t HYBM_SDMA_ORCH_LAUNCH_TIMEOUT_NEVER_S = 0;

class SdmaOrchestrator { // 协同模式传输准备单例(所有可变状态在 mutex_ 内维护)
public:
    static SdmaOrchestrator &GetInstance(); /* 进程级单例入口 */

    /* 显式禁用读取(env MF_SDMA_ORCH_DISABLE, 取值 "1"/"on" 为禁用);
     * 310P+SDMA 位默认启用协同(A3 配方必败, 协同为唯一可用链路) */
    static bool Disabled() noexcept;

    bool HasUnconfirmedResources() const noexcept
    {
        return (stopFlag_.load() && !workerDone_.load()) ||
               (epochLaunched_.load() && workerDone_.load() && !releaseConfirmed_.load());
    }

    /* 引用计数 +1; 首次进入执行传输准备(CP 流→工作区→kernel 含自动部署) */
    Result Acquire(uint16_t devId);

    /* 段尾保留区初始化 + epoch 参数装配 + 监督线程启动(实体在段就绪后调用, 一次绑定)
     * v29 4-rank 化: ownRank/poolBaseGva 供 epoch kernel 做对端到达环路由
     * (peerRank = (dstSeg - poolBaseGva)/symSize, 环下标按收发双方 rank 压缩) */
    Result ConfigureOrch(uint64_t ownSegGva, uint64_t physSize, uint64_t symSize, uint32_t ownRank,
                         uint64_t poolBaseGva, uint32_t rankCount);

    /* 引用计数 -1; 归零停止监督线程并逆序清理 */
    void Release();

    /* 初始化就绪标志(ready 且 configured 才完整可用) */
    bool Ready() const noexcept
    {
        return ready_.load(std::memory_order_acquire);
    }

    /* 引用计数只读查询(UT 断言失败后归零/Release 幂等语义用) */
    uint32_t GetRefCountForTest() const noexcept
    {
        return refCount_;
    }

    /* 工作区设备内存基址(uint64, 设备地址; meta 槽承载, host 侧就绪判定用) */
    uint64_t GetWorkspace() const noexcept
    {
        return workspace_.load(std::memory_order_acquire);
    }

    /* 停止服务并确认已提交SDMA结束；超时返回错误并保留资源。
     * 段拆除路径必须先于段 VA 解除映射调用(kernel 轮询段内环, VA 先拆会设备侧访问异常) */
    bool IsBoundTo(uint64_t address, uint64_t bytes) const noexcept
    {
        const auto base = boundReservation_.load();
        return base != 0 && address <= base && base - address < bytes;
    }

    Result StopEpoch();
    Result GetServiceStatus(HybmSdmaOrchResult &result);

private:
    SdmaOrchestrator() = default;
    Result InitOnce(uint16_t devId);     /* 全量初始化入口(锁内) */
    Result InitCpStream(uint16_t devId); /* CP 流 + sqId/streamId/dieId */
    Result AllocWorkspace();             /* 工作区分配与清零 */
    Result PrepareRuntimeJson(const std::string &jsonPath, std::string &runtimeJson);
    static Result ResolveOrchestratorJson(const std::string &directory, std::string &jsonPath);
    Result LoadOrchestratorKernel();                   /* json 解析 + 部署探测 + 自动部署后加载 */
    Result TryLoadKernel(const std::string &jsonPath); /* 单次加载尝试(日志由调用方分层记) */
    Result DeployOrchestratorKernel(const std::string &kernelDir, const std::string &launchJsonPath); /* 自动部署 */
    Result MigrateViaCustChannel(const std::string &dir, uint64_t &soSize); /* CUST 通道迁移落盘 */
    Result LoadKfcChannel(aclrtFuncHandle &fh); /* KFC 通道装载(临时 json→mode=0 加载→句柄) */
    /* 成功提交后sync返回507018表示结果未确认，由部署/清理调用方检查实际状态。 */
    Result KfcExec(const std::string &cmd);
    Result VerifyDeployment(const std::string &hostSo);
    Result CheckDeploymentResult(const void *bytes, size_t size, bool allowPresent = false);
    Result CopyToSearchPath(const std::string &deviceSoName, uint64_t soSize); /* 提交设备内复制 */
    Result LaunchEpoch(void *stream); /* 单轮下发到指定流(首轮 ConfigureOrch 内联, 后续由监督线程) */
    Result InitControl();
    Result CopyControl(uint64_t device, void *host, size_t bytes, uint32_t kind);
    Result ReadServiceResult(HybmSdmaOrchResult &result);
    Result StartServiceWorker();
    Result WaitServiceWorker();
    Result RetryServiceStop();
    void RecordHostFailure(uint32_t reason);
    Result RunService();
    Result PollService();
    Result UpdateServiceControl(bool &stopWritten, std::chrono::steady_clock::time_point &deadline,
                                std::chrono::steady_clock::time_point &nextHeartbeat);
    Result HandleServiceResult(const HybmSdmaOrchResult &result, bool &finished);
    void SupervisorLoop();          /* 更新健康序号、报告错误；有限轮次正常到期后重新提交 */
    Result DestroyEpochResources(); /* epoch 流 teardown(先同步确认退出再销毁; 幂等) */
    void Cleanup();                 /* 逆序清理(锁内, 归零时) */

    std::atomic<uint64_t> boundReservation_{0};
    HybmSdmaOrchResult cachedResult_{};
    SdmaServiceOptions serviceOptions_{};
    void *serviceContext_{nullptr};
    void *controlStream_{nullptr};
    void *controlHost_{nullptr};
    bool controlPending_{false};
    std::mutex workerMutex_;
    std::mutex stopMutex_;
    std::condition_variable workerChanged_;
    std::atomic<bool> workerDone_{true};
    std::atomic<bool> releaseConfirmed_{true};
    std::atomic<Result> startResult_{BM_NOT_INITIALIZED};
    std::atomic<Result> workerResult_{BM_OK};
    uint32_t hostPid_{0};      // CANN 迁移目录及本次设备 SO 名称使用的宿主机 PID
    std::string deviceSoName_; // 本次加载专用的设备文件名
    uint64_t deployNonce_{0};
    bool deviceSoOwned_{false}; // 仅清理本次部署创建的文件
    std::mutex mutex_;          /* 保护 refCount_ 与初始化/清理串行(ready_/workspace_ 另以原子保护) */
    uint32_t refCount_{0};      /* HYBM 初始化及实体共同持有引用 */
    bool configured_{false};    /* 保留区已配置 + 搬运 kernel 已下发(ConfigureOrch 一次绑定) */
    std::atomic<bool> epochLaunched_{false}; /* 搬运 kernel 在运行(StopEpoch 幂等性: 停过即不再重复停) */
    HybmSdmaOrchParam epochParam_{};         /* epoch kernel 参数(ConfigureOrch 装配) */
    uint16_t devId_{0};                      /* host 设备号(首次 Acquire 绑定) */
    uint32_t dieId_{0};                      /* die 号 = devId_ % 2 */
    uint32_t sqId_{0};                       /* CP 流 SQ id */
    int32_t streamId_{0};                    /* CP 流 id(SQE 的 rt_stream_id) */
    void *cpStream_{nullptr};                /* CP 流句柄(DEVICE_USE_ONLY) */
    void *launchStream_{nullptr};         /* KFC 部署通道流(init 期专用; epoch 不用——见 epochStream_) */
    void *epochStream_{nullptr};          // 独立服务Context中的AICPU任务流
    aclrtBinHandle binHandle_{nullptr};   /* AICPU kernel 二进制句柄 */
    aclrtFuncHandle funcHandle_{nullptr}; /* HybmSdmaOrchEpoch 函数句柄 */
    aclrtBinHandle custBinHandle_{nullptr};  /* CUST 迁移句柄(保活: 持至进程退出) */
    aclrtFuncHandle kfcFuncHandle_{nullptr}; /* KFC 通道句柄缓存(首次装载后复用, 免每次部署重装) */
    std::atomic<bool> ready_{false};         /* 就绪标志(原子: 无锁读与锁内写并发) */
    std::atomic<uint64_t> workspace_{0};     /* 工作区设备地址(原子: 同上) */
    std::atomic<bool> stopFlag_{false};      /* 监督线程退出标志(StopEpoch 置位) */
    std::thread supervisor_; /* 服务维护线程，停止请求及资源确认统一在该线程执行 */
};
} // namespace mf
} // namespace ock

#endif // MEM_FABRIC_HYBRID_HYBM_SDMA_ORCH_PREP_H
