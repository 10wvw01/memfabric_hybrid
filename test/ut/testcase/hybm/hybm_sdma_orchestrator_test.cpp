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

/* hybm_sdma_orchestrator_test — 310P 设备发起 SDMA 传输准备与 C API 的 UT
 *
 * UT 环境约束决定用例形态(acl_stub 的 aclrtMemcpy 为 no-op, 无设备内存模型;
 * DlRtApi 依赖 libruntime.so 在 mock 环境不可用) —— Acquire 在 UT 下必然失败,
 * 这正是就绪门控语义的确定性验证: 任何一层资源缺失都不得让 ready_ 置位。
 *
 * 覆盖:
 *   1. Acquire 失败语义: 返回非 0 / ready 不置位 / 引用计数归零可重试 / Release 幂等
 *   2. 未就绪时 C API 全拒绝(get_info/submit/wait/get_result)
 *   3. submit 的 chunks 边界(0/65 拒绝, 1~64 通过参数关)
 *   4. 工作区布局常量不变式(偏移不重叠, 哨兵区在邮箱区之外)
 *   5. smem 层 sdma 协同 API 的无效 handle 拒绝
 */
#include <gtest/gtest.h> // googletest 框架
#include <cstddef>      // offsetof
#include <vector>

#include "hybm_sdma_orchestrator.h" // 被测单例(public 接口: Acquire/Ready/GetRefCountForTest/Release)

#include "hybm_big_mem.h" // 被测 C API 声明(hybm_sdma_*)
#include "hybm_define.h"  // BM_OK/BM_INVALID_PARAM 等错误码
#include "smem.h"         // smem 层公共定义(SMEM_OK)
#include "smem_shm.h"     // 被测 smem 层 sdma 协同 API 声明
#include "smem_shm_sdma_layout.h" // 共享布局头(单一事实源, 无 AscendC 可进 host UT)

