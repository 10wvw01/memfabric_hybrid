/* probe_e0a — E0a 验收探针：验证 MemFabric 构建的协同编排 kernel 能在 CP1 内执行并完成
 * SQE 直投闭环（单进程/单设备版：邮箱预填 + mode13 role0，数据与标志均在本地设备内存）。
 *
 * 流程（对齐 xcclpp demo 的 host 侧配方，三流结构中本探针只用 cpStream+launchStream）：
 *   1. 建 CP 流(DEVICE_USE_ONLY) → rtStreamGetSqid/aclrtStreamGetId 取 sqId/streamId
 *   2. 分配资源（全部 HUGE_FIRST：result/ovf/mailbox/src/dst/sentinel/flag）
 *   3. 预填：src 写 pattern、sentinel 写魔数、mailbox={src地址,words}（role0 的请求描述符）
 *   4. 加载 kernel（AICPUKernel json, cpuMode=0）+ GetFunction("HybmOverlapRun")
 *   5. 装配 param → launch（launchStream）→ SynchronizeStream
 *   6. D2H 读回并校验六项判据
 *
 * 验收判据（全过 = E0a PASS）：
 *   ① result.magic == 0xA1C0DE01（kernel 真正在 CP1 执行过）
 *   ② result.mainRet == 0 且 stage == STAGE_DONE(5)（编排状态机走完全程）
 *   ③ result.headAfter != headBefore（TSFW 消费了 SQE —— 硬件执行证据）
 *   ④ flagDone == sentinel 魔数（标志 SQE 的 8B 哨兵搬运到达）
 *   ⑤ dst 与 src 逐字一致（数据 SQE 的搬运正确性）
 *   ⑥ result.pad 低16位 ssid 非 0xFFFF（SMMU ssid 实时查询成功）
 *
 * 用法：./probe_e0a <dev> [json路径]   （默认 json: aicpu_kernel/libmf_overlap_v1.json）
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include "acl/acl.h"
#include "acl/acl_rt.h"
#include "hybm_overlap_contract.h"

// libruntime 内部符号（CP 流 sqId 查询；弱依赖，缺失则 E0a 无法进行）
extern "C" {
typedef void* rtStream_t;
typedef int32_t rtError_t;
rtError_t rtStreamGetSqid(const rtStream_t stm, uint32_t* sqId);
}

#define E0A_WORDS 1024u               /* 数据量: 1024 字(4KB) */
#define E0A_SENTINEL 0xC0FFEE11ULL    /* 哨兵魔数(SDMA 8B 搬运的源值) */
#define E0A_TAG 0x5Au                 /* 数据 pattern 高位标识 */

// ACL 调用错误即断（打印表达式与返回码）
#define CHECK_ACL(expr, what)                                                                        \
    do {                                                                                             \
        aclError _ret = (expr);                                                                      \
        if (_ret != ACL_SUCCESS) {                                                                  \
            printf("[FAIL] %s ret=%d\n", what, (int)_ret);                                          \
            return 1;                                                                                \
        }                                                                                            \
    } while (0)

