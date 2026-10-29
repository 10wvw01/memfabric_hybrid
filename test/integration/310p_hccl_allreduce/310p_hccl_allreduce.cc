/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT ANY KIND OF WARRANTY OR
 * CONDITIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO
 * NON-INFRINGEMENT, MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

/* test_allreduce_2dev — 310P 卡内双芯 HCCL AllReduce 验证(真机集成测试)
 *
 * 用法: ./test_allreduce_2dev [baseDev]   # 默认 baseDev=0, 使用 dev{base, base+1}(须同卡)
 *
 * - 通信域: HcclCommInitAll —— 官方"单进程多设备通信域"API(hccl_comm.h, 免 MPI)
 * - 校验:   INT32 精确求和; rank r 输入 r*1000000+i, 两 rank 之和期望 1000000+2i
 * - 机制:   HCCL 设备侧入口 HcclKernelEntrance(libccl_kernel.so) 的
 *           opKernelLib=KFCKernel(opp/built-in/op_impl/aicpu/config/ccl_kernel.json)
 *           —— 即 hcomm 的 AICPU/KFC 通道在执行通信, 见
 *           docs/310p/hcomm-GM标志消息同步机制.md(含本测试的日志证据链)
 * - 纪律:   ① HcclCommInitAll 前须先 aclrtSetDevice(否则 HCCL_E_RUNTIME=15);
 *           ② 集合通信提交会阻塞等对端, rank 须各自线程并发出(单线程顺序下发
 *           ~2 分钟后 HCCL_E_TIMEOUT=9)
 */
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <thread>
#include "acl/acl.h"
#include "hccl/hccl.h"

