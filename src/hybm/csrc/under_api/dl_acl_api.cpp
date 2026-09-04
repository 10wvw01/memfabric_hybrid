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
#include <dlfcn.h>
#include "dl_acl_api.h"

namespace ock {
namespace mf {
bool DlAclApi::gLoaded = false;
std::mutex DlAclApi::gMutex;
void *DlAclApi::rtHandle;
const char *DlAclApi::gAscendAclLibName = "libascendcl.so";

aclrtGetDeviceFunc DlAclApi::pAclrtGetDevice = nullptr;
aclrtSetDeviceFunc DlAclApi::pAclrtSetDevice = nullptr;
aclrtDeviceEnablePeerAccessFunc DlAclApi::pAclrtDeviceEnablePeerAccess = nullptr;
aclrtDeviceDisablePeerAccessFunc DlAclApi::pAclrtDeviceDisablePeerAccess = nullptr;
aclrtDeviceCanAccessPeerFunc DlAclApi::pAclrtDeviceCanAccessPeer = nullptr;
aclrtCreateStreamFunc DlAclApi::pAclrtCreateStream = nullptr;
aclrtCreateStreamWithConfigFunc DlAclApi::pAclrtCreateStreamWithConfig = nullptr;

aclrtStreamGetIdFunc DlAclApi::pAclrtStreamGetId = nullptr;
aclrtCreateNotifyFunc DlAclApi::pAclrtCreateNotify = nullptr;
aclrtGetNotifyIdFunc DlAclApi::pAclrtGetNotifyId = nullptr;
aclrtDestroyNotifyFunc DlAclApi::pAclrtDestroyNotify = nullptr;
aclrtWaitAndResetNotifyFunc DlAclApi::pAclrtWaitAndResetNotify = nullptr;
aclrtGetCurrentContextFunc DlAclApi::pAclrtGetCurrentContext = nullptr;
aclrtCreateContextFunc DlAclApi::pAclrtCreateContext = nullptr;
aclrtSetCurrentContextFunc DlAclApi::pAclrtSetCurrentContext = nullptr;
aclrtSetStreamAttributeFunc DlAclApi::pAclrtSetStreamAttribute = nullptr;
aclrtDestroyStreamFunc DlAclApi::pAclrtDestroyStream = nullptr;
aclrtSynchronizeStreamFunc DlAclApi::pAclrtSynchronizeStream = nullptr;
aclrtBinaryLoadFromFileFunc DlAclApi::pAclrtBinaryLoadFromFile = nullptr;
aclrtBinaryGetFunctionFunc DlAclApi::pAclrtBinaryGetFunction = nullptr;
aclrtKernelArgsInitFunc DlAclApi::pAclrtKernelArgsInit = nullptr;
aclrtKernelArgsAppendFunc DlAclApi::pAclrtKernelArgsAppend = nullptr;
aclrtKernelArgsFinalizeFunc DlAclApi::pAclrtKernelArgsFinalize = nullptr;
aclrtLaunchKernelWithConfigFunc DlAclApi::pAclrtLaunchKernelWithConfig = nullptr;
aclrtMallocFunc DlAclApi::pAclrtMalloc = nullptr;
aclrtFreeFunc DlAclApi::pAclrtFree = nullptr;
aclrtMallocHostFunc DlAclApi::pAclrtMallocHost = nullptr;
aclrtFreeHostFunc DlAclApi::pAclrtFreeHost = nullptr;
aclrtMemcpyFunc DlAclApi::pAclrtMemcpy = nullptr;
aclrtMemcpyBatchFunc DlAclApi::pAclrtMemcpyBatch = nullptr;
aclrtMemcpyAsyncFunc DlAclApi::pAclrtMemcpyAsync = nullptr;
aclrtMemcpy2dFunc DlAclApi::pAclrtMemcpy2d = nullptr;
aclrtMemcpy2dAsyncFunc DlAclApi::pAclrtMemcpy2dAsync = nullptr;
aclrtMemsetFunc DlAclApi::pAclrtMemset = nullptr;
rtDeviceGetBareTgidFunc DlAclApi::pRtDeviceGetBareTgid = nullptr;
rtGetDeviceInfoFunc DlAclApi::pRtGetDeviceInfo = nullptr;
rtSetIpcMemorySuperPodPidFunc DlAclApi::pRtSetIpcMemorySuperPodPid = nullptr;
rtIpcDestroyMemoryNameFunc DlAclApi::pRtIpcDestroyMemoryName = nullptr;
rtIpcSetMemoryNameFunc DlAclApi::pRtIpcSetMemoryName = nullptr;
rtIpcOpenMemoryFunc DlAclApi::pRtIpcOpenMemory = nullptr;
rtIpcCloseMemoryFunc DlAclApi::pRtIpcCloseMemory = nullptr;
aclrtIpcMemGetExportKeyFunc DlAclApi::pAclrtIpcMemGetExportKey = nullptr;
aclrtIpcMemImportByKeyFunc DlAclApi::pAclrtIpcMemImportByKey = nullptr;
aclrtIpcMemCloseFunc DlAclApi::pAclrtIpcMemClose = nullptr;
aclrtGetSocNameFunc DlAclApi::pAclrtGetSocName = nullptr;
rtEnableP2PFunc DlAclApi::pRtEnableP2P = nullptr;
rtDisableP2PFunc DlAclApi::pRtDisableP2P = nullptr;
rtMemcpyAsyncFunc DlAclApi::pRtMemcpyAsync = nullptr;
rtGetLogicDevIdByUserDevIdFunc DlAclApi::pRtGetLogicDevIdByUserDevId = nullptr;
aclrtGetPhyDevIdByLogicDevIdFunc DlAclApi::pAclrtGetPhyDevIdByLogicDevId = nullptr;

aclrtMallocPhysicalFunc DlAclApi::pAclrtMallocPhysical = nullptr;
aclrtFreePhysicalFunc DlAclApi::pAclrtFreePhysical = nullptr;
aclrtReserveMemAddressFunc DlAclApi::pAclrtReserveMemAddress = nullptr;
aclrtReleaseMemAddressFunc DlAclApi::pAclrtReleaseMemAddress = nullptr;
aclrtMapMemFunc DlAclApi::pAclrtMapMem = nullptr;
aclrtUnmapMemFunc DlAclApi::pAclrtUnmapMem = nullptr;
aclrtMemExportToShareableHandleFunc DlAclApi::pAclrtMemExportToShareableHandle = nullptr;
aclrtMemImportFromShareableHandleFunc DlAclApi::pAclrtMemImportFromShareableHandle = nullptr;
aclrtMemSetAccessFunc DlAclApi::pAclrtMemSetAccess = nullptr;
aclrtMemGetAllocationGranularityFunc DlAclApi::pAclrtMemGetAllocationGranularity = nullptr;

Result DlAclApi::LoadLibrary(const std::string &libDirPath)
{
    std::lock_guard<std::mutex> guard(gMutex);
    if (gLoaded) {
        return BM_OK;
    }

    std::string realPath;
    if (!FileUtil::LibraryRealPath(libDirPath, std::string(gAscendAclLibName), realPath)) {
        BM_LOG_ERROR("get lib path failed, libDir: " << libDirPath << " libName: " << gAscendAclLibName);
        return BM_ERROR;
    }

    /* dlopen library */
    rtHandle = dlopen(realPath.c_str(), RTLD_NOW | RTLD_NODELETE);
    if (rtHandle == nullptr) {
        BM_LOG_ERROR("Failed to open library [" << realPath << "], error: " << dlerror());
        return BM_DL_FUNCTION_FAILED;
    }

    /* load sym */
    DL_LOAD_SYM(pAclrtGetDevice, aclrtGetDeviceFunc, rtHandle, "aclrtGetDevice");
    DL_LOAD_SYM(pAclrtSetDevice, aclrtSetDeviceFunc, rtHandle, "aclrtSetDevice");
    DL_LOAD_SYM(pAclrtDeviceEnablePeerAccess, aclrtDeviceEnablePeerAccessFunc, rtHandle, "aclrtDeviceEnablePeerAccess");
    DL_LOAD_SYM_OPTIONAL(pAclrtDeviceDisablePeerAccess, aclrtDeviceDisablePeerAccessFunc, rtHandle,
                         "aclrtDeviceDisablePeerAccess");
    DL_LOAD_SYM_OPTIONAL(pAclrtDeviceCanAccessPeer, aclrtDeviceCanAccessPeerFunc, rtHandle,
                         "aclrtDeviceCanAccessPeer");
    DL_LOAD_SYM(pAclrtCreateStream, aclrtCreateStreamFunc, rtHandle, "aclrtCreateStream");
    DL_LOAD_SYM(pAclrtCreateStreamWithConfig, aclrtCreateStreamWithConfigFunc, rtHandle, "aclrtCreateStreamWithConfig");
    DL_LOAD_SYM(pAclrtStreamGetId, aclrtStreamGetIdFunc, rtHandle, "aclrtStreamGetId");
    DL_LOAD_SYM(pAclrtCreateNotify, aclrtCreateNotifyFunc, rtHandle, "aclrtCreateNotify");
    DL_LOAD_SYM(pAclrtGetNotifyId, aclrtGetNotifyIdFunc, rtHandle, "aclrtGetNotifyId");
    DL_LOAD_SYM(pAclrtDestroyNotify, aclrtDestroyNotifyFunc, rtHandle, "aclrtDestroyNotify");
    DL_LOAD_SYM(pAclrtWaitAndResetNotify, aclrtWaitAndResetNotifyFunc, rtHandle, "aclrtWaitAndResetNotify");
    DL_LOAD_SYM(pAclrtGetCurrentContext, aclrtGetCurrentContextFunc, rtHandle, "aclrtGetCurrentContext");
    DL_LOAD_SYM_OPTIONAL(pAclrtCreateContext, aclrtCreateContextFunc, rtHandle, "aclrtCreateContext");
    DL_LOAD_SYM_OPTIONAL(pAclrtSetCurrentContext, aclrtSetCurrentContextFunc, rtHandle, "aclrtSetCurrentContext");
    DL_LOAD_SYM(pAclrtSetStreamAttribute, aclrtSetStreamAttributeFunc, rtHandle, "aclrtSetStreamAttribute");
    DL_LOAD_SYM(pAclrtDestroyStream, aclrtDestroyStreamFunc, rtHandle, "aclrtDestroyStream");
    DL_LOAD_SYM(pAclrtSynchronizeStream, aclrtSynchronizeStreamFunc, rtHandle, "aclrtSynchronizeStream");
    DL_LOAD_SYM_OPTIONAL(pAclrtBinaryLoadFromFile, aclrtBinaryLoadFromFileFunc, rtHandle, "aclrtBinaryLoadFromFile");
    DL_LOAD_SYM_OPTIONAL(pAclrtBinaryGetFunction, aclrtBinaryGetFunctionFunc, rtHandle, "aclrtBinaryGetFunction");
    DL_LOAD_SYM_OPTIONAL(pAclrtKernelArgsInit, aclrtKernelArgsInitFunc, rtHandle, "aclrtKernelArgsInit");
    DL_LOAD_SYM_OPTIONAL(pAclrtKernelArgsAppend, aclrtKernelArgsAppendFunc, rtHandle, "aclrtKernelArgsAppend");
    DL_LOAD_SYM_OPTIONAL(pAclrtKernelArgsFinalize, aclrtKernelArgsFinalizeFunc, rtHandle, "aclrtKernelArgsFinalize");
    DL_LOAD_SYM_OPTIONAL(pAclrtLaunchKernelWithConfig, aclrtLaunchKernelWithConfigFunc, rtHandle,
                         "aclrtLaunchKernelWithConfig");
    DL_LOAD_SYM(pAclrtMalloc, aclrtMallocFunc, rtHandle, "aclrtMalloc");
    DL_LOAD_SYM(pAclrtFree, aclrtFreeFunc, rtHandle, "aclrtFree");
    DL_LOAD_SYM(pAclrtMallocHost, aclrtMallocHostFunc, rtHandle, "aclrtMallocHost");
    DL_LOAD_SYM(pAclrtFreeHost, aclrtFreeHostFunc, rtHandle, "aclrtFreeHost");
    DL_LOAD_SYM(pAclrtMemcpy, aclrtMemcpyFunc, rtHandle, "aclrtMemcpy");
    DL_LOAD_SYM_OPTIONAL(pAclrtMemcpyBatch, aclrtMemcpyBatchFunc, rtHandle, "aclrtMemcpyBatch");
    DL_LOAD_SYM(pAclrtMemcpyAsync, aclrtMemcpyAsyncFunc, rtHandle, "aclrtMemcpyAsync");
    DL_LOAD_SYM(pAclrtMemcpy2d, aclrtMemcpy2dFunc, rtHandle, "aclrtMemcpy2d");
    DL_LOAD_SYM(pAclrtMemcpy2dAsync, aclrtMemcpy2dAsyncFunc, rtHandle, "aclrtMemcpy2dAsync");
    DL_LOAD_SYM(pAclrtMemset, aclrtMemsetFunc, rtHandle, "aclrtMemset");
    DL_LOAD_SYM(pRtDeviceGetBareTgid, rtDeviceGetBareTgidFunc, rtHandle, "rtDeviceGetBareTgid");
    DL_LOAD_SYM(pRtGetDeviceInfo, rtGetDeviceInfoFunc, rtHandle, "rtGetDeviceInfo");
    DL_LOAD_SYM(pRtSetIpcMemorySuperPodPid, rtSetIpcMemorySuperPodPidFunc, rtHandle, "rtSetIpcMemorySuperPodPid");
    DL_LOAD_SYM(pRtIpcSetMemoryName, rtIpcSetMemoryNameFunc, rtHandle, "rtIpcSetMemoryName");
    DL_LOAD_SYM(pRtIpcDestroyMemoryName, rtIpcDestroyMemoryNameFunc, rtHandle, "rtIpcDestroyMemoryName");
    DL_LOAD_SYM(pRtIpcOpenMemory, rtIpcOpenMemoryFunc, rtHandle, "rtIpcOpenMemory");
    DL_LOAD_SYM(pRtIpcCloseMemory, rtIpcCloseMemoryFunc, rtHandle, "rtIpcCloseMemory");
    DL_LOAD_SYM(pAclrtIpcMemGetExportKey, aclrtIpcMemGetExportKeyFunc, rtHandle, "aclrtIpcMemGetExportKey");
    DL_LOAD_SYM(pAclrtIpcMemImportByKey, aclrtIpcMemImportByKeyFunc, rtHandle, "aclrtIpcMemImportByKey");
    DL_LOAD_SYM(pAclrtIpcMemClose, aclrtIpcMemCloseFunc, rtHandle, "aclrtIpcMemClose");
    DL_LOAD_SYM(pAclrtGetSocName, aclrtGetSocNameFunc, rtHandle, "aclrtGetSocName");
    DL_LOAD_SYM(pRtEnableP2P, rtEnableP2PFunc, rtHandle, "rtEnableP2P");
    DL_LOAD_SYM(pRtDisableP2P, rtDisableP2PFunc, rtHandle, "rtDisableP2P");
    DL_LOAD_SYM(pRtGetLogicDevIdByUserDevId, rtGetLogicDevIdByUserDevIdFunc, rtHandle, "rtGetLogicDevIdByUserDevId");
    DL_LOAD_SYM_OPTIONAL(pAclrtGetPhyDevIdByLogicDevId, aclrtGetPhyDevIdByLogicDevIdFunc, rtHandle,
                         "aclrtGetPhyDevIdByLogicDevId");
    DL_LOAD_SYM(pRtMemcpyAsync, rtMemcpyAsyncFunc, rtHandle, "rtMemcpyAsyncWithoutCheckKind");

    DL_LOAD_SYM_OPTIONAL(pAclrtMallocPhysical, aclrtMallocPhysicalFunc, rtHandle, "aclrtMallocPhysical");
    DL_LOAD_SYM_OPTIONAL(pAclrtFreePhysical, aclrtFreePhysicalFunc, rtHandle, "aclrtFreePhysical");
    DL_LOAD_SYM_OPTIONAL(pAclrtReserveMemAddress, aclrtReserveMemAddressFunc, rtHandle, "aclrtReserveMemAddress");
    DL_LOAD_SYM_OPTIONAL(pAclrtReleaseMemAddress, aclrtReleaseMemAddressFunc, rtHandle, "aclrtReleaseMemAddress");
    DL_LOAD_SYM_OPTIONAL(pAclrtMapMem, aclrtMapMemFunc, rtHandle, "aclrtMapMem");
    DL_LOAD_SYM_OPTIONAL(pAclrtUnmapMem, aclrtUnmapMemFunc, rtHandle, "aclrtUnmapMem");
    DL_LOAD_SYM_OPTIONAL(pAclrtMemExportToShareableHandle, aclrtMemExportToShareableHandleFunc, rtHandle,
                         "aclrtMemExportToShareableHandle");
    DL_LOAD_SYM_OPTIONAL(pAclrtMemImportFromShareableHandle, aclrtMemImportFromShareableHandleFunc, rtHandle,
                         "aclrtMemImportFromShareableHandle");
    DL_LOAD_SYM_OPTIONAL(pAclrtMemSetAccess, aclrtMemSetAccessFunc, rtHandle, "aclrtMemSetAccess");
    DL_LOAD_SYM_OPTIONAL(pAclrtMemGetAllocationGranularity, aclrtMemGetAllocationGranularityFunc, rtHandle,
                         "aclrtMemGetAllocationGranularity");

    gLoaded = true;
    return BM_OK;
}

AscendSocType DlAclApi::GetAscendSocType()
{
    static AscendSocType cachedSocType = [&]() -> AscendSocType {
        auto name = DlAclApi::AclrtGetSocName();
        if (name == nullptr) {
            BM_LOG_WARN("AclrtGetSocName() failed.");
            return AscendSocType::ASCEND_UNKNOWN;
        }
        BM_LOG_DEBUG("success get soc name: " << name);
        std::string socName{name};
        if (socName.find("Ascend910B") != std::string::npos) {
            return AscendSocType::ASCEND_910B;
        } else if (socName.find("Ascend910_93") != std::string::npos) {
            return AscendSocType::ASCEND_910C;
        } else if (socName.find("Ascend910_95") != std::string::npos ||
                   socName.find("Ascend950") != std::string::npos) {
            return AscendSocType::ASCEND_950;
        } else if (socName.find("Ascend310P3") != std::string::npos ||
                   socName.find("Ascend_310P3") != std::string::npos) {
            return AscendSocType::ASCEND_310P3;
        } else if (socName.find("Ascend310P1") != std::string::npos ||
                   socName.find("Ascend_310P1") != std::string::npos) {
            return AscendSocType::ASCEND_310P1;
        }

        return AscendSocType::ASCEND_UNKNOWN;
    }();

    return cachedSocType;
}

void DlAclApi::CleanupLibrary()
{
    std::lock_guard<std::mutex> guard(gMutex);
    if (!gLoaded) {
        return;
    }

    pAclrtGetDevice = nullptr;
    pAclrtSetDevice = nullptr;
    pAclrtDeviceEnablePeerAccess = nullptr;
    pAclrtDeviceDisablePeerAccess = nullptr;
    pAclrtDeviceCanAccessPeer = nullptr;
    pAclrtCreateStream = nullptr;
    pAclrtCreateStreamWithConfig = nullptr;
    pAclrtDestroyStream = nullptr;
    pAclrtSynchronizeStream = nullptr;
    pAclrtBinaryLoadFromFile = nullptr;
    pAclrtBinaryGetFunction = nullptr;
    pAclrtKernelArgsInit = nullptr;
    pAclrtKernelArgsAppend = nullptr;
    pAclrtKernelArgsFinalize = nullptr;
    pAclrtLaunchKernelWithConfig = nullptr;
    pAclrtMalloc = nullptr;
    pAclrtFree = nullptr;
    pAclrtMallocHost = nullptr;
    pAclrtFreeHost = nullptr;
    pAclrtMemcpy = nullptr;
    pAclrtMemcpyAsync = nullptr;
    pAclrtMemcpy2d = nullptr;
    pAclrtMemcpy2dAsync = nullptr;
    pAclrtMemset = nullptr;
    pRtDeviceGetBareTgid = nullptr;
    pRtGetDeviceInfo = nullptr;
    pRtSetIpcMemorySuperPodPid = nullptr;
    pRtIpcDestroyMemoryName = nullptr;
    pRtIpcSetMemoryName = nullptr;
    pRtEnableP2P = nullptr;
    pRtDisableP2P = nullptr;
    pAclrtStreamGetId = nullptr;
    pAclrtCreateNotify = nullptr;
    pAclrtGetNotifyId = nullptr;
    pAclrtDestroyNotify = nullptr;
    pAclrtWaitAndResetNotify = nullptr;
    pAclrtGetCurrentContext = nullptr;
    pAclrtCreateContext = nullptr;
    pAclrtSetCurrentContext = nullptr;
    pAclrtSetStreamAttribute = nullptr;
    pAclrtGetPhyDevIdByLogicDevId = nullptr;
    pAclrtMallocPhysical = nullptr;
    pAclrtFreePhysical = nullptr;
    pAclrtReserveMemAddress = nullptr;
    pAclrtReleaseMemAddress = nullptr;
    pAclrtMapMem = nullptr;
    pAclrtUnmapMem = nullptr;
    pAclrtMemExportToShareableHandle = nullptr;
    pAclrtMemImportFromShareableHandle = nullptr;
    pAclrtMemSetAccess = nullptr;
    pAclrtMemGetAllocationGranularity = nullptr;
    pAclrtIpcMemGetExportKey = nullptr;
    pAclrtIpcMemImportByKey = nullptr;
    pAclrtIpcMemClose = nullptr;

    if (rtHandle != nullptr) {
        dlclose(rtHandle);
        rtHandle = nullptr;
    }
    gLoaded = false;
}
} // namespace mf
} // namespace ock
