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
 * 邮箱化: host 侧 submit/wait 数据面 API 废止(搬运由 AICore 经
 * smem_shm_sdma_put/get/quiet 设备侧自驱), 本类职责收敛为:
 *   1. 传输准备(CP 流/工作区/epoch kernel 部署, 见下方"kernel 部署"段);
 *   2. ConfigureOrch: 段尾保留区初始化(双环清零) + epoch 参数装配 + 首轮内联下发
 *      (失败立即报错返回) + 监督线程启动;
 *   3. StopEpoch: 写停止标志 → 停监督线程 → kernel ≤1ms 自退 → 同步确认 → 读结果区诊断。
 *
 * 执行模型(按时长分轮运行, 2026-10-12 定案): 每轮时长默认 15s
 * (MF_SDMA_ORCH_EPOCH_BUDGET_S 可调, 4294967295=永不退出的诊断模式), kernel 到点
 * 自退后监督线程重新拉起新一轮(环+游标语义零丢失, 换轮间隙约 100ms)。
 * 为什么不一次下发跑到停止: host 侧全设备同步(torch_npu 分配显存时内部触发)会等
 * 所有流的全部任务, 流标志与独立上下文均无法豁免(CP_PROCESS_USE 流拒收本进程
 * 任务下发 507000), 永不退出的 kernel 必阻塞它(serve 实测死因 507017)。
 * 分轮运行后全设备同步最多等当前轮剩余(<15s)。launch json 为 KFCKernel(type 5),
 * 设备侧系统监控不给该类型计时——轮超时无 28s 强制结束风险。
 *
 * kernel 部署(2026-11-19 定案, 参考 pypto 机制): 借 CP1 启动时预加载的 CANN
 * libaicpu_extend_kernels.so 里的 loader, 把本仓 libmf_sdma_orch.so 以 memfd 方式
 * 加载进 CP1(aicpu_scheduler)进程并填函数表——不落设备搜索路径, 不经 KFC 设备
 * shell(2026-11-18 驱动升级后 shell 平台级失效, 507018)。本仓 so 导出 loader 约定
 * 的三个同名符号(StaticTileFwkBackendKernelServer / DynTileFwkBackendKernelServerInit /
 * DynTileFwkBackendKernelServer), 每轮以 DynTileFwkKernelServer 下发, cfgdata 指向
 * epoch 参数。超时用 args.timeout=0xFFFF + flag 0x100(永不超时), 每轮时长仍由
 * kernel 读主机报时计数自限。
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

class SdmaOrchestrator { // 协同模式传输准备单例(所有可变状态在 mutex_ 内维护)
public:
    static SdmaOrchestrator &GetInstance();     /* 进程级单例入口 */

    /* 显式禁用读取(env MF_SDMA_ORCH_DISABLE, 取值 "1"/"on" 为禁用);
     * 310P+SDMA 位默认启用协同(A3 配方必败, 协同为唯一可用链路) */
    static bool Disabled() noexcept;

    /* 引用计数 +1; 首次进入执行传输准备(CP 流→工作区→kernel 含自动部署) */
    Result Acquire(uint16_t devId);

    /* 段尾保留区初始化 + epoch 参数装配 + 监督线程启动(实体在段就绪后调用, 一次绑定)
     * v29 4-rank 化: ownRank/poolBaseGva 供 epoch kernel 做对端到达环路由
     * (peerRank = (dstSeg - poolBaseGva)/symSize, 环下标按收发双方 rank 压缩) */
    Result ConfigureOrch(uint64_t ownSegGva, uint64_t physSize, uint64_t symSize, uint32_t ownRank,
                         uint64_t poolBaseGva);

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

