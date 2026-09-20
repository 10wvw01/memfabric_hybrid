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

#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include "devmm_svm_gva.h"
#include "hybm.h"
#include "hybm_ptracer.h"
#include "hybm_common_include.h"
#include "hybm_vmm_handle_registry.h"
#include "mf_env_define.h"
#include "hybm_gva.h"
#include "hybm_version.h"
#include "hybm_stream_manager.h"
#include "hybm_va_manager.h"
#include "dl_api.h"
#include "dl_acl_api.h"
#include "dl_hal_api.h"

using namespace ock::mf;

namespace {

uint64_t g_baseAddr = 0ULL;
int64_t initialized = 0;
int32_t initedDeviceId = -1;
void *g_allocHandle = nullptr;

std::mutex initMutex;
} // namespace

int32_t HybmGetInitDeviceId()
{
    return initedDeviceId; // userDeviceId
}

bool HybmHasInited()
{
    return initialized > 0;
}

static inline Result hybm_load_library()
{
    std::string libPath;
#if defined(ASCEND_NPU)
    const std::string &path = env::ASCEND_HOME_PATH;
    BM_VALIDATE_RETURN(!path.empty(), "Environment ASCEND_HOME_PATH is not set.", BM_ERROR);
    libPath = path + "/lib64";
    if (!ock::mf::FileUtil::Realpath(libPath) || !ock::mf::FileUtil::IsDir(libPath)) {
        BM_LOG_ERROR("ASCEND_HOME_PATH check failed, path: " << path << " libPath: " << libPath);
        return BM_ERROR;
    }
#elif defined(NVIDIA_GPU)
    const std::string &path = env::CUDA_HOME;
    BM_VALIDATE_RETURN(!path.empty(), "Environment CUDA_HOME is not set.", BM_ERROR);
    libPath = path + "/lib64";
    if (!ock::mf::FileUtil::Realpath(libPath) || !ock::mf::FileUtil::IsDir(libPath)) {
        BM_LOG_ERROR("CUDA_HOME check failed, path: " << path << " libPath: " << libPath);
        return BM_ERROR;
    }
#endif

    auto ret = DlApi::LoadLibrary(libPath, HybmGetGvaVersion());
    BM_LOG_ERROR_RETURN_IT_IF_NOT_OK(ret, "load library from path failed: " << ret << ", path:" << libPath);
    return BM_OK;
}

HYBM_API int32_t hybm_init(uint16_t deviceId, uint64_t flags)
{
    std::unique_lock<std::mutex> lockGuard{initMutex};
    if (initialized > 0) {
        if (initedDeviceId != deviceId) {
            BM_LOG_ERROR("this deviceId(" << deviceId << ") is not equal to the deviceId(" << initedDeviceId
                                          << ") of other module!");
            return BM_ERROR;
        }
        BM_LOG_ERROR_RETURN_IT_IF_NOT_OK(hybm_load_library(), "load library failed");
        if (g_baseAddr == 0) {
            auto ret = hybm_init_hbm_gva(deviceId, flags, g_baseAddr, DlAclApi::GetAscendSocType(), &g_allocHandle);
            BM_LOG_ERROR_RETURN_IT_IF_NOT_OK(ret, "init shm meta failed");
        }

        initialized++;
        return 0;
    }

    HybmVaManager::InitDirectionLut();
    BM_LOG_ERROR_RETURN_IT_IF_NOT_OK(HalGvaPrecheck(), "the current version of ascend driver does not support mf!");
    BM_LOG_ERROR_RETURN_IT_IF_NOT_OK(hybm_load_library(), "load library failed");
    ptracer_config_t config{.tracerType = 1, .dumpFilePath = "/var/log/memfabric_hybrid"};
    auto ret = ptracer_init(&config);
    if (ret != BM_OK) {
        BM_LOG_WARN("init ptracer module not successful, result: " << ret << ", msg: " << ptracer_get_last_err_msg());
    }

    ret = hybm_init_hbm_gva(deviceId, flags, g_baseAddr, DlAclApi::GetAscendSocType(), &g_allocHandle);
    if (ret != BM_OK) {
        ptracer_uninit();
        DlApi::CleanupLibrary();
        BM_LOG_ERROR("hybm_init_hbm_gva failed, deviceId: " << deviceId << " flags: 0x" << std::hex << flags
                                                            << " ret: " << ret);
        return BM_ERROR;
    }

    initedDeviceId = deviceId;
    initialized = 1L;
    BM_LOG_INFO("hybm init successfully, " << LIB_VERSION << ", deviceId: " << deviceId);
    return 0;
}

