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

/* hybm_sdma_orchestrator.h — 310P AICore×AICPU 协同模式传输准备(进程级单例 + 引用计数)
 *
 * 邮箱化: host 侧 submit/wait 数据面 API 庢止(搬运由 AICore 经
 * smem_shm_sdma_signal/wait/quiet 设备侧自驱), 本类职责收敛为:
 *   1. 传输准备(CP 流/工作区/epoch kernel 加载, 含设备侧自动部署);
 *   2. ConfigureOrch: 段尾保留区初始化(双环清零) + epoch 参数装配 + 首纪内联下发
 *      (失败立即报错返回) + 监督线程启动;
 *   3. StopEpoch: 写停止标志 → 停监督线程 → kernel ≤1ms 自退 → 同步确认 → 读结果区诊断。
 *
 * 执行模型(有限纪 + 监督重拉, 2026-10-12 定案): epoch 壁钟预算默认 15s
 * (MF_SDMA_ORCH_EPOCH_BUDGET_S 可调, 4294967295=常驻诊断模式), 到点自退后监督线程
 * 无缝重拉(环+游标语义零丢失, 重拉间隙实测 ≤5ms)。为什么不常驻: host 侧全设备同步
 * (AclrtSynchronizeDevice, torch_npu 分配器内部触发)等待所有流的全部任务且无任务类型
 * 豁免——runtime 的 IsStreamNotSync 仅豁免 CP_PROCESS_USE 流, 而该类流拒收本进程任务
 * 下发(507000), 两路皆死; 常驻 kernel 必阻塞任何全设备同步(serve 实测死因 507017)。
 * 有限纪下全设备同步最多等一纪残余(<15s)。launch json 为 KFCKernel(type 5), aicpu-sd
 * monitor 对该类型不计时——纪超时无 28s 击杀风险(v7 时代 type2 迟到自退的必杀缺陷不复存在)。
 *
 * 生命周期: 多池共享进程级资源, Acquire/Release 引用计数; ConfigureOrch 一次绑定
 * (约束: 每进程单 SDMA 池, 双环位于首个配置池的段尾保留区)。
 */
#ifndef MEM_FABRIC_HYBRID_HYBM_SDMA_ORCH_PREP_H
#define MEM_FABRIC_HYBRID_HYBM_SDMA_ORCH_PREP_H

#include <atomic>
#include <mutex>
#include <thread>

#include "dl_acl_api.h"
#include "hybm_common_include.h"
#include "hybm_sdma_contract.h"

namespace ock {
namespace mf {
/* 工作区布局(哨兵退役——戳 SQE 的源改为到达邮件像首字段) */
constexpr uint64_t HYBM_SDMA_ORCH_WS_SIZE = 4096;         /* 工作区总尺寸 */
constexpr uint64_t HYBM_SDMA_ORCH_WS_RESULT_OFF = 0;      /* epoch 结果区 64B(诊断) */
constexpr uint64_t HYBM_SDMA_ORCH_WS_OVF_OFF = 64;        /* SQE overflow 目标 64B */
constexpr uint64_t HYBM_SDMA_ORCH_WS_STOP_OFF = 128;      /* 停止标志 8B(工作区自有, 销毁序安全) */

/* 停止同步上限(秒): StopEpoch 写停止标志后 kernel 正常 ≤1ms 自退。
 * 0 = 永不超时(attr 值经 ConvertTimeoutToInner(0)=MAX_UINT64; 缺省 attr 则为默认
 * 超时 ≈6.4s 会误杀长时任务——507017, 故 attr 不可省略且必须显式置 0)。
 * 任何有限 attr 值都会在到达时把任务标记为超时(507017, host 侧语义, 设备侧不杀)
 * 导致监督线程的 sync 提前返回、不等 kernel 退出。有限纪的时长由 kernel 壁钟
 * (cntvct_el0)自限, 不经 attr。设备故障(kernel 卡在 HAL 调用)时 sync 不返回、
 * 销毁挂起——已接受的取舍 */
constexpr uint32_t HYBM_SDMA_ORCH_LAUNCH_TIMEOUT_NEVER_S = 0;

class SdmaOrchestrator { // 协同模式传输准备单例(所有可变状态在 mutex_ 内维护)
public:
    static SdmaOrchestrator &GetInstance();     /* 进程级单例入口 */

    /* 显式禁用读取(env MF_SDMA_ORCH_DISABLE, 取值 "1"/"on" 为禁用);
     * 310P+SDMA 位默认启用协同(A3 配方必败, 协同为唯一可用链路) */
    static bool Disabled() noexcept;

    /* 引用计数 +1; 首次进入执行传输准备(CP 流→工作区→kernel 含自动部署) */
    Result Acquire(uint16_t devId);

    /* 段尾保留区初始化 + epoch 参数装配 + 监督线程启动(实体在段就绪后调用, 一次绑定) */
    Result ConfigureOrch(uint64_t ownSegGva, uint64_t physSize, uint64_t symSize);

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

