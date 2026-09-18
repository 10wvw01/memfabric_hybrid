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

#ifndef MF_HYBM_CORE_DL_ACL_API_H
#define MF_HYBM_CORE_DL_ACL_API_H

#include "hybm_common_include.h"

namespace ock {
namespace mf {

enum class aclrtMemLocationType {
    ACL_MEM_LOCATION_TYPE_HOST = 0, // Host内存
    ACL_MEM_LOCATION_TYPE_DEVICE,   // Device内存
};

using aclrtMemLocation = struct aclrtMemLocation;
struct aclrtMemLocation {
    uint32_t id;
    aclrtMemLocationType type; // 内存所在位置
};

using aclrtMemcpyBatchAttr = struct aclrtMemcpyBatchAttr;
struct aclrtMemcpyBatchAttr {
    aclrtMemLocation dstLoc;
    aclrtMemLocation srcLoc;
    uint8_t rsv[16];
};

typedef enum {
    ACL_STREAM_ATTR_FAILURE_MODE = 1,
    ACL_STREAM_ATTR_FLOAT_OVERFLOW_CHECK = 2,
    ACL_STREAM_ATTR_USER_CUSTOM_TAG = 3,
    ACL_STREAM_ATTR_CACHE_OP_INFO = 4,
} aclrtStreamAttr;

typedef union {
    uint64_t failureMode;
    uint32_t overflowSwitch;
    uint32_t userCustomTag;
    uint32_t cacheOpInfoSwitch;
    uint32_t reserve[4];
} aclrtStreamAttrValue;

using aclError = int32_t;
constexpr aclError ACL_SUCCESS = 0;
using aclrtBinHandle = void *;
using aclrtFuncHandle = void *;
using aclrtArgsHandle = void *;
using aclrtParamHandle = void *;
using aclrtDrvMemHandle = void *;   // aclrtMallocPhysical 返回的物理内存句柄
using aclrtContext = void *;        // aclrtCreateContext 返回的 context 句柄

/* ---- ACL VMM 相关本地声明(与 CANN acl_rt.h 对齐, 310P 内存池使用) ----
 * 对齐版本: CANN 9.1.0 (aarch64-linux/include/acl/acl_rt.h)。
 * 不直接 include CANN 头以避免构建环境强依赖; 若 CANN 升级改变以下 ABI,
 * 需同步核对字段布局(与 app/zbal 的 dl_cann_api_def.h 各自独立维护, 见方案文档 TODO)。 */
enum class aclrtMemHandleType : uint32_t {
    ACL_MEM_HANDLE_TYPE_NONE = 0,
    ACL_MEM_HANDLE_TYPE_POSIX = 2,
};

enum class aclrtMemAllocationType : uint32_t {
    ACL_MEM_ALLOCATION_TYPE_PINNED = 0,
};

enum class aclrtMemAttr : uint32_t {
    ACL_MEM_NORMAL = 0,
    ACL_MEM_HUGE,
    ACL_MEM_HUGE1G,
    ACL_MEM_P2P_NORMAL,
    ACL_MEM_P2P_HUGE,
    ACL_MEM_P2P_HUGE1G,
    ACL_HBM_MEM_NORMAL,
    ACL_HBM_MEM_HUGE,
};

enum class aclrtMemAccessFlags : uint32_t {
    ACL_RT_MEM_ACCESS_FLAGS_NONE = 0x0,
    ACL_RT_MEM_ACCESS_FLAGS_READ = 0x1,
    ACL_RT_MEM_ACCESS_FLAGS_READWRITE = 0x3,
};

enum class aclrtMemGranularityOptions : uint32_t {
    ACL_RT_MEM_ALLOC_GRANULARITY_MINIMUM = 0,
    ACL_RT_MEM_ALLOC_GRANULARITY_RECOMMENDED,
};

constexpr uint64_t ACL_RT_VMM_EXPORT_FLAG_DISABLE_PID_VALIDATION = 0x1;

struct aclrtMemAccessDesc {
    aclrtMemAccessFlags flags;
    aclrtMemLocation location;
    uint8_t rsv[12];
};

struct aclrtPhysicalMemProp {
    aclrtMemHandleType handleType;
    aclrtMemAllocationType allocationType;
    aclrtMemAttr memAttr;
    aclrtMemLocation location;
    uint64_t reserve;
};

enum class aclrtBinaryLoadOptionType : int32_t {
    ACL_RT_BINARY_LOAD_OPT_LAZY_LOAD = 1,
    ACL_RT_BINARY_LOAD_OPT_LAZY_MAGIC = 2,
    ACL_RT_BINARY_LOAD_OPT_MAGIC = 2,
    ACL_RT_BINARY_LOAD_OPT_CPU_KERNEL_MODE = 3,
};

union aclrtBinaryLoadOptionValue {
    uint32_t isLazyLoad;
    uint32_t magic;
    int32_t cpuKernelMode;
    uint32_t rsv[4];
};

struct aclrtBinaryLoadOption {
    aclrtBinaryLoadOptionType type;
    aclrtBinaryLoadOptionValue value;
};

struct aclrtBinaryLoadOptions {
    aclrtBinaryLoadOption *options;
    size_t numOpt;
};

typedef enum aclrtLaunchKernelAttrId {
    ACL_RT_LAUNCH_KERNEL_ATTR_SCHEM_MODE = 1, // 调度模式
    ACL_RT_LAUNCH_KERNEL_ATTR_DYN_UBUF_SIZE = 2, // 用于指定SIMT算子执行时需要的VECTOR CORE内部UB buffer的大小
    ACL_RT_LAUNCH_KERNEL_ATTR_ENGINE_TYPE = 3,     // 算子执行引擎
    ACL_RT_LAUNCH_KERNEL_ATTR_BLOCKDIM_OFFSET,     // numBlocks偏移量
    ACL_RT_LAUNCH_KERNEL_ATTR_BLOCK_TASK_PREFETCH, // 任务下发时，是否阻止硬件预取本任务的信息
    ACL_RT_LAUNCH_KERNEL_ATTR_DATA_DUMP,           // 是否开启Dump
    ACL_RT_LAUNCH_KERNEL_ATTR_TIMEOUT,             // 任务调度器等待任务执行的超时时间，单位秒
    ACL_RT_LAUNCH_KERNEL_ATTR_TIMEOUT_US = 8,      // 任务调度器等待任务执行的超时时间，单位微秒
} aclrtLaunchKernelAttrId;

struct aclrtTimeoutUs {
    uint32_t timeoutLow;
    uint32_t timeoutHigh;
};

union aclrtLaunchKernelAttrValue {
    uint8_t schemMode;
    uint32_t localMemorySize;
    uint32_t engineType;
    uint32_t numBlocksOffset;
    uint8_t isBlockTaskPrefetch;
    uint8_t isDataDump;
    uint16_t timeout;
    aclrtTimeoutUs timeoutUs;
    uint32_t rsv[4];
};

struct aclrtLaunchKernelAttr {
    aclrtLaunchKernelAttrId id;
    aclrtLaunchKernelAttrValue value;
};

struct aclrtLaunchKernelCfg {
    aclrtLaunchKernelAttr *attrs;
    size_t numAttrs;
};

typedef enum aclrtMemMallocPolicy {
    ACL_MEM_MALLOC_HUGE_FIRST,
    ACL_MEM_MALLOC_HUGE_ONLY,
    ACL_MEM_MALLOC_NORMAL_ONLY,
    ACL_MEM_MALLOC_HUGE_FIRST_P2P,
    ACL_MEM_MALLOC_HUGE_ONLY_P2P,
    ACL_MEM_MALLOC_NORMAL_ONLY_P2P,
    ACL_MEM_MALLOC_HUGE1G_ONLY,
    ACL_MEM_MALLOC_HUGE1G_ONLY_P2P,
    ACL_MEM_TYPE_LOW_BAND_WIDTH = 0x0100U,
    ACL_MEM_TYPE_HIGH_BAND_WIDTH = 0x1000U,
    ACL_MEM_ACCESS_USER_SPACE_READONLY = 0x100000U,
} aclrtMemMallocPolicy;

using aclrtSetDeviceFunc = int32_t (*)(int32_t);
using aclrtGetDeviceFunc = int32_t (*)(int32_t *);
using aclrtDeviceEnablePeerAccessFunc = int32_t (*)(int32_t, uint32_t);
using aclrtCreateStreamFunc = int (*)(void **);
using aclrtCreateStreamWithConfigFunc = int (*)(void **, int32_t, uint32_t);
using aclrtStreamGetIdFunc = int (*)(void *, int32_t *);
using aclrtCreateNotifyFunc = int (*)(void **, uint64_t);
using aclrtGetNotifyIdFunc = int (*)(void *, uint32_t *);
using aclrtDestroyNotifyFunc = int (*)(void *);
using aclrtWaitAndResetNotifyFunc = int (*)(void *, void *, uint32_t);
using aclrtGetCurrentContextFunc = int (*)(void **);
using aclrtSetStreamAttributeFunc = int (*)(void *, aclrtStreamAttr, aclrtStreamAttrValue *);
using aclrtDestroyStreamFunc = int (*)(void *);
using aclrtSynchronizeStreamFunc = int (*)(void *);
using aclrtMallocFunc = int32_t (*)(void **, size_t, uint32_t);
using aclrtFreeFunc = int (*)(void *);
using aclrtMallocHostFunc = int32_t (*)(void **, size_t);
using aclrtFreeHostFunc = int (*)(void *);
using aclrtMemcpyFunc = int32_t (*)(void *, size_t, const void *, size_t, uint32_t);
using aclrtMemcpyAsyncFunc = int32_t (*)(void *, size_t, const void *, size_t, uint32_t, void *);
using aclrtMemcpy2dFunc = int32_t (*)(void *, size_t, const void *, size_t, size_t, size_t, uint32_t);
using aclrtMemcpy2dAsyncFunc = int32_t (*)(void *, size_t, const void *, size_t, size_t, size_t, uint32_t, void *);
using aclrtMemsetFunc = int32_t (*)(void *, size_t, int32_t, size_t);
using rtDeviceGetBareTgidFunc = int32_t (*)(uint32_t *);
using rtGetDeviceInfoFunc = int32_t (*)(uint32_t, int32_t, int32_t, int64_t *val);
using rtIpcSetMemoryNameFunc = int32_t (*)(const void *, uint64_t, char *, uint32_t);
using rtSetIpcMemorySuperPodPidFunc = int32_t (*)(const char *, uint32_t, int32_t *, int32_t);
using rtIpcDestroyMemoryNameFunc = int32_t (*)(const char *);
using rtEnableP2PFunc = int32_t (*)(uint32_t, uint32_t, uint32_t);
using rtDisableP2PFunc = int32_t (*)(uint32_t, uint32_t);
using rtGetLogicDevIdByUserDevIdFunc = int32_t (*)(const int32_t, int32_t *const);
using aclrtGetPhyDevIdByLogicDevIdFunc = int32_t (*)(const int32_t, int32_t *const);
using rtIpcOpenMemoryFunc = int32_t (*)(void **, const char *);
using rtIpcCloseMemoryFunc = int32_t (*)(const void *);
using rtMemcpyAsyncFunc = int32_t (*)(void *, size_t, const void *, size_t, uint32_t, void *);
using aclrtGetSocNameFunc = const char *(*)();
using aclrtCreateContextFunc = int32_t (*)(aclrtContext *, int32_t);
using aclrtSetCurrentContextFunc = int32_t (*)(aclrtContext);
using aclrtDestroyContextFunc = int32_t (*)(aclrtContext);
using aclrtMallocPhysicalFunc = int32_t (*)(aclrtDrvMemHandle *, size_t, const aclrtPhysicalMemProp *, uint64_t);
using aclrtFreePhysicalFunc = int32_t (*)(aclrtDrvMemHandle);
using aclrtReserveMemAddressFunc = int32_t (*)(void **, size_t, size_t, void *, uint64_t);
using aclrtReleaseMemAddressFunc = int32_t (*)(void *);
using aclrtMapMemFunc = int32_t (*)(void *, size_t, size_t, aclrtDrvMemHandle, uint64_t);
using aclrtUnmapMemFunc = int32_t (*)(void *);
using aclrtMemSetAccessFunc = int32_t (*)(void *, size_t, aclrtMemAccessDesc *, size_t);
using aclrtMemExportToShareableHandleFunc = int32_t (*)(aclrtDrvMemHandle, aclrtMemHandleType, uint64_t, uint64_t *);
using aclrtMemImportFromShareableHandleFunc = int32_t (*)(uint64_t, int32_t, aclrtDrvMemHandle *);
using aclrtMemSetPidToShareableHandleFunc = int32_t (*)(uint64_t, int32_t *, size_t);
using aclrtMemGetAllocationGranularityFunc = int32_t (*)(aclrtPhysicalMemProp *, aclrtMemGranularityOptions,
                                                         size_t *);
using aclrtMemcpyBatchFunc = int32_t (*)(void **, size_t *, void **, size_t *, size_t, aclrtMemcpyBatchAttr *, size_t *,
                                         size_t, size_t *);
using aclrtBinaryLoadFromFileFunc = int32_t (*)(const char *, aclrtBinaryLoadOptions *, aclrtBinHandle *);
using aclrtBinaryGetFunctionFunc = int32_t (*)(const aclrtBinHandle, const char *, aclrtFuncHandle *);
using aclrtKernelArgsInitFunc = int32_t (*)(aclrtFuncHandle, aclrtArgsHandle *);
using aclrtKernelArgsAppendFunc = int32_t (*)(aclrtArgsHandle, void *, size_t, aclrtParamHandle *);
using aclrtKernelArgsFinalizeFunc = int32_t (*)(aclrtArgsHandle);
using aclrtLaunchKernelWithConfigFunc = int32_t (*)(aclrtFuncHandle, uint32_t, void *, aclrtLaunchKernelCfg *,
                                                    aclrtArgsHandle, void *);
/* host args 版 launch(KFC 通道用; placeHolderArray 仅传 nullptr, 以 void* 镜像声明) */
using aclrtLaunchKernelWithHostArgsFunc = int32_t (*)(aclrtFuncHandle, uint32_t, void *,
                                                      aclrtLaunchKernelCfg *, void *, size_t, void *, size_t);

class DlAclApi {
public:
    static Result LoadLibrary(const std::string &libDirPath);
    static void CleanupLibrary();
    static AscendSocType GetAscendSocType();
    static AscendSocType ParseAscendSocName(const std::string &socName);

