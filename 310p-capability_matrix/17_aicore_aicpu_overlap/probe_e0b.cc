/* probe_e0b — E0b 关键实验：AclVmm 池 GVA 作为 SQE src/dst（数据面架构成立性裁决）
 *
 * 场景：双进程双 die（rank0=dev0 / rank1=dev1）建 32MB HBM 池（07 示例同款 AclVmm 链路），
 * rank0 在 CP1 内提交 SQE，**src/dst 直接使用池内 GVA**，SDMA 跨 die 搬运后 rank1
 * 从本端窗口 G2H 读回逐字校验。
 *
 * 两轮变体（隔离诊断，一次跑完）：
 *   轮A：src = 普通设备内存（E0a 已证），dst = peer 池 GVA —— 单独验证「导入 VA 可译」
 *   轮B：src = own 池 GVA，     dst = peer 池 GVA —— 真实使用形态（本端窗口 VA 同测）
 * 判读：A过B过=架构成立；A过B败=本端 AclVmm 窗口 VA 不可译；A败=导入 VA 不可译（触发 Plan B）。
 *
 * 标志 SQE 的 flag/sentinel 仍放本地普通设备内存（E0a 已证，隔离变量）。
 * AICPU 不解引用池 VA（D9 纪律）——池地址只出现在 SQE 字段里，由 SDMA 经 SMMU 翻译。
 *
 * 用法：./probe_e0b <0|1> [json路径]   （两个进程分别以 0/1 启动，json 默认 v1）
 */
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <unistd.h>
#include "acl/acl.h"
#include "acl/acl_rt.h"
#include "smem.h"
#include "smem_bm.h"
#include "hybm_overlap_contract.h"

// libruntime 内部符号（CP 流 sqId 查询）
extern "C" {
typedef void* rtStream_t;
typedef int32_t rtError_t;
rtError_t rtStreamGetSqid(const rtStream_t stm, uint32_t* sqId);
}

#define STORE_URL "tcp://127.0.0.1:8577"
#define HBM_POOL_SIZE (32UL * 1024UL * 1024UL)
#define TEST_LEN (8UL * 1024UL * 1024UL) /* 读回校验上限: 8MB(实际用 8KB) */
#define E0B_WORDS 1024u                  /* 每轮数据量: 1024 字(4KB) */
#define TAG_A 0x6Bu                      /* 轮A pattern 标识(纯本地 src) */
#define TAG_B 0x9Cu                      /* 轮B pattern 标识(池 src) */
#define SENT_A 0xC0FFEE11ULL             /* 轮A 哨兵魔数 */
#define SENT_B 0xC0FFEE22ULL             /* 轮B 哨兵魔数 */
#define WORK_DIR "/tmp/e0b_pool"
#define JOIN_TIMEOUT 120                 /* 文件标记等待上限(秒) */

static int g_rank = -1;

static void Step(const char* name, int32_t ret)
{
    if (ret != 0) {
        printf("[r%d FAIL] %-32s ret=%d\n", g_rank, name, ret);
        exit(1);
    }
    printf("[r%d OK]   %-32s\n", g_rank, name);
}

static void WriteDone(const char* name)
{
    char path[128];
    (void)snprintf(path, sizeof(path), "%s/%s%d", WORK_DIR, name, g_rank);
    FILE* fp = fopen(path, "w");
    if (fp != nullptr) {
        (void)fprintf(fp, "1\n");
        (void)fclose(fp);
    }
}

static void WaitPeerDone(const char* name)
{
    char path[128];
    (void)snprintf(path, sizeof(path), "%s/%s%d", WORK_DIR, name, 1 - g_rank);
    for (int i = 0; i < JOIN_TIMEOUT * 1000; i++) {
        if (access(path, F_OK) == 0) {
            return;
        }
        usleep(1000);
    }
    printf("[r%d FAIL] wait %s timeout\n", g_rank, name);
    exit(1);
}

