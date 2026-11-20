/*
 * main.cpp — 寄存器同步端到端验证 host 侧
 *
 * 验证流程(单设备, 不走 memfabric 池化, 只用 AclrtMalloc 普通显存):
 *   1. halMemCtl 拿 AICore 0 控制寄存器基址 → 构造 regBase[10] 数组 → H2D
 *   2. launch AICore kernel 1: set_cond(0xABCD) 写 COND 寄存器
 *   3. launch AICPU kernel: 轮询 regBase[0]+0x4C8 读到 0xABCD → 写 regBase[0]+0xA0=0x1234
 *   4. launch AICore kernel 2: get_data_main_base() 读 DATA_MAIN_BASE(应=0x1234)
 *   5. D2H 读回验证 C1(AICore set_cond→AICPU 读 0x4C8) + C2(AICPU 写 0xA0→AICore 读 DMB)
 *
 * 编译: CMakeLists.txt (AICore ascendc_library + host g++ + AICPU hcc 交叉编译)
 * 运行: ./reg_sync_e2e <devId>
 */
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <unistd.h>
#include <fstream>
#include <iterator>
#include <vector>
#include "acl/acl.h"
#include "reg_sync_aicore_dev.h"

/* ---- halMemCtl 类型定义(抄 hybm_sdma_orchestrator.cpp) ---- */
constexpr uint32_t HYBM_ADDR_MAP_TYPE_REG_AIC_CTRL = 2;
struct HybmAddrMapInPara { uint32_t addr_type; uint32_t devid; };
struct HybmAddrMapOutPara { uint64_t ptr; uint64_t len; };
typedef int (*halMemCtl_t)(int type, void *in, size_t inSize, void *out, size_t *outSizeRet);
constexpr char HYBM_HAL_LIB_PATH[] = "/usr/local/Ascend/driver/lib64/driver/libascend_hal.so";

/* ---- 常量 ---- */
constexpr uint64_t EXPECT_COND = 0xABCDULL;  /* AICore set_cond 写的值 */
constexpr uint64_t WRITE_DMB = 0x1234ULL;     /* AICPU 写 0xA0 的值 */
constexpr uint32_t REG_BASE_CORES = 10;
constexpr uint64_t REG_CORE_STRIDE = 0x200000ULL;
constexpr uint32_t RESULT_SLOTS = 32;
constexpr uint64_t RESULT_SIZE = RESULT_SLOTS * sizeof(uint64_t);

#define CHECK_ACL(x) do { \
    aclError __ret = x; \
    if (__ret != ACL_SUCCESS) { \
        fprintf(stderr, "[E] %s:%d aclError=%d\n", __FILE__, __LINE__, (int)__ret); \
        return 1; \
    } \
} while (0)

/* halMemCtl 查 AICore 控制寄存器基址 */
static bool QueryAicoreRegs(uint32_t devId, uint64_t &regBase0)
{
    void *halLib = dlopen(HYBM_HAL_LIB_PATH, RTLD_NOW | RTLD_GLOBAL);
    if (halLib == nullptr) {
        fprintf(stderr, "[E] dlopen libascend_hal.so failed: %s\n", dlerror());
        return false;
    }
    auto halMemCtl = reinterpret_cast<halMemCtl_t>(dlsym(halLib, "halMemCtl"));
    if (halMemCtl == nullptr) {
        fprintf(stderr, "[E] dlsym halMemCtl failed: %s\n", dlerror());
        dlclose(halLib);
        return false;
    }
    HybmAddrMapInPara inPara{};
    inPara.addr_type = HYBM_ADDR_MAP_TYPE_REG_AIC_CTRL;
    inPara.devid = devId;
    HybmAddrMapOutPara outPara{};
    size_t outSize = sizeof(outPara);
    int ret = halMemCtl(0, &inPara, sizeof(inPara), &outPara, &outSize);
    dlclose(halLib);
    if (ret != 0 || outPara.ptr == 0 || outPara.len == 0) {
        fprintf(stderr, "[E] halMemCtl failed: ret=%d ptr=0x%lx len=%lu\n", ret, outPara.ptr, outPara.len);
        return false;
    }
    printf("[i] halMemCtl AIC_CTRL: base=0x%lx len=%luMB\n", outPara.ptr, outPara.len / (1024 * 1024));
    regBase0 = outPara.ptr; /* AICore 0 的控制寄存器基址 */
    return true;
}

