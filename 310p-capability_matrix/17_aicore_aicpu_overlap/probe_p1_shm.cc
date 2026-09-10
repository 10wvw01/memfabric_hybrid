/* probe_p1_shm — P1/WP3 验证：SHM API 经 InitTransManager 挂载点走 310P 协同准备
 *
 * 与 E0b/E0c 探针的本质区别: 不再直接调 ACL 原语, 而是走产品化路径——
 *   smem_shm_init → smem_shm_create(SDMA op 位) → MemEntityDefault::InitTransManager
 *   → [310P + MF_310P_SHM_OVERLAP=1 分支] → HybmOverlapPrep::Acquire
 *   → CP 流 + 工作区 + kernel 加载 + 冒烟(E0a 式 SQE 自证, 全部在库内完成)
 *
 * 验收判据:
 *   ① smem_shm_create 成功返回非空 handle 且 gva 非空(此前 310P 上 SHM+SDMA 在
 *      SdmaTransportManager A3 配方处失败——本探针证明该断点已被协同分支修复);
 *   ② 设备元数据(HYBM_DEVICE_META_ADDR + entityId*128)读回: overlapWorkspace != 0
 *      且 overlapFlags bit0 置位(AICore 侧可见工作区);
 *   ③ 双池连续创建/销毁(同进程引用计数路径)第二次 create 亦成功。
 * 冒烟在 prep 内部完成(LaunchKernelWithConfig 路径, 与 P0 探针的
 * LaunchKernelWithHostArgs 等价性在此一并验证)。
 *
 * 用法: MF_310P_SHM_OVERLAP=1 ./probe_p1_shm <0|1>   (双进程)
 */
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <unistd.h>
#include "acl/acl.h"
#include "acl/acl_rt.h"
#include "smem.h"
#include "smem_shm.h"
/* HYBM_DEVICE_META_ADDR 等设备元数据布局常量(内部头 ock::mf 命名空间, 实验探针直用) */
#include "hybm_define.h"
using ock::mf::HYBM_DEVICE_META_ADDR;
using ock::mf::HYBM_DEVICE_GLOBAL_META_SIZE;
using ock::mf::HYBM_DEVICE_PRE_META_SIZE;

#define STORE_URL "tcp://127.0.0.1:8579"
#define SHM_LOCAL_SIZE (32UL * 1024UL * 1024UL) /* 各 rank 对称贡献 32MB */
#define WORK_DIR "/tmp/p1_shm"
#define JOIN_TIMEOUT 120

static int g_rank = -1;

static void Step(const char* name, int32_t ret)
{
    if (ret != 0) {
        printf("[r%d FAIL] %-28s ret=%d\n", g_rank, name, ret);
        exit(1);
    }
    printf("[r%d OK]   %-28s\n", g_rank, name);
}

