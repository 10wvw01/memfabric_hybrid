/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
*/
#ifndef __SMEM_SHM_ENTRY_H__
#define __SMEM_SHM_ENTRY_H__

#include <map>
#include <vector>
#include <string>
#include <functional>
#include "hybm_def.h"
#include "smem.h"
#include "smem_shm.h"
#include "smem_net_group_engine.h"

namespace ock {
namespace smem {

struct ShmEntryInitStep {
    std::string name;
    std::function<int32_t()> processor;
    std::function<void()> rollback;
    ShmEntryInitStep(std::string nm, std::function<int32_t()> p, std::function<void()> r)
        : name{std::move(nm)}, processor{std::move(p)}, rollback{std::move(r)}
    {}
};

class SmemShmEntry : public SmReferable {
public:
    explicit SmemShmEntry(uint32_t id);
    ~SmemShmEntry() override;

    int32_t Initialize(hybm_options &options);

    void SetConfig(const smem_shm_config_t &config);

    Result SetExtraContext(const void *context, uint32_t size);

    void *GetGva() const;

    uint64_t GetHbmMaxSize() const;

    SmemGroupEnginePtr GetGroup() const;

    uint32_t Id() const;

    Result GetReachInfo(uint32_t remoteRank, uint32_t &reachInfo) const;

    /* ---- 310P AICore×AICPU 协同模式(转发 hybm C API, 见 smem_shm.h 的 overlap 族) ---- */
    /* 提交一次流水搬运(邮箱 ring 逐 chunk → SQE 对 → 每 chunk doorbell; chunks=1 即单块) */
    Result SdmaSubmit(void *dstGva, void *flagDoneGva, uint32_t chunks);
    /* 等待协同搬运完成 */
    Result SdmaWait();
    /* 读取最近一次搬运的 kernel 结果(诊断) */
    Result SdmaGetResult(uint32_t &mainRet, uint32_t &stage, uint32_t &headAfter);
    /* 查询协同工作区地址(host 侧传给 AICore kernel 参数用); 未就绪返回 nullptr */
    void *SdmaGetWorkspace() const;

private:
    Result CreateGlobalTeam(uint32_t rankSize, uint32_t rankId);
    int32_t InitStepCreateEntity();
    void InitStepDestroyEntity();
    int32_t InitStepReserveMemory();
    void InitStepUnreserveMemory();
    int32_t InitStepAllocSlice();
    void InitStepFreeSlice();
    int32_t InitStepExchangeSlice();
    int32_t InitStepExchangeEntity();
    int32_t InitStepMap();

private:
    hybm_options options_{};
    std::vector<ShmEntryInitStep> initSteps_;
    SmemGroupEnginePtr globalGroup_ = nullptr;
    smem_shm_config_t extraConfig_;

    bool inited_ = false;
    const uint32_t id_;
    uint32_t localRank_ = UINT32_MAX;
    hybm_entity_t entity_ = nullptr;
    void *gva_ = nullptr;
    hybm_mem_slice_t slice_ = nullptr;

    std::mutex entryMutex_;
};
using SmemShmEntryPtr = SmRef<SmemShmEntry>;

inline SmemGroupEnginePtr SmemShmEntry::GetGroup() const
{
    return globalGroup_;
}

inline uint32_t SmemShmEntry::Id() const
{
    return id_;
}
} // namespace smem
} // namespace ock

#endif // __SMEM_SHM_ENTRY_H__