HYBM_API void hybm_uninit()
{
    std::unique_lock<std::mutex> lockGuard{initMutex};
    if (initialized <= 0L) {
        BM_LOG_WARN("hybm not initialized.");
        return;
    }

    if (--initialized > 0L) {
        return;
    }

    ptracer_uninit();
    auto socType = DlAclApi::GetAscendSocType();
    if (socType == AscendSocType::ASCEND_310P) { // 平台分派: 310P 分支(先于 GVA V4 分支)
        /* 310P: 与 Hybm310PInitMetaGva 对称的 AclVmm 释放路径 */
        if (g_baseAddr != 0) { // meta 区已建立才需要释放
            // 解除 meta 区固定约定 VA 上的映射(建链时 AclrtMapMem 的逆操作)
            auto ret = DlAclApi::AclrtUnmapMem(reinterpret_cast<void *>(HYBM_DEVICE_META_ADDR));
            // 记录解除映射结果(失败也继续清理, 尽力而为)
            BM_LOG_INFO("unmap meta info res: " << ret);
            if (g_allocHandle != nullptr) { // 物理内存句柄存在才释放
                // 释放 meta 区背后的物理页(建链时 AclrtMallocPhysical 的逆操作)
                ret = DlAclApi::AclrtFreePhysical(reinterpret_cast<aclrtDrvMemHandle>(g_allocHandle));
                g_allocHandle = nullptr; // 置空句柄防重复释放
                BM_LOG_INFO("free meta physical res: " << ret); // 记录物理页释放结果
            }
            // 释放预留的 meta VA 窗口(建链时 AclrtReserveMemAddress 的逆操作)
            ret = DlAclApi::AclrtReleaseMemAddress(reinterpret_cast<void *>(g_baseAddr));
            BM_LOG_INFO("release meta va res: " << ret);  // 记录 VA 窗口释放结果
            g_baseAddr = 0ULL;                            // 清零标记 meta 区已完全释放
        }
    } else if ((socType == AscendSocType::ASCEND_950) || (HybmGetGvaVersion() == HYBM_GVA_V4)) {
        if (g_baseAddr != 0) {
            if (socType == AscendSocType::ASCEND_310P) {
                auto unmapRet = DlAclApi::AclrtUnmapMem(reinterpret_cast<void *>(g_baseAddr));
                BM_LOG_INFO("310P unmap meta res: " << unmapRet);
                if (g_allocHandle != nullptr) {
                    auto freeRet = DlAclApi::AclrtFreePhysical(reinterpret_cast<aclrtDrvMemHandle>(g_allocHandle));
                    g_allocHandle = nullptr;
                    BM_LOG_INFO("310P free physical res: " << freeRet);
                }
                auto releaseRet = DlAclApi::AclrtReleaseMemAddress(reinterpret_cast<void *>(g_baseAddr));
                BM_LOG_INFO("310P release mem addr res: " << releaseRet);
            } else {
                auto ret = DlHalApi::HalMemUnmap(reinterpret_cast<void *>(g_baseAddr));
                BM_LOG_INFO("unmap meta info res: " << ret);
                if (g_allocHandle != nullptr) {
                    ret = DlHalApi::HalMemRelease((drv_mem_handle_t *)g_allocHandle);
                    g_allocHandle = nullptr;
                    BM_LOG_INFO("release meta memory handle res: " << ret);
                }
                ret = DlHalApi::HalMemAddressFree(reinterpret_cast<void *>(g_baseAddr));
                BM_LOG_INFO("free meta memory res: " << ret);
            }
            g_baseAddr = 0ULL;
        }
    } else {
        if (g_baseAddr != 0) {
            drv::HalGvaFree(HYBM_DEVICE_META_ADDR, HYBM_DEVICE_INFO_SIZE);
            auto ret = drv::HalGvaUnreserveMemory(g_baseAddr);
            BM_LOG_INFO("uninitialize GVA memory return: " << ret);
            g_baseAddr = 0ULL;
        }
    }

    HybmStreamManager::DestroyAllThreadHybmStream();
    DlApi::CleanupLibrary();
    initialized = 0;
}

HYBM_API void hybm_set_extern_logger(void (*logger)(int level, const char *msg))
{
    if (logger == nullptr) {
        return;
    }

    if (ock::mf::OutLogger::Instance().GetExternalLogFunction() != nullptr) {
        BM_LOG_WARN("External log function has already been set, which will be override with new log function");
    }
    ock::mf::OutLogger::Instance().SetExternalLogFunction(logger, true);
}