/* ---- CP1 extend lib loader ABI 镜像(抄 hybm_sdma_orchestrator.cpp:62-153) ----
 * loader 在 CP1 内读 DeviceArgs.aicpuSoBin/aicpuSoLen/deviceId(偏移 96/104/112)取 so 字节;
 * DeviceKernelArgs.cfgdata(偏移 40)承载本端参数指针. */
struct ExtendDeviceKernelArgs {
    int64_t *ctrlFlowCache{nullptr};
    int64_t *inputs{nullptr};
    int64_t *outputs{nullptr};
    int64_t *workspace{nullptr};
    int64_t *tilingdata{nullptr};
    int64_t *cfgdata{nullptr};
    int64_t *commContexts{nullptr};
    void *costmodeldata{nullptr};
    void *aicoreModel{nullptr};
    uint64_t taskWastTime{0};
    uint64_t maxDynamicAssembleOutcastMem{0};
    uint64_t maxDynamicCellMatchTableMem{0};
    uint8_t machineConfig{0};
    uint8_t pad[3]{0};
    uint8_t subMachineConfig[4]{0};
    uint32_t runMode{0};
    uint32_t p1{0};
    uint64_t globalRound{0};
    uint64_t ctrlBlockNum{0};
};
struct ExtendDeviceArgs {
    uint32_t nrAic{0};
    uint32_t nrAiv{0};
    uint32_t nrAicpu{0};
    uint32_t nrValidAic{0};
    uint64_t opaque{0};
    uint64_t devQueueAddr{0};
    uint64_t sharedBuffer{0};
    uint64_t coreRegAddr{0};
    uint64_t corePmuRegAddr{0};
    uint64_t corePmuAddr{0};
    uint64_t pmuEventAddr{0};
    uint64_t taskType : 4;
    uint64_t machineConfig : 8;
    uint64_t taskId : 52;
    uint64_t taskData{0};
    uint64_t taskWastTime{0};
    uint64_t aicpuSoBin{0};
    uint64_t aicpuSoLen{0};
    uint64_t deviceId{0};
    uint64_t runtimeDataRingBufferAddr{0};
    uint64_t dynamicCellMatchAddr{0};
    uint64_t dynamicCellMatchCapacity{0};
    uint32_t hostPid{0};
    uint32_t scheCpuNum{0};
    uint32_t enableCtrl : 2;
    uint32_t validGetPgMask : 30;
    uint64_t aicpuPerfAddr{0};
    uint64_t devDfxArgAddr{0};
    uint32_t maxAicpuNum{0};
    uint32_t launchSchedAicpuNum{7};
    bool launchSchedSameCluster{true};
    bool enableVFFusion{false};
    bool enableEslModel{false};
    bool hasAicpuTask{false};
    bool commByReg{false};
    uint32_t archInfo{2002};
    uint32_t profValue{0};
};
struct ExtendArgs {
    ExtendDeviceKernelArgs kArgs;
    char kernelName[32];
    char soName[32];
    char opName[32];
};
struct ExtendRtArgs {
    void *args;
    void *hostInputInfoPtr;
    void *kernelOffsetInfoPtr;
    uint32_t argsSize;
    uint16_t hostInputInfoNum;
    uint16_t kernelOffsetInfoNum;
    uint32_t soNameAddrOffset;
    uint32_t kernelNameAddrOffset;
    uint8_t isNoNeedH2DCopy;
    uint16_t timeout;
    uint8_t reserved;
};
static_assert(offsetof(ExtendDeviceArgs, aicpuSoBin) == 96, "aicpuSoBin offset");
static_assert(offsetof(ExtendDeviceArgs, aicpuSoLen) == 104, "aicpuSoLen offset");
static_assert(offsetof(ExtendDeviceArgs, deviceId) == 112, "deviceId offset");
static_assert(offsetof(ExtendDeviceKernelArgs, cfgdata) == 40, "cfgdata offset");
static_assert(offsetof(ExtendArgs, kernelName) == 128, "kernelName offset");
static_assert(offsetof(ExtendArgs, soName) == 160, "soName offset");
static_assert(sizeof(ExtendArgs) == 224, "ExtendArgs size");

