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
 * 挂载点: MemEntityDefault::InitTransManager 的 [310P + SDMA op 位 + 开关] 分支
 * (六级初始化地图的 L2-④, 见 310p-aicore-aicpu-开发进展.md D4)。
 *
 * 职责(P0 三实验已逐项实证, 机制档案见 310p-aicore-aicpu-开发进展.md):
 *   1. CP 流: ACL_STREAM_DEVICE_USE_ONLY(其 SQ 注册进 CP1 进程 trs 表, 是 AICPU
 *      kernel 内 halSqCqQuery 可查可投的前提, X3/A.3);
 *   2. 工作区: mailbox/sentinel/result/ovf, 普通设备内存 HUGE_FIRST(NORMAL_ONLY 落
 *      特殊段 SDMA 不可翻译, A.4-7), AICore 可直接 GM 访问(E0c 结论);
 *   3. kernel 加载: 先按 launch json 直接加载(cpuMode=0, CP1 搜索路径); 失败(设备侧
 *      未部署, 最常见情形)时自动部署——CUST 通道(cpuMode=1)迁移落盘 + KFC 通道复制
 *      进 CP1 搜索路径 + 完整性校验(入口符号 + 文件尺寸)——后重试加载
 *      (json 路径: env MF_SDMA_ORCH_JSON, 缺省依次: 安装前缀自动发现 → CWD 的
 *      libmf_sdma_orch_v4.json);
 *   4. Launch/Wait: URMA 同款 KernelArgs+LaunchKernelWithConfig 路径。
 *
 * 生命周期: 多池(多 entity)共享同一份进程级资源, Acquire/Release 引用计数,
 * 归零才真正释放(CP 流销毁/工作区 free/binHandle 卸载)。
 */
#ifndef MEM_FABRIC_HYBRID_HYBM_SDMA_ORCH_PREP_H /* 头文件包含守卫(防重复包含) */
#define MEM_FABRIC_HYBRID_HYBM_SDMA_ORCH_PREP_H /* 同上, 与文件尾 #endif 配对 */

#include <mutex> // 互斥量(串行化单例初始化/清理与引用计数)

#include "dl_acl_api.h"            // ACL 运行时接口封装(dlsym 动态加载 aclrt* 符号)
#include "hybm_common_include.h"   // hybm 公共类型(Result 错误码/日志宏等)
#include "hybm_sdma_contract.h" // host↔kernel wire 契约(HybmSdmaOrchParam/Result)

namespace ock { // MemFabric 总命名空间
namespace mf {  // hybm 内存池引擎子命名空间
/* 工作区布局(总 4KB, 偏移常量; 设备侧镜像见 smem_shm_aicore_sdma.h) */
constexpr uint64_t HYBM_SDMA_ORCH_WS_SIZE = 4096;              /* 工作区总尺寸 */
constexpr uint64_t HYBM_SDMA_ORCH_WS_MAILBOX_OFF = 0;          /* 邮箱 ring: 16B/slot × 64 */
constexpr uint64_t HYBM_SDMA_ORCH_WS_MAILBOX_SLOTS = 64;       /* 邮箱槽数(chunk 数上限) */
constexpr uint64_t HYBM_SDMA_ORCH_WS_SENTINEL_OFF = 1024;      /* 哨兵 8B(SQE 对标志源) */
constexpr uint64_t HYBM_SDMA_ORCH_WS_RESULT_OFF = 1032;        /* kernel result 64B */
constexpr uint64_t HYBM_SDMA_ORCH_WS_OVF_OFF = 1096;           /* SQE overflow 目标 64B */

class SdmaOrchestrator { // 协同模式传输准备单例(所有可变状态在 mutex_ 内维护)
public:
    static SdmaOrchestrator &GetInstance();     /* 进程级单例入口 */

    /* 显式禁用读取(env MF_SDMA_ORCH_DISABLE, 取值 "1"/"on" 为禁用);
     * 310P+SDMA 位默认启用协同(A3 配方必败, 协同为唯一可用链路) */
    static bool Disabled() noexcept;

    /* 引用计数 +1; 首次进入执行全量初始化(CP 流→工作区→kernel)。
     * 任一步失败: 计数回退并返回错误(调用方报错, 不静默), 关键参数见 ERROR 日志 */
    Result Acquire(uint16_t devId);

    /* 引用计数 -1; 归零执行逆序清理(CP 流/工作区/kernel) */
    void Release();