int main(int argc, char** argv)
{
    int dev = argc >= 2 ? atoi(argv[1]) : 0;                          // 目标设备号
    const char* json = argc >= 3 ? argv[2] : "aicpu_kernel/libmf_overlap_v1.json"; // launch json
    printf("=== probe_e0a: dev=%d json=%s ===\n", dev, json);

    CHECK_ACL(aclInit(nullptr), "aclInit");
    CHECK_ACL(aclrtSetDevice(dev), "aclrtSetDevice");

    // ---- 1. CP 流：其 SQ 注册进 CP1 进程 trs 表（kernel 内 halSqCqQuery 可查的关键前提）----
    aclrtStream cpStream = nullptr;
    CHECK_ACL(aclrtCreateStreamWithConfig(&cpStream, 0, ACL_STREAM_DEVICE_USE_ONLY),
              "aclrtCreateStreamWithConfig(CP)");
    uint32_t cpSqId = 0;
    int32_t cpStreamId = 0;
    CHECK_ACL(aclrtStreamGetId(cpStream, &cpStreamId), "aclrtStreamGetId(CP)");
    if (rtStreamGetSqid(cpStream, &cpSqId) != 0) {
        printf("[FAIL] rtStreamGetSqid(CP) failed\n");
        return 1;
    }
    const uint32_t dieId = static_cast<uint32_t>(dev % 2); // CP1 内 HAL devId = die 号(0903 定论)
    printf("CP 流: streamId=%d sqId=%u dieId=%u\n", cpStreamId, cpSqId, dieId);

    // ---- 2. 资源分配（HUGE_FIRST：NORMAL_ONLY 落特殊段 SDMA 不可翻译, xcclpp M7 实测）----
    void *resultBuf = nullptr, *ovfBuf = nullptr, *mailbox = nullptr;
    void *srcBuf = nullptr, *dstBuf = nullptr, *sentinel = nullptr, *flagDone = nullptr;
    CHECK_ACL(aclrtMalloc(&resultBuf, 4096, ACL_MEM_MALLOC_HUGE_FIRST), "malloc(result)");
    CHECK_ACL(aclrtMalloc(&ovfBuf, 64, ACL_MEM_MALLOC_HUGE_FIRST), "malloc(ovf)");
    CHECK_ACL(aclrtMalloc(&mailbox, 16, ACL_MEM_MALLOC_HUGE_FIRST), "malloc(mailbox)");
    CHECK_ACL(aclrtMalloc(&srcBuf, E0A_WORDS * 4, ACL_MEM_MALLOC_HUGE_FIRST), "malloc(src)");
    CHECK_ACL(aclrtMalloc(&dstBuf, E0A_WORDS * 4, ACL_MEM_MALLOC_HUGE_FIRST), "malloc(dst)");
    CHECK_ACL(aclrtMalloc(&sentinel, 8, ACL_MEM_MALLOC_HUGE_FIRST), "malloc(sentinel)");
    CHECK_ACL(aclrtMalloc(&flagDone, 8, ACL_MEM_MALLOC_HUGE_FIRST), "malloc(flagDone)");
    CHECK_ACL(aclrtMemset(resultBuf, 4096, 0, 4096), "memset(result)");
    CHECK_ACL(aclrtMemset(dstBuf, E0A_WORDS * 4, 0, E0A_WORDS * 4), "memset(dst)");
    CHECK_ACL(aclrtMemset(flagDone, 8, 0, 8), "memset(flagDone)");

    // ---- 3. 预填：src pattern / 哨兵魔数 / 邮箱描述符（host H2D 写 GM, AICPU 裸读可见）----
    {
        uint32_t host[E0A_WORDS];
        for (uint32_t i = 0; i < E0A_WORDS; i++) {
            host[i] = (E0A_TAG << 24) | i; // pattern = (tag<<24)|idx, 逐字可溯源
        }
        CHECK_ACL(aclrtMemcpy(srcBuf, E0A_WORDS * 4, host, E0A_WORDS * 4, ACL_MEMCPY_HOST_TO_DEVICE),
                  "H2D(src)");
        uint64_t sent = E0A_SENTINEL;
        CHECK_ACL(aclrtMemcpy(sentinel, 8, &sent, 8, ACL_MEMCPY_HOST_TO_DEVICE), "H2D(sentinel)");
        uint64_t mb[2] = {reinterpret_cast<uint64_t>(srcBuf), E0A_WORDS}; // {数据地址, 字数}
        CHECK_ACL(aclrtMemcpy(mailbox, 16, mb, 16, ACL_MEMCPY_HOST_TO_DEVICE), "H2D(mailbox)");
    }

    // ---- 4. 加载 kernel（AICPUKernel json + cpuMode=0, 走 CP1 搜索路径）----
    aclrtBinaryLoadOptions lo = {};
    aclrtBinaryLoadOption opt = {};
    opt.type = ACL_RT_BINARY_LOAD_OPT_CPU_KERNEL_MODE;
    opt.value.cpuKernelMode = 0;
    lo.numOpt = 1;
    lo.options = &opt;
    aclrtBinHandle bh = nullptr;
    CHECK_ACL(aclrtBinaryLoadFromFile(json, &lo, &bh), "aclrtBinaryLoadFromFile");
    aclrtFuncHandle fh = nullptr;
    CHECK_ACL(aclrtBinaryGetFunction(bh, "HybmOverlapRun", &fh), "aclrtBinaryGetFunction");

    // ---- 5. param 装配 + launch + sync ----
    HybmOverlapParam p = {};
    p.devId = dieId;
    p.sqId = cpSqId;
    p.streamId = cpStreamId;
    p.submitMode = SUBMIT_MODE_CHAIN; // mode13: 八步闭环(本探针只走 role0 的正向半程)
    p.dstAddr = reinterpret_cast<uint64_t>(dstBuf);          // 单设备版: 目的为本地设备内存
    p.overflowAddr = reinterpret_cast<uint64_t>(ovfBuf);
    p.flagSentinelAddr = reinterpret_cast<uint64_t>(sentinel);
    p.flagDoneAddr = reinterpret_cast<uint64_t>(flagDone);
    p.resultAddr = reinterpret_cast<uint64_t>(resultBuf);
    p.reserved[1] = 0x8u; // doorbell 强制走 HAL(demo 同款, 规避 GM 直写路径差异)
    p.reserved[3] = 0;    // ssid 实时查询(严禁硬编码)
    p.mailboxAddr = reinterpret_cast<uint64_t>(mailbox);
    p.timelineAddr = 0;   // 关闭时间线打点
    p.role = 0;           // 发送方: 轮询邮箱 → SQE 对 → doorbell
    p.roleRsv[1] = 0;

    aclrtStream launchStream = nullptr;
    CHECK_ACL(aclrtCreateStream(&launchStream), "aclrtCreateStream(launch)");
    printf("launch HybmOverlapRun (mode13 role0) ...\n");
    CHECK_ACL(aclrtLaunchKernelWithHostArgs(fh, 1, launchStream, nullptr, &p, sizeof(p), nullptr, 0),
              "aclrtLaunchKernelWithHostArgs");
    CHECK_ACL(aclrtSynchronizeStream(launchStream), "aclrtSynchronizeStream(launch)");

    // ---- 6. 读回校验（六项判据）----
    HybmOverlapResult result = {};
    uint64_t flagVal = 0;
    uint32_t hostDst[E0A_WORDS];
    CHECK_ACL(aclrtMemcpy(&result, sizeof(result), resultBuf, sizeof(result), ACL_MEMCPY_DEVICE_TO_HOST),
              "D2H(result)");
    CHECK_ACL(aclrtMemcpy(&flagVal, 8, flagDone, 8, ACL_MEMCPY_DEVICE_TO_HOST), "D2H(flag)");
    CHECK_ACL(aclrtMemcpy(hostDst, E0A_WORDS * 4, dstBuf, E0A_WORDS * 4, ACL_MEMCPY_DEVICE_TO_HOST),
              "D2H(dst)");

    int pass = 1;
    printf("result: magic=0x%X stage=%u mainRet=%d sqTail(headBefore)=%u headAfter=%u ssidPad=0x%X\n",
           result.magic, result.stage, result.mainRet, result.sqTail, result.headAfter, result.pad & 0xFFFFu);
    if (result.magic != HYBM_OVERLAP_RESULT_MAGIC) {
        printf("[FAIL] ① magic 0x%X != 0x%X (kernel 未在 CP1 执行?)\n", result.magic,
               HYBM_OVERLAP_RESULT_MAGIC);
        pass = 0;
    }
    if (result.mainRet != 0 || result.stage != STAGE_DONE) {
        printf("[FAIL] ② mainRet=%d stage=%u (状态机未走完)\n", result.mainRet, result.stage);
        pass = 0;
    }
    if (result.headAfter == result.sqTail) {
        printf("[FAIL] ③ headAfter=%u == headBefore=%u (TSFW 未消费 SQE)\n", result.headAfter,
               result.sqTail);
        pass = 0;
    }
    if (flagVal != E0A_SENTINEL) {
        printf("[FAIL] ④ flag=0x%llX != 0x%llX (标志 SQE 未搬运)\n", (unsigned long long)flagVal,
               (unsigned long long)E0A_SENTINEL);
        pass = 0;
    }
    uint32_t bad = 0;
    for (uint32_t i = 0; i < E0A_WORDS; i++) {
        if (hostDst[i] != ((E0A_TAG << 24) | i)) {
            bad++;
        }
    }
    if (bad != 0) {
        printf("[FAIL] ⑤ 数据 mismatch=%u/%u\n", bad, E0A_WORDS);
        pass = 0;
    }
    if ((result.pad & 0xFFFFu) == 0xFFFFu) {
        printf("[FAIL] ⑥ ssid 查询失败(0xFFFF): SMMU 翻译不可用\n");
        pass = 0;
    }

    // 资源清理
    CHECK_ACL(aclrtDestroyStream(launchStream), "destroy(launchStream)");
    CHECK_ACL(aclrtDestroyStream(cpStream), "destroy(cpStream)");
    CHECK_ACL(aclrtFree(resultBuf), "free(result)");
    CHECK_ACL(aclrtFree(ovfBuf), "free(ovf)");
    CHECK_ACL(aclrtFree(mailbox), "free(mailbox)");
    CHECK_ACL(aclrtFree(srcBuf), "free(src)");
    CHECK_ACL(aclrtFree(dstBuf), "free(dst)");
    CHECK_ACL(aclrtFree(sentinel), "free(sentinel)");
    CHECK_ACL(aclrtFree(flagDone), "free(flag)");
    (void)aclrtResetDevice(dev);
    (void)aclFinalize();

    printf("=== probe_e0a %s ===\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