using RtAicpuLaunchFn = int (*)(uint32_t, const char *, uint32_t, const ExtendRtArgs *, void *, const void *,
                                uint32_t);
constexpr char EXTEND_SO[] = "libaicpu_extend_kernels.so"; /* CP1 预加载的 CANN extend 库 */
constexpr char EXTEND_INIT_FUNC[] = "DynTileFwkKernelServerInit";
constexpr char EXTEND_EXEC_FUNC[] = "DynTileFwkKernelServer";
constexpr uint32_t EXTEND_KERNEL_TYPE = 5;      /* AICPU_KFC */
constexpr uint32_t EXTEND_LAUNCH_FLAG = 0x100U; /* RT_KERNEL_USE_SPECIAL_TIMEOUT */
constexpr uint16_t EXTEND_TIMEOUT_NEVER = 0xFFFFU;

/* 与 reg_sync_aicpu_kernel.cc 的 RegSyncAicpuParams 布局一致(经 cfgdata 传入) */
struct RegSyncAicpuParams {
    uint64_t regBaseAddr;
    uint64_t resultAddr;
    uint64_t expectCond;
    uint64_t writeDmb;
    uint64_t coreId;
    uint64_t recvCoreId;
};

static RtAicpuLaunchFn ResolveRtLaunch()
{
    auto fn = reinterpret_cast<RtAicpuLaunchFn>(dlsym(RTLD_DEFAULT, "rtAicpuKernelLaunchExWithArgs"));
    if (fn != nullptr) {
        return fn;
    }
    void *lib = dlopen("libruntime.so", RTLD_NOW | RTLD_GLOBAL);
    if (lib == nullptr) {
        fprintf(stderr, "[E] dlopen libruntime.so failed: %s\n", dlerror());
        return nullptr;
    }
    fn = reinterpret_cast<RtAicpuLaunchFn>(dlsym(lib, "rtAicpuKernelLaunchExWithArgs"));
    if (fn == nullptr) {
        fprintf(stderr, "[E] dlsym rtAicpuKernelLaunchExWithArgs failed: %s\n", dlerror());
    }
    return fn;
}

/* 经 CP1 extend loader 下发一次: init=加载 so 填函数表, exec=分发到本 so 同名导出 */
static int LaunchExtend(RtAicpuLaunchFn launch, const char *func, void *cfgdataDev, void *stream, bool isExec)
{
    ExtendArgs args{};
    args.kArgs.cfgdata = reinterpret_cast<int64_t *>(cfgdataDev);
    (void)snprintf(args.kernelName, sizeof(args.kernelName), "%s", func);
    (void)snprintf(args.soName, sizeof(args.soName), "%s", EXTEND_SO);
    ExtendRtArgs rt{};
    rt.args = &args;
    rt.argsSize = sizeof(args);
    rt.kernelNameAddrOffset = offsetof(ExtendArgs, kernelName);
    rt.soNameAddrOffset = offsetof(ExtendArgs, soName);
    if (isExec) {
        rt.timeout = EXTEND_TIMEOUT_NEVER; /* 永不超时(与主链路一致) */
    }
    return launch(EXTEND_KERNEL_TYPE, "AST_DYN_AICPU", 1, &rt, nullptr, stream,
                  isExec ? EXTEND_LAUNCH_FLAG : 0U);
}

