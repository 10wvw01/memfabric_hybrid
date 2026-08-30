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

namespace ock {
namespace mf {
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

bool HostDataOpAclMemcpy::IsSupportedDirection(hybm_data_copy_direction direction) const noexcept
{
    switch (direction) {
        case HYBM_LOCAL_DEVICE_TO_GLOBAL_DEVICE:
        case HYBM_GLOBAL_DEVICE_TO_LOCAL_DEVICE:
        case HYBM_GLOBAL_DEVICE_TO_GLOBAL_DEVICE:
        case HYBM_LOCAL_HOST_TO_GLOBAL_DEVICE:
        case HYBM_GLOBAL_DEVICE_TO_LOCAL_HOST:
            return true;
        default:
            return false;
    }
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

Result HostDataOpAclMemcpy::SubmitD2D(void *dest, const void *src, uint64_t length) noexcept
{
    // 同步 D2D 跨 die 返回 507899 不可用, 必须 async 提交(P0 实测 F15)
    auto ret = DlAclApi::AclrtMemcpyAsync(dest, length, src, length, ACL_MEMCPY_DEVICE_TO_DEVICE, stream_);
    if (ret != BM_OK) {
        BM_LOG_ERROR("AclrtMemcpyAsync(D2D) failed, ret:" << ret << " rankId:" << rankId_ << " src:" << src
                                                          << " dst:" << dest << " size:" << length);
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

Result HostDataOpAclMemcpy::CopyDeviceToDevice(void *dest, const void *src, uint64_t length) noexcept
{
    auto ret = SubmitD2D(dest, src, length);
    if (ret != BM_OK) {
        return ret;
    }
    return SyncStream();
}

Result HostDataOpAclMemcpy::CopyByDirection(void *dest, const void *src, uint64_t length,
                                            hybm_data_copy_direction direction) noexcept
{
    switch (direction) {
        case HYBM_LOCAL_DEVICE_TO_GLOBAL_DEVICE:
        case HYBM_GLOBAL_DEVICE_TO_LOCAL_DEVICE:
        case HYBM_GLOBAL_DEVICE_TO_GLOBAL_DEVICE:
            return CopyDeviceToDevice(dest, src, length);
        case HYBM_LOCAL_HOST_TO_GLOBAL_DEVICE: {
            auto ret = DlAclApi::AclrtMemcpy(dest, length, src, length, ACL_MEMCPY_HOST_TO_DEVICE);
            if (ret != BM_OK) {
                BM_LOG_ERROR("AclrtMemcpy(H2D) failed, ret:" << ret << " rankId:" << rankId_ << " dst:" << dest
                                                             << " size:" << length);
            }
            return ret;
        }
        case HYBM_GLOBAL_DEVICE_TO_LOCAL_HOST: {
            auto ret = DlAclApi::AclrtMemcpy(dest, length, src, length, ACL_MEMCPY_DEVICE_TO_HOST);
            if (ret != BM_OK) {
                BM_LOG_ERROR("AclrtMemcpy(D2H) failed, ret:" << ret << " rankId:" << rankId_ << " src:" << src
                                                             << " size:" << length);
            }
            return ret;
        }
        default:
            BM_LOG_ERROR("unsupported direction:" << direction << " rankId:" << rankId_);
            return BM_INVALID_PARAM;
    }
}

Result HostDataOpAclMemcpy::DataCopy(hybm_copy_params &params, hybm_data_copy_direction direction,
                                     const ExtOptions &options) noexcept
{
    (void)options;
    BM_ASSERT_LOG_AND_RETURN(inited_, "inited_ = " << inited_, BM_NOT_INITIALIZED);
    if (!IsSupportedDirection(direction)) {
        BM_LOG_ERROR("data copy invalid direction for acl memcpy:" << direction << " rankId:" << rankId_
               << " size:" << params.dataSize);
        return BM_INVALID_PARAM;
    }
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
    return SyncStream();
}

Result HostDataOpAclMemcpy::BatchDataCopy(hybm_batch_copy_params &params, hybm_data_copy_direction direction,
                                          const ExtOptions &options) noexcept
{
    (void)options;
    BM_ASSERT_LOG_AND_RETURN(inited_, "inited_ = " << inited_, BM_NOT_INITIALIZED);
    if (!IsSupportedDirection(direction)) {
        BM_LOG_ERROR("batch copy invalid direction for acl memcpy:" << direction << " rankId:" << rankId_);
        return BM_INVALID_PARAM;
    }
    if (IsD2DDirection(direction)) {
        return BatchCopyD2D(params);
    }
    for (uint32_t i = 0; i < params.batchSize; ++i) {
        auto ret = CopyByDirection(params.destinations[i], params.sources[i], params.dataSizes[i], direction);
        if (ret != BM_OK) {
            BM_LOG_ERROR("batch copy item failed, ret:" << ret << " index:" << i << " rankId:" << rankId_
                       << " size:" << params.dataSizes[i]);
            return ret;
        }
    }
    return BM_OK;
}

Result HostDataOpAclMemcpy::DataCopyAsync(hybm_copy_params &params, hybm_data_copy_direction direction,
                                          const ExtOptions &options) noexcept
{
    // 一期 async 降级为同步执行(逐跳 Sync), 由 Wait no-op 兜底语义
    return DataCopy(params, direction, options);
}

Result HostDataOpAclMemcpy::Wait(int32_t waitId) noexcept
{
    (void)waitId;
    return BM_OK;
}

void HostDataOpAclMemcpy::TransformVa(void *&src, void *&dst, hybm_data_copy_direction direction) noexcept
{
    // 对称窗口 GVA==DVA==HVA(HybmAclVmmSegment 登记), 地址直通无需换算
    (void)src;
    (void)dst;
    (void)direction;
}
} // namespace mf
} // namespace ock
