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

#include <algorithm>
#include "smem_common_includes.h"
#include "smem_shm_entry_manager.h"
#include "hybm_big_mem.h"
#include "smem_store_factory.h"
#include "smem_shm_entry.h"

namespace ock {
namespace smem {

SmemShmEntry::SmemShmEntry(uint32_t id) : id_{id}, entity_{nullptr}, gva_{nullptr}
{
    (void)smem_shm_config_init(&extraConfig_);

    auto emptyRollback = []() {};
    initSteps_.emplace_back(ShmEntryInitStep{"01_create_entity", [this]() { return InitStepCreateEntity(); },
                                             [this]() { InitStepDestroyEntity(); }});
    initSteps_.emplace_back(ShmEntryInitStep{"02_reserve_memory", [this]() { return InitStepReserveMemory(); },
                                             [this]() { InitStepUnreserveMemory(); }});
    initSteps_.emplace_back(ShmEntryInitStep{"03_alloc_slice", [this]() { return InitStepAllocSlice(); },
                                             [this]() { InitStepFreeSlice(); }});
    initSteps_.emplace_back(
        ShmEntryInitStep{"04_exchange_slice", [this]() { return InitStepExchangeSlice(); }, emptyRollback});
    initSteps_.emplace_back(
        ShmEntryInitStep{"05_exchange_entity", [this]() { return InitStepExchangeEntity(); }, emptyRollback});
    initSteps_.emplace_back(ShmEntryInitStep{"05_map_memory", [this]() { return InitStepMap(); }, emptyRollback});
}

SmemShmEntry::~SmemShmEntry()
{
    if (globalGroup_ != nullptr) {
        globalGroup_->GroupSnClean();
        globalGroup_ = nullptr;
    }
    uint32_t flags = 0;
    if (entity_ != nullptr && slice_ != nullptr) {
        hybm_free_local_memory(entity_, slice_, 1, flags);
    }

    if (entity_ != nullptr && gva_ != nullptr) {
        hybm_unreserve_mem_space(entity_, 0);
        gva_ = nullptr;
    }

    if (entity_ != nullptr) {
        hybm_destroy_entity(entity_, 0);
        entity_ = nullptr;
    }
}

static void ReleaseAfterFailed(hybm_entity_t entity, hybm_mem_slice_t slice, void *reservedMem)
{
    uint32_t flags = 0;
    if (entity != nullptr && slice != 0) {
        hybm_free_local_memory(entity, slice, 1, flags);
    }

    if (entity != nullptr) {
        hybm_unreserve_mem_space(entity, flags);
    }

    if (entity != nullptr) {
        hybm_destroy_entity(entity, flags);
    }
}

Result SmemShmEntry::CreateGlobalTeam(uint32_t rankSize, uint32_t rankId)
{
    auto client = SmemShmEntryManager::Instance().GetStoreClient();
    SM_VALIDATE_RETURN(client != nullptr, "GetStoreClient failed (store not initialized), shmId: " << id_,
                       SM_INVALID_PARAM);

    std::string prefix = "SHM_(" + std::to_string(id_) + ")_";
    StorePtr store = StoreFactory::PrefixStore(client, prefix);
    SM_VALIDATE_RETURN(store != nullptr, "PrefixStore failed, prefix: " << prefix << " shmId: " << id_, SM_ERROR);

    SmemGroupOption opt = {rankSize, rankId,  extraConfig_.controlOperationTimeout * SECOND_TO_MILLSEC,
                           false,    nullptr, nullptr,
                           nullptr,  nullptr};
    SmemGroupEnginePtr group = SmemNetGroupEngine::Create(store, opt);
    SM_VALIDATE_RETURN(group != nullptr,
                       "SmemNetGroupEngine::Create failed, shmId: " << id_ << " rankSize: " << rankSize
                                                                    << " rankId: " << rankId,
                       SM_ERROR);

    globalGroup_ = group;
    return globalGroup_->GroupBarrier(); // 保证所有rank都初始化了
}

Result SmemShmEntry::Initialize(hybm_options &options)
{
    localRank_ = options.rankId;
    SM_LOG_ERROR_RETURN_IT_IF_NOT_OK(CreateGlobalTeam(options.rankCount, options.rankId), "create global team failed");

    options_ = options;
    for (auto it = initSteps_.begin(); it != initSteps_.end(); ++it) {
        SM_LOG_DEBUG("process init step : " << it->name);
        auto stepRet = it->processor();
        if (stepRet != 0) {
            SM_LOG_ERROR("init step(" << it->name << ") process failed: " << stepRet);
            auto fit = it;
            while (fit != initSteps_.begin()) {
                --fit;
                fit->rollback();
            }
            return stepRet;
        }
    }

    inited_ = true;
    return SM_OK;
}

void SmemShmEntry::SetConfig(const smem_shm_config_t &config)
{
    extraConfig_ = config;
    SM_LOG_INFO("shmId: " << id_ << " set_config control_operation_timeout: " << extraConfig_.controlOperationTimeout);
}

Result SmemShmEntry::SetExtraContext(const void *context, uint32_t size)
{
    if (!inited_ || entity_ == nullptr) {
        SM_LOG_ERROR("smem shm entry has not been initialized");
        return SM_ERROR;
    }

    return hybm_set_extra_context(entity_, context, size);
}

void *SmemShmEntry::GetGva() const
{
    return gva_;
}

uint64_t SmemShmEntry::GetHbmMaxSize() const
{
    return options_.maxHBMSize;
}

int32_t SmemShmEntry::InitStepCreateEntity()
{
    auto entityId = id_ + HYBM_ENTITY_ID_SHM_BASE;
    auto entity = hybm_create_entity(entityId, &options_, 0);
    if (entity == nullptr) {
        SM_LOG_ERROR("hybm_create_entity failed, entityId: " << entityId << " rankId: " << options_.rankId);
        return SM_ERROR;
    }

    entity_ = entity;
    return SM_OK;
}

void SmemShmEntry::InitStepDestroyEntity()
{
    hybm_destroy_entity(entity_, 0);
    entity_ = nullptr;
}

int32_t SmemShmEntry::InitStepReserveMemory()
{
    auto ret = hybm_reserve_mem_space(entity_, 0);
    if (ret != 0) {
        SM_LOG_ERROR("reserve mem failed, result: " << ret);
        return SM_ERROR;
    }

    gva_ = hybm_get_memory_ptr(entity_, HYBM_MEM_TYPE_DEVICE);
    return SM_OK;
}

void SmemShmEntry::InitStepUnreserveMemory()
{
    auto ret = hybm_unreserve_mem_space(entity_, 0);
    if (ret != 0) {
        SM_LOG_WARN("unable to unreserve mem space: " << ret);
    }
    gva_ = nullptr;
}

int32_t SmemShmEntry::InitStepAllocSlice()
{
    auto slice = hybm_alloc_local_memory(entity_, HYBM_MEM_TYPE_DEVICE, options_.deviceVASpace, 0);
    if (slice == nullptr) {
        SM_LOG_ERROR("alloc local mem failed, size: " << options_.deviceVASpace);
        return SM_ERROR;
    }

    slice_ = slice;
    return SM_OK;
}

void SmemShmEntry::InitStepFreeSlice()
{
    auto slice = slice_;
    auto ret = hybm_free_local_memory(entity_, slice, 0, 0);
    if (ret != 0) {
        SM_LOG_WARN("unable to free mem slice: " << ret);
    }
    slice_ = nullptr;
}

int32_t SmemShmEntry::InitStepExchangeSlice()
{
    hybm_exchange_info exInfo;
    bzero(&exInfo, sizeof(exInfo));
    auto ret = hybm_export(entity_, slice_, 0, &exInfo);
    if (ret != 0) {
        SM_LOG_ERROR("hybm export slice failed, result: " << ret);
        return ret;
    }

    hybm_exchange_info *allExInfo = new hybm_exchange_info[options_.rankCount];
    SM_ASSERT_RETURN(allExInfo != nullptr, SM_MALLOC_FAILED);
    ret = globalGroup_->GroupAllGather((char *)&exInfo, sizeof(hybm_exchange_info), (char *)allExInfo,
                                       sizeof(hybm_exchange_info) * options_.rankCount);
    if (ret != 0) {
        SM_LOG_ERROR("hybm gather export slice failed, result: " << ret);
        delete[] allExInfo;
        return ret;
    }

    ret = hybm_import(entity_, allExInfo, options_.rankCount, nullptr, 0);
    if (ret != 0) {
        SM_LOG_ERROR("hybm import failed, result: " << ret);
        delete[] allExInfo;
        return ret;
    }

    ret = globalGroup_->GroupBarrier();
    if (ret != 0) {
        SM_LOG_ERROR("hybm barrier for slice failed, result: " << ret);
    }

    delete[] allExInfo;
    return ret;
}

int32_t SmemShmEntry::InitStepExchangeEntity()
{
    hybm_exchange_info exInfo;
    bzero(&exInfo, sizeof(exInfo));
    auto ret = hybm_export(entity_, nullptr, HYBM_FLAG_EXPORT_ENTITY, &exInfo);
    if (ret != 0) {
        SM_LOG_ERROR("hybm export entity failed, result: " << ret);
        return ret;
    }

    if (exInfo.descLen == 0) {
        return SM_OK;
    }

    hybm_exchange_info *allExInfo = new hybm_exchange_info[options_.rankCount];
    ret = globalGroup_->GroupAllGather((char *)&exInfo, sizeof(hybm_exchange_info), (char *)allExInfo,
                                       sizeof(hybm_exchange_info) * options_.rankCount);
    if (ret != 0) {
        SM_LOG_ERROR("hybm gather export entity failed, result: " << ret);
        delete[] allExInfo;
        return ret;
    }

    ret = hybm_import(entity_, allExInfo, options_.rankCount, nullptr, HYBM_FLAG_EXPORT_ENTITY);
    if (ret != 0) {
        SM_LOG_ERROR("hybm import entity failed, result: " << ret);
        delete[] allExInfo;
        return ret;
    }

    ret = globalGroup_->GroupBarrier();
    if (ret != 0) {
        SM_LOG_ERROR("hybm barrier for entity failed, result: " << ret);
    }

    delete[] allExInfo;
    return ret;
}

int32_t SmemShmEntry::InitStepMap()
{
    auto ret = hybm_mmap(entity_, 0);
    if (ret != 0) {
        SM_LOG_ERROR("hybm mmap failed, result: " << ret);
        return ret;
    }
    return SM_OK;
}

Result SmemShmEntry::GetReachInfo(uint32_t remoteRank, uint32_t &reachInfo) const
{
    if (entity_ == nullptr) {
        SM_LOG_ERROR("entity_ is null, cannot get reach info.");
        return SM_NOT_STARTED;
    }

    hybm_data_op_type reachesTypes;
    auto ret = hybm_entity_reach_types(entity_, remoteRank, reachesTypes, 0);
    if (ret != 0) {
        SM_LOG_ERROR("hybm_entity_reach_types failed, remoteRank: " << remoteRank << " ret: " << ret);
        return SM_ERROR;
    }

    reachInfo = 0U;
    if (reachesTypes & HYBM_DOP_TYPE_MTE) {
        reachInfo |= SMEMS_DATA_OP_MTE;
    }

    if (reachesTypes & HYBM_DOP_TYPE_SDMA) {
        reachInfo |= SMEMS_DATA_OP_SDMA;
    }

    if (reachesTypes & HYBM_DOP_TYPE_AIV_SDMA) {
        reachInfo |= SMEMS_DATA_OP_SDMA;
    }

    if (reachesTypes & HYBM_DOP_TYPE_DEVICE_RDMA) {
        reachInfo |= SMEMS_DATA_OP_RDMA;
    }

    return SM_OK;
}

/* ---- 310P 协同模式转发(实现细节见 hybm_sdma_capi.cpp) ---- */
Result SmemShmEntry::SdmaSubmit(void *dstGva, void *flagDoneGva, uint32_t chunks) // 流水提交
{
    if (entity_ == nullptr) { // 底层 entity 未建
        SM_LOG_ERROR("overlap launch failed: entity is null, shmId: " << id_); // 根因日志: shmId
        return SM_NOT_INITIALIZED; // 未初始化
    }
    // 统一走流水提交(chunks=1 即单块语义)
    auto ret = hybm_sdma_submit(reinterpret_cast<uint64_t>(dstGva), reinterpret_cast<uint64_t>(flagDoneGva),
                                   chunks);
    if (ret != 0) { // hybm 提交失败
        // 根因日志: ret/shmId/dstGva/chunks
        SM_LOG_ERROR("overlap submit failed, ret: " << ret << " shmId: " << id_ << " dstGva: " << dstGva
                                                    << " chunks: " << chunks);
        return SM_ERROR; // 透传失败(根因已记录)
    }
    return SM_OK; // 提交成功
}

Result SmemShmEntry::SdmaWait() // 等待搬运完成
{
    auto ret = hybm_sdma_wait(); // 转发 hybm C API
    if (ret != 0) { // 等待失败
        SM_LOG_ERROR("hybm_sdma_wait failed, ret: " << ret << " shmId: " << id_); // 根因日志: ret/shmId
        return SM_ERROR; // 透传失败(根因已记录)
    }
    return SM_OK; // 等待成功
}

Result SmemShmEntry::SdmaGetResult(uint32_t &mainRet, uint32_t &stage, uint32_t &headAfter) // 诊断读回
{
    auto ret = hybm_sdma_get_result(&mainRet, &stage, &headAfter); // 转发 hybm C API
    if (ret != 0) { // 读取失败
        SM_LOG_ERROR("hybm_sdma_get_result failed, ret: " << ret << " shmId: " << id_); // 根因日志: ret/shmId
        return SM_ERROR; // 透传失败(根因已记录)
    }
    return SM_OK; // 读取成功
}

void *SmemShmEntry::SdmaGetWorkspace() const // 工作区查询
{
    hybm_sdma_info_t info = {}; // 就绪信息快照(零初始化)
    if (hybm_sdma_get_info(&info) != 0) { // 未就绪(开关关/初始化失败)
        return nullptr; /* 未就绪(开关关/初始化失败), 上层以 nullptr 判定 */
    }
    return reinterpret_cast<void *>(info.workspace); // 返回工作区设备地址
}

} // namespace smem
} // namespace ock