    /* 停止搬运 epoch kernel(幂等): 写停止标志 → kernel ≤1ms 自退 → 同步确认 → 读结果区诊断。
     * 段拆除路径必须先于段 VA 解除映射调用(kernel 轮询段内环, VA 先拆会设备侧访问异常) */
    void StopEpoch();

private:
    SdmaOrchestrator() = default;
    Result InitOnce(uint16_t devId);       /* 全量初始化入口(锁内) */
    Result InitCpStream(uint16_t devId);   /* CP 流 + sqId/streamId/dieId */
    Result AllocWorkspace();               /* 工作区分配与清零 */
    static Result ResolveOrchestratorJson(const std::string &directory, std::string &jsonPath);
    Result LoadOrchestratorKernel();       /* json 解析 + 经 CANN extend lib loader 部署到 CP1 */
    /* pypto 机制部署: so 字节经 DeviceArgs 传给 CP1 预加载的 libaicpu_extend_kernels.so,
     * 由其在 CP1 进程内 memfd 加载并填函数表(不落设备搜索路径, 不经 KFC shell) */
    Result DeployOrchestratorKernel(const std::string &kernelDir);
    Result LaunchEpoch(void *stream);     /* 单轮下发到指定流(首轮 ConfigureOrch 内联, 后续由监督线程) */
    void SupervisorLoop();                 /* 监督线程: 每 100ms 写报时计数+读结果区, 发现当前轮退出即重新拉起 */
    void DestroyEpochResources();          /* epoch 流 teardown(先同步确认退出再销毁; 幂等) */
    void Cleanup();                        /* 逆序清理(锁内, 归零时) */

    bool extendLoaderReady_{false}; // CP1 extend lib loader 已装载本 so(函数表已填)
    void *devSo_{nullptr};          // so 字节设备缓冲(loader 输入, 进程内保活)
    void *devArgs_{nullptr};        // DeviceArgs 设备缓冲(loader 输入)
    void *devParam_{nullptr};       // epoch 参数设备缓冲(每轮 H2D)
    void *rtLaunch_{nullptr};       // rtAicpuKernelLaunchExWithArgs(libruntime.so)
    void *runtimeLib_{nullptr};     // libruntime.so 句柄(保活)
    std::mutex mutex_;              /* 保护 refCount_ 与初始化/清理串行(ready_/workspace_ 另以原子保护) */
    uint32_t refCount_{0};          /* 引用计数(entity 池级) */
    bool configured_{false};        /* 保留区已配置 + 搬运 kernel 已下发(ConfigureOrch 一次绑定) */
    bool epochLaunched_{false};     /* 搬运 kernel 在运行(StopEpoch 幂等性: 停过即不再重复停) */
    HybmSdmaOrchParam epochParam_{}; /* epoch kernel 参数(ConfigureOrch 装配) */
    uint16_t devId_{0};             /* host 设备号(首次 Acquire 绑定) */
    uint32_t dieId_{0};             /* die 号 = devId_ % 2 */
    uint32_t sqId_{0};              /* CP 流 SQ id */
    int32_t streamId_{0};           /* CP 流 id(SQE 的 rt_stream_id) */
    void *cpStream_{nullptr};       /* CP 流句柄(DEVICE_USE_ONLY) */
    void *launchStream_{nullptr};   /* KFC 部署通道流(init 期专用; epoch 不用——见 epochStream_) */
    void *epochStream_{nullptr};    /* 搬运 kernel 提交流(普通流, 每轮任务下发于此;
                                      * 曾尝试带隔离标志建流使全设备同步看不见它, 实测装机版本无效且
                                      * 拒收任务下发, 已回退——见平台事实 2026-10-12 条目) */
    std::atomic<bool> ready_{false};       /* 就绪标志(原子: 无锁读与锁内写并发) */
    std::atomic<uint64_t> workspace_{0};   /* 工作区设备地址(原子: 同上) */
    std::atomic<bool> stopFlag_{false};    /* 监督线程退出标志(StopEpoch 置位) */
    std::thread supervisor_;               /* 监督线程(分轮模式启动: 写报时计数/读结果区/换轮; 永不退出诊断模式不启动) */
};
} // namespace mf
} // namespace ock

#endif // MEM_FABRIC_HYBRID_HYBM_SDMA_ORCH_PREP_H
