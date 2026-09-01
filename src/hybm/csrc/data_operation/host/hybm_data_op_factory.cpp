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
#include "hybm_data_op_sdma.h"
#include "hybm_data_op_device_rdma.h"
#include "hybm_data_op_device_urma.h"
#include "hybm_data_op_host_shm.h"
#include "hybm_data_op_host_rdma.h"
#include "hybm_data_op_acl_memcpy.h"
#include "hybm_data_op_factory.h"
#include "dl_acl_api.h"

namespace ock {
namespace mf {
DataOperatorPtr DataOperatorFactory::CreateSdmaDataOperator(uint32_t rankId)
{
    if (DlAclApi::GetAscendSocType() == AscendSocType::ASCEND_310P) {
        // 310P: HostDataOpSDMA 的 paramSpace(HalHostRegister)与 STARS SQE 直投均不可用,
        // 以 aclrtMemcpy 算子占 SDMA 槽位(保证 compose 的 hybm_wait 语义)
        return std::make_shared<HostDataOpAclMemcpy>(rankId);
    }
    return std::make_shared<HostDataOpSDMA>();
}

DataOperatorPtr DataOperatorFactory::CreateDevRdmaDataOperator(uint32_t rankId, const transport::TransManagerPtr &tm)
{
    return std::make_shared<DataOpDeviceRDMA>(rankId, tm);
}

DataOperatorPtr DataOperatorFactory::CreateDevUrmaDataOperator(uint32_t rankId, const transport::TransManagerPtr &tm)
{
    return std::make_shared<DataOpDeviceURMA>(rankId, tm);
}

DataOperatorPtr DataOperatorFactory::CreateHostRdmaDataOperator(uint32_t rankId, const transport::TransManagerPtr &tm)
{
    return std::make_shared<HostDataOpRDMA>(rankId, tm);
}

DataOperatorPtr DataOperatorFactory::CreateHostShmDataOperator(uint32_t rankId)
{
    return std::make_shared<HostDataOpHostShm>(rankId);
}
} // namespace mf
} // namespace ock
