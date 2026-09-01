/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

/*
 * P5 实机端到端验收: 310P 单卡双 die(dev0/dev1) 2-rank BM 双池
 * HBM 池(SDMA 位 -> HybmAclVmmSegment + HostDataOpAclMemcpy) +
 * DRAM 池(HOST_SHM -> HybmHostShmSegment)
 * 校验流: HBM 池 H2G(peer) -> G2H(own) 读回; G2G peer->own 读回; DRAM 池 H2GH/GH2H
 * 用法: ./p5_e2e 0 &  ./p5_e2e 1 &  wait
 */
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <unistd.h>
#include "smem.h"
#include "smem_bm.h"

#define STORE_URL "tcp://127.0.0.1:8576"
#define HBM_POOL_SIZE (32UL * 1024UL * 1024UL)
#define DRAM_POOL_SIZE (32UL * 1024UL * 1024UL)
#define TEST_LEN (8UL * 1024UL * 1024UL)
#define WORK_DIR "/tmp/p5_e2e"
#define TAG_HBM 0x5A5AA5A500000000ULL
#define TAG_DRAM 0xA55A5AA500000000ULL

static int g_rank = -1;

static void Step(const char *name, int32_t ret)
{
    if (ret != 0) {
        printf("[r%d FAIL] %-36s ret=%d\n", g_rank, name, ret);
        exit(1);
    }
    printf("[r%d OK]   %-36s\n", g_rank, name);
}

static void WriteDone(const char *name)
{
    char path[128];
    (void)snprintf(path, sizeof(path), "%s/%s%d", WORK_DIR, name, g_rank);
    FILE *fp = fopen(path, "w");
    if (fp != nullptr) {
        (void)fprintf(fp, "1\n");
        (void)fclose(fp);
    }
}