static void PrepareWorkDir(void)
{
    const char* marker = WORK_DIR "/dir_ready";
    if (g_rank == 0) {
        /* 仅 rank0 清理/建目录, 消除双进程 rm -rf 与 mkdir 的竞态(07 同款纪律) */
        (void)system("rm -rf " WORK_DIR " && mkdir -p " WORK_DIR);
        FILE* fp = fopen(marker, "w");
        if (fp != nullptr) {
            (void)fprintf(fp, "%d\n", static_cast<int>(getpid()));
            (void)fclose(fp);
        }
        return;
    }
    for (int i = 0; i < JOIN_TIMEOUT * 1000; i++) {
        if (access(marker, F_OK) == 0) {
            return;
        }
        usleep(1000);
    }
    printf("[r1 FAIL] wait workdir ready timeout\n");
    exit(1);
}

// ACL 调用错误即断
#define CHECK_ACL(expr, what)                                                                        \
    do {                                                                                             \
        aclError _ret = (expr);                                                                      \
        if (_ret != ACL_SUCCESS) {                                                                  \
            printf("[r%d FAIL] %s ret=%d\n", g_rank, what, (int)_ret);                              \
            exit(1);                                                                                \
        }                                                                                            \
    } while (0)

// 校验一轮的 kernel result(六判据中的前四项 + ssid), 返回 0=过
static int CheckResult(const HybmOverlapResult& r, const char* label)
{
    int pass = 1;
    printf("[r0] %s: magic=0x%X stage=%u mainRet=%d headBefore=%u headAfter=%u ssid=0x%X\n", label, r.magic,
           r.stage, r.mainRet, r.sqTail, r.headAfter, r.pad & 0xFFFFu);
    if (r.magic != HYBM_OVERLAP_RESULT_MAGIC || r.mainRet != 0 || r.stage != STAGE_DONE ||
        r.headAfter == r.sqTail || (r.pad & 0xFFFFu) == 0xFFFFu) {
        pass = 0;
    }
    return pass;
}

