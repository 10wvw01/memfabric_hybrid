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

/* hybm_overlap_prep.h — 310P AICore×AICPU 协同模式传输准备(进程级单例 + 引用计数)
 *
 * 挂载点: MemEntityDefault::InitTransManager 的 [310P + SDMA op 位 + 开关] 分支
 * (六级初始化地图的 L2-④, 见 310p-aicore-aicpu-开发计划.md D4)。
 *
 * 职责(P0 三实验已逐项实证, 见 310p-capability_matrix/17_aicore_aicpu_overlap/):
 *   1. CP 流: ACL_STREAM_DEVICE_USE_ONLY(其 SQ 注册进 CP1 进程 trs 表, 是 AICPU
 *      kernel 内 halSqCqQuery 可查可投的前提, X3/A.3);
 *   2. 工作区: mailbox/sentinel/result/ovf, 普通设备内存 HUGE_FIRST(NORMAL_ONLY 落
 *      特殊段 SDMA 不可翻译, A.4-7), AICore 可直接 GM 访问(E0c 结论);
 *   3. kernel 加载: BinaryLoadFromFile(cpuMode=0)+GetFunction(json 路径:
 *      env MF_310P_OVERLAP_JSON, 缺省 CWD 的 libmf_overlap_v1.json);
 *   4. 冒烟探测(D2): E0a 式单发自证; 不过=功能不可用报错, 不静默降级;
 *   5. Launch/Wait: URMA 同款 KernelArgs+LaunchKernelWithConfig 路径。
 *
 * 生命周期: 多池(多 entity)共享同一份进程级资源, Acquire/Release 引用计数,
 * 归零才真正释放(CP 流销毁/工作区 free/binHandle 卸载)。
 */
#ifndef MEM_FABRIC_HYBRID_HYBM_OVERLAP_PREP_H
#define MEM_FABRIC_HYBRID_HYBM_OVERLAP_PREP_H

#include <mutex>

#include "dl_acl_api.h"
#include "hybm_common_include.h"
#include "hybm_overlap_contract.h"

namespace ock {
namespace mf {
/* 工作区布局(总 4KB, 偏移常量; 设备侧镜像见 smem_shm_aicore_overlap.h) */
constexpr uint64_t HYBM_OVERLAP_WS_SIZE = 4096;              /* 工作区总尺寸 */
constexpr uint64_t HYBM_OVERLAP_WS_MAILBOX_OFF = 0;          /* 邮箱 ring: 16B/slot × 64 */
constexpr uint64_t HYBM_OVERLAP_WS_MAILBOX_SLOTS = 64;       /* 邮箱槽数(mode14 chunk 上限) */
constexpr uint64_t HYBM_OVERLAP_WS_SENTINEL_OFF = 1024;      /* 哨兵 8B(SQE 对标志源) */
constexpr uint64_t HYBM_OVERLAP_WS_RESULT_OFF = 1032;        /* kernel result 64B */
constexpr uint64_t HYBM_OVERLAP_WS_OVF_OFF = 1096;           /* SQE overflow 目标 64B */
/* 冒烟自证用哨兵魔数(与 P0 探针同值, 不进入生产数据面) */
constexpr uint64_t HYBM_OVERLAP_SMOKE_SENTINEL = 0xC0FFEE11ULL;

class HybmOverlapPrep {
public:
    static HybmOverlapPrep &GetInstance();     /* 进程级单例入口 */

    /* 开关读取(env MF_310P_SHM_OVERLAP, 取值 "1"/"on" 为开) */
    static bool SwitchOn() noexcept;

    /* 引用计数 +1; 首次进入执行全量初始化(CP 流→工作区→kernel→冒烟)。
     * 任一步失败: 计数回退并返回错误(调用方报错, 不静默), 关键参数见 ERROR 日志 */
    Result Acquire(uint16_t devId);

    /* 引用计数 -1; 归零执行逆序清理(CP 流/工作区/kernel) */
    void Release();

    /* 冒烟通过标志(ready 才可被数据面使用) */
    bool Ready() const noexcept
    {
        return ready_;
    }

    /* 引用计数只读查询(UT 断言失败后归零/Release 幂等语义用) */
    uint32_t GetRefCountForTest() const noexcept
    {
        return refCount_;
    }

    /* 查询接口(WP4 设备头文件/WP5 host API 装配 param 用) */
    uint32_t GetSqId() const noexcept
    {
        return sqId_;
    }

    int32_t GetStreamId() const noexcept
    {
        return streamId_;
    }

    /* CP1 内 HAL devId = die 号(host 设备号 % 2, 0903 定论见计划 A.3) */
    uint32_t GetDieId() const noexcept
    {
        return dieId_;
    }

    /* 工作区设备内存基址(uint64, 设备地址) */
    uint64_t GetWorkspace() const noexcept
    {
        return workspace_;
    }

    /* AICPU 编排 kernel 提交(URMA 同款 KernelArgs+LaunchKernelWithConfig) */
    Result LaunchOrchestrator(const HybmOverlapParam &param);

    /* 等待编排 kernel 完成(SynchronizeStream) */
    Result WaitOrchestrator();

private:
    HybmOverlapPrep() = default;
    Result InitOnce(uint16_t devId);       /* 全量初始化入口(锁内) */
    Result InitCpStream(uint16_t devId);   /* CP 流 + sqId/streamId/dieId */
    Result AllocWorkspace();               /* 工作区分配与清零 */
    Result LoadOrchestratorKernel();       /* json 路径解析 + BinaryLoad + GetFunction */
    Result SmokeTest();                    /* E0a 式单发自证(D2) */
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
    aclrtFuncHandle funcHandle_{nullptr}; /* HybmOverlapRun 函数句柄 */
    bool ready_{false};             /* 冒烟通过标志 */
};
} // namespace mf
} // namespace ock

#endif // MEM_FABRIC_HYBRID_HYBM_OVERLAP_PREP_H
