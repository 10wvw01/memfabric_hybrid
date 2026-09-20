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
 * V5 邮箱化(2026-09-18): host 侧 submit/wait 数据面 API 废止(搬运由 AICore 经
 * smem_shm_sdma_signal/wait/quiet 设备侧自驱), 本类职责收敛为:
 *   1. 传输准备(CP 流/工作区/epoch kernel 加载, 含设备侧自动部署);
 *   2. ConfigureOrch: 段尾保留区初始化(双环清零) + epoch 参数装配;
 *   3. 监督线程: epoch kernel(~22s 自限)到期立即续命 —— "常驻消费"在 watchdog 30s
 *      实证约束下的实现形态, 环+游标语义使续命间隙无损。
 *
 * 生命周期: 多池共享进程级资源, Acquire/Release 引用计数; ConfigureOrch 一次绑定
 * (V5 约束: 每进程单 SDMA 池, 双环位于首个配置池的段尾保留区)。
 */
#ifndef MEM_FABRIC_HYBRID_HYBM_SDMA_ORCH_PREP_H /* 头文件包含守卫(防重复包含) */
#define MEM_FABRIC_HYBRID_HYBM_SDMA_ORCH_PREP_H /* 同上, 与文件尾 #endif 配对 */

#include <atomic>   // 原子布尔(监督线程停止标志)
#include <mutex>    // 互斥量(串行化单例初始化/清理与引用计数)
#include <thread>   // 监督线程(epoch 续命)

#include "dl_acl_api.h"          // ACL 运行时接口封装(dlsym 动态加载 aclrt* 符号)
#include "hybm_common_include.h" // hybm 公共类型(Result 错误码/日志宏等)
#include "hybm_sdma_contract.h"  // V5 wire 契约(HybmSdmaOrchParam/Result + 保留区布局)

namespace ock { // MemFabric 总命名空间
namespace mf {  // hybm 内存池引擎子命名空间
/* 工作区布局(V5 精简: 哨兵退役——戳 SQE 的源改为到达邮件像首字段) */
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

    /* 段尾保留区初始化 + epoch 参数装配 + 监督线程启动(实体在段就绪后调用, 一次绑定) */
    Result ConfigureOrch(uint64_t ownSegGva, uint64_t physSize, uint64_t symSize);

    /* 引用计数 -1; 归零停止监督线程并逆序清理 */
    void Release();

    /* 初始化就绪标志(ready 且 configured 才完整可用) */
    bool Ready() const noexcept
    {
        return ready_.load(std::memory_order_acquire); // 原子读(与锁内写并发安全)
    }

    /* 引用计数只读查询(UT 断言失败后归零/Release 幂等语义用) */
    uint32_t GetRefCountForTest() const noexcept
    {
        return refCount_; // 引用计数只读返回(仅供 UT 断言)
    }

    /* 工作区设备内存基址(uint64, 设备地址; meta 槽承载, host 侧就绪判定用) */
    uint64_t GetWorkspace() const noexcept
    {
        return workspace_.load(std::memory_order_acquire); // 原子读(与锁内写并发安全)
    }

private:
    SdmaOrchestrator() = default; // 私有默认构造(仅 GetInstance 的静态局部实例可构造)
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
    Result LaunchEpoch();                  /* 提交一纪 epoch kernel(监督线程内) */
    void SupervisorLoop();                 /* 监督线程主循环(续命直至停止) */
    void StopSupervisor();                 /* 停止监督线程(写设备停止标志+join) */
    void Cleanup();                        /* 逆序清理(锁内, 归零时) */

    std::mutex mutex_;              /* 保护 refCount_ 与初始化/清理串行(ready_/workspace_ 另以原子保护) */
    uint32_t refCount_{0};          /* 引用计数(entity 池级) */
    bool configured_{false};        /* 保留区已配置(ConfigureOrch 一次绑定) */
    std::atomic<bool> stopFlag_{false}; /* 监督线程停止标志(host 侧) */
    std::thread supervisor_;        /* epoch 续命监督线程 */
    HybmSdmaOrchParam epochParam_{}; /* epoch kernel 参数(ConfigureOrch 装配) */
    uint16_t devId_{0};             /* host 设备号(首次 Acquire 绑定) */
    uint32_t dieId_{0};             /* die 号 = devId_ % 2 */
    uint32_t sqId_{0};              /* CP 流 SQ id */
    int32_t streamId_{0};           /* CP 流 id(SQE 的 rt_stream_id) */
    void *cpStream_{nullptr};       /* CP 流句柄(DEVICE_USE_ONLY) */
    void *launchStream_{nullptr};   /* epoch kernel 提交流(监督线程独占) */
    aclrtBinHandle binHandle_{nullptr};   /* AICPU kernel 二进制句柄 */
    aclrtFuncHandle funcHandle_{nullptr}; /* HybmSdmaOrchEpoch 函数句柄 */
    aclrtBinHandle custBinHandle_{nullptr}; /* CUST 迁移句柄(保活: 持至进程退出) */
    aclrtFuncHandle kfcFuncHandle_{nullptr}; /* KFC 通道句柄缓存(首次装载后复用, 免每次部署重装) */
    std::atomic<bool> ready_{false};       /* 就绪标志(原子: 无锁读与锁内写并发) */
    std::atomic<uint64_t> workspace_{0};   /* 工作区设备地址(原子: 同上) */
};
} // namespace mf
} // namespace ock

#endif // MEM_FABRIC_HYBRID_HYBM_SDMA_ORCH_PREP_H