    static inline Result AclrtSetDevice(int32_t deviceId, bool force = false)
    {
        if (pAclrtSetDevice == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        if (force) {
            return pAclrtSetDevice(deviceId);
        }
        int32_t nowDeviceId = -1;
        if (AclrtGetDevice(&nowDeviceId) == 0 && nowDeviceId == deviceId) {
            return BM_OK;
        } else {
            return pAclrtSetDevice(deviceId);
        }
    }

    static inline Result AclrtGetDevice(int32_t *deviceId)
    {
        if (pAclrtGetDevice == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pAclrtGetDevice(deviceId);
    }

    static inline Result AclrtDeviceEnablePeerAccess(int32_t peerDeviceId, uint32_t flags)
    {
        if (pAclrtDeviceEnablePeerAccess == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pAclrtDeviceEnablePeerAccess(peerDeviceId, flags);
    }

    static inline Result AclrtCreateStream(void **stream)
    {
        if (pAclrtCreateStream == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pAclrtCreateStream(stream);
    }

    static inline Result AclrtCreateStreamWithConfig(void **stream, uint32_t prot, uint32_t config)
    {
        if (pAclrtCreateStreamWithConfig == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pAclrtCreateStreamWithConfig(stream, prot, config);
    }

    static inline Result AclrtStreamGetId(void *stream, int32_t *streamId)
    {
        if (pAclrtStreamGetId == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pAclrtStreamGetId(stream, streamId);
    }

    static inline Result AclrtCreateNotify(void **notify, uint64_t flag)
    {
        if (pAclrtCreateNotify == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pAclrtCreateNotify(notify, flag);
    }

    static inline Result AclrtGetNotifyId(void *notify, uint32_t *notifyId)
    {
        if (pAclrtGetNotifyId == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pAclrtGetNotifyId(notify, notifyId);
    }

    static inline Result AclrtDestroyNotify(void *notify)
    {
        if (pAclrtDestroyNotify == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pAclrtDestroyNotify(notify);
    }

    static inline Result AclrtWaitAndResetNotify(void *notify, void *stream, uint32_t timeout)
    {
        if (pAclrtWaitAndResetNotify == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pAclrtWaitAndResetNotify(notify, stream, timeout);
    }

    static inline Result AclrtGetCurrentContext(void **context)
    {
        if (pAclrtGetCurrentContext == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pAclrtGetCurrentContext(context);
    }

    static inline Result AclrtSetStreamAttribute(void *stream, aclrtStreamAttr stmAttrType, aclrtStreamAttrValue *value)
    {
        if (pAclrtSetStreamAttribute == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pAclrtSetStreamAttribute(stream, stmAttrType, value);
    }

    static inline Result AclrtDestroyStream(void *stream)
    {
        if (pAclrtDestroyStream == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pAclrtDestroyStream(stream);
    }

    static inline Result AclrtSynchronizeStream(void *stream)
    {
        if (pAclrtSynchronizeStream == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pAclrtSynchronizeStream(stream);
    }

    static inline Result AclrtBinaryLoadFromFile(const char *binPath, aclrtBinaryLoadOptions *options,
                                                 aclrtBinHandle *binHandle)
    {
        if (pAclrtBinaryLoadFromFile == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pAclrtBinaryLoadFromFile(binPath, options, binHandle);
    }

    static inline Result AclrtBinaryGetFunction(aclrtBinHandle binHandle, const char *kernelName,
                                                aclrtFuncHandle *funcHandle)
    {
        if (pAclrtBinaryGetFunction == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pAclrtBinaryGetFunction(binHandle, kernelName, funcHandle);
    }

    static inline Result AclrtKernelArgsInit(aclrtFuncHandle funcHandle, aclrtArgsHandle *argsHandle)
    {
        if (pAclrtKernelArgsInit == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pAclrtKernelArgsInit(funcHandle, argsHandle);
    }

    static inline Result AclrtKernelArgsAppend(aclrtArgsHandle argsHandle, void *param, size_t paramSize,
                                               aclrtParamHandle *paramHandle)
    {
        if (pAclrtKernelArgsAppend == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pAclrtKernelArgsAppend(argsHandle, param, paramSize, paramHandle);
    }

    static inline Result AclrtKernelArgsFinalize(aclrtArgsHandle argsHandle)
    {
        if (pAclrtKernelArgsFinalize == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pAclrtKernelArgsFinalize(argsHandle);
    }

    static inline Result AclrtLaunchKernelWithConfig(aclrtFuncHandle funcHandle, uint32_t blockDim, void *stream,
                                                     aclrtLaunchKernelCfg *cfg, aclrtArgsHandle argsHandle,
                                                     void *reserved)
    {
        if (pAclrtLaunchKernelWithConfig == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pAclrtLaunchKernelWithConfig(funcHandle, blockDim, stream, cfg, argsHandle, reserved);
    }

    /* host args 版 kernel 提交(KFC 通道: 命令串作为 host args 透传设备 OS 执行) */
    static inline Result AclrtLaunchKernelWithHostArgs(aclrtFuncHandle funcHandle, uint32_t numBlocks,
                                                       void *stream, aclrtLaunchKernelCfg *cfg, void *hostArgs,
                                                       size_t argsSize, void *placeHolderArray,
                                                       size_t placeHolderNum)
    {
        if (pAclrtLaunchKernelWithHostArgs == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pAclrtLaunchKernelWithHostArgs(funcHandle, numBlocks, stream, cfg, hostArgs, argsSize,
                                              placeHolderArray, placeHolderNum);
    }

    static inline Result AclrtMalloc(void **ptr, size_t count, uint32_t type)
    {
        if (pAclrtMalloc == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pAclrtMalloc(ptr, count, type);
    }

    static inline Result AclrtFree(void *ptr)
    {
        if (pAclrtFree == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pAclrtFree(ptr);
    }

    static inline Result AclrtMallocHost(void **ptr, size_t count)
    {
        if (pAclrtMallocHost == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pAclrtMallocHost(ptr, count);
    }

    static inline Result AclrtFreeHost(void *ptr)
    {
        if (pAclrtFreeHost == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pAclrtFreeHost(ptr);
    }

    static inline Result AclrtMemcpy(void *dst, size_t destMax, const void *src, size_t count, uint32_t kind)
    {
        if (pAclrtMemcpy == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pAclrtMemcpy(dst, destMax, src, count, kind);
    }

    static inline Result AclrtMemcpyAsync(void *dst, size_t destMax, const void *src, size_t count, uint32_t kind,
                                          void *stream)
    {
        if (pAclrtMemcpyAsync == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pAclrtMemcpyAsync(dst, destMax, src, count, kind, stream);
    }

    static inline Result AclrtMemcpyBatch(void **dsts, size_t *destMax, void **srcs, size_t *sizes, size_t numBatches,
                                          aclrtMemcpyBatchAttr *attrs, size_t *attrsIndexes, size_t numAttrs,
                                          size_t *failIndex)
    {
        if (pAclrtMemcpyBatch == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pAclrtMemcpyBatch(dsts, destMax, srcs, sizes, numBatches, attrs, attrsIndexes, numAttrs, failIndex);
    }

    static inline Result AclrtMemcpy2d(void *dst, size_t dpitch, const void *src, size_t spitch, size_t width,
                                       size_t height, uint32_t kind)
    {
        if (pAclrtMemcpy2d == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pAclrtMemcpy2d(dst, dpitch, src, spitch, width, height, kind);
    }

    static inline Result AclrtMemcpy2dAsync(void *dst, size_t dpitch, const void *src, size_t spitch, size_t width,
                                            size_t height, uint32_t kind, void *stream)
    {
        if (pAclrtMemcpy2dAsync == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pAclrtMemcpy2dAsync(dst, dpitch, src, spitch, width, height, kind, stream);
    }

    static inline Result AclrtMemset(void *dst, size_t destMax, int32_t value, size_t count)
    {
        if (pAclrtMemset == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pAclrtMemset(dst, destMax, value, count);
    }

    static inline Result RtDeviceGetBareTgid(uint32_t *pid)
    {
        if (pRtDeviceGetBareTgid == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pRtDeviceGetBareTgid(pid);
    }

    static inline Result RtGetDeviceInfo(uint32_t deviceId, int32_t moduleType, int32_t infoType, int64_t *val)
    {
        if (pRtGetDeviceInfo == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pRtGetDeviceInfo(deviceId, moduleType, infoType, val);
    }

    static inline Result RtSetIpcMemorySuperPodPid(const char *name, uint32_t sdid, int32_t pid[], int32_t num)
    {
        if (pRtSetIpcMemorySuperPodPid == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pRtSetIpcMemorySuperPodPid(name, sdid, pid, num);
    }

    static inline Result RtIpcSetMemoryName(const void *ptr, uint64_t byteCount, char *name, uint32_t len)
    {
        if (pRtIpcSetMemoryName == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pRtIpcSetMemoryName(ptr, byteCount, name, len);
    }

    static inline Result RtIpcDestroyMemoryName(const char *name)
    {
        if (pRtIpcDestroyMemoryName == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pRtIpcDestroyMemoryName(name);
    }

    static inline Result RtIpcOpenMemory(void **ptr, const char *name)
    {
        if (pRtIpcOpenMemory == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pRtIpcOpenMemory(ptr, name);
    }

    static inline Result RtIpcCloseMemory(const void *ptr)
    {
        if (pRtIpcCloseMemory == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pRtIpcCloseMemory(ptr);
    }

    static inline const char *AclrtGetSocName()
    {
#ifdef NO_XPU
        return nullptr;
#endif
        if (pAclrtGetSocName == nullptr) {
            BM_LOG_ERROR("pAclrtGetSocName is nullptr, bm under api unload.");
            return nullptr;
        }
        return pAclrtGetSocName();
    }

    static inline Result RtEnableP2P(uint32_t devIdDes, uint32_t phyIdSrc, uint32_t flag)
    {
        if (pRtEnableP2P == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pRtEnableP2P(devIdDes, phyIdSrc, flag);
    }

    static inline Result RtDisableP2P(uint32_t devIdDes, uint32_t phyIdSrc)
    {
        if (pRtDisableP2P == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pRtDisableP2P(devIdDes, phyIdSrc);
    }

    static inline Result RtGetLogicDevIdByUserDevId(const int32_t userDevId, int32_t *const logicDevId)
    {
        if (pRtGetLogicDevIdByUserDevId == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pRtGetLogicDevIdByUserDevId(userDevId, logicDevId);
    }

    static inline Result AclrtGetPhyDevIdByLogicDevId(const int32_t logicDevId, int32_t *const phyDevId)
    {
        if (pAclrtGetPhyDevIdByLogicDevId == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pAclrtGetPhyDevIdByLogicDevId(logicDevId, phyDevId);
    }

    static inline Result RtMemcpyAsync(void *dst, size_t destMax, const void *src, size_t count, uint32_t kind,
                                       void *stream)
    {
        if (pRtMemcpyAsync == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pRtMemcpyAsync(dst, destMax, src, count, kind, stream);
    }

    static inline Result AclrtCreateContext(aclrtContext *context, int32_t deviceId)
    {
        if (pAclrtCreateContext == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pAclrtCreateContext(context, deviceId);
    }

    static inline Result AclrtSetCurrentContext(aclrtContext context)
    {
        if (pAclrtSetCurrentContext == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pAclrtSetCurrentContext(context);
    }

    static inline Result AclrtDestroyContext(aclrtContext context)
    {
        if (pAclrtDestroyContext == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pAclrtDestroyContext(context);
    }

    static inline Result AclrtMallocPhysical(aclrtDrvMemHandle *handle, size_t size, const aclrtPhysicalMemProp *prop,
                                             uint64_t flags)
    {
        if (pAclrtMallocPhysical == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pAclrtMallocPhysical(handle, size, prop, flags);
    }

    static inline Result AclrtFreePhysical(aclrtDrvMemHandle handle)
    {
        if (pAclrtFreePhysical == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pAclrtFreePhysical(handle);
    }

    static inline Result AclrtReserveMemAddress(void **virPtr, size_t size, size_t alignment, void *expectPtr,
                                                uint64_t flags)
    {
        if (pAclrtReserveMemAddress == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pAclrtReserveMemAddress(virPtr, size, alignment, expectPtr, flags);
    }

    static inline Result AclrtReleaseMemAddress(void *virPtr)
    {
        if (pAclrtReleaseMemAddress == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pAclrtReleaseMemAddress(virPtr);
    }

    static inline Result AclrtMapMem(void *virPtr, size_t size, size_t offset, aclrtDrvMemHandle handle,
                                     uint64_t flags)
    {
        if (pAclrtMapMem == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pAclrtMapMem(virPtr, size, offset, handle, flags);
    }

    static inline Result AclrtUnmapMem(void *virPtr)
    {
        if (pAclrtUnmapMem == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pAclrtUnmapMem(virPtr);
    }

    static inline Result AclrtMemSetAccess(void *virPtr, size_t size, aclrtMemAccessDesc *desc, size_t count)
    {
        if (pAclrtMemSetAccess == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pAclrtMemSetAccess(virPtr, size, desc, count);
    }

    static inline Result AclrtMemExportToShareableHandle(aclrtDrvMemHandle handle, aclrtMemHandleType handleType,
                                                         uint64_t flags, uint64_t *shareableHandle)
    {
        if (pAclrtMemExportToShareableHandle == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pAclrtMemExportToShareableHandle(handle, handleType, flags, shareableHandle);
    }

    static inline Result AclrtMemImportFromShareableHandle(uint64_t shareableHandle, int32_t deviceId,
                                                           aclrtDrvMemHandle *handle)
    {
        if (pAclrtMemImportFromShareableHandle == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pAclrtMemImportFromShareableHandle(shareableHandle, deviceId, handle);
    }

    static inline Result AclrtMemSetPidToShareableHandle(uint64_t shareableHandle, int32_t *pid, size_t pidNum)
    {
        if (pAclrtMemSetPidToShareableHandle == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pAclrtMemSetPidToShareableHandle(shareableHandle, pid, pidNum);
    }

    static inline Result AclrtMemGetAllocationGranularity(aclrtPhysicalMemProp *prop,
                                                          aclrtMemGranularityOptions option, size_t *granularity)
    {
        if (pAclrtMemGetAllocationGranularity == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pAclrtMemGetAllocationGranularity(prop, option, granularity);
    }

private:
    static std::mutex gMutex;
    static bool gLoaded;
    static void *rtHandle;
    static const char *gAscendAclLibName;

    static aclrtSetDeviceFunc pAclrtSetDevice;
    static aclrtGetDeviceFunc pAclrtGetDevice;
    static aclrtDeviceEnablePeerAccessFunc pAclrtDeviceEnablePeerAccess;
    static aclrtCreateStreamFunc pAclrtCreateStream;
    static aclrtCreateStreamWithConfigFunc pAclrtCreateStreamWithConfig;
    static aclrtStreamGetIdFunc pAclrtStreamGetId;
    static aclrtCreateNotifyFunc pAclrtCreateNotify;
    static aclrtGetNotifyIdFunc pAclrtGetNotifyId;
    static aclrtDestroyNotifyFunc pAclrtDestroyNotify;
    static aclrtWaitAndResetNotifyFunc pAclrtWaitAndResetNotify;
    static aclrtGetCurrentContextFunc pAclrtGetCurrentContext;
    static aclrtSetStreamAttributeFunc pAclrtSetStreamAttribute;
    static aclrtDestroyStreamFunc pAclrtDestroyStream;
    static aclrtSynchronizeStreamFunc pAclrtSynchronizeStream;
    static aclrtBinaryLoadFromFileFunc pAclrtBinaryLoadFromFile;
    static aclrtBinaryGetFunctionFunc pAclrtBinaryGetFunction;
    static aclrtKernelArgsInitFunc pAclrtKernelArgsInit;
    static aclrtKernelArgsAppendFunc pAclrtKernelArgsAppend;
    static aclrtKernelArgsFinalizeFunc pAclrtKernelArgsFinalize;
    static aclrtLaunchKernelWithConfigFunc pAclrtLaunchKernelWithConfig;
    static aclrtLaunchKernelWithHostArgsFunc pAclrtLaunchKernelWithHostArgs;
    static aclrtMallocFunc pAclrtMalloc;
    static aclrtFreeFunc pAclrtFree;
    static aclrtMallocHostFunc pAclrtMallocHost;
    static aclrtFreeHostFunc pAclrtFreeHost;
    static aclrtMemcpyFunc pAclrtMemcpy;
    static aclrtMemcpyBatchFunc pAclrtMemcpyBatch;
    static aclrtMemcpyAsyncFunc pAclrtMemcpyAsync;
    static aclrtMemcpy2dFunc pAclrtMemcpy2d;
    static aclrtMemcpy2dAsyncFunc pAclrtMemcpy2dAsync;
    static aclrtMemsetFunc pAclrtMemset;
    static rtDeviceGetBareTgidFunc pRtDeviceGetBareTgid;
    static rtGetDeviceInfoFunc pRtGetDeviceInfo;
    static rtSetIpcMemorySuperPodPidFunc pRtSetIpcMemorySuperPodPid;
    static rtIpcSetMemoryNameFunc pRtIpcSetMemoryName;
    static rtIpcDestroyMemoryNameFunc pRtIpcDestroyMemoryName;
    static rtIpcOpenMemoryFunc pRtIpcOpenMemory;
    static rtIpcCloseMemoryFunc pRtIpcCloseMemory;
    static aclrtGetSocNameFunc pAclrtGetSocName;
    static rtEnableP2PFunc pRtEnableP2P;
    static rtDisableP2PFunc pRtDisableP2P;
    static rtMemcpyAsyncFunc pRtMemcpyAsync;
    static rtGetLogicDevIdByUserDevIdFunc pRtGetLogicDevIdByUserDevId;
    static aclrtGetPhyDevIdByLogicDevIdFunc pAclrtGetPhyDevIdByLogicDevId;
    static aclrtCreateContextFunc pAclrtCreateContext;
    static aclrtSetCurrentContextFunc pAclrtSetCurrentContext;
    static aclrtDestroyContextFunc pAclrtDestroyContext;
    static aclrtMallocPhysicalFunc pAclrtMallocPhysical;
    static aclrtFreePhysicalFunc pAclrtFreePhysical;
    static aclrtReserveMemAddressFunc pAclrtReserveMemAddress;
    static aclrtReleaseMemAddressFunc pAclrtReleaseMemAddress;
    static aclrtMapMemFunc pAclrtMapMem;
    static aclrtUnmapMemFunc pAclrtUnmapMem;
    static aclrtMemSetAccessFunc pAclrtMemSetAccess;
    static aclrtMemExportToShareableHandleFunc pAclrtMemExportToShareableHandle;
    static aclrtMemImportFromShareableHandleFunc pAclrtMemImportFromShareableHandle;
    static aclrtMemSetPidToShareableHandleFunc pAclrtMemSetPidToShareableHandle;
    static aclrtMemGetAllocationGranularityFunc pAclrtMemGetAllocationGranularity;
};
} // namespace mf
} // namespace ock

#endif // MF_HYBM_CORE_DL_ACL_API_H