static void WaitPeerDone(const char *name)
{
    char path[128];
    (void)snprintf(path, sizeof(path), "%s/%s%d", WORK_DIR, name, 1 - g_rank);
    for (int i = 0; i < 30000; i++) {
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
    const char *marker = WORK_DIR "/dir_ready";
    if (g_rank == 0) {
        /* 仅 rank0 清理/建目录, 消除双进程 rm -rf 与 mkdir 的竞态; 标记写入 PID,
         * rank1 以内容区分"上一轮旧标记"与"本轮新标记"(rank0 先行场景免白等) */
        (void)system("rm -rf " WORK_DIR " && mkdir -p " WORK_DIR);
        FILE *fp = fopen(marker, "w");
        if (fp != nullptr) {
            (void)fprintf(fp, "%d\n", static_cast<int>(getpid()));
            (void)fclose(fp);
        }
        return;
    }
    /* rank1: 先等旧标记消失或内容变化(rank0 已开始新一轮清理), 再等新标记出现 */
    char oldContent[32] = {0};
    FILE *fp = fopen(marker, "r");
    if (fp != nullptr) {
        (void)fgets(oldContent, sizeof(oldContent), fp);
        (void)fclose(fp);
    }
    bool hadMarker = oldContent[0] != '\0';
    char content[32] = {0};
    for (int i = 0; i < 30000; i++) {
        FILE *cur = fopen(marker, "r");
        bool exists = cur != nullptr;
        if (exists) {
            content[0] = '\0';
            (void)fgets(content, sizeof(content), cur);
            (void)fclose(cur);
        }
        /* 无旧标记: 等出现即可; 有旧标记: 等它消失或变成不同 PID(内容变化即新一轮) */
        if ((!hadMarker && exists) || (hadMarker && (!exists || strcmp(content, oldContent) != 0))) {
            return;
        }
        usleep(1000);
    }
    printf("[r1 FAIL] wait workdir ready timeout\n");
    exit(1);
}

static int CopyCheck(smem_bm_t h, void *dst, const void *src, size_t len, smem_bm_copy_type_t t, uint64_t tag,
                     const char *label)
{
    smem_copy_params_t p;
    p.src = src;
    p.dest = dst;
    p.dataSize = len;
    p.stream = nullptr;
    auto ret = smem_bm_copy(h, &p, t, 0);
    if (ret != 0) {
        printf("[r%d FAIL] copy(%s) ret=%d\n", g_rank, label, ret);
        return -1;
    }
    uint64_t *buf = static_cast<uint64_t *>(malloc(len));
    p.src = dst;
    p.dest = buf;
    p.dataSize = len;
    p.stream = nullptr;
    ret = smem_bm_copy(h, &p, SMEMB_COPY_G2H, 0);
    if (ret != 0) {
        printf("[r%d FAIL] copy back(%s) ret=%d\n", g_rank, label, ret);
        free(buf);
        return -1;
    }
    uint64_t mismatch = 0;
    for (uint64_t i = 0; i < len / sizeof(uint64_t); i++) {
        if (buf[i] != tag + (uint64_t)g_rank * 0x10000ULL + i) {
            mismatch++;
        }
    }
    free(buf);
    printf("[r%d %s] %-36s mismatch=%llu\n", g_rank, mismatch == 0 ? "OK" : "FAIL", label,
           (unsigned long long)mismatch);
    return mismatch == 0 ? 0 : -1;
}

int main(int argc, char *argv[])
{
    if (argc != 2 || (strcmp(argv[1], "0") != 0 && strcmp(argv[1], "1") != 0)) {
        printf("usage: %s <0|1>\n", argv[0]);
        return 1;
    }
    g_rank = atoi(argv[1]);
    PrepareWorkDir(); /* rank0 建目录并放标记, rank1 等标记就绪(代替固定 usleep) */

    Step("smem_init", smem_init(0));
    smem_bm_config_t cfg;
    Step("smem_bm_config_init", smem_bm_config_init(&cfg));
    cfg.autoRanking = false;
    cfg.rankId = static_cast<uint32_t>(g_rank);
    cfg.startConfigStoreServer = (g_rank == 0);
    char buf[256];
    (void)snprintf(buf, sizeof(buf), "smem_bm_init");
    Step("smem_bm_init", smem_bm_init(STORE_URL, 2, static_cast<uint16_t>(g_rank), &cfg));

    /* ---- HBM 池(SDMA 位 -> 310P AclVmm 链路) ---- */
    smem_bm_create_option_t hbmOpt{};
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

    void *ownHbm = smem_bm_ptr_by_mem_type(hbm, SMEM_MEM_TYPE_DEVICE, static_cast<uint16_t>(g_rank));
    void *peerHbm = smem_bm_ptr_by_mem_type(hbm, SMEM_MEM_TYPE_DEVICE, static_cast<uint16_t>(1 - g_rank));
    printf("[r%d INFO] HBM own=%p peer=%p\n", g_rank, ownHbm, peerHbm);
    if (ownHbm == nullptr || peerHbm == nullptr) {
        printf("[r%d FAIL] HBM gva null\n", g_rank);
        return 1;
    }

    /* ---- DRAM 池(HOST_SHM) ---- */
    smem_bm_create_option_t dramOpt{};
    dramOpt.maxDramSize = DRAM_POOL_SIZE;
    dramOpt.localDRAMSize = DRAM_POOL_SIZE;
    dramOpt.dataOpType = SMEMB_DATA_OP_HOST_SHM;
    smem_bm_t dram = smem_bm_create2(1, &dramOpt);
    if (dram == nullptr) {
        printf("[r%d FAIL] smem_bm_create2(DRAM)\n", g_rank);
        return 1;
    }
    Step("smem_bm_create2(DRAM)", 0);
    Step("smem_bm_join(DRAM)", smem_bm_join(dram, 0));
    void *peerDram = smem_bm_ptr_by_mem_type(dram, SMEM_MEM_TYPE_HOST, static_cast<uint16_t>(1 - g_rank));
    void *ownDram = smem_bm_ptr_by_mem_type(dram, SMEM_MEM_TYPE_HOST, static_cast<uint16_t>(g_rank));
    printf("[r%d INFO] DRAM own=%p peer=%p\n", g_rank, ownDram, peerDram);
    if (ownDram == nullptr || peerDram == nullptr) {
        printf("[r%d FAIL] DRAM gva null\n", g_rank);
        return 1;
    }

    /* 双方双池 join 完成后再开始拷贝(join 为动态成员语义, 先 join 者需等后 join 者) */
    WriteDone("joined");
    WaitPeerDone("joined");

    /* ---- 校验 Phase A: HBM 池 H2G 写 peer -> 对端 G2H 读回校验 ---- */
    uint64_t *hostSrc = static_cast<uint64_t *>(malloc(TEST_LEN));
    for (uint64_t i = 0; i < TEST_LEN / sizeof(uint64_t); i++) {
        hostSrc[i] = TAG_HBM + (uint64_t)g_rank * 0x10000ULL + i;
    }
    smem_copy_params_t p{};
    p.src = hostSrc;
    p.dest = peerHbm;
    p.dataSize = TEST_LEN;
    p.stream = nullptr;
    Step("H2G write peer HBM", smem_bm_copy(hbm, &p, SMEMB_COPY_H2G, 0));
    WriteDone("hbmA");
    WaitPeerDone("hbmA");
    /* 对端已写我的 HBM, 从 own GVA 读回校验 */
    p.src = ownHbm;
    p.dest = hostSrc;
    Step("G2H read own HBM", smem_bm_copy(hbm, &p, SMEMB_COPY_G2H, 0));
    uint64_t mm = 0;
    for (uint64_t i = 0; i < TEST_LEN / sizeof(uint64_t); i++) {
        if (hostSrc[i] != TAG_HBM + (uint64_t)(1 - g_rank) * 0x10000ULL + i) {
            mm++;
        }
    }
    printf("[r%d %s] %-36s mismatch=%llu\n", g_rank, mm == 0 ? "OK" : "FAIL", "PhaseA cross H2G/G2H",
           (unsigned long long)mm);
    if (mm != 0) {
        return 1;
    }

    /* ---- 校验 Phase B: HBM 池 G2G(peer前段 -> own后段, 与 pattern 区分离避免交叉覆写) ---- */
    for (uint64_t i = 0; i < TEST_LEN / sizeof(uint64_t); i++) {
        hostSrc[i] = TAG_HBM + 0x555500000000ULL + (uint64_t)g_rank * 0x10000ULL + i;
    }
    p.src = hostSrc;
    p.dest = ownHbm; /* pattern 写 own 前段 */
    Step("H2G write own HBM", smem_bm_copy(hbm, &p, SMEMB_COPY_H2G, 0));
    WriteDone("hbmB");
    WaitPeerDone("hbmB");

    void *g2gDst = static_cast<char *>(ownHbm) + TEST_LEN; /* own 后段作 G2G 目的 */
    p.src = peerHbm;
    p.dest = g2gDst;
    Step("G2G peer->own(tail)", smem_bm_copy(hbm, &p, SMEMB_COPY_G2G, 0));
    p.src = g2gDst;
    p.dest = hostSrc;
    Step("G2H read own tail", smem_bm_copy(hbm, &p, SMEMB_COPY_G2H, 0));
    mm = 0;
    for (uint64_t i = 0; i < TEST_LEN / sizeof(uint64_t); i++) {
        if (hostSrc[i] != TAG_HBM + 0x555500000000ULL + (uint64_t)(1 - g_rank) * 0x10000ULL + i) {
            mm++;
        }
    }
    printf("[r%d %s] %-36s mismatch=%llu\n", g_rank, mm == 0 ? "OK" : "FAIL", "PhaseB G2G cross-die",
           (unsigned long long)mm);
    if (mm != 0) {
        return 1;
    }

    /* ---- 校验 Phase C: DRAM 池 H2GH 写 peer -> GH2H 读回 own ---- */
    for (uint64_t i = 0; i < TEST_LEN / sizeof(uint64_t); i++) {
        hostSrc[i] = TAG_DRAM + (uint64_t)g_rank * 0x10000ULL + i;
    }
    p.src = hostSrc;
    p.dest = peerDram;
    Step("H2GH write peer DRAM", smem_bm_copy(dram, &p, SMEMB_COPY_H2GH, 0));
    WriteDone("dramC");
    WaitPeerDone("dramC");
    p.src = ownDram;
    p.dest = hostSrc;
    Step("GH2H read own DRAM", smem_bm_copy(dram, &p, SMEMB_COPY_G2H, 0));
    mm = 0;
    for (uint64_t i = 0; i < TEST_LEN / sizeof(uint64_t); i++) {
        if (hostSrc[i] != TAG_DRAM + (uint64_t)(1 - g_rank) * 0x10000ULL + i) {
            mm++;
        }
    }
    printf("[r%d %s] %-36s mismatch=%llu\n", g_rank, mm == 0 ? "OK" : "FAIL", "PhaseC DRAM H2GH/GH2H",
           (unsigned long long)mm);
    if (mm != 0) {
        return 1;
    }
    free(hostSrc);

    /* ---- 销毁 ---- */
    smem_bm_destroy(dram);
    Step("smem_bm_destroy(DRAM)", 0);
    smem_bm_destroy(hbm);
    Step("smem_bm_destroy(HBM)", 0);
    smem_bm_uninit(0);
    Step("smem_bm_uninit", 0);
    printf("=== P5 r%d DONE ===\n", g_rank);
    return 0;
}
