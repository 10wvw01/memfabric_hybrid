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
#include <cstdint>
#include <cstddef>
#include <cstdlib>

constexpr int32_t RETURN_OK = 0;
constexpr int32_t RETURN_ERROR = -1;
constexpr uint64_t START_ADDR = 0x100000000000ULL;

/* UT 失败注入/计数: 非 0 时 aclrtSynchronizeStream 返回错误(stream 自恢复用例) */
extern "C" {
int32_t g_stubSyncStreamFail = 0;
int32_t g_stubSyncStreamCalls = 0;
int32_t g_stubCreateStreamCalls = 0;
int32_t g_stubDestroyStreamCalls = 0;
int32_t g_stubSetCurrentContextFail = 0; // 非 0 时 SetCurrentContext 返回错误(die 对导入防护用例)
int32_t g_stubSetDeviceFail = 0;         // 非 0 时 SetDevice 返回错误(die 对导入恢复失败用例)
int32_t g_stubDestroyContextCalls = 0;   // 计数: context 销毁次数(验证失败时跳过销毁)
int32_t g_stubMallocP2pOom = 0;          // 非 0 时 memAttr=P2P(4/5) 的 MallocPhysical 返回 207001(分区 OOM 注入)
}

extern "C" {
int32_t aclrtSetDevice(int32_t deviceId)
{
    (void)deviceId;
    return g_stubSetDeviceFail != 0 ? RETURN_ERROR : RETURN_OK;
}

int32_t aclrtGetDevice(int32_t *deviceId)
{
    if (deviceId != nullptr) {
        *deviceId = 0;
    }
    return RETURN_OK;
}

int32_t aclrtDeviceEnablePeerAccess(int32_t peerDeviceId, uint32_t flags)
{
    return RETURN_OK;
}

int32_t aclrtCreateStream(void **stream)
{
    g_stubCreateStreamCalls++;
    return RETURN_OK;
}

int32_t aclrtDestroyStream(void *stream)
{
    g_stubDestroyStreamCalls++;
    return RETURN_OK;
}

int32_t aclrtSynchronizeStream(void *stream)
{
    g_stubSyncStreamCalls++;
    return g_stubSyncStreamFail != 0 ? RETURN_ERROR : RETURN_OK;
}

int32_t aclrtMalloc(void **ptr, size_t count, uint32_t type)
{
    if (ptr == nullptr) {
        return RETURN_ERROR;
    }
    *ptr = (void *)START_ADDR;
    return RETURN_OK;
}

int32_t aclrtFree(void *ptr)
{
    return RETURN_OK;
}

int32_t aclrtMemcpy(void *dst, size_t destMax, const void *src, size_t count, uint32_t kind)
{
    return RETURN_OK;
}

int32_t aclrtMemcpyAsync(void *dst, size_t destMax, const void *src, size_t count, uint32_t kind, void *stream)
{
    if (stream != nullptr) {
        *reinterpret_cast<uint64_t *>(stream) += 1;
    }
    return RETURN_OK;
}

int32_t rtMemcpyAsyncWithoutCheckKind(void *dst, size_t destMax, const void *src, size_t count, uint32_t kind,
                                      void *stream)
{
    if (stream != nullptr) {
        *reinterpret_cast<uint64_t *>(stream) += 1;
    }
    return RETURN_OK;
}

int32_t aclrtMemcpy2d(void *dst, size_t dpitch, const void *src, size_t spitch, size_t width, size_t height,
                      uint32_t kind)
{
    return RETURN_OK;
}

int32_t aclrtMemcpy2dAsync(void *dst, size_t dpitch, const void *src, size_t spitch, size_t width, size_t height,
                           uint32_t kind, void *stream)
{
    return RETURN_OK;
}

int32_t aclrtMemset(void *dst, size_t destMax, int32_t value, size_t count)
{
    return RETURN_OK;
}

int32_t rtDeviceGetBareTgid(uint32_t *pid)
{
    if (pid != nullptr) {
        *pid = 0;
    }
    return RETURN_OK;
}

int32_t rtGetDeviceInfo(uint32_t deviceId, int32_t moduleType, int32_t infoType, int64_t *val)
{
    if (val != nullptr) {
        *val = 0;
    }
    return RETURN_OK;
}

int32_t rtSetIpcMemorySuperPodPid(const char *name, uint32_t sdid, int32_t pid[], int32_t num)
{
    return RETURN_OK;
}

int32_t rtIpcSetMemoryName(const void *ptr, uint64_t byteCount, char *name, uint32_t len)
{
    return RETURN_OK;
}

int32_t rtIpcDestroyMemoryName(const char *name)
{
    return RETURN_OK;
}

int32_t rtIpcOpenMemory(void **ptr, const char *name)
{
    if (ptr == nullptr) {
        return RETURN_ERROR;
    }
    *ptr = reinterpret_cast<void *>(0x3200);
    return RETURN_OK;
}

int32_t aclrtCreateStreamWithConfig(void **stream, uint32_t prot, uint32_t config)
{
    return 0;
}

int32_t aclrtStreamGetId(void *stream, int32_t *streamId)
{
    return 0;
}

int32_t aclrtCreateNotify(void **notify, uint64_t flag)
{
    return 0;
}

int32_t aclrtGetNotifyId(void *notify, uint32_t *notifyId)
{
    return 0;
}

int32_t aclrtDestroyNotify(void *notify)
{
    return 0;
}

int32_t aclrtWaitAndResetNotify(void *notify, void *stream, uint32_t timeout)
{
    return 0;
}

int32_t aclrtGetCurrentContext(void **context)
{
    return 0;
}

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

int32_t aclrtSetStreamAttribute(void *stream, aclrtStreamAttr stmAttrType, aclrtStreamAttrValue *value)
{
    return 0;
}

int32_t aclrtMallocHost(void **ptr, size_t count)
{
    (*ptr) = malloc(count);
    return 0;
}

int32_t aclrtFreeHost(void *ptr)
{
    free(ptr);
    return 0;
}

int32_t rtIpcCloseMemory(const void *ptr)
{
    return 0;
}

char *aclrtGetSocName()
{
    static char soc[] = "Ascend910_93";
    return soc;
}

int32_t rtEnableP2P(uint32_t devIdDes, uint32_t phyIdSrc, uint32_t flag)
{
    return 0;
}

int32_t rtDisableP2P(uint32_t devIdDes, uint32_t phyIdSrc)
{
    return 0;
}

int32_t rtGetLogicDevIdByUserDevId(const int32_t userDevId, int32_t *const logicDevId)
{
    *logicDevId = userDevId;
    return 0;
}

int32_t aclrtGetPhyDevIdByLogicDevId(const int32_t logicDevId, int32_t *const phyDevId)
{
    *phyDevId = logicDevId;
    return 0;
}

int32_t rtStreamGetSqid(const void *stm, uint32_t *sqId)
{
    if (sqId != nullptr) {
        *sqId = 0;
    }
    return RETURN_OK;
}

int32_t rtStreamGetCqid(const void *stm, uint32_t *cqId, uint32_t *logicCqId)
{
    if (cqId != nullptr) {
        *cqId = 0;
    }
    if (logicCqId != nullptr) {
        *logicCqId = 0;
    }
    return RETURN_OK;
}

struct aclTensor {};
struct aclOpExecutor {};

typedef enum {
    ACL_FORMAT_UNDEFINED = -1,
    ACL_FORMAT_NCHW = 0,
    ACL_FORMAT_NHWC = 1,
    ACL_FORMAT_ND = 2,
    ACL_FORMAT_NC1HWC0 = 3,
} aclFormat;

typedef enum {
    ACL_DT_UNDEFINED = -1,
    ACL_FLOAT = 0,
    ACL_FLOAT16 = 1,
    ACL_INT8 = 2,
    ACL_INT32 = 3,
    ACL_UINT8 = 4,
    ACL_INT16 = 6,
    ACL_UINT16 = 7,
    ACL_UINT32 = 8,
    ACL_INT64 = 9,
    ACL_UINT64 = 10,
    ACL_DOUBLE = 11,
    ACL_BOOL = 12,
    ACL_STRING = 13,
    ACL_COMPLEX64 = 16,
    ACL_COMPLEX128 = 17,
    ACL_BF16 = 27,
    ACL_INT4 = 29,
    ACL_UINT1 = 30,
    ACL_COMPLEX32 = 33,
} aclDataType;

aclTensor *aclCreateTensor(const int64_t *viewDims, uint64_t viewDimsNum, aclDataType dataType, const int64_t *stride,
                           int64_t offset, aclFormat format, const int64_t *storageDims, uint64_t storageDimsNum,
                           void *tensorData)
{
    (void)viewDims;
    (void)viewDimsNum;
    (void)dataType;
    (void)stride;
    (void)offset;
    (void)format;
    (void)storageDims;
    (void)storageDimsNum;
    (void)tensorData;
    return reinterpret_cast<aclTensor *>(0xFFFF0000ULL);
}

int32_t aclDestroyTensor(aclTensor *tensor)
{
    (void)tensor;
    return RETURN_OK;
}

int32_t aclnnShmemSdmaStarsQueryGetWorkspaceSize(aclTensor *input, aclTensor *output, uint64_t *workspaceSize,
                                                 aclOpExecutor **executor)
{
    (void)input;
    (void)output;
    if (workspaceSize != nullptr) {
        *workspaceSize = 0;
    }
    if (executor != nullptr) {
        *executor = reinterpret_cast<aclOpExecutor *>(0xAAAA0000ULL);
    }
    return RETURN_OK;
}

int32_t aclnnShmemSdmaStarsQuery(void *workspace, uint64_t workspaceSize, aclOpExecutor *executor, void *stream)
{
    (void)workspace;
    (void)workspaceSize;
    (void)executor;
    (void)stream;
    return RETURN_OK;
}

typedef void *aclrtBinHandle;
typedef void *aclrtFuncHandle;
typedef void *aclrtArgsHandle;
typedef void *aclrtParamHandle;

struct aclrtBinaryLoadOptions {};
struct aclrtLaunchKernelCfg {};

int32_t aclrtBinaryLoadFromFile(const char *binPath, aclrtBinaryLoadOptions *options, aclrtBinHandle *binHandle)
{
    (void)binPath;
    (void)options;
    if (binHandle != nullptr) {
        *binHandle = reinterpret_cast<void *>(0x1001);
    }
    return RETURN_OK;
}

int32_t aclrtBinaryGetFunction(const aclrtBinHandle binHandle, const char *kernelName, aclrtFuncHandle *funcHandle)
{
    (void)binHandle;
    (void)kernelName;
    if (funcHandle != nullptr) {
        *funcHandle = reinterpret_cast<void *>(0x1002);
    }
    return RETURN_OK;
}

int32_t aclrtKernelArgsInit(aclrtFuncHandle funcHandle, aclrtArgsHandle *argsHandle)
{
    (void)funcHandle;
    if (argsHandle != nullptr) {
        *argsHandle = reinterpret_cast<void *>(0x1003);
    }
    return RETURN_OK;
}

int32_t aclrtKernelArgsAppend(aclrtArgsHandle argsHandle, void *param, size_t paramSize, aclrtParamHandle *paramHandle)
{
    (void)argsHandle;
    (void)param;
    (void)paramSize;
    if (paramHandle != nullptr) {
        *paramHandle = reinterpret_cast<void *>(0x1004);
    }
    return RETURN_OK;
}

int32_t aclrtKernelArgsFinalize(aclrtArgsHandle argsHandle)
{
    (void)argsHandle;
    return RETURN_OK;
}

int32_t aclrtLaunchKernelWithConfig(aclrtFuncHandle funcHandle, uint32_t blockDim, void *stream,
                                    aclrtLaunchKernelCfg *cfg, aclrtArgsHandle argsHandle, void *reserved)
{
    (void)funcHandle;
    (void)blockDim;
    (void)stream;
    (void)cfg;
    (void)argsHandle;
    (void)reserved;
    return RETURN_OK;
}

/* ---- ACL VMM 符号 stub(310P 内存池, 与 dl_acl_api 本地类型对齐) ---- */
static uint64_t g_stubVmmBase = 0x12c140000000UL;

int32_t aclrtCreateContext(void **context, int32_t deviceId)
{
    (void)deviceId;
    if (context != nullptr) {
        *context = reinterpret_cast<void *>(0x2001);
    }
    return RETURN_OK;
}

int32_t aclrtSetCurrentContext(void *context)
{
    (void)context;
    return g_stubSetCurrentContextFail != 0 ? RETURN_ERROR : RETURN_OK;
}

int32_t aclrtDestroyContext(void *context)
{
    (void)context;
    g_stubDestroyContextCalls++;
    return RETURN_OK;
}

int32_t aclrtMallocPhysical(void **handle, size_t size, const void *prop, uint64_t flags)
{
    (void)size;
    (void)flags;
    if (handle != nullptr) {
        *handle = reinterpret_cast<void *>(0x2002);
    }
    if (g_stubMallocP2pOom != 0 && prop != nullptr) {
        /* aclrtPhysicalMemProp 布局: handleType(4B)+allocationType(4B)+memAttr(4B), 偏移 8 处取 memAttr;
         * 值与 CANN aclrtMemAttr 对齐: 15=ACL_MEM_P2P_NORMAL, 16=ACL_MEM_P2P_HUGE */
        auto memAttr = *reinterpret_cast<const uint32_t *>(reinterpret_cast<const uint8_t *>(prop) + 8);
        if (memAttr == 15U || memAttr == 16U) {
            return 207001; // RT_ERROR_DRV_OUT_MEMORY(P2P 分区 OOM)
        }
    }
    return RETURN_OK;
}

int32_t aclrtFreePhysical(void *handle)
{
    (void)handle;
    return RETURN_OK;
}

int32_t aclrtReserveMemAddress(void **virPtr, size_t size, size_t alignment, void *expectPtr, uint64_t flags)
{
    (void)size;
    (void)alignment;
    (void)flags;
    if (virPtr == nullptr) {
        return -1;
    }
    if (expectPtr != nullptr) {
        *virPtr = expectPtr;
    } else {
        *virPtr = reinterpret_cast<void *>(g_stubVmmBase);
    }
    return RETURN_OK;
}

int32_t aclrtReleaseMemAddress(void *virPtr)
{
    (void)virPtr;
    return RETURN_OK;
}

int32_t aclrtMapMem(void *virPtr, size_t size, size_t offset, void *handle, uint64_t flags)
{
    (void)virPtr;
    (void)size;
    (void)offset;
    (void)handle;
    (void)flags;
    return RETURN_OK;
}

int32_t aclrtUnmapMem(void *virPtr)
{
    (void)virPtr;
    return RETURN_OK;
}

int32_t aclrtMemSetAccess(void *virPtr, size_t size, void *desc, size_t count)
{
    (void)virPtr;
    (void)size;
    (void)desc;
    (void)count;
    return RETURN_OK;
}

int32_t aclrtMemExportToShareableHandle(void *handle, uint32_t handleType, uint64_t flags, uint64_t *shareableHandle)
{
    (void)handle;
    (void)handleType;
    (void)flags;
    if (shareableHandle != nullptr) {
        *shareableHandle = 0x2003UL;
    }
    return RETURN_OK;
}

int32_t aclrtMemImportFromShareableHandle(uint64_t shareableHandle, int32_t deviceId, void **handle)
{
    (void)shareableHandle;
    (void)deviceId;
    if (handle != nullptr) {
        *handle = reinterpret_cast<void *>(0x2004);
    }
    return RETURN_OK;
}

int32_t aclrtMemSetPidToShareableHandle(uint64_t shareableHandle, int32_t *pid, size_t pidNum)
{
    (void)shareableHandle;
    (void)pid;
    (void)pidNum;
    return RETURN_OK;
}

int32_t aclrtMemGetAllocationGranularity(void *prop, uint32_t option, size_t *granularity)
{
    (void)prop;
    (void)option;
    if (granularity != nullptr) {
        *granularity = 2UL * 1024UL * 1024UL; /* 2MB, 与 310P 实测一致 */
    }
    return RETURN_OK;
}
}