static void PrepareWorkDir(void)
{
    const char* marker = WORK_DIR "/dir_ready";
    if (g_rank == 0) {
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
    printf("[r1 FAIL] wait workdir timeout\n");
    exit(1);
}

/* 读回设备元数据(128B), 解析 overlap 扩展字段(偏移 40/48, 见 HybmDeviceMeta) */
static int ReadOverlapMeta(uint32_t entityId, uint64_t &workspace, uint64_t &flags)
{
    const uint64_t metaAddr = HYBM_DEVICE_META_ADDR + HYBM_DEVICE_GLOBAL_META_SIZE +
                              static_cast<uint64_t>(entityId) * HYBM_DEVICE_PRE_META_SIZE;
    uint8_t meta[HYBM_DEVICE_PRE_META_SIZE] = {0};
    if (aclrtMemcpy(meta, sizeof(meta), reinterpret_cast<const void *>(metaAddr), sizeof(meta),
                    ACL_MEMCPY_DEVICE_TO_HOST) != ACL_SUCCESS) {
        printf("[r%d FAIL] 读设备元数据失败 addr=0x%llx\n", g_rank, (unsigned long long)metaAddr);
        return 1;
    }
    (void)memcpy(&workspace, meta + 40, sizeof(workspace)); /* overlapWorkspace @ reserved[0] */
    (void)memcpy(&flags, meta + 48, sizeof(flags));         /* overlapFlags @ reserved[1] */
    return 0;
}

int main(int argc, char* argv[])
{
    if (argc < 2 || (strcmp(argv[1], "0") != 0 && strcmp(argv[1], "1") != 0)) {
        printf("usage: MF_310P_SHM_OVERLAP=1 %s <0|1>\n", argv[0]);
        return 1;
    }
    g_rank = atoi(argv[1]);
    /* 诊断: main 入口即打印 env(判定启动时缺失 vs 进程内被清除) */
    const char *rawEnvAtEntry = std::getenv("MF_310P_SHM_OVERLAP");
    printf("[r%d INFO] main entry env=%s\n", g_rank, rawEnvAtEntry == nullptr ? "<null>" : rawEnvAtEntry);
    fflush(stdout);
    PrepareWorkDir();
    if (aclInit(nullptr) != ACL_SUCCESS) {
        printf("[r%d FAIL] aclInit\n", g_rank);
        return 1;
    }

    /* ---- SHM 初始化 + 建池(SDMA op 位 → 310P 协同分支) ---- */
    Step("smem_init", smem_init(0));
    smem_shm_config_t cfg;
    Step("smem_shm_config_init", smem_shm_config_init(&cfg));
    cfg.startConfigStoreServer = (g_rank == 0);
    Step("smem_shm_init", smem_shm_init(STORE_URL, 2, static_cast<uint32_t>(g_rank),
                                        static_cast<uint16_t>(g_rank), &cfg));

    void *gva = nullptr;
    smem_shm_t shm = smem_shm_create(0, 2, static_cast<uint32_t>(g_rank), SHM_LOCAL_SIZE,
                                     SMEMS_DATA_OP_SDMA, 0, &gva);
    if (shm == nullptr || gva == nullptr) {
        printf("[r%d FAIL] smem_shm_create: %s\n", g_rank, smem_get_last_err_msg());
        return 1;
    }
    Step("smem_shm_create(SDMA)", 0);
    printf("[r%d INFO] gva=%p symmetricSize=0x%llx\n", g_rank, gva,
           (unsigned long long)smem_shm_get_symmetric_size(shm));

    /* ---- 判据②: 设备元数据 overlap 字段 ---- */
    int overall = 1;
    uint64_t ws = 0;
    uint64_t flags = 0;
    if (ReadOverlapMeta(0 /* entityId: SHM id 0 + SHM_BASE(0) */, ws, flags) != 0) {
        overall = 0;
    } else if (ws == 0 || (flags & 0x1UL) == 0) {
        printf("[r%d FAIL] meta: workspace=0x%llx flags=0x%llx (期望非零/ready)\n", g_rank,
               (unsigned long long)ws, (unsigned long long)flags);
        overall = 0;
    } else {
        printf("[r%d OK]   meta: overlapWorkspace=0x%llx overlapFlags=0x%llx\n", g_rank,
               (unsigned long long)ws, (unsigned long long)flags);
    }

    /* ---- 判据③: 第二个池(同进程引用计数路径, 不重复初始化) ---- */
    void *gva2 = nullptr;
    smem_shm_t shm2 = smem_shm_create(1, 2, static_cast<uint32_t>(g_rank), SHM_LOCAL_SIZE,
                                      SMEMS_DATA_OP_SDMA, 0, &gva2);
    if (shm2 == nullptr || gva2 == nullptr) {
        printf("[r%d FAIL] 第二次 smem_shm_create: %s\n", g_rank, smem_get_last_err_msg());
        overall = 0;
    } else {
        printf("[r%d OK]   第二池创建成功(引用计数路径)\n", g_rank);
        Step("smem_shm_destroy(2nd)", smem_shm_destroy(shm2, 0));
    }

    Step("smem_shm_destroy", smem_shm_destroy(shm, 0));
    smem_shm_uninit(0); /* 返回 void */
    Step("smem_shm_uninit", 0);
    smem_uninit(); /* 返回 void */
    Step("smem_uninit", 0);
    (void)aclrtResetDevice(g_rank);
    (void)aclFinalize();
    printf("=== P1-SHM r%d %s ===\n", g_rank, overall ? "PASS" : "FAIL");
    return overall ? 0 : 1;
}