    /* 停止常驻 epoch kernel(幂等): 写停止标志 → kernel ≤1ms 自退 → 同步确认 → 读结果区诊断。
     * 段拆除路径必须先于段 VA 解除映射调用(kernel 轮询段内环, VA 先拆会设备侧访问异常) */
    void StopEpoch();

private:
    SdmaOrchestrator() = default;
    Result InitOnce(uint16_t devId);       /* 全量初始化入口(锁内) */
    Result InitCpStream(uint16_t devId);   /* CP 流 + sqId/streamId/dieId */
    Result AllocWorkspace();               /* 工作区分配与清零 */
    Result LoadOrchestratorKernel();       /* json 解析 + 部署探测 + 自动部署后加载 */
    Result TryLoadKernel(const std::string &jsonPath);   /* 单次加载尝试(日志由调用方分层记) */
    Result DeployOrchestratorKernel(const std::string &launchJsonPath, const std::string &dst); /* 自动部署 */
    Result MigrateViaCustChannel(const std::string &dir, uint64_t &soSize); /* CUST 通道迁移落盘 */
    Result LoadKfcChannel(aclrtFuncHandle &fh); /* KFC 通道装载(临时 json→mode=0 加载→句柄) */
    /* KFC 通道执行一条设备 OS 命令; probe=true 时"文件缺失"退出码降级为 INFO(首轮探测预期, 仅部署链内部使用) */
    Result KfcExec(const std::string &cmd, bool probe = false);
    Result CopyToSearchPath(const std::string &deviceSoName, uint64_t soSize); /* 设备内复制+完整性校验 */
    Result LaunchEpoch(void *stream);     /* 单纪下发到指定流(首纪 ConfigureOrch 内联, 后续由监督线程) */
    void SupervisorLoop();                 /* 监督线程: 等当前纪退出 → 未停则重拉(隔离流上, 同步不受追纪影响) */
    void DestroyEpochResources();          /* epoch 流 teardown(先同步确认退出再销毁; 幂等) */
    void Cleanup();                        /* 逆序清理(锁内, 归零时) */

    std::mutex mutex_;              /* 保护 refCount_ 与初始化/清理串行(ready_/workspace_ 另以原子保护) */
    uint32_t refCount_{0};          /* 引用计数(entity 池级) */
    bool configured_{false};        /* 保留区已配置 + 常驻 kernel 已下发(ConfigureOrch 一次绑定) */
    bool epochLaunched_{false};     /* 常驻 kernel 在运行(StopEpoch 幂等性: 停过即不再重复停) */
    HybmSdmaOrchParam epochParam_{}; /* epoch kernel 参数(ConfigureOrch 装配) */
    uint16_t devId_{0};             /* host 设备号(首次 Acquire 绑定) */
    uint32_t dieId_{0};             /* die 号 = devId_ % 2 */
    uint32_t sqId_{0};              /* CP 流 SQ id */
    int32_t streamId_{0};           /* CP 流 id(SQE 的 rt_stream_id) */
    void *cpStream_{nullptr};       /* CP 流句柄(DEVICE_USE_ONLY) */
    void *launchStream_{nullptr};   /* KFC 部署通道流(init 期专用; epoch 不用——见 epochStream_) */
    void *epochStream_{nullptr};    /* epoch 提交流: 以 RTS 私有 RT_STREAM_FORBIDDEN_DEFAULT(0x10) 标志创建,
                                      * 不注册进 context 的 streams_——全设备同步(AclrtSynchronizeDevice)遍历
                                      * 不到它。这是长循环任务的确定性隔离通道(流级豁免/独立上下文两路
                                      * 实测皆不通, 见平台事实 2026-10-12 条目) */
    aclrtBinHandle binHandle_{nullptr};   /* AICPU kernel 二进制句柄 */
    aclrtFuncHandle funcHandle_{nullptr}; /* HybmSdmaOrchEpoch 函数句柄 */
    aclrtBinHandle custBinHandle_{nullptr}; /* CUST 迁移句柄(保活: 持至进程退出) */
    aclrtFuncHandle kfcFuncHandle_{nullptr}; /* KFC 通道句柄缓存(首次装载后复用, 免每次部署重装) */
    std::atomic<bool> ready_{false};       /* 就绪标志(原子: 无锁读与锁内写并发) */
    std::atomic<uint64_t> workspace_{0};   /* 工作区设备地址(原子: 同上) */
    std::atomic<bool> stopFlag_{false};    /* 监督线程退出标志(StopEpoch 置位) */
    std::thread supervisor_;               /* 监督线程(有限纪模式; 常驻模式不启动) */
};
} // namespace mf
} // namespace ock

#endif // MEM_FABRIC_HYBRID_HYBM_SDMA_ORCH_PREP_H