namespace ock { // MemFabric 总命名空间
namespace mf {  // hybm 引擎子命名空间

/* 用例 1: Acquire 在资源缺失环境(UT stub)必然失败, 且失败后状态干净可重试 */
TEST(SdmaOrchestratorTest, AcquireFailsCleanlyAndRetriable)
{
    auto &prep = SdmaOrchestrator::GetInstance(); // 被测单例
    for (int round = 0; round < 2; round++) { /* 两次: 验证失败后可重试无残留 */
        auto ret = prep.Acquire(0); // 设备 0(stub 环境必然初始化失败)
        EXPECT_NE(ret, BM_OK) << "round " << round << ": UT stub 环境下 Acquire 必须失败"; // 必须失败
        EXPECT_FALSE(prep.Ready()) << "round " << round << ": 失败后不得置 ready"; // 不得就绪
        EXPECT_EQ(prep.GetRefCountForTest(), 0U) << "round " << round << ": 失败后引用计数须归零"; // 计数归零
    }
    /* Release 幂等(refCount 已为 0 时为 no-op, 不得崩溃) */
    prep.Release(); // 第一次(计数已 0)
    prep.Release(); // 第二次(幂等不崩溃)
    EXPECT_EQ(prep.GetRefCountForTest(), 0U); // 计数仍为 0
}

/* 用例 2: 未就绪时 C API 拒绝(就绪门控的外显语义; 数据面 API 已废止) */
TEST(SdmaOrchestratorTest, CApiRejectWhenNotReady)
{
    hybm_sdma_info_t info = {}; // 就绪信息出参
    EXPECT_NE(hybm_sdma_get_info(&info), BM_OK); // 未就绪须拒绝
    EXPECT_NE(hybm_sdma_get_info(nullptr), BM_OK); /* 出参 null 拒绝 */
}

/* 用例 3: 双头布局锁(hybm 契约与 smem 设备头镜像一致; 违反即布局回归)
 * v29 4-rank 化: 请求/到达环按对端划分; v30 邮件像经 SQE ⓪ 投递;
 * v31 队列按 AICore block 扩展; v32 队列数对齐 hcomm 48(每对端 16 槽位, block 0..7 在用) */
TEST(SdmaOrchestratorTest, DualHeaderLayoutInvariants)
{
    /* 布局头钉死值(单一事实源; 漂移即回归) */
    EXPECT_EQ(SMEM_SHM_SDMA_RESERVED, 2048UL * 1024UL);
    EXPECT_EQ(SMEM_SHM_SDMA_RS_REQ_RING, 0x0000U);
    EXPECT_EQ(SMEM_SHM_SDMA_RS_REQ_SLOTS, 64U);
    EXPECT_EQ(SMEM_SHM_SDMA_RS_REQ_RING_STRIDE, 0x800U);
    EXPECT_EQ(SMEM_SHM_SDMA_RS_REQ_TAIL, 0x18000U);
    EXPECT_EQ(SMEM_SHM_SDMA_RS_REQ_HEAD, 0x18C00U);
    EXPECT_EQ(SMEM_SHM_SDMA_RS_CURSOR_STRIDE, 0x40U);
    EXPECT_EQ(SMEM_SHM_SDMA_RS_QUIET, 0x19800U);
    EXPECT_EQ(SMEM_SHM_SDMA_RS_QUIET_SLOTS, 64U);
    EXPECT_EQ(SMEM_SHM_SDMA_RS_QUIET_STRIDE, 0x200U);
    EXPECT_EQ(SMEM_SHM_SDMA_RS_STAGE, 0x1F800U);
    EXPECT_EQ(SMEM_SHM_SDMA_RS_STAGE_SLOTS, 512U);
    EXPECT_EQ(SMEM_SHM_SDMA_RS_STAGE_STRIDE, 0x4000U);
    EXPECT_EQ(SMEM_SHM_SDMA_RS_PEER_RINGS, 0xDF800U);
    EXPECT_EQ(SMEM_SHM_SDMA_RS_PEER_STAMP, 0x0000U);
    EXPECT_EQ(SMEM_SHM_SDMA_RS_PEER_MAIL, 0x1000U);
    EXPECT_EQ(SMEM_SHM_SDMA_RS_PEER_HEAD, 0x5000U);
    EXPECT_EQ(SMEM_SHM_SDMA_RS_PEER_SLOTS, 512U);
    EXPECT_EQ(SMEM_SHM_SDMA_RS_PEER_STRIDE, 0x5040U);
    EXPECT_EQ(SMEM_SHM_SDMA_MAX_RANKS, 4U);
    EXPECT_EQ(SMEM_SHM_SDMA_MAX_PEERS, 3U);
    EXPECT_EQ(SMEM_SHM_SDMA_MAX_BLOCKS, 8U);
    EXPECT_EQ(SMEM_SHM_SDMA_QUEUES_PER_PEER, 16U);
    EXPECT_EQ(SMEM_SHM_SDMA_QUEUE_COUNT, 48U);
    /* 布局不变式: 本端四组数组各 48 队列完整覆盖且相互不压(数组首尾相接恒等式) */
    EXPECT_EQ(SMEM_SHM_SDMA_RS_REQ_RING + SMEM_SHM_SDMA_QUEUE_COUNT * SMEM_SHM_SDMA_RS_REQ_RING_STRIDE,
              SMEM_SHM_SDMA_RS_REQ_TAIL);
    EXPECT_EQ(SMEM_SHM_SDMA_RS_REQ_TAIL + SMEM_SHM_SDMA_QUEUE_COUNT * SMEM_SHM_SDMA_RS_CURSOR_STRIDE,
              SMEM_SHM_SDMA_RS_REQ_HEAD);
    EXPECT_EQ(SMEM_SHM_SDMA_RS_REQ_HEAD + SMEM_SHM_SDMA_QUEUE_COUNT * SMEM_SHM_SDMA_RS_CURSOR_STRIDE,
              SMEM_SHM_SDMA_RS_QUIET);
    EXPECT_EQ(SMEM_SHM_SDMA_RS_QUIET + SMEM_SHM_SDMA_QUEUE_COUNT * SMEM_SHM_SDMA_RS_QUIET_STRIDE,
              SMEM_SHM_SDMA_RS_STAGE);
    EXPECT_EQ(SMEM_SHM_SDMA_RS_STAGE + SMEM_SHM_SDMA_QUEUE_COUNT * SMEM_SHM_SDMA_RS_STAGE_STRIDE,
              SMEM_SHM_SDMA_RS_PEER_RINGS);
    /* 到达子环: 队列内区块不重叠 + 全部落保留区内(含最深一队列的游标) */
    EXPECT_GE(SMEM_SHM_SDMA_RS_PEER_MAIL, SMEM_SHM_SDMA_RS_PEER_STAMP + 8U * 512U);           // 邮件不压戳
    EXPECT_GE(SMEM_SHM_SDMA_RS_PEER_HEAD, SMEM_SHM_SDMA_RS_PEER_MAIL + 32U * 512U);           // 游标不压邮件
    EXPECT_LE(SMEM_SHM_SDMA_RS_PEER_RINGS + SMEM_SHM_SDMA_QUEUE_COUNT * SMEM_SHM_SDMA_RS_PEER_STRIDE,
              SMEM_SHM_SDMA_RESERVED);                                                        // 全落在 1.25MB 内
    /* v23 缓存行分隔纪律(共行回写竞态=并发丢件挂死类根因; 多 block 并发写同区各槽独占行) */
    for (uint64_t v : {SMEM_SHM_SDMA_RS_REQ_TAIL, SMEM_SHM_SDMA_RS_REQ_HEAD, SMEM_SHM_SDMA_RS_QUIET,
                       SMEM_SHM_SDMA_RS_STAGE, SMEM_SHM_SDMA_RS_PEER_RINGS}) {
        EXPECT_EQ(v & 0x3FU, 0U) << "base 0x" << std::hex << v;
    }
    for (uint64_t v : {SMEM_SHM_SDMA_RS_REQ_RING_STRIDE, SMEM_SHM_SDMA_RS_CURSOR_STRIDE,
                       SMEM_SHM_SDMA_RS_QUIET_STRIDE, SMEM_SHM_SDMA_RS_STAGE_STRIDE,
                       SMEM_SHM_SDMA_RS_PEER_STRIDE, SMEM_SHM_SDMA_RS_PEER_MAIL, SMEM_SHM_SDMA_RS_PEER_HEAD}) {
        EXPECT_EQ(v & 0x3FU, 0U) << "stride 0x" << std::hex << v;
    }
    /* 暂存槽数 > 单队列在途上界(SQ 环深/4——每封 4 支 SQE ①⓪②③) */
    EXPECT_GT(SMEM_SHM_SDMA_RS_STAGE_SLOTS, 1024U / 4U);
    /* 到达深度 > 单子环在途上界(请求环未消费 64 + SQ 在途 256) */
    EXPECT_GT(SMEM_SHM_SDMA_RS_PEER_SLOTS, 64U + 1024U / 4U);
    /* 环深度契约 */
    EXPECT_EQ(SMEM_SHM_SDMA_RS_REQ_SLOTS, 64U);   // 每队列请求环 64
    EXPECT_EQ(SMEM_SHM_SDMA_RS_PEER_SLOTS, 512U);  // 每队列到达子环 512
    /* 队列下标编码不变式: (对端, block) 双射 [0, 队列数) */
    for (uint32_t own = 0; own < SMEM_SHM_SDMA_MAX_RANKS; own++) {
        std::vector<uint32_t> seen(SMEM_SHM_SDMA_QUEUE_COUNT, 0);
        for (uint32_t peer = 0; peer < SMEM_SHM_SDMA_MAX_RANKS; peer++) {
            if (peer == own) {
                continue;
            }
            for (uint32_t b = 0; b < SMEM_SHM_SDMA_MAX_BLOCKS; b++) {
                const uint32_t q = SmemShmSdmaQueueIdx(SmemShmSdmaPeerRingIdx(own, peer), b);
                EXPECT_LT(q, SMEM_SHM_SDMA_QUEUE_COUNT) << "own:" << own << " peer:" << peer << " b:" << b;
                seen[q]++;
            }
        }
        for (uint32_t q = 0; q < SMEM_SHM_SDMA_QUEUE_COUNT; q++) {
            /* 在用槽位(block < MAX_BLOCKS)恰好一次; 预留槽位(8..15)无映射恒零 */
            EXPECT_EQ(seen[q],
                      (q % SMEM_SHM_SDMA_QUEUES_PER_PEER < SMEM_SHM_SDMA_MAX_BLOCKS) ? 1U : 0U)
                << "own:" << own << " q:" << q;
        }
    }
    /* 工作区布局(result/ovf/stop) */
    EXPECT_GT(HYBM_SDMA_ORCH_WS_OVF_OFF, HYBM_SDMA_ORCH_WS_RESULT_OFF);                       // ovf 在 result 后
    EXPECT_GE(HYBM_SDMA_ORCH_WS_STOP_OFF, HYBM_SDMA_ORCH_WS_OVF_OFF + 64U);                   // stop 不压 ovf
    EXPECT_LE(HYBM_SDMA_ORCH_WS_STOP_OFF + 8U, HYBM_SDMA_ORCH_WS_SIZE);                       // 落在 4KB 内
    /* 心跳槽(v11 时限时钟): 不压 stop(可紧邻), 落在 4KB 内 */
    EXPECT_GE(HYBM_SDMA_ORCH_WS_HEARTBEAT_OFF, HYBM_SDMA_ORCH_WS_STOP_OFF + 8U);
    EXPECT_LE(HYBM_SDMA_ORCH_WS_HEARTBEAT_OFF + 8U, HYBM_SDMA_ORCH_WS_SIZE);
    EXPECT_EQ(HYBM_SDMA_ORCH_EXIT_RUNNING, 0xFFFFFFFFu); // 运行中标记(host 轮询判据)
}

/* 用例 3b: 契约不变式(KFCKernel/type 5 不受设备侧 28s 强制结束约束; v11: 按时长分轮+主机报时) */
TEST(SdmaOrchestratorTest, V9ResidentInvariants)
{
    /* 参数布局锁: v29 重构(reqBase/quietBase→resBase, 尾部追加 rank/rsv/poolBaseGva), 80B */
    EXPECT_EQ(sizeof(HybmSdmaOrchParam), 80U);
    EXPECT_EQ(offsetof(HybmSdmaOrchParam, epochBudgetS), 12U); // 第 4 个 u32 槽位
    EXPECT_EQ(offsetof(HybmSdmaOrchParam, resBase), 16U);       // 保留区基址(定位原点)
    EXPECT_EQ(offsetof(HybmSdmaOrchParam, rank), 64U);          // v29 追加: 本端 rank
    EXPECT_EQ(offsetof(HybmSdmaOrchParam, poolBaseGva), 80U - 8U); // v29 追加: 池窗口基址
    /* 永不退出的专用值: 全 F(不判报时/无循环上限, 跑到停止/错误); 与默认值 0 语义区分 */
    EXPECT_EQ(HYBM_SDMA_ORCH_EPOCH_BUDGET_RESIDENT, 0xFFFFFFFFu);
    EXPECT_NE(HYBM_SDMA_ORCH_EPOCH_BUDGET_RESIDENT, HYBM_SDMA_ORCH_EPOCH_TIME_BUDGET_S);
    /* launch attr 超时: 必须 0(永不超时)——有限值到达即把任务标记为 507017 超时,
     * StopEpoch 的 sync 提前返回不等 kernel 退出; attr 缺省为默认超时 ≈6.4s 同样误杀 */
    EXPECT_EQ(HYBM_SDMA_ORCH_LAUNCH_TIMEOUT_NEVER_S, 0U);
    /* 契约版本单调递增(部署链按文件名缓存刷新, 禁止回退); v32=48 队列对齐 hcomm */
    EXPECT_GE(HYBM_SDMA_ORCH_KVER, 32U);
}

/* 用例 4: smem 层就绪判定 API 的无效 handle 拒绝(不依赖任何初始化; 仅存 host API) */
TEST(SmemShmSdmaApiTest, InvalidHandleRejected)
{
    EXPECT_EQ(smem_shm_sdma_get_workspace(nullptr), nullptr); // 工作区返回 NULL(就绪判定)
}
} // namespace mf
} // namespace ock