#define CHECK_ACL(x)                                                                            \
    do {                                                                                        \
        aclError _e = (x);                                                                      \
        if (_e != 0) {                                                                          \
            printf("[FAIL] acl %s -> %d (line %d)\n", #x, (int)_e, __LINE__);                   \
            exit(1);                                                                            \
        }                                                                                       \
    } while (0)

static const uint32_t kRankNum = 2;
static const uint64_t kCount = 1u << 20; /* 1M 个 int32 = 4MB/rank */

struct RankCtx {
    int32_t dev = 0;
    HcclComm comm = nullptr;
    void *send = nullptr;
    void *recv = nullptr;
    aclrtStream stream = nullptr;
    int32_t *host = nullptr;
};

/* 各 rank 并发下发一次 AllReduce(提交会阻塞等对端, 不可单线程顺序) */
static void SubmitAllReduce(RankCtx &rk, HcclResult &out)
{
    aclError aclRet = aclrtSetDevice(rk.dev);
    if (aclRet != 0) {
        printf("[FAIL] rank(dev%d) aclrtSetDevice -> %d\n", rk.dev, (int)aclRet);
        out = HCCL_E_RUNTIME;
        return;
    }
    out = HcclAllReduce(rk.send, rk.recv, kCount, HCCL_DATA_TYPE_INT32, HCCL_REDUCE_SUM, rk.comm,
        rk.stream);
}

static int VerifyRank(RankCtx &rk, uint32_t rank)
{
    const uint64_t bytes = kCount * sizeof(int32_t);
    CHECK_ACL(aclrtSetDevice(rk.dev));
    CHECK_ACL(aclrtSynchronizeStream(rk.stream));
    CHECK_ACL(aclrtMemcpy(rk.host, bytes, rk.recv, bytes, ACL_MEMCPY_DEVICE_TO_HOST));
    int fail = 0;
    for (uint64_t i = 0; i < kCount; i++) {
        int32_t expect = (int32_t)(1000000 + 2 * i);
        if (rk.host[i] != expect) {
            if (fail < 5) {
                printf("[FAIL] rank%u [%llu] got %d expect %d\n", rank, (unsigned long long)i,
                    rk.host[i], expect);
            }
            fail++;
        }
    }
    return fail;
}

int main(int argc, char *argv[])
{
    int32_t base = (argc > 1) ? atoi(argv[1]) : 0;
    printf("== 卡内双芯片 AllReduce 测试: dev%d+dev%d(同卡), INT32 SUM, 单次, %llu 元素/rank ==\n",
        base, base + 1, (unsigned long long)kCount);

    RankCtx rk[kRankNum];
    for (uint32_t r = 0; r < kRankNum; r++) {
        rk[r].dev = base + (int32_t)r;
    }

    CHECK_ACL(aclInit(nullptr));
    CHECK_ACL(aclrtSetDevice(rk[0].dev)); /* HCCL 内部 rtGetDevice 需已置当前设备 */

    int32_t devs[kRankNum] = {0, 0};
    for (uint32_t r = 0; r < kRankNum; r++) {
        devs[r] = rk[r].dev;
    }
    HcclComm comms[kRankNum] = {nullptr, nullptr};
    HcclResult ret = HcclCommInitAll(kRankNum, devs, comms);
    if (ret != HCCL_SUCCESS) {
        printf("[FAIL] HcclCommInitAll -> %d (dev%d/dev%d 须同卡且空闲)\n", (int)ret, devs[0],
            devs[1]);
        return 1;
    }
    for (uint32_t r = 0; r < kRankNum; r++) {
        rk[r].comm = comms[r];
    }
    printf("[1] HcclCommInitAll OK (单进程 2 设备通信域)\n");

    const uint64_t bytes = kCount * sizeof(int32_t);
    for (uint32_t r = 0; r < kRankNum; r++) {
        CHECK_ACL(aclrtSetDevice(rk[r].dev));
        CHECK_ACL(aclrtCreateStream(&rk[r].stream));
        CHECK_ACL(aclrtMalloc(&rk[r].send, bytes, ACL_MEM_MALLOC_HUGE_FIRST));
        CHECK_ACL(aclrtMalloc(&rk[r].recv, bytes, ACL_MEM_MALLOC_HUGE_FIRST));
        CHECK_ACL(aclrtMallocHost((void **)&rk[r].host, bytes));
        for (uint64_t i = 0; i < kCount; i++) {
            rk[r].host[i] = (int32_t)(r * 1000000 + i);
        }
        CHECK_ACL(aclrtMemcpy(rk[r].send, bytes, rk[r].host, bytes, ACL_MEMCPY_HOST_TO_DEVICE));
    }
    printf("[2] 两 rank 缓冲就绪, 输入已下发\n");

    HcclResult opRet[kRankNum] = {HCCL_SUCCESS, HCCL_SUCCESS};
    std::thread th[kRankNum];
    for (uint32_t r = 0; r < kRankNum; r++) {
        th[r] = std::thread(SubmitAllReduce, std::ref(rk[r]), std::ref(opRet[r]));
    }
    for (uint32_t r = 0; r < kRankNum; r++) {
        th[r].join();
    }
    for (uint32_t r = 0; r < kRankNum; r++) {
        if (opRet[r] != HCCL_SUCCESS) {
            printf("[FAIL] rank%u HcclAllReduce -> %d\n", r, (int)opRet[r]);
            return 1;
        }
    }
    printf("[3] HcclAllReduce 已下发 x1 (双 rank 并发)\n");

    int fail = 0;
    for (uint32_t r = 0; r < kRankNum; r++) {
        int rankFail = VerifyRank(rk[r], r);
        fail += rankFail;
        printf("[4] rank%u(dev%d) 回读校验: %s\n", r, rk[r].dev, rankFail ? "发现错误" : "OK");
    }

    for (uint32_t r = 0; r < kRankNum; r++) {
        CHECK_ACL(aclrtSetDevice(rk[r].dev));
        CHECK_ACL(aclrtFreeHost(rk[r].host));
        CHECK_ACL(aclrtFree(rk[r].send));
        CHECK_ACL(aclrtFree(rk[r].recv));
        CHECK_ACL(aclrtDestroyStream(rk[r].stream));
        HcclResult dRet = HcclCommDestroy(rk[r].comm);
        if (dRet != HCCL_SUCCESS) {
            printf("[FAIL] rank%u HcclCommDestroy -> %d\n", r, (int)dRet);
            fail++;
        }
        CHECK_ACL(aclrtResetDevice(rk[r].dev));
    }
    CHECK_ACL(aclFinalize());

    if (fail != 0) {
        printf("== FAIL: %d 处数据错误 ==\n", fail);
        return 1;
    }
    printf("== PASS: 卡内 dev%d+dev%d 一次 AllReduce(INT32 SUM) 结果正确 ==\n", base, base + 1);
    return 0;
}
