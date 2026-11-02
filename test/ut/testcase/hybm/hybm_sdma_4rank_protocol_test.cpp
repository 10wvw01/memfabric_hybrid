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
 * v29 按对端划分请求环与到达环(每对端一套环, 各自独立序号), 本文件在主机内存上
 * 仿真 4 rank 段尾保留区, 收发两侧使用与 kernel/设备原语完全相同的共享布局常量
 * 与映射函数, 验证 4-rank 全对全路由正确性与多发送方并发隔离:
 *   1. PeerRingIdx/PeerRankOf 映射不变式(全 12 有向对, 每视角环下标双射)
 *   2. 布局地址不变式(同接收方 3 环不重叠且落 192KB; 请求环数组完整覆盖)
 *   3. 路由往返一致(signal 侧 dst→环 与 kernel 侧 环→目的 rank 互逆)
 *   4. 全对全投递: 12 有向对各 1 封, wait_peer 逐对校验内容/来源
 *   5. 多目的地交错: 单发送方轮流发 3 对端, 各对端环序号连续无空洞(旧协议必败)
 *   6. 多发送方并发同一接收方: 3 发送方交错投递, 各环独立按序, 环间零污染
 *   7. wait_any 归并消费: 收齐 3×N 封, 来源分布正确
 *   8. 单环溢出语义: 消费落后超一环 → OVERRUN 显式暴露
 */
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

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

struct TestMail { /* 到达邮件像(镜像 HybmSdmaArrMail 4×u64) */
    uint64_t seq, dst, len, imm;
};

/* 4 rank 段尾保留区 + 发送方 per-ring 请求序号计数(host 内存仿真; 模拟 host 建池清零) */
struct RankSim {
    std::vector<uint8_t> buf;
    uint8_t *res;                    /* 保留区基址指针 */
    uint64_t ringSeq[SMEM_SHM_SDMA_MAX_PEERS]; /* 本 rank 各请求环已投递序号(signal 侧推进) */
    RankSim() : buf(SMEM_SHM_SDMA_RESERVED, 0), res(buf.data()), ringSeq{0, 0, 0} {}
    uint64_t segGva(uint32_t rank) const /* 本 rank 段基址 */
    {
        return POOL_BASE + static_cast<uint64_t>(rank) * SYM_SIZE;
    }
};

/* ---- 发送侧投递(复刻 signal_at 路由 + kernel consumeOneMail 的到达写算术) ---- */
/* dst 段基 → 目的 rank → 本端请求环(ringIdx), 环序号自增; 到达写接收方对应环。
 * kernel 复核式: peerSeg == poolBase + Q×symSize(路由往返用例单独覆盖)。 */