HYBM_API void hybm_set_extern_alarm(void (*alarm)(uint16_t code, const char *msg), void (*resume)(uint16_t code))
{
    if (alarm == nullptr) {
        return;
    }

    if (ock::mf::OutLogger::Instance().GetAlarmLogFunction() != nullptr) {
        BM_LOG_WARN("Alarm log function has already been set, which will be override with new log function");
    }
    ock::mf::OutLogger::Instance().SetAlarmLogFunction(alarm, resume, true);
}

HYBM_API int32_t hybm_set_log_level(int level)
{
    BM_VALIDATE_RETURN(ock::mf::OutLogger::ValidateLevel(level),
                       "set log level failed, invalid param, level should be 0~4", -1);
    ock::mf::OutLogger::Instance().SetLogLevel(static_cast<ock::mf::LogLevel>(level));
    return 0;
}

HYBM_API const char *hybm_get_error_string(int32_t errCode)
{
    static thread_local std::string info = std::string("error(").append(std::to_string(errCode)).append(")");
    return info.c_str();
}

HYBM_API int32_t hybm_get_soc_type(void)
{
    return static_cast<int32_t>(DlAclApi::GetAscendSocType());
}

HYBM_API int32_t hybm_get_phy_chip_id(uint32_t deviceId)
{
    int64_t val = 0;
    auto ret = DlAclApi::RtGetDeviceInfo(deviceId, 0, INFO_TYPE_PHY_CHIP_ID, &val);
    if (ret != BM_OK) {
        return -1;
    }
    return static_cast<int32_t>(val);
}

HYBM_API int32_t hybm_enable_peer_access(uint32_t localDeviceId, uint32_t peerDeviceId)
{
    if (localDeviceId == peerDeviceId) {
        return 0;
    }
    /* 310P3 Duo: deviceId/2 identifies the physical card.
     * dev0/dev1 → card0, dev2/dev3 → card1, etc.
     * Same card = no P2P needed. */
    if ((localDeviceId / 2) == (peerDeviceId / 2)) {
        BM_LOG_INFO("hybm_enable_peer_access: same card by deviceId/2, local:" << localDeviceId
                                                                               << " peer:" << peerDeviceId);
        return 0;
    }
    auto ret = DlAclApi::AclrtDeviceEnablePeerAccess(static_cast<int32_t>(peerDeviceId), 0);
    if (ret != 0) {
        BM_LOG_ERROR("hybm_enable_peer_access failed:" << ret << " local:" << localDeviceId
                                                       << " peer:" << peerDeviceId);
        return ret;
    }
    BM_LOG_INFO("hybm_enable_peer_access: local:" << localDeviceId << " peer:" << peerDeviceId << " ret:" << ret);
    return 0;
}

HYBM_API int32_t hybm_gva_to_va(uint64_t gva, hybm_mem_type vaMemType, uint64_t *va)
{
    if (va == nullptr) {
        BM_LOG_ERROR("input va is null.");
        return BM_ERROR;
    }

    if (!HybmHasInited()) {
        BM_LOG_ERROR("hybm not initialized.");
        return BM_ERROR;
    }

    // Validate memory type
    if (vaMemType >= HYBM_MEM_TYPE_BUTT) {
        BM_LOG_ERROR("invalid memory type: " << vaMemType);
        return BM_ERROR;
    }
    // Determine output VA type based on memory type
    uint32_t outputType = vaMemType == HYBM_MEM_TYPE_DEVICE ? HVM_DVA : HVM_HVA;
    uint64_t convertedVa = HybmVaManager::GetInstance().TransformVa(gva, HVM_GVA, outputType);
    if (convertedVa == 0) {
        BM_LOG_ERROR("GVA to VA conversion failed for address: 0x" << std::hex << gva);
        return BM_ERROR;
    }

    *va = convertedVa;
    return BM_OK;
}

