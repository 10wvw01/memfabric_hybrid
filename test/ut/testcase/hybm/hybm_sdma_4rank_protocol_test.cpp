/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT ANY KIND OF EITHER EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY OR FIT FOR A
 * PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

/* hybm_sdma_4rank_protocol_test — 310P 协同模式 4-rank 邮箱协议的 UT
 *
 * 旧 wire 协议为单生产者假设(单请求环 + 单到达环, 序号跨目的地共享):
 *   - 多发送方写同一到达环槽位互相覆盖;
 *   - 发往不同对端的邮件在对端环上序号有空洞, 接收方 head+1 连续性期望失配;
 *   仅支持"每目的段唯一远端写者"(2-rank)。
 * v29 按对端划分请求环与到达环(每对端一套环, 各自独立序号);
 * v31 邮箱队列按 AICore block 扩展(每对端 × 每 block 一套队列, 3×8=24)。
 * 本文件在主机内存上仿真 4 rank 段尾保留区, 收发两侧使用与 kernel/设备原语
 * 完全相同的共享布局常量与映射函数, 验证 4-rank 路由正确性与多发送方/多 block
 * 并发隔离:
 *   1. 环下标映射不变式(全 12 有向对, 每视角双射) + 队列下标双射(对端 × block)
 *   2. 布局地址不变式(24 队列不重叠且落 1.25MB; 缓存行纪律)
 *   3. 路由往返一致(signal 侧 dst→队列 与 kernel 侧 队列→目的 rank 互逆)
 *   4. 全对全投递: 12 有向对各 1 封, wait_peer 逐对校验内容/来源
 *   5. 多目的地交错: 单发送方轮流发 3 对端, 各对端环序号连续无空洞
 *   6. 多发送方并发同一接收方: 交错投递, 各环独立按序, 环间零污染
 *   7. wait_any 归并消费: 收齐 3×N 封, 来源分布正确
 *   8. 单队列溢出语义: 消费落后超一环 → OVERRUN 显式暴露
 *   9. block 列投递(AllReduce 形态): 8 个发起 block 各发各列, 接收 block 列内收齐
 *  10. 多 block 并发隔离: 不同 block 队列互不干扰
 */
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

#include "hybm_sdma_contract.h"     // wire 契约(含布局头单一事实源)
#include "smem_shm_sdma_layout.h"   // 共享布局常量与映射函数(与 kernel/设备原语同源)