void SimSend(RankSim ranks[], uint32_t sender, uint32_t receiver, uint64_t dstGva, uint64_t len, uint64_t imm,
             uint64_t *seqOut = nullptr)
{
    /* signal 侧路由: dstSeg - ownSeg = symm×(Q-rank)(有符号精确整除) */
    const uint64_t dstSeg = dstGva & ~(SYM_SIZE - 1);
    const uint64_t ownSeg = ranks[sender].segGva(sender);
    const int64_t segOff = static_cast<int64_t>(dstSeg - ownSeg) / static_cast<int64_t>(SYM_SIZE);
    ASSERT_EQ(sender + segOff, static_cast<int64_t>(receiver)); /* 用例内自检: 目的即意图接收方 */
    const uint32_t reqRing = SmemShmSdmaPeerRingIdx(sender, receiver); /* 本端请求环下标 */
    const uint64_t d = ranks[sender].ringSeq[reqRing]++;                /* 环序号(连续无空洞) */
    if (seqOut != nullptr) {
        *seqOut = d;
    }
    /* kernel 侧到达写: 接收方环下标 = 接收方视角压缩发送方 rank */
    const uint32_t arrRing = SmemShmSdmaPeerRingIdx(receiver, sender);
    const uint64_t ring = SMEM_SHM_SDMA_RS_PEER_RINGS + arrRing * SMEM_SHM_SDMA_RS_PEER_STRIDE;
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

/* ---- 接收侧读(复刻设备原语 smem_shm_sdma_wait_peer_at 的消费算术) ---- */
struct RecvMail { /* 镜像 smem_shm_sdma_mail_t 的出参 */
    uint64_t dst, len, imm;
    uint32_t seq, peer, status;
};

RecvMail SimWaitPeer(RankSim &rank, uint32_t ownRank, uint32_t peerRank)
{
    RecvMail m = {0, 0, 0, 0, 0, 0};
    const uint64_t ring = SMEM_SHM_SDMA_RS_PEER_RINGS +
                          SmemShmSdmaPeerRingIdx(ownRank, peerRank) * SMEM_SHM_SDMA_RS_PEER_STRIDE;
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

/* 归并消费(复刻 wait_any_at: 轮询本端全部环, 就绪即消费; 未就绪返回 TIMEOUT) */
RecvMail SimWaitAny(RankSim &rank, uint32_t ownRank)
{
    RecvMail m = {0, 0, 0, 0, 0, 1};
    for (uint32_t ringIdx = 0; ringIdx < RANKS - 1; ringIdx++) {
        const uint64_t ring = SMEM_SHM_SDMA_RS_PEER_RINGS + ringIdx * SMEM_SHM_SDMA_RS_PEER_STRIDE;
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
        m.peer = SmemShmSdmaPeerRankOf(ownRank, ringIdx);
        m.status = 0;
        *reinterpret_cast<uint64_t *>(rank.res + ring + SMEM_SHM_SDMA_RS_PEER_HEAD) = head + 1;
        return m;
    }
    return m;
}
} // namespace

/* 用例 1: 环下标映射不变式——4-rank 全部有向对合法且每视角双射 */
TEST(Sdma4RankProtocolTest, PeerRingIdxBijection)
{
    for (uint32_t own = 0; own < RANKS; own++) {
        std::vector<uint32_t> seen; /* 本视角各对端的环下标 */
        for (uint32_t peer = 0; peer < RANKS; peer++) {
            if (peer == own) {
                continue; /* 自送非法, signal/kernel 侧均显式拒绝 */
            }
            const uint32_t idx = SmemShmSdmaPeerRingIdx(own, peer);
            EXPECT_LT(idx, SMEM_SHM_SDMA_MAX_PEERS) << "own:" << own << " peer:" << peer;
            seen.push_back(idx);
            /* 双射反演: 环下标 → 对端 rank 还原(signal 侧与 kernel 侧互逆的基石) */
            EXPECT_EQ(SmemShmSdmaPeerRankOf(own, idx), peer) << "own:" << own << " peer:" << peer;
        }
        /* 3 个对端映射到 3 个互不相同的环(无共享环即无写者冲突) */
        ASSERT_EQ(seen.size(), SMEM_SHM_SDMA_MAX_PEERS);
        EXPECT_NE(seen[0], seen[1]);
        EXPECT_NE(seen[0], seen[2]);
        EXPECT_NE(seen[1], seen[2]);
    }
}

/* 用例 2: 布局地址不变式——请求环数组完整覆盖且游标不压环; 同接收方 3 个到达环
 * 的邮件/戳/游标地址不重叠且落 192KB(跨 rank 仿真缓冲独立, 环间重叠只可能在同接收方内) */
TEST(Sdma4RankProtocolTest, LayoutAddressInvariants)
{
    /* 请求环数组: 3 环完整覆盖 [REQ_RING, REQ_TAIL), 游标数组不压环 */
    EXPECT_EQ(SMEM_SHM_SDMA_RS_REQ_RING + 3U * SMEM_SHM_SDMA_RS_REQ_RING_STRIDE, SMEM_SHM_SDMA_RS_REQ_TAIL);
    EXPECT_EQ(SMEM_SHM_SDMA_RS_REQ_HEAD + 3U * SMEM_SHM_SDMA_RS_CURSOR_STRIDE, SMEM_SHM_SDMA_RS_QUIET);
    EXPECT_EQ(SMEM_SHM_SDMA_RS_QUIET + 3U * SMEM_SHM_SDMA_RS_QUIET_STRIDE, SMEM_SHM_SDMA_RS_STAGE);
    EXPECT_EQ(SMEM_SHM_SDMA_RS_STAGE + 3U * SMEM_SHM_SDMA_RS_STAGE_STRIDE, SMEM_SHM_SDMA_RS_PEER_RINGS);
    /* 暂存槽数 > 单环在途上界(SQ 环深/4——每封 4 支 SQE ①⓪②③) */
    EXPECT_GT(SMEM_SHM_SDMA_RS_STAGE_SLOTS, 1024U / 4U);
    /* 到达环数组完整落在保留区内(含最深一环的游标) */
    EXPECT_LE(SMEM_SHM_SDMA_RS_PEER_RINGS + SMEM_SHM_SDMA_MAX_PEERS * SMEM_SHM_SDMA_RS_PEER_STRIDE,
              SMEM_SHM_SDMA_RESERVED);
    /* v23 缓存行分隔纪律: 各区块基址与间距 64B 对齐(共行回写竞态=丢件类根因) */
    for (uint64_t v : {SMEM_SHM_SDMA_RS_REQ_TAIL, SMEM_SHM_SDMA_RS_REQ_HEAD, SMEM_SHM_SDMA_RS_QUIET,
                       SMEM_SHM_SDMA_RS_STAGE, SMEM_SHM_SDMA_RS_PEER_RINGS}) {
        EXPECT_EQ(v & 0x3FU, 0U) << "base 0x" << std::hex << v;
    }
    for (uint64_t v : {SMEM_SHM_SDMA_RS_REQ_RING_STRIDE, SMEM_SHM_SDMA_RS_CURSOR_STRIDE,
                       SMEM_SHM_SDMA_RS_QUIET_STRIDE, SMEM_SHM_SDMA_RS_STAGE_STRIDE,
                       SMEM_SHM_SDMA_RS_PEER_STRIDE, SMEM_SHM_SDMA_RS_PEER_MAIL, SMEM_SHM_SDMA_RS_PEER_HEAD}) {
        EXPECT_EQ(v & 0x3FU, 0U) << "stride 0x" << std::hex << v;
    }
    /* 同接收方 3 环两两不重叠 */
    const uint64_t mailEnd = SMEM_SHM_SDMA_RS_PEER_MAIL + SMEM_SHM_SDMA_RS_PEER_SLOTS * 32;
    const uint64_t stampEnd = SMEM_SHM_SDMA_RS_PEER_STAMP + SMEM_SHM_SDMA_RS_PEER_SLOTS * 8;
    for (uint32_t i = 0; i < SMEM_SHM_SDMA_MAX_PEERS; i++) {
        for (uint32_t j = i + 1; j < SMEM_SHM_SDMA_MAX_PEERS; j++) {
            const uint64_t ri = SMEM_SHM_SDMA_RS_PEER_RINGS + i * SMEM_SHM_SDMA_RS_PEER_STRIDE;
            const uint64_t rj = SMEM_SHM_SDMA_RS_PEER_RINGS + j * SMEM_SHM_SDMA_RS_PEER_STRIDE;
            EXPECT_FALSE(ri < rj + mailEnd && rj < ri + mailEnd) << "mail rings " << i << " vs " << j;
            EXPECT_FALSE(ri < rj + stampEnd && rj < ri + stampEnd) << "stamp rings " << i << " vs " << j;
        }
    }
}

/* 用例 3: 路由往返一致——signal 侧 dst→目的 rank→请求环 与 kernel 侧 环→目的 rank
 * 互逆(kernel 复核式 peerSeg == poolBase + Q×symSize 对全部有向对成立) */
TEST(Sdma4RankProtocolTest, RoutingRoundTripConsistent)
{
    for (uint32_t sender = 0; sender < RANKS; sender++) {
        for (uint32_t dest = 0; dest < RANKS; dest++) {
            if (sender == dest) {
                continue;
            }
            /* signal 侧: 目的段内任一地址 → 目的 rank(段基掩码 + 有符号偏移整除) */
            const uint64_t dstSeg = POOL_BASE + dest * SYM_SIZE;
            const uint64_t dst = dstSeg + 0x1234; /* 段内任意偏移 */
            const uint64_t masked = dst & ~(SYM_SIZE - 1);
            const int64_t segOff =
                static_cast<int64_t>(masked - (POOL_BASE + sender * SYM_SIZE)) / static_cast<int64_t>(SYM_SIZE);
            ASSERT_EQ(sender + segOff, static_cast<int64_t>(dest));
            /* kernel 侧: 消费的请求环反推目的 rank(与到达环定位公式一致) */
            const uint32_t reqRing = SmemShmSdmaPeerRingIdx(sender, dest);
            const uint32_t recoveredQ = SmemShmSdmaPeerRankOf(sender, reqRing);
            EXPECT_EQ(recoveredQ, dest) << "sender:" << sender << " dest:" << dest;
            /* kernel 复核式: dst 段基与目的 rank 段基一致 */
            EXPECT_EQ(masked, POOL_BASE + recoveredQ * SYM_SIZE) << "sender:" << sender << " dest:" << dest;
        }
    }
}

/* 用例 4: 全对全投递——12 有向对各 1 封, 接收方按对端逐封校验内容与来源 */
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
            EXPECT_EQ(m.seq, 0U); /* 每对端首封: 环序号 0 */
            EXPECT_EQ(m.dst, ranks[receiver].segGva(receiver) + 0x1000 + sender);
            EXPECT_EQ(m.len, 0x40U + sender);
            EXPECT_EQ(m.imm, sender * 7U);
        }
    }
}

