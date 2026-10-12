/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 * You may use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

#include "hybm_data_op_acl_memcpy.h"

#include "hybm_define.h"
#include "hybm_logger.h"
#include "hybm_data_op.h"
#include "hybm_va_manager.h"
#include "hybm_stream_manager.h"

namespace ock {
namespace mf {

static hybm_mem_type DirectionSrcMemType[HYBM_DATA_COPY_DIRECTION_BUTT] = {
    HYBM_MEM_TYPE_HOST,   // 0  LH2GH
    HYBM_MEM_TYPE_HOST,   // 1  LH2GD
    HYBM_MEM_TYPE_DEVICE, // 2  LD2GH
    HYBM_MEM_TYPE_DEVICE, // 3  LD2GD
    HYBM_MEM_TYPE_HOST,   // 4  GH2GH
    HYBM_MEM_TYPE_HOST,   // 5  GH2GD
    HYBM_MEM_TYPE_HOST,   // 6  GH2LH
    HYBM_MEM_TYPE_HOST,   // 7  GH2LD
    HYBM_MEM_TYPE_DEVICE, // 8  GD2GH
    HYBM_MEM_TYPE_DEVICE, // 9  GD2GD
    HYBM_MEM_TYPE_DEVICE, // 10 GD2LH
    HYBM_MEM_TYPE_DEVICE, // 11 GD2LD
};
static hybm_mem_type DirectionDestMemType[HYBM_DATA_COPY_DIRECTION_BUTT] = {
    HYBM_MEM_TYPE_HOST,   // 0  LH2GH
    HYBM_MEM_TYPE_DEVICE, // 1  LH2GD
    HYBM_MEM_TYPE_HOST,   // 2  LD2GH
    HYBM_MEM_TYPE_DEVICE, // 3  LD2GD
    HYBM_MEM_TYPE_HOST,   // 4  GH2GH
    HYBM_MEM_TYPE_DEVICE, // 5  GH2GD
    HYBM_MEM_TYPE_HOST,   // 6  GH2LH
    HYBM_MEM_TYPE_DEVICE, // 7  GH2LD
    HYBM_MEM_TYPE_HOST,   // 8  GD2GH
    HYBM_MEM_TYPE_DEVICE, // 9  GD2GD
    HYBM_MEM_TYPE_HOST,   // 10 GD2LH
    HYBM_MEM_TYPE_DEVICE, // 11 GD2LD
};

HostDataOpAclMemcpy::~HostDataOpAclMemcpy()
{
    UnInitialize();
}

Result HostDataOpAclMemcpy::Initialize() noexcept
{
    if (inited_) {
        return BM_OK;
    }
    auto ret = DlAclApi::AclrtCreateStream(&stream_);
    if (ret != BM_OK) {
        BM_LOG_ERROR("AclrtCreateStream failed, ret:" << ret << " rankId:" << rankId_);
        return ret;
    }
    inited_ = true;
    return BM_OK;
}

void HostDataOpAclMemcpy::UnInitialize() noexcept
{
    if (stream_ != nullptr) {
        auto ret = DlAclApi::AclrtDestroyStream(stream_);
        if (ret != BM_OK) {
            BM_LOG_ERROR("AclrtDestroyStream failed, ret:" << ret << " rankId:" << rankId_);
        }
        stream_ = nullptr;
    }
    inited_ = false;
}

bool HostDataOpAclMemcpy::IsD2DDirection(hybm_data_copy_direction direction) noexcept
{
    switch (direction) {
        case HYBM_LOCAL_DEVICE_TO_GLOBAL_DEVICE:
        case HYBM_GLOBAL_DEVICE_TO_LOCAL_DEVICE:
        case HYBM_GLOBAL_DEVICE_TO_GLOBAL_DEVICE:
            return true;
        default:
            return false;
    }
}

bool HostDataOpAclMemcpy::IsH2DDirection(hybm_data_copy_direction direction) noexcept
{
    switch (direction) {
        case HYBM_LOCAL_HOST_TO_GLOBAL_DEVICE:
        case HYBM_LOCAL_HOST_TO_GLOBAL_HOST:
        case HYBM_GLOBAL_HOST_TO_LOCAL_DEVICE:
        case HYBM_GLOBAL_HOST_TO_GLOBAL_DEVICE:
        case HYBM_GLOBAL_HOST_TO_GLOBAL_HOST:
            return true;
        default:
            return false;
    }
}

bool HostDataOpAclMemcpy::IsD2HDirection(hybm_data_copy_direction direction) noexcept
{
    switch (direction) {
        case HYBM_GLOBAL_DEVICE_TO_LOCAL_HOST:
        case HYBM_GLOBAL_HOST_TO_LOCAL_HOST:
        case HYBM_LOCAL_DEVICE_TO_GLOBAL_HOST:
        case HYBM_GLOBAL_DEVICE_TO_GLOBAL_HOST:
            return true;
        default:
            return false;
    }
}

Result HostDataOpAclMemcpy::SubmitD2D(void *dest, const void *src, uint64_t length) noexcept
{
    auto ret = DlAclApi::AclrtMemcpyAsync(dest, length, src, length, ACL_MEMCPY_DEVICE_TO_DEVICE, stream_);
    if (ret != BM_OK) {
        BM_LOG_WARN("AclrtMemcpyAsync(D2D) failed, ret:" << ret << " rankId:" << rankId_ << " src:" << src
                                                         << " dst:" << dest << " size:" << length << ", retry sync");
        ret = DlAclApi::AclrtMemcpy(dest, length, src, length, ACL_MEMCPY_DEVICE_TO_DEVICE);
        if (ret != BM_OK) {
            BM_LOG_ERROR("AclrtMemcpy(D2D fallback) failed, ret:" << ret << " rankId:" << rankId_ << " src:" << src
                                                                  << " dst:" << dest << " size:" << length);
        }
    }
    return ret;
}

Result HostDataOpAclMemcpy::SyncStream() noexcept
{
    auto ret = DlAclApi::AclrtSynchronizeStream(stream_);
    if (ret == BM_OK) {
        return BM_OK;
    }
    BM_LOG_ERROR("AclrtSynchronizeStream failed, ret:" << ret << " rankId:" << rankId_);
    // 同步失败意味着提交或执行出错, stream 可能进入持久错误态:
    // 重建 stream 恢复算子可用性, 否则后续所有 D2D 都返回历史错误
    (void)DlAclApi::AclrtDestroyStream(stream_);
    stream_ = nullptr;
    auto createRet = DlAclApi::AclrtCreateStream(&stream_);
    if (createRet != BM_OK) {
        BM_LOG_ERROR("AclrtCreateStream(recover) failed, ret:" << createRet << " rankId:" << rankId_);
        inited_ = false;
    }
    return ret;
}

Result HostDataOpAclMemcpy::CopyD2D(void *dest, const void *src, uint64_t length) noexcept
{
    auto ret = SubmitD2D(dest, src, length);
    if (ret != BM_OK) {
        return ret;
    }
    ret = SyncStream();
    if (ret != BM_OK) {
        BM_LOG_WARN("D2D async+sync failed, retry sync AclrtMemcpy, rankId:" << rankId_);
        ret = DlAclApi::AclrtMemcpy(dest, length, src, length, ACL_MEMCPY_DEVICE_TO_DEVICE);
        if (ret != BM_OK) {
            BM_LOG_ERROR("AclrtMemcpy(D2D fallback) failed, ret:" << ret << " rankId:" << rankId_ << " src:" << src
                                                                  << " dst:" << dest << " size:" << length);
        }
    }
    return ret;
}

Result HostDataOpAclMemcpy::CopyH2D(void *dest, const void *src, uint64_t length) noexcept
{
    auto ret = DlAclApi::AclrtMemcpyAsync(dest, length, src, length, ACL_MEMCPY_HOST_TO_DEVICE, stream_);
    if (ret != BM_OK) {
        BM_LOG_WARN("AclrtMemcpyAsync(H2D) failed, ret:" << ret << " rankId:" << rankId_ << " dst:" << dest
                                                         << " size:" << length << ", retry sync");
        ret = DlAclApi::AclrtMemcpy(dest, length, src, length, ACL_MEMCPY_HOST_TO_DEVICE);
        if (ret != BM_OK) {
            BM_LOG_ERROR("AclrtMemcpy(H2D fallback) failed, ret:" << ret << " rankId:" << rankId_ << " dst:" << dest
                                                                  << " size:" << length);
        }
        return ret;
    }
    ret = SyncStream();
    if (ret != BM_OK) {
        BM_LOG_WARN("H2D async+sync failed, retry sync AclrtMemcpy, rankId:" << rankId_);
        ret = DlAclApi::AclrtMemcpy(dest, length, src, length, ACL_MEMCPY_HOST_TO_DEVICE);
        if (ret != BM_OK) {
            BM_LOG_ERROR("AclrtMemcpy(H2D fallback) failed, ret:" << ret << " rankId:" << rankId_ << " dst:" << dest
                                                                  << " size:" << length);
        }
    }
    return ret;
}

Result HostDataOpAclMemcpy::CopyD2H(void *dest, const void *src, uint64_t length) noexcept
{
    int32_t curDevice = -1;
    DlAclApi::AclrtGetDevice(&curDevice);
    auto ret = DlAclApi::AclrtMemcpyAsync(dest, length, src, length, ACL_MEMCPY_DEVICE_TO_HOST, stream_);
    if (ret != BM_OK) {
        BM_LOG_WARN("AclrtMemcpyAsync(D2H) failed, ret:" << ret << " rankId:" << rankId_ << " src:" << src
                                                         << " size:" << length << " curDevice:" << curDevice
                                                         << " stream:" << stream_ << ", retry sync");
        ret = DlAclApi::AclrtMemcpy(dest, length, src, length, ACL_MEMCPY_DEVICE_TO_HOST);
        if (ret != BM_OK) {
            BM_LOG_ERROR("AclrtMemcpy(D2H fallback) failed, ret:" << ret << " rankId:" << rankId_ << " src:" << src
                                                                  << " size:" << length << " curDevice:" << curDevice);
        }
        return ret;
    }
    ret = SyncStream();
    if (ret != BM_OK) {
        BM_LOG_WARN("D2H async+sync failed, retry sync AclrtMemcpy, rankId:" << rankId_);
        ret = DlAclApi::AclrtMemcpy(dest, length, src, length, ACL_MEMCPY_DEVICE_TO_HOST);
        if (ret != BM_OK) {
            BM_LOG_ERROR("AclrtMemcpy(D2H fallback) failed, ret:" << ret << " rankId:" << rankId_ << " src:" << src
                                                                  << " size:" << length);
        }
    }
    return ret;
}

Result HostDataOpAclMemcpy::CopyByDirection(void *dest, const void *src, uint64_t length,
                                            hybm_data_copy_direction direction) noexcept
{
    if (IsD2DDirection(direction)) {
        return CopyD2D(dest, src, length);
    }
    if (IsH2DDirection(direction)) {
        return CopyH2D(dest, src, length);
    }
    if (IsD2HDirection(direction)) {
        return CopyD2H(dest, src, length);
    }
    BM_LOG_ERROR("unsupported direction:" << direction << " rankId:" << rankId_);
    return BM_INVALID_PARAM;
}

Result HostDataOpAclMemcpy::DataCopy(hybm_copy_params &params, hybm_data_copy_direction direction,
                                     const ExtOptions &options) noexcept
{
    BM_ASSERT_LOG_AND_RETURN(inited_, "inited_ = " << inited_, BM_NOT_INITIALIZED);
    if (options.flags & ASYNC_COPY_FLAG || options.stream != nullptr) {
        return DataCopyAsync(params, direction, options);
    }
    TransformVa(params.src, params.dest, direction);
    return CopyByDirection(params.dest, params.src, params.dataSize, direction);
}

Result HostDataOpAclMemcpy::BatchCopyD2D(hybm_batch_copy_params &params) noexcept
{
    // 同一 stream 上 async 操作保序: 先批量提交, 末尾统一同步一次(代替逐项同步)
    for (uint32_t i = 0; i < params.batchSize; ++i) {
        auto ret = SubmitD2D(params.destinations[i], params.sources[i], params.dataSizes[i]);
        if (ret != BM_OK) {
            BM_LOG_ERROR("batch submit failed, ret:" << ret << " index:" << i << " rankId:" << rankId_
                                                     << " size:" << params.dataSizes[i]);
            // 已提交的前 i 项仍悬在流上: 补一次同步清空未决任务(顺带触发失败自恢复重建),
            // 否则返回失败时流带未决操作, 后续 DestroyStream 会报错
            (void)SyncStream();
            return ret;
        }
    }
    auto ret = SyncStream();
    if (ret != BM_OK) {
        BM_LOG_WARN("batch D2D async+sync failed, retry sync per-item, rankId:" << rankId_);
        for (uint32_t i = 0; i < params.batchSize; ++i) {
            ret = DlAclApi::AclrtMemcpy(params.destinations[i], params.dataSizes[i], params.sources[i],
                                        params.dataSizes[i], ACL_MEMCPY_DEVICE_TO_DEVICE);
            if (ret != BM_OK) {
                BM_LOG_ERROR("batch D2D sync fallback failed, ret:" << ret << " index:" << i << " rankId:" << rankId_
                                                                    << " size:" << params.dataSizes[i]);
                return ret;
            }
        }
    }
    return BM_OK;
}

Result HostDataOpAclMemcpy::BatchCopyNonD2D(hybm_batch_copy_params &params, int32_t kind) noexcept
{
    /* same pattern as BatchCopyD2D: batch submit then single SyncStream,
     * with per-item sync fallback on stream sync failure */
    for (uint32_t i = 0; i < params.batchSize; ++i) {
        auto ret = DlAclApi::AclrtMemcpyAsync(params.destinations[i], params.dataSizes[i], params.sources[i],
                                              params.dataSizes[i], kind, stream_);
        if (ret != BM_OK) {
            BM_LOG_ERROR("batch submit failed, ret:" << ret << " index:" << i << " rankId:" << rankId_
                                                     << " size:" << params.dataSizes[i]);
            (void)SyncStream();
            return ret;
        }
    }
    auto ret = SyncStream();
    if (ret != BM_OK) {
        BM_LOG_WARN("batch copy async+sync failed, retry sync per-item, rankId:" << rankId_);
        for (uint32_t i = 0; i < params.batchSize; ++i) {
            ret = DlAclApi::AclrtMemcpy(params.destinations[i], params.dataSizes[i], params.sources[i],
                                        params.dataSizes[i], kind);
            if (ret != BM_OK) {
                BM_LOG_ERROR("batch copy sync fallback failed, ret:" << ret << " index:" << i << " rankId:" << rankId_
                                                                     << " size:" << params.dataSizes[i]);
                return ret;
            }
        }
    }
    return BM_OK;
}

Result HostDataOpAclMemcpy::BatchDataCopy(hybm_batch_copy_params &params, hybm_data_copy_direction direction,
                                          const ExtOptions &options) noexcept
{
    BM_ASSERT_LOG_AND_RETURN(inited_, "inited_ = " << inited_, BM_NOT_INITIALIZED);
    for (uint32_t i = 0; i < params.batchSize; i++) {
        TransformVa(params.sources[i], params.destinations[i], direction);
    }
    if (IsD2DDirection(direction)) {
        return BatchCopyD2D(params);
    }
    if (IsH2DDirection(direction)) {
        return BatchCopyNonD2D(params, ACL_MEMCPY_HOST_TO_DEVICE);
    }
    if (IsD2HDirection(direction)) {
        return BatchCopyNonD2D(params, ACL_MEMCPY_DEVICE_TO_HOST);
    }
    BM_LOG_ERROR("BatchDataCopy unsupported direction:" << direction << " rankId:" << rankId_);
    return BM_INVALID_PARAM;
}

Result HostDataOpAclMemcpy::DataCopyAsync(hybm_copy_params &params, hybm_data_copy_direction direction,
                                          const ExtOptions &options) noexcept
{
    BM_ASSERT_LOG_AND_RETURN(inited_, "inited_ = " << inited_, BM_NOT_INITIALIZED);
    TransformVa(params.src, params.dest, direction);
    if (options.stream != nullptr) {
        if (IsD2DDirection(direction)) {
            return DlAclApi::AclrtMemcpyAsync(params.dest, params.dataSize, params.src, params.dataSize,
                                              ACL_MEMCPY_DEVICE_TO_DEVICE, options.stream);
        }
        if (IsH2DDirection(direction)) {
            return DlAclApi::AclrtMemcpyAsync(params.dest, params.dataSize, params.src, params.dataSize,
                                              ACL_MEMCPY_HOST_TO_DEVICE, options.stream);
        }
        if (IsD2HDirection(direction)) {
            return DlAclApi::AclrtMemcpyAsync(params.dest, params.dataSize, params.src, params.dataSize,
                                              ACL_MEMCPY_DEVICE_TO_HOST, options.stream);
        }
        BM_LOG_ERROR("DataCopyAsync unsupported direction with user stream, direction:" << direction
                                                                                         << " rankId:" << rankId_);
        return BM_INVALID_PARAM;
    }
    /* no user stream: submit to internal stream_ without per-item sync,
     * then let InnerWait/Wait do a single SyncStream (batch submit → single sync) */
    int32_t kind = ACL_MEMCPY_DEVICE_TO_DEVICE;
    if (IsD2DDirection(direction)) {
        kind = ACL_MEMCPY_DEVICE_TO_DEVICE;
    } else if (IsH2DDirection(direction)) {
        kind = ACL_MEMCPY_HOST_TO_DEVICE;
    } else if (IsD2HDirection(direction)) {
        kind = ACL_MEMCPY_DEVICE_TO_HOST;
    } else {
        BM_LOG_ERROR("DataCopyAsync unsupported direction:" << direction << " rankId:" << rankId_);
        return BM_INVALID_PARAM;
    }
    auto ret = DlAclApi::AclrtMemcpyAsync(params.dest, params.dataSize, params.src, params.dataSize, kind, stream_);
    if (ret != BM_OK) {
        BM_LOG_ERROR("DataCopyAsync submit failed, ret:" << ret << " rankId:" << rankId_);
        return ret;
    }
    return InnerWait(options, 0);
}

Result HostDataOpAclMemcpy::Wait(int32_t waitId) noexcept
{
    (void)waitId;
    if (stream_ == nullptr) {
        return BM_OK;
    }
    auto ret = DlAclApi::AclrtSynchronizeStream(stream_);
    if (ret != BM_OK) {
        BM_LOG_WARN("Wait sync stream failed, ret:" << ret << " rankId:" << rankId_);
        (void)SyncStream();
    }
    return ret;
}

void HostDataOpAclMemcpy::TransformVa(void *&src, void *&dst, hybm_data_copy_direction direction) noexcept
{
    /* validate direction before indexing DirectionSrcMemType/DirectionDestMemType arrays,
     * which are sized HYBM_DATA_COPY_DIRECTION_BUTT. Invalid direction is caught by
     * CopyByDirection/IsXxxDirection downstream, but we must not read out-of-bounds here. */
    if (direction >= HYBM_DATA_COPY_DIRECTION_BUTT) {
        BM_LOG_ERROR("invalid direction:" << direction << " rankId:" << rankId_);
        return;
    }
    uint32_t srcType = (DirectionSrcMemType[direction] == HYBM_MEM_TYPE_HOST) ? HVM_HVA : HVM_DVA;
    uint32_t dstType = (DirectionDestMemType[direction] == HYBM_MEM_TYPE_HOST) ? HVM_HVA : HVM_DVA;

    if (src != nullptr) {
        auto out = HybmVaManager::GetInstance().TransformVa(reinterpret_cast<uint64_t>(src), HVM_GVA, srcType);
        src = (out != 0) ? reinterpret_cast<void *>(out) : src;
    }
    if (dst != nullptr) {
        auto out = HybmVaManager::GetInstance().TransformVa(reinterpret_cast<uint64_t>(dst), HVM_GVA, dstType);
        dst = (out != 0) ? reinterpret_cast<void *>(out) : dst;
    }
}

void HostDataOpAclMemcpy::CleanUp() noexcept
{
    /* AclMemcpy 使用自管理 stream, 不持有 HybmStreamManager 资源, 无需清理 */
}

Result HostDataOpAclMemcpy::InnerWait(const ExtOptions &options, int32_t waitId) noexcept
{
    if (options.flags & ASYNC_COPY_FLAG) {
        return BM_OK;
    }
    if (options.stream != nullptr) {
        auto ret = DlAclApi::AclrtSynchronizeStream(options.stream);
        if (ret != BM_OK) {
            BM_LOG_ERROR("InnerWait sync stream failed, ret:" << ret << " rankId:" << rankId_);
        }
        return ret;
    }
    return Wait(waitId);
}

} // namespace mf
} // namespace ock