namespace ock {
namespace mf {
namespace {

/* 仿真拓扑: 2 卡 × 2 die = 4 rank; 段参数镜像实机 2 的幂约束 */
constexpr uint32_t RANKS = SMEM_SHM_SDMA_MAX_RANKS;
constexpr uint64_t SYM_SIZE = 256UL * 1024UL * 1024UL;  /* 每段对称大小 256MB(2 的幂) */
constexpr uint64_t PHYS_SIZE = 256UL * 1024UL * 1024UL; /* 段物理大小 */
constexpr uint64_t POOL_BASE = 0x12c200000000UL;         /* 池窗口基址(固定起点) */
constexpr uint32_t BLOCKS = SMEM_SHM_SDMA_MAX_BLOCKS;   /* 每 die AICore block 数 */

struct TestMail { /* 到达邮件像(镜像 HybmSdmaArrMail 4×u64) */
    uint64_t seq, dst, len, imm;
};

/* 4 rank 段尾保留区 + 各队列已投递序号(host 内存仿真; 模拟 host 建池清零) */
struct RankSim {
    std::vector<uint8_t> buf;
    uint8_t *res;                     /* 保留区基址指针 */
    uint64_t ringSeq[SMEM_SHM_SDMA_QUEUE_COUNT]; /* 各队列已投递序号(signal 侧推进) */
    RankSim() : buf(SMEM_SHM_SDMA_RESERVED, 0), res(buf.data()), ringSeq{} {}
    uint64_t segGva(uint32_t rank) const /* 本 rank 段基址 */
    {
        return POOL_BASE + static_cast<uint64_t>(rank) * SYM_SIZE;
    }
};

/* ---- 发送侧投递(复刻 signal_at 路由 + kernel consumeOneMail 的到达写算术) ---- */
/* dst 段基 → 目的 rank → 本端(对端, senderBlock)队列, 队列序号自增; 到达写接收方
 * 对应(发送方, senderBlock)子环。kernel 复核式: peerSeg == poolBase + Q×symSize
 * (路由往返用例单独覆盖)。senderBlock 缺省 0(单 block 兼容既有用例)。 */
void SimSend(RankSim ranks[], uint32_t sender, uint32_t receiver, uint64_t dstGva, uint64_t len, uint64_t imm,
             uint32_t senderBlock = 0, uint64_t *seqOut = nullptr)
{
    /* signal 侧路由: dstSeg - ownSeg = symm×(Q-rank)(有符号精确整除) */
    const uint64_t dstSeg = dstGva & ~(SYM_SIZE - 1);
    const uint64_t ownSeg = ranks[sender].segGva(sender);
    const int64_t segOff = static_cast<int64_t>(dstSeg - ownSeg) / static_cast<int64_t>(SYM_SIZE);
    ASSERT_EQ(sender + segOff, static_cast<int64_t>(receiver)); /* 用例内自检: 目的即意图接收方 */
    /* 发送队列 = 发送方视角(对端环下标, 发起 block); 环序号连续无空洞 */
    const uint32_t reqQ = SmemShmSdmaQueueIdx(SmemShmSdmaPeerRingIdx(sender, receiver), senderBlock);
    const uint64_t d = ranks[sender].ringSeq[reqQ]++;
    if (seqOut != nullptr) {
        *seqOut = d;
    }
    /* kernel 侧到达写: 接收方子环 = 接收方视角(发送方环下标, 发起 block) */
    const uint32_t arrQ = SmemShmSdmaQueueIdx(SmemShmSdmaPeerRingIdx(receiver, sender), senderBlock);
    const uint64_t ring = SMEM_SHM_SDMA_RS_PEER_RINGS + arrQ * SMEM_SHM_SDMA_RS_PEER_STRIDE;
    TestMail *mail = reinterpret_cast<TestMail *>(ranks[receiver].res + ring + SMEM_SHM_SDMA_RS_PEER_MAIL +
                                                 (d % SMEM_SHM_SDMA_RS_PEER_SLOTS) * sizeof(TestMail));
    mail->seq = d + 1; /* 1 基戳(0 为空哨兵) */
    mail->dst = dstGva;
    mail->len = len;
    mail->imm = imm;
    uint64_t *stamp = reinterpret_cast<uint64_t *>(ranks[receiver].res + ring + SMEM_SHM_SDMA_RS_PEER_STAMP +
                                                   (d % SMEM_SHM_SDMA_RS_PEER_SLOTS) * 8);
    *stamp = d + 1; /* SQE FIFO 保证戳晚于邮件落地, 仿真直接成对写 */
}

/* ---- 接收侧读(复刻设备原语 smem_shm_sdma_wait_peer_at 的消费算术, block 列对齐) ---- */
struct RecvMail { /* 镜像 smem_shm_sdma_mail_t 的出参 */
    uint64_t dst, len, imm;
    uint32_t seq, peer, status;
};

RecvMail SimWaitPeer(RankSim &rank, uint32_t ownRank, uint32_t peerRank, uint32_t block = 0)
{
    RecvMail m = {0, 0, 0, 0, 0, 0};
    const uint64_t ring = SMEM_SHM_SDMA_RS_PEER_RINGS +
                          SmemShmSdmaQueueIdx(SmemShmSdmaPeerRingIdx(ownRank, peerRank), block) *
                              SMEM_SHM_SDMA_RS_PEER_STRIDE;
    const uint64_t head = *reinterpret_cast<uint64_t *>(rank.res + ring + SMEM_SHM_SDMA_RS_PEER_HEAD);
    const uint64_t stamp = *reinterpret_cast<uint64_t *>(
        rank.res + ring + SMEM_SHM_SDMA_RS_PEER_STAMP + (head % SMEM_SHM_SDMA_RS_PEER_SLOTS) * 8);
    if (stamp == 0 || stamp < head + 1) { /* 空槽/旧圈值 = 未就绪 */
        m.status = 1;                     /* TIMEOUT(仿真不等轮询, 直接判未就绪) */
        m.seq = static_cast<uint32_t>(head);
        return m;
    }
    if (stamp > head + 1) { /* 戳超前期望 = 落后至少一整环(显式溢出) */
        m.status = 2;      /* OVERRUN */
        m.seq = static_cast<uint32_t>(head);
        return m;
    }
    const TestMail *mail = reinterpret_cast<const TestMail *>(
        rank.res + ring + SMEM_SHM_SDMA_RS_PEER_MAIL + (head % SMEM_SHM_SDMA_RS_PEER_SLOTS) * sizeof(TestMail));
    m.dst = mail->dst;
    m.len = mail->len;
    m.imm = mail->imm;
    m.seq = static_cast<uint32_t>(mail->seq - 1);
    m.peer = peerRank;
    m.status = 0;
    *reinterpret_cast<uint64_t *>(rank.res + ring + SMEM_SHM_SDMA_RS_PEER_HEAD) = head + 1;
    return m;
}

/* 归并消费(复刻 wait_any_at: 轮询本 block 列全部子环, 就绪即消费; 未就绪返回 TIMEOUT) */
RecvMail SimWaitAny(RankSim &rank, uint32_t ownRank, uint32_t block = 0)
{
    RecvMail m = {0, 0, 0, 0, 0, 1};
    for (uint32_t pr = 0; pr < RANKS - 1; pr++) { /* 对端环下标序 */
        const uint64_t ring = SMEM_SHM_SDMA_RS_PEER_RINGS +
                              SmemShmSdmaQueueIdx(pr, block) * SMEM_SHM_SDMA_RS_PEER_STRIDE;
        const uint64_t head = *reinterpret_cast<uint64_t *>(rank.res + ring + SMEM_SHM_SDMA_RS_PEER_HEAD);
        const uint64_t stamp = *reinterpret_cast<uint64_t *>(
            rank.res + ring + SMEM_SHM_SDMA_RS_PEER_STAMP + (head % SMEM_SHM_SDMA_RS_PEER_SLOTS) * 8);
        if (stamp != head + 1) { /* 空/旧/超前都跳过(溢出语义由 wait_peer 用例单测) */
            continue;
        }
        const TestMail *mail = reinterpret_cast<const TestMail *>(
            rank.res + ring + SMEM_SHM_SDMA_RS_PEER_MAIL + (head % SMEM_SHM_SDMA_RS_PEER_SLOTS) * sizeof(TestMail));
        m.dst = mail->dst;
        m.len = mail->len;
        m.imm = mail->imm;
        m.seq = static_cast<uint32_t>(mail->seq - 1);
        m.peer = SmemShmSdmaPeerRankOf(ownRank, pr);
        m.status = 0;
        *reinterpret_cast<uint64_t *>(rank.res + ring + SMEM_SHM_SDMA_RS_PEER_HEAD) = head + 1;
        return m;
    }
    return m;
}
} // namespace

/* 用例 1: 环/队列下标映射不变式——每视角双射(对端 × block 全 24 队列恰一次) */
TEST(Sdma4RankProtocolTest, RingAndQueueIdxBijection)
{
    for (uint32_t own = 0; own < RANKS; own++) {
        std::vector<uint32_t> seen(SMEM_SHM_SDMA_QUEUE_COUNT, 0);
        for (uint32_t peer = 0; peer < RANKS; peer++) {
            if (peer == own) {
                continue;
            }
            const uint32_t prIdx = SmemShmSdmaPeerRingIdx(own, peer);
            EXPECT_LT(prIdx, SMEM_SHM_SDMA_MAX_PEERS) << "own:" << own << " peer:" << peer;
            EXPECT_EQ(SmemShmSdmaPeerRankOf(own, prIdx), peer) << "own:" << own << " peer:" << peer;
            for (uint32_t b = 0; b < BLOCKS; b++) {
                const uint32_t q = SmemShmSdmaQueueIdx(prIdx, b);
                EXPECT_LT(q, SMEM_SHM_SDMA_QUEUE_COUNT) << "own:" << own << " peer:" << peer << " b:" << b;
                seen[q]++;
                /* 队列分解往返一致(kernel 消费侧反推目的 rank 与发起 block) */
                EXPECT_EQ(SmemShmSdmaQueuePeerRing(q), prIdx) << "q:" << q;
                EXPECT_EQ(SmemShmSdmaQueueBlock(q), b) << "q:" << q;
            }
        }
        for (uint32_t q = 0; q < SMEM_SHM_SDMA_QUEUE_COUNT; q++) {
            EXPECT_EQ(seen[q], 1U) << "own:" << own << " q:" << q; /* 恰好一次(双射) */
        }
    }
}

/* 用例 2: 布局地址不变式——四组本端数组各 24 队列首尾相接, 到达子环队列内不重叠、
 * 全部落 1.25MB; 各基址/间距 64B 对齐(跨写者字段独占行纪律) */
TEST(Sdma4RankProtocolTest, LayoutAddressInvariants)
{
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
    /* 暂存槽数 > 单队列在途上界(SQ 环深/4); 到达深度 > 在途上界(请求环 64 + SQ 在途 256) */
    EXPECT_GT(SMEM_SHM_SDMA_RS_STAGE_SLOTS, 1024U / 4U);
    EXPECT_GT(SMEM_SHM_SDMA_RS_PEER_SLOTS, 64U + 1024U / 4U);
    /* 到达子环数组完整落在保留区内(含最深一队列的游标) */
    EXPECT_LE(SMEM_SHM_SDMA_RS_PEER_RINGS + SMEM_SHM_SDMA_QUEUE_COUNT * SMEM_SHM_SDMA_RS_PEER_STRIDE,
              SMEM_SHM_SDMA_RESERVED);
    /* 64B 行对齐纪律 */
    for (uint64_t v : {SMEM_SHM_SDMA_RS_REQ_TAIL, SMEM_SHM_SDMA_RS_REQ_HEAD, SMEM_SHM_SDMA_RS_QUIET,
                       SMEM_SHM_SDMA_RS_STAGE, SMEM_SHM_SDMA_RS_PEER_RINGS}) {
        EXPECT_EQ(v & 0x3FU, 0U) << "base 0x" << std::hex << v;
    }
    for (uint64_t v : {SMEM_SHM_SDMA_RS_REQ_RING_STRIDE, SMEM_SHM_SDMA_RS_CURSOR_STRIDE,
                       SMEM_SHM_SDMA_RS_QUIET_STRIDE, SMEM_SHM_SDMA_RS_STAGE_STRIDE,
                       SMEM_SHM_SDMA_RS_PEER_STRIDE, SMEM_SHM_SDMA_RS_PEER_MAIL, SMEM_SHM_SDMA_RS_PEER_HEAD}) {
        EXPECT_EQ(v & 0x3FU, 0U) << "stride 0x" << std::hex << v;
    }
    /* 同 rank 任意两到达子环的邮件/戳区块两两不重叠 */
    const uint64_t mailEnd = SMEM_SHM_SDMA_RS_PEER_MAIL + SMEM_SHM_SDMA_RS_PEER_SLOTS * 32;
    const uint64_t stampEnd = SMEM_SHM_SDMA_RS_PEER_STAMP + SMEM_SHM_SDMA_RS_PEER_SLOTS * 8;
    for (uint32_t i = 0; i < SMEM_SHM_SDMA_QUEUE_COUNT; i++) {
        for (uint32_t j = i + 1; j < SMEM_SHM_SDMA_QUEUE_COUNT; j++) {
            const uint64_t ri = SMEM_SHM_SDMA_RS_PEER_RINGS + i * SMEM_SHM_SDMA_RS_PEER_STRIDE;
            const uint64_t rj = SMEM_SHM_SDMA_RS_PEER_RINGS + j * SMEM_SHM_SDMA_RS_PEER_STRIDE;
            EXPECT_FALSE(ri < rj + mailEnd && rj < ri + mailEnd) << "mail queues " << i << " vs " << j;
            EXPECT_FALSE(ri < rj + stampEnd && rj < ri + stampEnd) << "stamp queues " << i << " vs " << j;
        }
    }
}

/* 用例 3: 路由往返一致——signal 侧 dst→目的 rank→队列 与 kernel 侧 队列→(目的 rank,
 * 发起 block) 互逆; kernel 复核式对全部(有向对 × block)成立 */
TEST(Sdma4RankProtocolTest, RoutingRoundTripConsistent)
{
    for (uint32_t sender = 0; sender < RANKS; sender++) {
        for (uint32_t dest = 0; dest < RANKS; dest++) {
            if (sender == dest) {
                continue;
            }
            for (uint32_t b = 0; b < BLOCKS; b++) {
                /* signal 侧: 目的段内任一地址 → 目的 rank(段基掩码 + 有符号偏移整除) */
                const uint64_t dst = POOL_BASE + dest * SYM_SIZE + 0x1234;
                const uint64_t masked = dst & ~(SYM_SIZE - 1);
                const int64_t segOff =
                    static_cast<int64_t>(masked - (POOL_BASE + sender * SYM_SIZE)) / static_cast<int64_t>(SYM_SIZE);
                ASSERT_EQ(sender + segOff, static_cast<int64_t>(dest));
                /* 队列编码与 kernel 侧分解互逆 */
                const uint32_t q = SmemShmSdmaQueueIdx(SmemShmSdmaPeerRingIdx(sender, dest), b);
                const uint32_t recoveredQ = SmemShmSdmaPeerRankOf(sender, SmemShmSdmaQueuePeerRing(q));
                EXPECT_EQ(recoveredQ, dest) << "sender:" << sender << " dest:" << dest << " b:" << b;
                EXPECT_EQ(SmemShmSdmaQueueBlock(q), b) << "sender:" << sender << " dest:" << dest;
                /* kernel 复核式: dst 段基与分解所得目的 rank 段基一致 */
                EXPECT_EQ(masked, POOL_BASE + recoveredQ * SYM_SIZE) << "sender:" << sender;
                /* kernel 到达定位: 对端子环 = 接收方视角(发送方环下标, 发起 block) */
                const uint32_t arrQ = SmemShmSdmaQueueIdx(SmemShmSdmaPeerRingIdx(dest, sender), b);
                EXPECT_LT(arrQ, SMEM_SHM_SDMA_QUEUE_COUNT);
            }
        }
    }
}

/* 用例 4: 全对全投递——12 有向对各 1 封(block 0), 接收方按对端逐封校验内容与来源 */
TEST(Sdma4RankProtocolTest, AllToAllDelivery)
{
    RankSim ranks[RANKS];
    for (uint32_t sender = 0; sender < RANKS; sender++) {
        for (uint32_t receiver = 0; receiver < RANKS; receiver++) {
            if (sender == receiver) {
                continue;
            }
            const uint64_t dst = ranks[receiver].segGva(receiver) + 0x1000 + sender; /* 落点含发送方标识 */
            SimSend(ranks, sender, receiver, dst, /*len=*/0x40 + sender, /*imm=*/sender * 7);
        }
    }
    for (uint32_t receiver = 0; receiver < RANKS; receiver++) {
        for (uint32_t sender = 0; sender < RANKS; sender++) {
            if (sender == receiver) {
                continue;
            }
            const auto m = SimWaitPeer(ranks[receiver], receiver, sender);
            ASSERT_EQ(m.status, 0U) << "receiver:" << receiver << " sender:" << sender;
            EXPECT_EQ(m.peer, sender);
            EXPECT_EQ(m.seq, 0U); /* 每对端首封: 队列序号 0 */
            EXPECT_EQ(m.dst, ranks[receiver].segGva(receiver) + 0x1000 + sender);
            EXPECT_EQ(m.len, 0x40U + sender);
            EXPECT_EQ(m.imm, sender * 7U);
        }
    }
}

/* 用例 5: 多目的地交错——单发送方轮流发 3 对端, 各对端环序号连续无空洞。
 * 旧协议(单请求环全局序号)下: 第 2/3 目的的首封序号为 1/2, 接收方 head+1 失配必败;
 * 按队列划分后各目的独立计数, 每对端首封序号恒为 0。 */
TEST(Sdma4RankProtocolTest, InterleavedDestinationsNoSeqHoles)
{
    RankSim ranks[RANKS];
    constexpr uint32_t rounds = 4;
    for (uint32_t i = 0; i < rounds; i++) {
        for (uint32_t dest = 1; dest < RANKS; dest++) { /* rank0 轮流发 rank1/2/3 */
            SimSend(ranks, 0, dest, ranks[dest].segGva(dest) + i * 0x100, 0x20 + i, i * 8 + dest);
        }
    }
    for (uint32_t dest = 1; dest < RANKS; dest++) {
        for (uint32_t i = 0; i < rounds; i++) {
            const auto m = SimWaitPeer(ranks[dest], dest, 0);
            ASSERT_EQ(m.status, 0U) << "dest:" << dest << " round:" << i;
            EXPECT_EQ(m.peer, 0U);
            EXPECT_EQ(m.seq, i); /* 该对端队列内序号连续 0..rounds-1, 无跨目的地空洞 */
            EXPECT_EQ(m.len, 0x20U + i);
            EXPECT_EQ(m.imm, i * 8U + dest);
        }
    }
}

/* 用例 6: 多发送方并发同一接收方(旧协议必败场景)——3 发送方各 N 封交错投递,
 * 各环独立按序, 环间零污染 */
TEST(Sdma4RankProtocolTest, ConcurrentSendersIsolated)
{
    RankSim ranks[RANKS];
    constexpr uint64_t N = 5;
    /* rank3 为接收方; rank0/1/2 交错各发 N 封(同序号交错 = 旧协议最恶劣冲突形态) */
    for (uint64_t seq = 0; seq < N; seq++) {
        for (uint32_t sender = 0; sender < RANKS - 1; sender++) {
            const uint64_t dst = ranks[RANKS - 1].segGva(RANKS - 1) + seq * 0x100;
            SimSend(ranks, sender, RANKS - 1, dst, 0x20 + sender, seq * 100 + sender);
        }
    }
    for (uint32_t sender = 0; sender < RANKS - 1; sender++) {
        for (uint64_t seq = 0; seq < N; seq++) {
            const auto m = SimWaitPeer(ranks[RANKS - 1], RANKS - 1, sender);
            ASSERT_EQ(m.status, 0U) << "sender:" << sender << " seq:" << seq;
            EXPECT_EQ(m.peer, sender);
            EXPECT_EQ(m.seq, seq); /* 每发送方内部严格按序 */
            EXPECT_EQ(m.len, 0x20U + sender);
            EXPECT_EQ(m.imm, seq * 100 + sender);
        }
    }
}

/* 用例 7: wait_any 归并消费——3 发送方并发, 接收方无参收齐全部, 来源分布正确 */
TEST(Sdma4RankProtocolTest, WaitAnyGatherFromAllPeers)
{
    RankSim ranks[RANKS];
    constexpr uint64_t PER_PEER = 3;
    for (uint32_t sender = 0; sender < RANKS - 1; sender++) {
        for (uint64_t seq = 0; seq < PER_PEER; seq++) {
            SimSend(ranks, sender, RANKS - 1, ranks[RANKS - 1].segGva(RANKS - 1) + seq, 0x10, sender);
        }
    }
    uint32_t perSenderCount[RANKS] = {0, 0, 0, 0};
    for (uint64_t i = 0; i < (RANKS - 1) * PER_PEER; i++) {
        const auto m = SimWaitAny(ranks[RANKS - 1], RANKS - 1);
        ASSERT_EQ(m.status, 0U) << "gather idx:" << i;
        ASSERT_LT(m.peer, RANKS);
        perSenderCount[m.peer]++;
    }
    for (uint32_t sender = 0; sender < RANKS - 1; sender++) {
        EXPECT_EQ(perSenderCount[sender], PER_PEER) << "sender:" << sender; /* 每来源恰好收齐 */
    }
    const auto drained = SimWaitAny(ranks[RANKS - 1], RANKS - 1);
    EXPECT_EQ(drained.status, 1U); /* 全部收尽后再等 = 未就绪(轮询预算耗尽语义) */
}

/* 用例 8: 单队列溢出语义——消费落后超一环(未消费封数 > 512)时显式 OVERRUN, 不静默覆写 */
TEST(Sdma4RankProtocolTest, QueueOverrunExposed)
{
    RankSim ranks[RANKS];
    constexpr uint64_t overflowAhead = SMEM_SHM_SDMA_RS_PEER_SLOTS + 8;
    for (uint64_t i = 0; i < overflowAhead; i++) {
        SimSend(ranks, 0, 1, ranks[1].segGva(1) + i, 8, i);
    }
    const auto m = SimWaitPeer(ranks[1], 1, 0);
    EXPECT_EQ(m.status, 2U); /* 首槽已被第 513 封覆写 → 戳超前期望 → OVERRUN */
}

/* 用例 9: block 列投递(AllReduce 形态)——每 rank 的 8 个发起 block 各向 3 个对端
 * 发本 block 分片(imm 编码源 rank 与 block, 可溯源); 接收方各 block 列内逐对端收齐,
 * 仅收到本列(发送方同 block 号)内容——多 block 列投递与隔离的协议级验证 */
TEST(Sdma4RankProtocolTest, BlockColumnAllReduceDelivery)
{
    RankSim ranks[RANKS];
    /* 全部 (发送方, block, 接收方) 组合: 4×8×3 = 96 封 */
    for (uint32_t sender = 0; sender < RANKS; sender++) {
        for (uint32_t b = 0; b < BLOCKS; b++) {
            for (uint32_t dest = 0; dest < RANKS; dest++) {
                if (dest == sender) {
                    continue;
                }
                const uint64_t dst = ranks[dest].segGva(dest) + b * 0x10000; /* 落点编码 block 号 */
                SimSend(ranks, sender, dest, dst, 0x1000 + b, (static_cast<uint64_t>(sender) << 8) | dest,
                        b, nullptr);
            }
        }
    }
    /* 接收方: 每个 block 列内按对端逐封收齐(1 封/对端/block), 校验来源/内容/block */
    for (uint32_t receiver = 0; receiver < RANKS; receiver++) {
        for (uint32_t b = 0; b < BLOCKS; b++) {
            for (uint32_t s = 0; s < RANKS; s++) {
                if (s == receiver) {
                    continue;
                }
                const auto m = SimWaitPeer(ranks[receiver], receiver, s, b);
                ASSERT_EQ(m.status, 0U) << "receiver:" << receiver << " b:" << b << " sender:" << s;
                EXPECT_EQ(m.peer, s);
                EXPECT_EQ(m.seq, 0U); /* 每 (对端, block) 队列首封 */
                EXPECT_EQ(m.dst, ranks[receiver].segGva(receiver) + b * 0x10000); /* 落点即发送 block 号 */
                EXPECT_EQ(m.len, 0x1000U + b);
                EXPECT_EQ(m.imm, (static_cast<uint64_t>(s) << 8) | receiver); /* 路由对账 */
            }
        }
    }
}

/* 用例 10: 多 block 并发隔离——同一发送方不同 block 并发打同一对端, 各队列独立按序,
 * 队列间零污染(block 维的多生产者竞争回归) */
TEST(Sdma4RankProtocolTest, MultiBlockConcurrentIsolated)
{
    RankSim ranks[RANKS];
    constexpr uint64_t N = 4;
    /* rank0 的 8 个 block 并发各发 N 封到 rank2(同序号交错 = 队列竞争最恶劣形态) */
    for (uint64_t seq = 0; seq < N; seq++) {
        for (uint32_t b = 0; b < BLOCKS; b++) {
            SimSend(ranks, 0, 2, ranks[2].segGva(2) + b * 0x1000 + seq, 0x80 + b, seq * 16 + b, b, nullptr);
        }
    }
    for (uint32_t b = 0; b < BLOCKS; b++) {
        for (uint64_t seq = 0; seq < N; seq++) {
            const auto m = SimWaitPeer(ranks[2], 2, 0, b);
            ASSERT_EQ(m.status, 0U) << "b:" << b << " seq:" << seq;
            EXPECT_EQ(m.seq, seq);        /* 各 block 队列内严格按序 */
            EXPECT_EQ(m.len, 0x80U + b);  /* 内容无跨 block 污染 */
            EXPECT_EQ(m.imm, seq * 16 + b);
        }
    }
}

/* 用例 11: 契约参数与 KVER——wire 变更布局锁(v31: 布局锁不变, KVER≥31) */
TEST(Sdma4RankProtocolTest, ContractParamInvariants)
{
    EXPECT_EQ(sizeof(HybmSdmaOrchParam), 80U);
    EXPECT_EQ(offsetof(HybmSdmaOrchParam, resBase), 16U);      /* 保留区基址(队列定位原点) */
    EXPECT_EQ(offsetof(HybmSdmaOrchParam, rank), 64U);         /* 本端 rank(队列路由) */
    EXPECT_EQ(offsetof(HybmSdmaOrchParam, poolBaseGva), 72U);  /* 池窗口基址(路由复核) */
    EXPECT_GE(HYBM_SDMA_ORCH_KVER, 31U);                       /* v31 = 队列按 AICore block 扩展 */
    EXPECT_EQ(SMEM_SHM_SDMA_MAX_RANKS, 4U);
    EXPECT_EQ(SMEM_SHM_SDMA_MAX_PEERS, 3U);
    EXPECT_EQ(SMEM_SHM_SDMA_MAX_BLOCKS, 8U);
    EXPECT_EQ(SMEM_SHM_SDMA_QUEUE_COUNT, 24U);
}
} // namespace mf
} // namespace ock
