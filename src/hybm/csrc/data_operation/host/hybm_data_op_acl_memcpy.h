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

/* 310P(Ascend310P3) 拷贝算子: 基于 aclrtMemcpy 系列符号。
 * 与 HostDataOpSDMA(910B/950, STARS SQE 直投 + HalHostRegister paramSpace) 不同,
 * 310P host register 全系不可用且无 SQE 直投能力, D2D/H2D/D2H 均走 aclrtMemcpyAsync
 * (SDMA 引擎) + SynchronizeStream; 同步 D2D 跨 die 返回 507899 不可用。
 * async 失败时回退到同步 aclrtMemcpy 作为安全网。
 * 占用 compose 的 sdmaDataOperator_ 槽位, 使 hybm_wait 语义可用。
 * TransformVa 与 HostDataOpSDMA 一致, 通过 HybmVaManager 将 GVA 翻译为 DVA/HVA。
 */
#ifndef MF_HYBM_DATA_OP_ACL_MEMCPY_H
#define MF_HYBM_DATA_OP_ACL_MEMCPY_H

#include "hybm_data_operator.h"
#include "dl_acl_api.h"

namespace ock {
namespace mf {
class HostDataOpAclMemcpy : public DataOperator {
public:
    explicit HostDataOpAclMemcpy(uint32_t rankId) noexcept : rankId_(rankId) {}

    ~HostDataOpAclMemcpy() override;

    Result Initialize() noexcept override;
    void UnInitialize() noexcept override;

    Result DataCopy(hybm_copy_params &params, hybm_data_copy_direction direction,
                    const ExtOptions &options) noexcept override;
    Result BatchDataCopy(hybm_batch_copy_params &params, hybm_data_copy_direction direction,
                         const ExtOptions &options) noexcept override;
    Result DataCopyAsync(hybm_copy_params &params, hybm_data_copy_direction direction,
                         const ExtOptions &options) noexcept override;
    Result Wait(int32_t waitId) noexcept override;
    void TransformVa(void *&src, void *&dst, hybm_data_copy_direction direction) noexcept override;
    void CleanUp() noexcept override;

private:
    static bool IsD2DDirection(hybm_data_copy_direction direction) noexcept;
    static bool IsH2DDirection(hybm_data_copy_direction direction) noexcept;
    static bool IsD2HDirection(hybm_data_copy_direction direction) noexcept;
    Result SubmitD2D(void *dest, const void *src, uint64_t length) noexcept;
    Result SyncStream() noexcept;
    Result CopyD2D(void *dest, const void *src, uint64_t length) noexcept;
    Result CopyH2D(void *dest, const void *src, uint64_t length) noexcept;
    Result CopyD2H(void *dest, const void *src, uint64_t length) noexcept;
    Result CopyByDirection(void *dest, const void *src, uint64_t length,
                           hybm_data_copy_direction direction) noexcept;
    Result BatchCopyD2D(hybm_batch_copy_params &params) noexcept;
    Result InnerWait(const ExtOptions &options, int32_t waitId) noexcept;

    uint32_t rankId_;
    bool inited_{false};
    void *stream_{nullptr};
};
} // namespace mf
} // namespace ock

#endif // MF_HYBM_DATA_OP_ACL_MEMCPY_H