int main(int argc, char* argv[])
{
    if (argc < 2 || (strcmp(argv[1], "0") != 0 && strcmp(argv[1], "1") != 0)) {
        printf("usage: %s <0|1> [json]\n", argv[0]);
        return 1;
    }
    g_rank = atoi(argv[1]);
    const char* json = argc >= 3 ? argv[2] : "aicpu_kernel/libmf_overlap_v1.json";
    PrepareWorkDir();
    CHECK_ACL(aclInit(nullptr), "aclInit");

    /* ---- 建池(07 示例同款: HBM 池 SDMA 位 → 310P AclVmm 链路) ---- */
    Step("smem_init", smem_init(0));
    smem_bm_config_t cfg;
    Step("smem_bm_config_init", smem_bm_config_init(&cfg));
    cfg.autoRanking = false;
    cfg.rankId = static_cast<uint32_t>(g_rank);
    cfg.startConfigStoreServer = (g_rank == 0);
    Step("smem_bm_init", smem_bm_init(STORE_URL, 2, static_cast<uint16_t>(g_rank), &cfg));

    smem_bm_create_option_t hbmOpt = {};
    hbmOpt.maxHbmSize = HBM_POOL_SIZE;
    hbmOpt.localHBMSize = HBM_POOL_SIZE;
    hbmOpt.dataOpType = SMEMB_DATA_OP_SDMA;
    smem_bm_t hbm = smem_bm_create2(0, &hbmOpt);
    if (hbm == nullptr) {
        printf("[r%d FAIL] smem_bm_create2(HBM)\n", g_rank);
        return 1;
    }
    Step("smem_bm_create2(HBM)", 0);
    Step("smem_bm_join(HBM)", smem_bm_join(hbm, 0));
    void* ownHbm = smem_bm_ptr_by_mem_type(hbm, SMEM_MEM_TYPE_DEVICE, static_cast<uint16_t>(g_rank));
    void* peerHbm = smem_bm_ptr_by_mem_type(hbm, SMEM_MEM_TYPE_DEVICE, static_cast<uint16_t>(1 - g_rank));
    printf("[r%d INFO] HBM own=%p peer=%p\n", g_rank, ownHbm, peerHbm);
    if (ownHbm == nullptr || peerHbm == nullptr) {
        printf("[r%d FAIL] HBM gva null\n", g_rank);
        return 1;
    }

    /* 双方 join 完成(动态成员语义, 07 教训: join 返回=双方 import 就绪, 标记兜底) */
    WriteDone("joined");
    WaitPeerDone("joined");

    int overall = 1;
    if (g_rank == 0) {
        /* ---- rank0: 建 CP 流 + 资源 + 轮A/轮B 两轮 SQE(池 GVA 直投) ---- */
        aclrtStream cpStream = nullptr;
        CHECK_ACL(aclrtCreateStreamWithConfig(&cpStream, 0, ACL_STREAM_DEVICE_USE_ONLY), "create CP stream");
        uint32_t cpSqId = 0;
        int32_t cpStreamId = 0;
        CHECK_ACL(aclrtStreamGetId(cpStream, &cpStreamId), "aclrtStreamGetId(CP)");
        if (rtStreamGetSqid(cpStream, &cpSqId) != 0) {
            printf("[r0 FAIL] rtStreamGetSqid\n");
            return 1;
        }
        const uint32_t dieId = 0; // rank0=dev0=die0
        printf("[r0 INFO] CP 流: streamId=%d sqId=%u\n", cpStreamId, cpSqId);

        // 资源(HUGE_FIRST): result/ovf/mailbox/sentinel×2/flag×2/纯本地 src(轮A)
        void *resultBuf = nullptr, *ovfBuf = nullptr, *mailbox = nullptr;
        void *plainSrc = nullptr, *sentA = nullptr, *flagA = nullptr, *sentB = nullptr, *flagB = nullptr;
        CHECK_ACL(aclrtMalloc(&resultBuf, 4096, ACL_MEM_MALLOC_HUGE_FIRST), "malloc(result)");
        CHECK_ACL(aclrtMalloc(&ovfBuf, 64, ACL_MEM_MALLOC_HUGE_FIRST), "malloc(ovf)");
        CHECK_ACL(aclrtMalloc(&mailbox, 16, ACL_MEM_MALLOC_HUGE_FIRST), "malloc(mailbox)");
        CHECK_ACL(aclrtMalloc(&plainSrc, E0B_WORDS * 4, ACL_MEM_MALLOC_HUGE_FIRST), "malloc(plainSrc)");
        CHECK_ACL(aclrtMalloc(&sentA, 8, ACL_MEM_MALLOC_HUGE_FIRST), "malloc(sentA)");
        CHECK_ACL(aclrtMalloc(&flagA, 8, ACL_MEM_MALLOC_HUGE_FIRST), "malloc(flagA)");
        CHECK_ACL(aclrtMalloc(&sentB, 8, ACL_MEM_MALLOC_HUGE_FIRST), "malloc(sentB)");
        CHECK_ACL(aclrtMalloc(&flagB, 8, ACL_MEM_MALLOC_HUGE_FIRST), "malloc(flagB)");
        CHECK_ACL(aclrtMemset(flagA, 8, 0, 8), "memset(flagA)");
        CHECK_ACL(aclrtMemset(flagB, 8, 0, 8), "memset(flagB)");

        // 加载 kernel(AICPUKernel json, cpuMode=0)
        aclrtBinaryLoadOptions lo = {};
        aclrtBinaryLoadOption opt = {};
        opt.type = ACL_RT_BINARY_LOAD_OPT_CPU_KERNEL_MODE;
        opt.value.cpuKernelMode = 0;
        lo.numOpt = 1;
        lo.options = &opt;
        aclrtBinHandle bh = nullptr;
        CHECK_ACL(aclrtBinaryLoadFromFile(json, &lo, &bh), "BinaryLoadFromFile");
        aclrtFuncHandle fh = nullptr;
        CHECK_ACL(aclrtBinaryGetFunction(bh, "HybmOverlapRun", &fh), "BinaryGetFunction");

        aclrtStream launchStream = nullptr;
        CHECK_ACL(aclrtCreateStream(&launchStream), "create launch stream");

        /* ---- 轮A: src=纯本地设备内存(E0a 已证), dst=peer 池 GVA —— 单验导入 VA ---- */
        {
            uint32_t host[E0B_WORDS];
            for (uint32_t i = 0; i < E0B_WORDS; i++) {
                host[i] = (TAG_A << 24) | i;
            }
            CHECK_ACL(aclrtMemcpy(plainSrc, E0B_WORDS * 4, host, E0B_WORDS * 4, ACL_MEMCPY_HOST_TO_DEVICE),
                      "H2D(plainSrc)");
            uint64_t sent = SENT_A;
            CHECK_ACL(aclrtMemcpy(sentA, 8, &sent, 8, ACL_MEMCPY_HOST_TO_DEVICE), "H2D(sentA)");
            uint64_t mb[2] = {reinterpret_cast<uint64_t>(plainSrc), E0B_WORDS};
            CHECK_ACL(aclrtMemcpy(mailbox, 16, mb, 16, ACL_MEMCPY_HOST_TO_DEVICE), "H2D(mailbox A)");

            HybmOverlapParam p = {};
            p.devId = dieId;
            p.sqId = cpSqId;
            p.streamId = cpStreamId;
            p.submitMode = SUBMIT_MODE_CHAIN;
            p.dstAddr = reinterpret_cast<uint64_t>(peerHbm); // ★ 池 GVA(对端导入映射)
            p.overflowAddr = reinterpret_cast<uint64_t>(ovfBuf);
            p.flagSentinelAddr = reinterpret_cast<uint64_t>(sentA);
            p.flagDoneAddr = reinterpret_cast<uint64_t>(flagA);
            p.resultAddr = reinterpret_cast<uint64_t>(resultBuf);
            p.reserved[1] = 0x8u; // doorbell 强制走 HAL
            p.mailboxAddr = reinterpret_cast<uint64_t>(mailbox);
            p.role = 0;
            printf("[r0] 轮A launch: src=纯本地 dst=peer池GVA ...\n");
            CHECK_ACL(aclrtLaunchKernelWithHostArgs(fh, 1, launchStream, nullptr, &p, sizeof(p), nullptr, 0),
                      "launch A");
            CHECK_ACL(aclrtSynchronizeStream(launchStream), "sync A");
            HybmOverlapResult r = {};
            uint64_t flagVal = 0;
            CHECK_ACL(aclrtMemcpy(&r, sizeof(r), resultBuf, sizeof(r), ACL_MEMCPY_DEVICE_TO_HOST), "D2H(rA)");
            CHECK_ACL(aclrtMemcpy(&flagVal, 8, flagA, 8, ACL_MEMCPY_DEVICE_TO_HOST), "D2H(flagA)");
            int pass = CheckResult(r, "轮A");
            if (flagVal != SENT_A) {
                printf("[r0][FAIL] 轮A flag=0x%llX != 0x%llX\n", (unsigned long long)flagVal,
                       (unsigned long long)SENT_A);
                pass = 0;
            }
            printf("[r0] 轮A %s\n", pass ? "PASS" : "FAIL");
            overall &= pass;
        }

        /* ---- 轮B: src=own 池 GVA, dst=peer 池 GVA —— 真实使用形态 ---- */
        {
            // 先把 TAG_B pattern 写进 own HBM 前段(经 smem H2G, 07 PhaseB 已证路径)
            uint32_t* hostB = static_cast<uint32_t*>(malloc(E0B_WORDS * 4));
            for (uint32_t i = 0; i < E0B_WORDS; i++) {
                hostB[i] = (TAG_B << 24) | i;
            }
            smem_copy_params_t sp = {};
            sp.src = hostB;
            sp.dest = ownHbm;
            sp.dataSize = E0B_WORDS * 4;
            sp.stream = nullptr;
            Step("H2G write own HBM(tagB)", smem_bm_copy(hbm, &sp, SMEMB_COPY_H2G, 0));
            free(hostB);

            uint64_t sent = SENT_B;
            CHECK_ACL(aclrtMemcpy(sentB, 8, &sent, 8, ACL_MEMCPY_HOST_TO_DEVICE), "H2D(sentB)");
            uint64_t mb[2] = {reinterpret_cast<uint64_t>(ownHbm), E0B_WORDS}; // ★ src=池 GVA
            CHECK_ACL(aclrtMemcpy(mailbox, 16, mb, 16, ACL_MEMCPY_HOST_TO_DEVICE), "H2D(mailbox B)");

            HybmOverlapParam p = {};
            p.devId = dieId;
            p.sqId = cpSqId;
            p.streamId = cpStreamId;
            p.submitMode = SUBMIT_MODE_CHAIN;
            p.dstAddr = reinterpret_cast<uint64_t>(peerHbm) + E0B_WORDS * 4; // ★ 池 GVA(后段, 不覆写轮A)
            p.overflowAddr = reinterpret_cast<uint64_t>(ovfBuf);
            p.flagSentinelAddr = reinterpret_cast<uint64_t>(sentB);
            p.flagDoneAddr = reinterpret_cast<uint64_t>(flagB);
            p.resultAddr = reinterpret_cast<uint64_t>(resultBuf);
            p.reserved[1] = 0x8u;
            p.mailboxAddr = reinterpret_cast<uint64_t>(mailbox);
            p.role = 0;
            printf("[r0] 轮B launch: src=own池GVA dst=peer池GVA ...\n");
            CHECK_ACL(aclrtLaunchKernelWithHostArgs(fh, 1, launchStream, nullptr, &p, sizeof(p), nullptr, 0),
                      "launch B");
            CHECK_ACL(aclrtSynchronizeStream(launchStream), "sync B");
            HybmOverlapResult r = {};
            uint64_t flagVal = 0;
            CHECK_ACL(aclrtMemcpy(&r, sizeof(r), resultBuf, sizeof(r), ACL_MEMCPY_DEVICE_TO_HOST), "D2H(rB)");
            CHECK_ACL(aclrtMemcpy(&flagVal, 8, flagB, 8, ACL_MEMCPY_DEVICE_TO_HOST), "D2H(flagB)");
            int pass = CheckResult(r, "轮B");
            if (flagVal != SENT_B) {
                printf("[r0][FAIL] 轮B flag=0x%llX != 0x%llX\n", (unsigned long long)flagVal,
                       (unsigned long long)SENT_B);
                pass = 0;
            }
            printf("[r0] 轮B %s\n", pass ? "PASS" : "FAIL");
            overall &= pass;
        }
        WriteDone("copied"); // 通知 rank1 数据已到

        CHECK_ACL(aclrtDestroyStream(launchStream), "destroy launch");
        CHECK_ACL(aclrtDestroyStream(cpStream), "destroy cp");
        CHECK_ACL(aclrtFree(resultBuf), "free result");
        CHECK_ACL(aclrtFree(ovfBuf), "free ovf");
        CHECK_ACL(aclrtFree(mailbox), "free mailbox");
        CHECK_ACL(aclrtFree(plainSrc), "free plainSrc");
        CHECK_ACL(aclrtFree(sentA), "free sentA");
        CHECK_ACL(aclrtFree(flagA), "free flagA");
        CHECK_ACL(aclrtFree(sentB), "free sentB");
        CHECK_ACL(aclrtFree(flagB), "free flagB");
    } else {
        /* ---- rank1: 等数据到, G2H 读 own HBM 前后段校验两轮 pattern ---- */
        WaitPeerDone("copied");
        uint32_t* host = static_cast<uint32_t*>(malloc(E0B_WORDS * 4 * 2));
        smem_copy_params_t rp = {};
        rp.src = ownHbm;
        rp.dest = host;
        rp.dataSize = E0B_WORDS * 4 * 2;
        rp.stream = nullptr;
        Step("G2H read own HBM", smem_bm_copy(hbm, &rp, SMEMB_COPY_G2H, 0));
        uint32_t badA = 0, badB = 0;
        for (uint32_t i = 0; i < E0B_WORDS; i++) {
            if (host[i] != ((TAG_A << 24) | i)) {
                badA++; // 前段应为轮A pattern(纯本地 src 搬来)
            }
            if (host[E0B_WORDS + i] != ((TAG_B << 24) | i)) {
                badB++; // 后段应为轮B pattern(own 池 GVA src 搬来)
            }
        }
        printf("[r1] 轮A数据(前4KB) mismatch=%u/%u → %s\n", badA, E0B_WORDS, badA == 0 ? "PASS" : "FAIL");
        printf("[r1] 轮B数据(后4KB) mismatch=%u/%u → %s\n", badB, E0B_WORDS, badB == 0 ? "PASS" : "FAIL");
        free(host);
        overall &= (badA == 0) ? 1 : 0;
        overall &= (badB == 0) ? 1 : 0;
    }

    /* ---- 收尾(07 顺序: 先实际调用后打点, destroy 族返回 void) ---- */
    smem_bm_destroy(hbm);
    Step("smem_bm_destroy(HBM)", 0);
    smem_bm_uninit(0);
    Step("smem_bm_uninit", 0);
    smem_uninit();
    Step("smem_uninit", 0);
    (void)aclrtResetDevice(g_rank);
    (void)aclFinalize();
    printf("=== E0b r%d %s ===\n", g_rank, overall ? "PASS" : "FAIL");
    return overall ? 0 : 1;
}