    /* 初始化就绪标志(ready 才可被数据面使用) */
    bool Ready() const noexcept
    {
        return ready_; // 就绪标志只读返回(三步初始化完成即置位)
    }

    /* 引用计数只读查询(UT 断言失败后归零/Release 幂等语义用) */
    uint32_t GetRefCountForTest() const noexcept
    {
        return refCount_; // 引用计数只读返回(仅供 UT 断言)
    }

    /* 查询接口(WP4 设备头文件/WP5 host API 装配 param 用) */
    uint32_t GetSqId() const noexcept
    {
        return sqId_; // CP 流 SQ id(kernel 内活查询用)
    }

    int32_t GetStreamId() const noexcept // CP 流 id(SQE 的 rt_stream_id)
    {
        return streamId_; // CP 流 id 只读返回
    }

    /* CP1 内 HAL devId = die 号(host 设备号 % 2, 0903 定论见计划 A.3) */
    uint32_t GetDieId() const noexcept
    {
        return dieId_; // die 号只读返回(devId%2)
    }

    /* 工作区设备内存基址(uint64, 设备地址) */
    uint64_t GetWorkspace() const noexcept
    {
        return workspace_; // 工作区设备地址只读返回
    }

    /* AICPU 编排 kernel 提交(URMA 同款 KernelArgs+LaunchKernelWithConfig) */
    Result LaunchOrchestrator(const HybmSdmaOrchParam &param);

    /* 等待编排 kernel 完成(SynchronizeStream) */
    Result WaitOrchestrator();

private:
    SdmaOrchestrator() = default; // 私有默认构造(仅 GetInstance 的静态局部实例可构造)
    Result InitOnce(uint16_t devId);       /* 全量初始化入口(锁内) */
    Result InitCpStream(uint16_t devId);   /* CP 流 + sqId/streamId/dieId */
    Result AllocWorkspace();               /* 工作区分配与清零 */
    Result LoadOrchestratorKernel();       /* json 解析 + 设备侧部署探测(KFC), 缺失自动部署后加载 */
    Result TryLoadKernel(const std::string &jsonPath);   /* 单次加载尝试(cpuMode=0, 日志由调用方分层记) */
    Result DeployOrchestratorKernel(const std::string &launchJsonPath, const std::string &dst); /* 自动部署 */
    Result MigrateViaCustChannel(const std::string &dir, uint64_t &soSize); /* CUST 通道迁移落盘(出参 so 尺寸) */
    Result LoadKfcChannel(aclrtFuncHandle &fh); /* KFC 通道装载(临时 json→mode=0 加载→句柄) */
    Result KfcExec(const std::string &cmd); /* KFC 通道执行一条设备 OS 命令(仅部署链内部使用) */
    Result CopyToSearchPath(const std::string &deviceSoName, uint64_t soSize); /* 设备内复制+完整性校验(带重试) */
    void Cleanup();                        /* 逆序清理(锁内, 归零时) */

    std::mutex mutex_;              /* 保护 refCount_ 与初始化/清理串行 */
    uint32_t refCount_{0};          /* 引用计数(entity 池级) */
    uint16_t devId_{0};             /* host 设备号(首次 Acquire 绑定) */
    uint32_t dieId_{0};             /* die 号 = devId_ % 2 */
    uint32_t sqId_{0};              /* CP 流 SQ id(kernel 内活查询用) */
    int32_t streamId_{0};           /* CP 流 id(SQE 的 rt_stream_id) */
    void *cpStream_{nullptr};       /* CP 流句柄(DEVICE_USE_ONLY) */
    void *launchStream_{nullptr};   /* 编排 kernel 提交流(普通流) */
    uint64_t workspace_{0};         /* 工作区设备地址(HUGE_FIRST) */
    aclrtBinHandle binHandle_{nullptr};   /* AICPU kernel 二进制句柄 */
    aclrtFuncHandle funcHandle_{nullptr}; /* HybmSdmaOrchBatch 函数句柄 */
    aclrtBinHandle custBinHandle_{nullptr}; /* CUST 迁移句柄(保活: 持至进程退出, 退出才触发清理链) */
    bool ready_{false};             /* 初始化就绪标志 */
};
} // namespace mf
} // namespace ock

#endif // MEM_FABRIC_HYBRID_HYBM_SDMA_ORCH_PREP_H