int main(int argc, char **argv)
{
    int devId = (argc >= 2) ? atoi(argv[1]) : 4;

    printf("=== 寄存器同步端到端验证 (C1+C2) ===\n");
    printf("目标设备: devId=%d\n\n", devId);

    /* 0. ACL 初始化 + SetDevice(halMemCtl 前置要求: 设备需先 set 否则返回错误码 4) */
    CHECK_ACL(aclInit(nullptr));
    CHECK_ACL(aclrtSetDevice(devId));
    printf("[i] aclrtSetDevice(%d) OK\n", devId);

    /* 1. halMemCtl 查 AICore 控制寄存器基址 */
    uint64_t regBase0 = 0;
    if (!QueryAicoreRegs(static_cast<uint32_t>(devId), regBase0)) {
        fprintf(stderr, "[E] halMemCtl 查询失败, commByReg 降级\n");
        return 1;
    }
    printf("[i] regBase[0] = 0x%lx\n\n", regBase0);

    aclrtStream aicoreStream = nullptr;
    aclrtStream aicpuStream = nullptr;
    CHECK_ACL(aclrtCreateStream(&aicoreStream));
    CHECK_ACL(aclrtCreateStream(&aicpuStream));

    /* 3. 分配 regBase 数组设备内存 + H2D */
    uint64_t regBaseHost[REG_BASE_CORES] = {0};
    regBaseHost[0] = regBase0;
    for (uint32_t i = 1; i < REG_BASE_CORES; i++) {
        regBaseHost[i] = regBase0 + i * REG_CORE_STRIDE;
    }
    uint64_t *regBaseDev = nullptr;
    CHECK_ACL(aclrtMalloc((void **)&regBaseDev, REG_BASE_CORES * sizeof(uint64_t), ACL_MEM_MALLOC_HUGE_FIRST));
    CHECK_ACL(aclrtMemcpy(regBaseDev, REG_BASE_CORES * sizeof(uint64_t), regBaseHost,
                          REG_BASE_CORES * sizeof(uint64_t), ACL_MEMCPY_HOST_TO_DEVICE));
    printf("[i] regBase 数组 H2D 完成: devAddr=%p\n", (void *)regBaseDev);

    /* 4. 分配 GM 结果 buffer(AICore + AICPU 共用, 16×u64) */
    uint64_t *resultDev = nullptr;
    CHECK_ACL(aclrtMalloc((void **)&resultDev, RESULT_SIZE, ACL_MEM_MALLOC_HUGE_FIRST));
    uint64_t zeros[RESULT_SLOTS] = {0};
    CHECK_ACL(aclrtMemcpy(resultDev, RESULT_SIZE, zeros, RESULT_SIZE, ACL_MEMCPY_HOST_TO_DEVICE));
    printf("[i] GM result buffer: devAddr=%p (%lu bytes)\n\n", (void *)resultDev, RESULT_SIZE);

    /* ========== 步骤 1: AICore set_cond(0xABCD) ========== */
    printf("--- 步骤 1: AICore set_cond(0x%lx) 写 COND 寄存器 ---\n", EXPECT_COND);
    RegSyncAicoreSend_do(static_cast<void *>(aicoreStream), reinterpret_cast<uint8_t *>(resultDev), EXPECT_COND);
    CHECK_ACL(aclrtSynchronizeStream(aicoreStream));
    printf("[i] AICore kernel 1 (Send) 完成\n");

    /* 验证 AICore kernel 1 执行 + 读出 coreId(AICPU 用 regBase[coreId] 访问正确核心) */
    uint64_t result1[RESULT_SLOTS] = {0};
    CHECK_ACL(aclrtMemcpy(result1, RESULT_SIZE, resultDev, RESULT_SIZE, ACL_MEMCPY_DEVICE_TO_HOST));
    printf("  [0] set_cond 回显 = 0x%lx %s\n", result1[0], result1[0] == EXPECT_COND ? "✅" : "❌");
    printf("  [1] 尾标记       = 0x%lx %s\n", result1[1], result1[1] == 0xDEADBEEF ? "✅" : "❌");
    uint64_t aicoreCoreId = result1[4];
    printf("  [4] AICore coreId = %lu\n\n", aicoreCoreId);

    /* ========== 步骤 2: AICPU 轮询 regBase[coreId]+0x4C8 + 写 0xA0 ==========
     * 部署走 CP1 extend loader(与主链路一致, 绕开已失效的 KFC 搜索路径):
     *   1) so 字节 H2D → DeviceArgs.aicpuSoBin
     *   2) 下发 DynTileFwkKernelServerInit: loader 在 CP1 内 memfd 加载本 so 并填函数表
     *   3) 下发 DynTileFwkKernelServer: CP1 内分发到本 so 的同名导出, 参数经 cfgdata 传入 */
    printf("--- 步骤 2: AICPU 轮询 regBase[%lu]+0x4C8 (期望 0x%lx) → 写 0xA0=0x%lx ---\n",
           aicoreCoreId, EXPECT_COND, WRITE_DMB);

    const char *aicpuSoPath = std::getenv("REG_SYNC_AICPU_SO");
    if (aicpuSoPath == nullptr) {
        aicpuSoPath = "build/libreg_sync_aicpu.so"; /* 默认 CWD 下构建产物 */
    }
    std::ifstream soInput(aicpuSoPath, std::ios::binary);
    if (!soInput) {
        fprintf(stderr, "[E] AICPU so not accessible: %s\n", aicpuSoPath);
        return 1;
    }
    std::vector<char> soBytes((std::istreambuf_iterator<char>(soInput)), std::istreambuf_iterator<char>());
    if (soBytes.empty()) {
        fprintf(stderr, "[E] AICPU so empty: %s\n", aicpuSoPath);
        return 1;
    }
    void *devSo = nullptr;
    CHECK_ACL(aclrtMalloc(&devSo, soBytes.size(), ACL_MEM_MALLOC_HUGE_FIRST));
    CHECK_ACL(aclrtMemcpy(devSo, soBytes.size(), soBytes.data(), soBytes.size(), ACL_MEMCPY_HOST_TO_DEVICE));
    printf("[i] AICPU so H2D: %s (%zu bytes) devAddr=%p\n", aicpuSoPath, soBytes.size(), devSo);

    /* DeviceArgs: loader 只读 aicpuSoBin/aicpuSoLen/deviceId(偏移 96/104/112) */
    ExtendDeviceArgs da{};
    da.deviceId = static_cast<uint64_t>(devId);
    da.archInfo = 2002; /* DAV_2002 */
    da.nrAic = 10;
    da.nrValidAic = 8;
    da.nrAicpu = 5;
    da.maxAicpuNum = 15;
    da.scheCpuNum = 3;
    da.hostPid = static_cast<uint32_t>(getpid());
    da.aicpuSoBin = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(devSo));
    da.aicpuSoLen = soBytes.size();
    void *devArgs = nullptr;
    CHECK_ACL(aclrtMalloc(&devArgs, sizeof(da), ACL_MEM_MALLOC_HUGE_FIRST));
    CHECK_ACL(aclrtMemcpy(devArgs, sizeof(da), &da, sizeof(da), ACL_MEMCPY_HOST_TO_DEVICE));

    RtAicpuLaunchFn rtLaunch = ResolveRtLaunch();
    if (rtLaunch == nullptr) {
        return 1;
    }
    int ret = LaunchExtend(rtLaunch, EXTEND_INIT_FUNC, devArgs, aicpuStream, false);
    if (ret != 0) {
        fprintf(stderr, "[E] extend loader init launch failed: %d\n", ret);
        return 1;
    }
    aclError syncRet = aclrtSynchronizeStream(aicpuStream);
    if (syncRet != ACL_SUCCESS) {
        fprintf(stderr, "[E] extend loader init sync failed: %d\n", (int)syncRet);
        return 1;
    }
    printf("[i] extend loader 部署成功(memfd 加载 + 函数表填充)\n");

    /* 先启动 AICore kernel 2(轮询 DATA_MAIN_BASE), 等它跑起来再让 AICPU 写 0xA0:
     * 若先写后派发, 派发时 runtime 会重置该 SPR. */
    printf("--- 步骤 3(提前): 启动 AICore Recv 轮询 DATA_MAIN_BASE ---\n");
    uint64_t clearSlots[2] = {0, 0};
    CHECK_ACL(aclrtMemcpy(resultDev + 2, 2 * sizeof(uint64_t), clearSlots, 2 * sizeof(uint64_t),
                          ACL_MEMCPY_HOST_TO_DEVICE));
    CHECK_ACL(aclrtMemcpy(resultDev + 6, sizeof(uint64_t), clearSlots, sizeof(uint64_t),
                          ACL_MEMCPY_HOST_TO_DEVICE));
    RegSyncAicoreRecv_do(static_cast<void *>(aicoreStream), reinterpret_cast<uint8_t *>(resultDev), 2);
    /* 等 Recv kernel 启动标记(slot 6 = 0xAAAA0001), 最长 ~5s;
     * 同时读 slot 7 = Recv 实际所在核(Recv kernel 启动时写入) —
     * 0xA0 必须写给 Recv 所在核, 旧版按 Send 的 coreId 写而 Send/Recv 常不同核 → 假阴性 */
    bool recvStarted = false;
    uint64_t recvCoreId = 0;
    for (int i = 0; i < 500; i++) {
        uint64_t markerAndCore[2] = {0, 0};
        CHECK_ACL(aclrtMemcpy(markerAndCore, 2 * sizeof(uint64_t), resultDev + 6, 2 * sizeof(uint64_t),
                              ACL_MEMCPY_DEVICE_TO_HOST));
        if (markerAndCore[0] == 0xAAAA0001ULL) {
            recvStarted = true;
            recvCoreId = markerAndCore[1];
            break;
        }
        usleep(10000);
    }
    printf("[i] AICore Recv 启动标记: %s, Recv 所在核 = %lu\n", recvStarted ? "已就绪" : "未看到(继续)",
           recvCoreId);

    /* 参数经 cfgdata 传入(设备地址) */
    RegSyncAicpuParams params{};
    params.regBaseAddr = reinterpret_cast<uint64_t>(regBaseDev);
    params.resultAddr = reinterpret_cast<uint64_t>(resultDev);
    params.expectCond = EXPECT_COND;
    params.writeDmb = WRITE_DMB;
    params.coreId = aicoreCoreId;
    params.recvCoreId = recvCoreId;
    void *devParams = nullptr;
    CHECK_ACL(aclrtMalloc(&devParams, sizeof(params), ACL_MEM_MALLOC_HUGE_FIRST));
    CHECK_ACL(aclrtMemcpy(devParams, sizeof(params), &params, sizeof(params), ACL_MEMCPY_HOST_TO_DEVICE));

    ret = LaunchExtend(rtLaunch, EXTEND_EXEC_FUNC, devParams, aicpuStream, true);
    if (ret != 0) {
        fprintf(stderr, "[E] extend loader exec launch failed: %d\n", ret);
        return 1;
    }
    /* AICPU kernel sync: 507018 = kernel 退出码非 0, 仍需读 GM 诊断 */
    syncRet = aclrtSynchronizeStream(aicpuStream);
    if (syncRet != ACL_SUCCESS) {
        printf("[i] AICPU kernel sync 返回 %d (507018=kernel 退出码非0, 读 GM 诊断)\n", (int)syncRet);
    } else {
        printf("[i] AICPU kernel 完成\n");
    }

    /* 验证 AICPU kernel 结果(写到 resultDev[8..15]) — 无论 sync 是否成功都读 */
    uint64_t result2[RESULT_SLOTS] = {0};
    aclError cpyRet = aclrtMemcpy(result2, RESULT_SIZE, resultDev, RESULT_SIZE, ACL_MEMCPY_DEVICE_TO_HOST);
    if (cpyRet != ACL_SUCCESS) {
        fprintf(stderr, "[E] D2H result2 failed: %d\n", (int)cpyRet);
        return 1;
    }
    printf("  [8]  condVal(0x4C8 轮询值) = 0x%lx %s\n", result2[8], result2[8] == EXPECT_COND ? "✅" : "❌");
    printf("  [9]  C1 通过标志           = %lu\n", result2[9]);
    printf("  [10] 写后读回 0xA0 (关键)  = 0x%lx %s\n", result2[10],
           result2[10] == WRITE_DMB ? "✅ 写已落 MMIO" : "❌ 写未落入 MMIO");
    printf("  [11] pollCnt               = %lu\n", result2[11]);
    printf("  [12] 写后再读 0x4C8 (对照) = 0x%lx\n", result2[12]);
    printf("  [13] coreId                = %lu\n", result2[13]);
    printf("  [14] 写后读回 0xD0 (偏移排查) = 0x%lx\n", result2[14]);
    printf("  [19] 精确写 Recv 核后读回 0xA0 = 0x%lx %s\n", result2[19],
           result2[19] == WRITE_DMB ? "✅ 写已落 Recv 核 MMIO" : "❌");
    printf("  [20] Recv coreId            = %lu\n", result2[20]);
    printf("  [15] 尾标记                 = 0x%lx %s\n\n", result2[15], result2[15] == 0xDEADBEEF ? "✅" : "❌");

    /* ========== 步骤 3: 等待 AICore Recv 完成 ========== */
    printf("--- 步骤 3: AICore get_data_main_base() 读 DATA_MAIN_BASE (期望 0x%lx) ---\n", WRITE_DMB);
    CHECK_ACL(aclrtSynchronizeStream(aicoreStream));
    printf("[i] AICore kernel 2 (Recv) 完成\n");

    /* 验证 AICore kernel 2 结果(写到 resultDev[2..3]/[5]/[17..18]) */
    uint64_t result3[RESULT_SLOTS] = {0};
    CHECK_ACL(aclrtMemcpy(result3, RESULT_SIZE, resultDev, RESULT_SIZE, ACL_MEMCPY_DEVICE_TO_HOST));
    printf("  [2]  裸 asm 读 DMB 值(pypto 同款) = 0x%lx %s\n", result3[2],
           result3[2] == WRITE_DMB ? "✅ (C2 通过)" : "❌ (C2 失败)");
    printf("  [3]  尾标记          = 0x%lx %s\n", result3[3], result3[3] == 0xDEADBEEF ? "✅" : "❌");
    printf("  [5]  裸 asm 轮询次数  = %lu\n", result3[5]);
    printf("  [7]  Recv 所在核      = %lu\n", result3[7]);
    printf("  [17] intrinsic 读 DMB 值(对照)   = 0x%lx %s\n", result3[17],
           result3[17] == WRITE_DMB ? "✅" : "❌");
    printf("  [18] intrinsic 轮询次数(对照)    = %lu\n\n", result3[18]);

    /* ========== 汇总 ========== */
    bool c1Ok = (result2[8] == EXPECT_COND && result2[9] == 1);
    bool c2AsmOk = (result3[2] == WRITE_DMB);
    bool c2IntOk = (result3[17] == WRITE_DMB);
    /* C2 判定以 pypto 同款裸 asm 为准(aicore_entry.h:160-165, 实测稳定);
     * intrinsic 对照常被编译器消掉轮询循环(intPollCnt=0), 不可作为 gate, 仅记录 */
    bool c2Ok = c2AsmOk;
    printf("=== 验证汇总 ===\n");
    printf("| 检查项 | 结果 |\n");
    printf("|--------|------|\n");
    printf("| C1: AICore set_cond→AICPU 轮询 0x4C8 | %s |\n", c1Ok ? "✅ 通过" : "❌ 失败");
    printf("| C2: AICPU 写 0xA0→AICore 读 DMB | %s |\n", c2Ok ? "✅ 通过" : "❌ 失败");
    printf("\n结论:\n");
    if (c1Ok && c2Ok) {
        printf("  ✅ 寄存器同步端到端验证通过: C1+C2 双向通路正常\n");
        printf("  ✅ memfabric commByReg 方案核心路径验证成功\n");
    } else {
        printf("  ❌ 寄存器同步端到端验证失败\n");
        if (!c1Ok) printf("  → C1 失败: AICore set_cond 写的值未被 AICPU 从 0x4C8 读到\n");
        if (!c2AsmOk) printf("  → C2(裸 asm) 失败: AICPU 写 Recv 核 0xA0 的值未被裸 asm 读到\n");
        if (!c2IntOk) printf("  → C2(intrinsic) 失败: AICPU 写 Recv 核 0xA0 的值未被 intrinsic 读到\n");
    }

    /* 清理 */
    CHECK_ACL(aclrtFree(regBaseDev));
    CHECK_ACL(aclrtFree(resultDev));
    CHECK_ACL(aclrtDestroyStream(aicoreStream));
    CHECK_ACL(aclrtDestroyStream(aicpuStream));
    (void)aclrtFree(devSo);
    (void)aclrtFree(devArgs);
    (void)aclrtFree(devParams);
    CHECK_ACL(aclrtResetDevice(devId));
    (void)aclFinalize();
    return (c1Ok && c2Ok) ? 0 : 1;
}