HYBM_API void *hybm_alloc_vmm_buffer(uint32_t deviceId, uint32_t worldSize, size_t size)
{
    auto setRet = DlAclApi::AclrtSetDevice(static_cast<int32_t>(deviceId), true);
    if (setRet != 0) {
        BM_LOG_ERROR("AclrtSetDevice failed, ret:" << setRet << " deviceId:" << deviceId);
        return nullptr;
    }

    aclrtPhysicalMemProp prop{};
    prop.handleType = aclrtMemHandleType::ACL_MEM_HANDLE_TYPE_NONE;
    prop.allocationType = ACL_MEM_ALLOCATION_TYPE_PINNED;
    prop.memAttr = static_cast<uint32_t>(aclrtMemAttr::ACL_MEM_P2P_HUGE);
    prop.location.id = deviceId;
    prop.location.type = aclrtMemLocationType::ACL_MEM_LOCATION_TYPE_DEVICE;
    prop.reserve = 0ULL;

    size_t granularity = 0;
    auto ret = DlAclApi::AclrtMemGetAllocationGranularity(&prop, 1U, &granularity);
    if (ret != 0 || granularity == 0) {
        BM_LOG_ERROR("AclrtMemGetAllocationGranularity failed, ret:" << ret << " deviceId:" << deviceId);
        return nullptr;
    }
    size_t alignedSize = (size + granularity - 1) & ~(granularity - 1);

    aclrtDrvMemHandle handle = nullptr;
    ret = DlAclApi::AclrtMallocPhysical(&handle, alignedSize, &prop, 0);
    if (ret != 0 || handle == nullptr) {
        BM_LOG_ERROR("AclrtMallocPhysical failed, ret:" << ret << " deviceId:" << deviceId << " size:" << alignedSize);
        return nullptr;
    }

    void *va = nullptr;
    ret = DlAclApi::AclrtReserveMemAddress(&va, alignedSize, 0, nullptr, 0);
    if (ret != 0 || va == nullptr) {
        BM_LOG_ERROR("AclrtReserveMemAddress failed, ret:" << ret << " deviceId:" << deviceId
                                                           << " size:" << alignedSize);
        DlAclApi::AclrtFreePhysical(handle);
        return nullptr;
    }

    ret = DlAclApi::AclrtMapMem(va, alignedSize, 0, handle, 0);
    if (ret != 0) {
        BM_LOG_ERROR("AclrtMapMem failed, ret:" << ret << " deviceId:" << deviceId << " va:" << va);
        DlAclApi::AclrtReleaseMemAddress(va);
        DlAclApi::AclrtFreePhysical(handle);
        return nullptr;
    }

    aclrtMemAccessDesc localDesc{
        ACL_RT_MEM_ACCESS_FLAGS_READWRITE, {deviceId, aclrtMemLocationType::ACL_MEM_LOCATION_TYPE_DEVICE}, {}};
    auto localAccessRet = DlAclApi::AclrtMemSetAccess(va, alignedSize, &localDesc, 1);
    if (localAccessRet != 0) {
        BM_LOG_ERROR("AclrtMemSetAccess(local) failed, ret:" << localAccessRet << " deviceId:" << deviceId
                                                              << " va:" << va << " size:" << alignedSize);
        DlAclApi::AclrtUnmapMem(va);
        DlAclApi::AclrtReleaseMemAddress(va);
        DlAclApi::AclrtFreePhysical(handle);
        return nullptr;
    }

    for (uint32_t d = 0; d < worldSize; d++) {
        if (d == deviceId) {
            continue;
        }
        if ((d / 2U) == (deviceId / 2U)) {
            continue;
        }
        aclrtMemAccessDesc desc{
            ACL_RT_MEM_ACCESS_FLAGS_READWRITE, {d, aclrtMemLocationType::ACL_MEM_LOCATION_TYPE_DEVICE}, {}};
        auto r = DlAclApi::AclrtMemSetAccess(va, alignedSize, &desc, 1);
        if (r != 0) {
            BM_LOG_WARN("AclrtMemSetAccess for dev:" << d << " failed:" << r << " (non-fatal, may be set later)");
        }
    }

    VmmHandleRegistry::Register(va, handle, deviceId);

    BM_LOG_INFO("hybm_alloc_vmm_buffer success, deviceId:" << deviceId << " va:" << va << " size:" << alignedSize
                                                           << " handle:" << handle);
    return va;
}

HYBM_API void hybm_free_vmm_buffer(void *addr, size_t size)
{
    (void)size;
    if (addr == nullptr) {
        return;
    }

    aclrtDrvMemHandle handle = nullptr;
    uint32_t deviceId = 0;
    if (!VmmHandleRegistry::Unregister(addr, handle, deviceId)) {
        BM_LOG_WARN("hybm_free_vmm_buffer: addr not found in registry, addr:" << addr);
        return;
    }

    auto setRet = DlAclApi::AclrtSetDevice(static_cast<int32_t>(deviceId), true);
    if (setRet != 0) {
        BM_LOG_ERROR("hybm_free_vmm_buffer AclrtSetDevice failed, ret:" << setRet << " deviceId:" << deviceId);
    }
    auto unmapRet = DlAclApi::AclrtUnmapMem(addr);
    auto releaseRet = DlAclApi::AclrtReleaseMemAddress(addr);
    auto freeRet = DlAclApi::AclrtFreePhysical(handle);
    BM_LOG_INFO("hybm_free_vmm_buffer done, addr:" << addr << " handle:" << handle << " unmapRet:" << unmapRet
                                                   << " releaseRet:" << releaseRet << " freeRet:" << freeRet);
}
