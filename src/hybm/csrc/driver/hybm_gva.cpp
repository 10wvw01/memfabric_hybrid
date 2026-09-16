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

#include "hybm_common_include.h"
#include "devmm_svm_gva.h"
#include "dl_api.h"
#include "dl_acl_api.h"
#include "dl_hal_api.h"
#include "hybm_cmd.h"
#include "hybm_gva.h"

namespace ock {
namespace mf {

namespace {
int32_t initedLogicDeviceId = -1;
} // namespace

int32_t HybmGetInitedLogicDeviceId()
{
    return initedLogicDeviceId; // logicDeviceId
}

int32_t HybmModernInitMetaGva(void **globalMemoryBase, size_t allocSize, void **allocHandle)
{
    drv_mem_handle_t **handle = (drv_mem_handle_t **)allocHandle;
    uint64_t va = SVM_END_ADDR - GB;
    auto ret = DlHalApi::HalMemAddressReserve(globalMemoryBase, GB, 0, reinterpret_cast<void *>(va), 0);
    if (ret != 0) {
        BM_LOG_ERROR("prepare virtual memory size(" << GB << ") failed. ret: " << ret);
        return BM_ERROR;
    }
    drv_mem_prop memprop;
    memprop.side = MEM_DEV_SIDE;
    memprop.devid = initedLogicDeviceId;
    memprop.module_id = 0;
    memprop.pg_type = MEM_HUGE_PAGE_TYPE;
    memprop.mem_type = MEM_HBM_TYPE;
    memprop.reserve = 0;
    ret = DlHalApi::HalMemCreate(handle, allocSize, &memprop, 0);
    if (ret != BM_OK) {
        BM_LOG_ERROR("HalMemCreate failed: " << ret);
        return BM_ERROR;
    }
    ret = DlHalApi::HalMemMap(reinterpret_cast<void *>(HYBM_DEVICE_META_ADDR), allocSize, 0, *handle, 0);
    if (ret != BM_OK) {
        BM_LOG_ERROR("HalMemMap failed, ret: " << ret << " addr: 0x" << std::hex << HYBM_DEVICE_META_ADDR << " size: 0x"
                                               << allocSize << " handle: " << *handle
                                               << " devid: " << initedLogicDeviceId);
        DlHalApi::HalMemRelease(*handle);
        return BM_ERROR;
    }
    return BM_OK;
}

/* 310P meta 区创建: AclVmm 路径 —— halMem* 语义在 310P 不可用(F16), 改用
 * HybmAclVmmSegment 同款 API 配方(ReserveMemAddress + MallocPhysical + MapMem)。
 * meta 区地址 HYBM_DEVICE_META_ADDR 是设备侧头文件(smem_shm_aicore_base_meta.h)
 * 硬编码的约定地址, 必须 expectPtr 精确命中, 偏移即失败。 */
int32_t Hybm310PInitMetaGva(uint16_t deviceId, void **globalMemoryBase, size_t allocSize, void **allocHandle)
{
    // 第一步: 在约定地址 HYBM_DEVICE_META_ADDR 预留 VA 窗口(expectPtr 精确指定)
    auto ret = DlAclApi::AclrtReserveMemAddress(globalMemoryBase, allocSize, 0,
                                                reinterpret_cast<void *>(HYBM_DEVICE_META_ADDR), 0);
    if (ret != BM_OK) { // 预留失败
        // 根因日志: ret/size/deviceId
        BM_LOG_ERROR("reserve meta va failed, ret: " << ret << " size: 0x" << std::hex << allocSize
                                                     << " deviceId: " << deviceId);
        return BM_ERROR; // 透传失败(根因已记录)
    }
    // 校验预留基址必须精确命中约定地址(设备侧头文件硬编码依赖, 偏移即失败)
    if (*globalMemoryBase != reinterpret_cast<void *>(HYBM_DEVICE_META_ADDR)) {
        // 根因日志: 实得基址/期望基址/deviceId
        BM_LOG_ERROR("meta va base mismatch, got: 0x" << std::hex << *globalMemoryBase << " expect: 0x"
                                                      << HYBM_DEVICE_META_ADDR << " deviceId: " << deviceId);
        (void)DlAclApi::AclrtReleaseMemAddress(*globalMemoryBase); // 回滚: 释放刚预留的 VA 窗口
        return BM_ERROR;                                            // 透传失败(根因已记录)
    }
    aclrtPhysicalMemProp prop{}; // 物理内存属性描述符(零初始化后逐字段填写)
    prop.handleType = aclrtMemHandleType::ACL_MEM_HANDLE_TYPE_NONE;      /* F10: 310P 仅 NONE 可用 */
    prop.allocationType = aclrtMemAllocationType::ACL_MEM_ALLOCATION_TYPE_PINNED; /* 物理页钉住不换出 */
    prop.memAttr = aclrtMemAttr::ACL_MEM_P2P_HUGE;                       /* F10: P2P_HUGE 实测可用 */
    prop.location.id = static_cast<uint32_t>(deviceId);                  /* 内存归属设备号 */
    prop.location.type = aclrtMemLocationType::ACL_MEM_LOCATION_TYPE_DEVICE; /* 归属类型: 设备内存 */
    aclrtDrvMemHandle handle = nullptr; // 物理内存句柄(成功后经 allocHandle 出参带出)
    // 第二步: 按上述属性分配 meta 区物理页
    ret = DlAclApi::AclrtMallocPhysical(&handle, allocSize, &prop, 0);
    if (ret != BM_OK) { // 分配失败
        // 根因日志: ret/size/deviceId
        BM_LOG_ERROR("malloc meta physical failed, ret: " << ret << " size: 0x" << std::hex << allocSize
                                                          << " deviceId: " << deviceId);
        (void)DlAclApi::AclrtReleaseMemAddress(*globalMemoryBase); // 回滚: 释放已预留的 VA 窗口
        return BM_ERROR;                                           // 透传失败(根因已记录)
    }
    // 第三步: 物理页映射到约定 VA(建链完成, 与 hybm_uninit 的 310P 分支对称)
    ret = DlAclApi::AclrtMapMem(reinterpret_cast<void *>(HYBM_DEVICE_META_ADDR), allocSize, 0, handle, 0);
    if (ret != BM_OK) { // 映射失败
        // 根因日志: ret/addr/size/deviceId
        BM_LOG_ERROR("map meta va failed, ret: " << ret << " addr: 0x" << std::hex << HYBM_DEVICE_META_ADDR
                                                 << " size: 0x" << allocSize << " deviceId: " << deviceId);
        (void)DlAclApi::AclrtFreePhysical(handle);                  // 回滚: 释放刚分配的物理页
        (void)DlAclApi::AclrtReleaseMemAddress(*globalMemoryBase);  // 回滚: 释放已预留的 VA 窗口
        return BM_ERROR;                                            // 透传失败(根因已记录)
    }
    *allocHandle = handle; // 物理句柄交调用方保管(uninit 释放用)
    return BM_OK;          // 三步全部成功
}

int32_t HybmLegacyInitMetaGva(void **globalMemoryBase, size_t allocSize, uint64_t flags){
    auto ret = drv::HalGvaReserveMemory((uint64_t *)globalMemoryBase, allocSize, initedLogicDeviceId, flags);
    if (ret != 0 || reinterpret_cast<uint64_t>(*globalMemoryBase) != (SVM_END_ADDR - GB)) {
        if (ret == 0 && *globalMemoryBase != nullptr) {
            (void)drv::HalGvaUnreserveMemory((uint64_t)(*globalMemoryBase));
        }
        BM_LOG_ERROR("initialize meta memory failed: " << ret << " size:0x" << std::hex << allocSize << " flag:0x"
                                                       << flags << " ret_addr:" << *globalMemoryBase);
        return BM_ERROR;
    }

    ret = drv::HalGvaAlloc(HYBM_DEVICE_META_ADDR, HYBM_DEVICE_INFO_SIZE, 0);
    if (ret != BM_OK) {
        (void)drv::HalGvaUnreserveMemory((uint64_t)(*globalMemoryBase));
        BM_LOG_ERROR("HalGvaAlloc hybm meta memory failed: " << ret);
        return BM_MALLOC_FAILED;
    }
    return BM_OK;
}

int32_t hybm_init_hbm_gva(uint16_t deviceId, uint64_t flags, uint64_t &baseAddress, AscendSocType socType,
                          void **allocHandle)
{
#if !defined(ASCEND_NPU)
    return BM_OK;
#else
    initedLogicDeviceId = -1;
    DlAclApi::RtGetLogicDevIdByUserDevId(deviceId, &initedLogicDeviceId);
    if (initedLogicDeviceId < 0) {
        BM_LOG_ERROR("RtGetLogicDevIdByUserDevId failed, deviceId: " << deviceId
                                                                     << " logicDeviceId: " << initedLogicDeviceId);
        return BM_ERROR;
    }
    BM_LOG_INFO("Success get deviceId: " << deviceId << ", logicDeviceId: " << initedLogicDeviceId
                                         << " sco:" << (int)socType << " ver:" << HybmGvaVersion());
    auto ret = DlAclApi::AclrtSetDevice(deviceId);
    if (ret != BM_OK) {
        BM_LOG_ERROR("set device id to be " << deviceId << " failed: " << ret);
        return BM_ERROR;
    }
    drv::HybmInitialize(initedLogicDeviceId, DlHalApi::GetFd());

    if ((flags & HYBM_FLAG_INIT_SHMEM_META) == 0) {
        BM_LOG_DEBUG("skip init shm meta space:" << flags);
        baseAddress = 0;
        return BM_OK;
    } else {
        BM_LOG_DEBUG("restore init flag");
        flags &= ~HYBM_FLAG_INIT_SHMEM_META;
    }

    void *globalMemoryBase = nullptr;
    size_t allocSize = HYBM_DEVICE_INFO_SIZE; // 申请meta空间
    if (socType == AscendSocType::ASCEND_310P) { // 平台分派: 310P 分支(必须先于 V4 判断)
        /* 310P: halMem* 不可用, 走 AclVmm 创建(判断须先于 V4 —— 310P 驱动被映射为 V4
         * 但无 halMem 语义, 2-rank 计划 F2/F16 定论) */
        ret = Hybm310PInitMetaGva(deviceId, &globalMemoryBase, allocSize, allocHandle); // AclVmm 建链
    } else if ((socType == AscendSocType::ASCEND_950) || (HybmGetGvaVersion() == HYBM_GVA_V4)) {
        ret = HybmModernInitMetaGva(&globalMemoryBase, allocSize, allocHandle);
    } else {
        ret = HybmLegacyInitMetaGva(&globalMemoryBase, allocSize, flags);
    }
    if (ret != BM_OK) {
        BM_LOG_ERROR("hybm init meta gva failed: " << ret);
        return BM_ERROR;
    }
    baseAddress = reinterpret_cast<uint64_t>(globalMemoryBase);
    return BM_OK;
#endif
}

} // namespace mf
} // namespace ock