/* 用例 5: 多目的地交错——单发送方轮流发 3 对端, 各对端环序号连续无空洞。
 * 旧协议(单请求环全局序号)下: 第 2/3 目的的首封序号为 1/2, 接收方 head+1 失配必败;
 * per-ring 请求环下各目的独立计数, 每对端首封序号恒为 0。 */
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
            EXPECT_EQ(m.seq, i); /* 该对端环内序号连续 0..rounds-1, 无跨目的地空洞 */
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

/* 用例 8: 单环溢出语义——落后超一环(未消费封数 > 1024)时显式 OVERRUN, 不静默覆写 */
TEST(Sdma4RankProtocolTest, RingOverrunExposed)
{
    RankSim ranks[RANKS];
    constexpr uint64_t overflowAhead = SMEM_SHM_SDMA_RS_PEER_SLOTS + 8;
    for (uint64_t i = 0; i < overflowAhead; i++) {
        SimSend(ranks, 0, 1, ranks[1].segGva(1) + i, 8, i);
    }
    const auto m = SimWaitPeer(ranks[1], 1, 0);
    EXPECT_EQ(m.status, 2U); /* 首槽已被第 1025 封覆写 → 戳超前期望 → OVERRUN */
}

/* 用例 9: 契约参数与 KVER——wire 变更布局锁(v29: resBase/rank/poolBaseGva, 80B, KVER≥29) */
TEST(Sdma4RankProtocolTest, ContractParamInvariants)
{
    EXPECT_EQ(sizeof(HybmSdmaOrchParam), 80U);
    EXPECT_EQ(offsetof(HybmSdmaOrchParam, resBase), 16U);      /* 保留区基址(三请求环定位原点) */
    EXPECT_EQ(offsetof(HybmSdmaOrchParam, rank), 64U);         /* 本端 rank(环路由) */
    EXPECT_EQ(offsetof(HybmSdmaOrchParam, poolBaseGva), 72U);  /* 池窗口基址(路由复核) */
    EXPECT_GE(HYBM_SDMA_ORCH_KVER, 30U);                       /* v30 = 邮件像经 SDMA 投递 */
    /* 布局上限与 rank 上限一致(协同模式 4-rank 定界) */
    EXPECT_EQ(SMEM_SHM_SDMA_MAX_RANKS, 4U);
    EXPECT_EQ(SMEM_SHM_SDMA_MAX_PEERS, 3U);
}
} // namespace mf
} // namespace ock
