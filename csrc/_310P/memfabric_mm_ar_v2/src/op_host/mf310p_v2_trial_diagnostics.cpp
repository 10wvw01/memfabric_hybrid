// 仅供测试/验证：T1 小 M、T2 AR-only、分阶段诊断和地址探针。
#include "mf310p_v2_trial_internal.h"
#include "mf310p_v2_trial_diagnostics_api.h"
#include <cstdlib>
extern "C" int mf310p_v2_trial_launch_async(
    uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
    uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
    uint64_t, uint32_t, uint32_t, uint32_t, uint32_t, aclrtStream);
extern "C" int mf310p_v2_nz_direct_small_launch_async(
    uint64_t, uint64_t, uint64_t, uint64_t,
    uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
    uint64_t, uint64_t, uint64_t, uint64_t,
    uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, aclrtStream);
extern "C" int mf310p_v2_ar_stage_launch_async(
    uint64_t, uint64_t, uint64_t, uint32_t, aclrtStream);
extern "C" int mf310p_v2_ar_only_launch_async(
    uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
    uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
    uint32_t, uint32_t, uint32_t, uint32_t, aclrtStream);
extern "C" int mf310p_v2_diag_mm_async(
    uint64_t, uint64_t, uint64_t, uint32_t, uint32_t, aclrtStream);
extern "C" int mf310p_v2_diag_v1mm_async(
    uint64_t, uint64_t, uint64_t, uint32_t, uint32_t, aclrtStream);
extern "C" int mf310p_v2_diag_noib_async(
    uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
    uint64_t, uint64_t, uint32_t, uint32_t, aclrtStream);
extern "C" int mf310p_v2_diag_ib_async(
    uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
    uint64_t, uint64_t, uint64_t, uint32_t, uint32_t, aclrtStream);
extern "C" int mf310p_v2_diag_ibdone_async(
    uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
    uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint32_t,
    uint32_t, aclrtStream);
extern "C" int mf310p_v2_diag_ibdone16_async(
    uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
    uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint32_t,
    uint32_t, uint32_t, uint32_t, aclrtStream);
extern "C" int mf310p_v2_diag_loop_async(
    uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
    uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint32_t,
    uint32_t, uint32_t, uint32_t, aclrtStream);
namespace {
uint32_t small_template(uint32_t m)
{
    if (m <= 16) return 16;
    if (m <= 32) return 32;
    if (m <= 64) return 64;
    if (m <= 128) return 128;
    return 256;
}
uint32_t small_ti(uint32_t templ)
{
    if (templ == 16) return 0;
    if (templ == 32) return 1;
    if (templ == 64) return 2;
    if (templ == 128) return 3;
    return 4;
}
uint32_t small_ni(uint32_t cores)
{
    if (cores == 1) return 0;
    if (cores == 2) return 1;
    if (cores == 4) return 2;
    return 3;
}
}
/* T1：直接使用完整 NZ 权重，将精确 M 行结果写入对称 ND 输出；不切分缓存权重、不暂存输入、不散排输出。MM 核数与 Add 核数独立设置为 1/2/4/8。一次公共 SDMA 消息传输 M*2048*2 字节。 */
extern "C" int mf310p_v2_trial_run_nz_direct_small(
    mf310p_v2_trial_t* s, uint64_t x, uint64_t w, uint64_t y,
    uint32_t m, uint32_t mm_cores, uint32_t add_cores, void* stream)
{
    if (!s || !s->ib_probed || !stream || !x || !w || !y ||
        m == 0 || m > 256 ||
        (mm_cores != 1 && mm_cores != 2 &&
         mm_cores != 4 && mm_cores != 8) ||
        (add_cores != 1 && add_cores != 2 &&
         add_cores != 4 && add_cores != 8))
        return -1;

    const auto acl_stream = static_cast<aclrtStream>(stream);
    const auto& l = s->layout;
    const bool rank0 = s->rank == 0;
    const uint64_t ownC = rank0 ? l.send_arena : l.recv_arena;
    const uint64_t peerC = rank0 ? l.recv_arena : l.send_arena;
    const uint64_t peerDst = rank0 ? l.peer_segment : l.peer_recv_arena;
    const uint64_t expectedRecvDst =
        rank0 ? l.expected_recv_base :
                l.expected_recv_base - l.arena_bytes;
    const uint32_t templ = small_template(m);
    bool& warmed = s->nz_direct_symbol_warmed[small_ti(templ)]
                                               [small_ni(mm_cores)];
    if (!warmed) {
        for (int i = 0; i < 2; ++i) {
            const int rc = mf310p_v2_nz_direct_small_launch_async(
                l.pool_base, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                0, 0, 0, templ, 0, mm_cores, add_cores, 0, acl_stream);
            if (rc != 0) return rc;
        }
        warmed = true;
    }
    /* 与 run() 使用同类首次 MTE3 写保护，但仅同步启动 Stream，不使用池存活时可能耗时 0～15 秒的 aclrtSynchronizeDevice。临时试验机制，不适用于 Graph 捕获；调用方须逐次验证。 */
    if (getenv("MF310P_V2_NOWARMUP") == nullptr && s->warmed_y != y) {
        const int sync_err = static_cast<int>(aclrtSynchronizeStream(
            acl_stream));
        if (sync_err) return sync_err;
        s->warmed_y = y;
    }
    const int err = static_cast<int>(aclrtMemsetAsync(
        s->status, kStatusSize, 0, kStatusSize, acl_stream));
    if (err) return err;
    return mf310p_v2_nz_direct_small_launch_async(
        l.pool_base, x, w, y, ownC, peerC, peerDst, expectedRecvDst,
        reinterpret_cast<uint64_t>(s->ib),
        reinterpret_cast<uint64_t>(s->status),
        l.ack_slot, l.peer_ack_slot, l.expected_credit_dst,
        templ, m, mm_cores, add_cores, 1, acl_stream);
}


/* T2 传输隔离基线：同一 Stream 先运行框架原生 MM，再由该 ABI 将局部结果搬到对称 C，启动独立 AR-only 内核。D2D 与 AR 两阶段都必须计入与 Stock 的性能比较。尚未验证普通 PyTorch Tensor 可直接用作 SDMA 源，因此保留必要暂存，且不接入模型路由。 */
extern "C" int mf310p_v2_trial_run_ar_only(
    mf310p_v2_trial_t* s, uint64_t nativePartial, uint64_t y,
    uint32_t m, uint32_t addCores, void* stream)
{
    if (!s || !s->ib_probed || !stream || !nativePartial || !y ||
        nativePartial == y || m == 0 || m > kWindowRows ||
        (addCores != 1 && addCores != 2 &&
         addCores != 4 && addCores != 8))
        return -1;

    const auto acl_stream = static_cast<aclrtStream>(stream);
    const auto& l = s->layout;
    const bool rank0 = s->rank == 0;
    const uint64_t ownC = rank0 ? l.send_arena : l.recv_arena;
    const uint64_t peerC = rank0 ? l.recv_arena : l.send_arena;
    const uint64_t peerDst = rank0 ? l.peer_segment : l.peer_recv_arena;
    const uint64_t expectedRecvDst =
        rank0 ? l.expected_recv_base :
                l.expected_recv_base - l.arena_bytes;
    constexpr uint64_t kNativeRowBytes = 2048ULL * sizeof(uint16_t);
    const uint64_t bytes = static_cast<uint64_t>(m) * kNativeRowBytes;
    if (bytes > l.arena_bytes) return -1;

    if (!s->ar_only_symbol_warmed) {
        for (int i = 0; i < 2; ++i) {
            const int rc = mf310p_v2_ar_only_launch_async(
                l.pool_base, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                0, s->chunk_rows, addCores, 0, acl_stream);
            if (rc) return rc;
        }
        for (int i = 0; i < 2; ++i) {
            const int rc = mf310p_v2_ar_stage_launch_async(
                0, 0, 0, 0, acl_stream);
            if (rc) return rc;
        }
        s->ar_only_symbol_warmed = true;
    }

    /* 2026-10-18 T2 实测首写守护：AR-only 符号首次真实执行会丢失 MTE3 输出，Stream 同步无效；建池后新分配的原生 MM 暂存源可能读到垃圾数据，协议 status 仍为零。沿用 T0 已验证的全设备同步，在首次真实运行以及出现未保护的输出/暂存地址时触发；使用 64 项有界地址集合。建池后可能耗时 0～15 秒，稳定地址无重复成本。仅供试验，不兼容 Graph。 */
    const bool partial_guard =
        getenv("MF310P_V2_AR_ONLY_PARTIAL_GUARD") == nullptr;
    bool y_known = false;
    for (uint32_t k = 0; k < 64; ++k) {
        if (s->ar_only_warmed_ys[k] == y) { y_known = true; break; }
    }
    bool partial_known = !partial_guard;
    for (uint32_t k = 0; k < 64 && partial_guard; ++k) {
        if (s->ar_only_warmed_partials[k] == nativePartial) {
            partial_known = true; break;
        }
    }
    if (getenv("MF310P_V2_AR_ONLY_NOWARMUP") == nullptr &&
        (!s->ar_only_symbol_launched || !y_known || !partial_known)) {
        const int err = static_cast<int>(aclrtSynchronizeDevice());
        if (err) return err;
        if (!y_known) {
            s->ar_only_warmed_ys[s->ar_only_warmed_y_next] = y;
            s->ar_only_warmed_y_next =
                (s->ar_only_warmed_y_next + 1) % 64;
        }
        if (partial_guard && !partial_known) {
            s->ar_only_warmed_partials[s->ar_only_warmed_partial_next] =
                nativePartial;
            s->ar_only_warmed_partial_next =
                (s->ar_only_warmed_partial_next + 1) % 64;
        }
    }
    s->ar_only_symbol_launched = true;

    // 原生 MM 已由调用方入队到当前 Stream。
    // 暂存采用 AICore V2ArStageKernel，不使用 aclrtMemcpyAsync：
    // 该平台拷贝引擎的读取与同 Stream AICore
    // 写入并非一致（T2 曾复现旧数据，status=0）。暂存内核结尾
    // 通过 CleanLines 清理 ownC，使后续 AICPU SDMA signal 能看到
    // 新暂存数据，遵循 T0 已验证的规则。同 Stream 顺序
    // 还保证 AR-only 内核在暂存完成后才启动。
    // 因此必须保持上述暂存与通信 Kernel 的串行依赖。
    {
        /* 空闲窗口后的首次 MTE3 写可能被丢弃：设备同步或长 Stream 同步间隔后，单次暂存启动可无写出，导致两个 Rank 复现上一轮结果。连续启动更可靠，故在真实执行之前添加两次牺牲性启动；M=1 曾发现两次仍偶发失败。该行为属于 T0 BUG#3 同类风险，enable=0 预热无效。牺牲性输出会被同 Stream 的真实启动覆盖。 */
        const int sacrificial = mf310p_v2_ar_stage_launch_async(
            ownC, nativePartial, bytes, 1, acl_stream);
        if (sacrificial) return sacrificial;
        const int sacrificial2 = mf310p_v2_ar_stage_launch_async(
            ownC, nativePartial, bytes, 1, acl_stream);
        if (sacrificial2) return sacrificial2;
    }
    const int stage_err = mf310p_v2_ar_stage_launch_async(
        ownC, nativePartial, bytes, 1, acl_stream);
    if (stage_err) return stage_err;
    const auto clear = aclrtMemsetAsync(
        s->status, kStatusSize, 0, kStatusSize, acl_stream);
    if (clear != ACL_SUCCESS) return static_cast<int>(clear);

    return mf310p_v2_ar_only_launch_async(
        l.pool_base, ownC, peerC, y, peerDst, expectedRecvDst,
        reinterpret_cast<uint64_t>(s->ib),
        reinterpret_cast<uint64_t>(s->status),
        l.ack_slot, l.peer_ack_slot, l.expected_credit_dst,
        m, s->chunk_rows, addCores, 1, acl_stream);
}

/* 测试辅助：以 DDR 视图读取本 Rank 的发送区。 */
extern "C" int mf310p_v2_trial_read_send_arena(
    mf310p_v2_trial_t* s, void* host_dst, uint32_t bytes)
{
    if (!s || !host_dst || !bytes ||
        bytes > static_cast<uint32_t>(s->layout.arena_bytes))
        return -1;
    const uint64_t ownC = s->rank == 0 ? s->layout.send_arena
                                       : s->layout.recv_arena;
    return static_cast<int>(aclrtMemcpy(
        host_dst, bytes, reinterpret_cast<void*>(ownC), bytes,
        ACL_MEMCPY_DEVICE_TO_HOST));
}

/* 测试辅助：暴露对称内存关键地址供诊断。 */
extern "C" int mf310p_v2_trial_addresses(
    mf310p_v2_trial_t* s, uint64_t* out /* 地址字段：pool_base、send_arena、recv_arena、peer_segment、peer_recv_arena、expected_recv_base、arena_bytes。 */)
{
    if (!s || !out) return -1;
    const mf310p_layout_t& l = s->layout;
    out[0] = l.pool_base;
    out[1] = l.send_arena;
    out[2] = l.recv_arena;
    out[3] = l.peer_segment;
    out[4] = l.peer_recv_arena;
    out[5] = l.expected_recv_base;
    out[6] = l.arena_bytes;
    return 0;
}

/* 测试辅助：返回 IB 工作区和状态缓冲区的设备地址。 */
extern "C" uint64_t mf310p_v2_trial_ib_addr(mf310p_v2_trial_t* s)
{
    return s ? reinterpret_cast<uint64_t>(s->ib) : 0;
}

extern "C" uint64_t mf310p_v2_trial_status_addr(mf310p_v2_trial_t* s)
{
    return s ? reinterpret_cast<uint64_t>(s->status) : 0;
}

/* 测试辅助：读取完整 64B 状态区（8 个字）。 */
extern "C" int mf310p_v2_trial_status_words(
    mf310p_v2_trial_t* s, uint64_t* out8)
{
    if (!s || !out8) return -1;
    return static_cast<int>(aclrtMemcpy(
        out8, 8 * sizeof(uint64_t), s->status, 8 * sizeof(uint64_t),
        ACL_MEMCPY_DEVICE_TO_HOST));
}

namespace {
/* 计算对称区地址的公共逻辑，供 run() 与 diag() 复用。 */
struct SymmetricAddr {
    uint64_t ownC, ownPeerC, peerDst, expectedRecvDst;
};

SymmetricAddr symmetric_addr(const mf310p_layout_t& l, uint32_t rank)
{
    const bool rank0 = rank == 0;
    SymmetricAddr a;
    a.ownC = rank0 ? l.send_arena : l.recv_arena;
    a.ownPeerC = rank0 ? l.recv_arena : l.send_arena;
    a.peerDst = rank0 ? l.peer_segment : l.peer_recv_arena;
    a.expectedRecvDst =
        rank0 ? l.expected_recv_base : l.expected_recv_base - l.arena_bytes;
    return a;
}
} // 匿名命名空间

extern "C" int mf310p_v2_trial_diag(
    mf310p_v2_trial_t* s, uint64_t x, uint64_t w, uint64_t y,
    uint32_t diag, void* stream_ptr)
{
    if (!s || !s->ib_probed || !stream_ptr || !x || !w || !y || diag > 7)
        return -1;
    auto acl_stream = static_cast<aclrtStream>(stream_ptr);
    const SymmetricAddr a = symmetric_addr(s->layout, s->rank);
    const int clear_err = static_cast<int>(aclrtMemsetAsync(
        s->status, kStatusSize, 0, kStatusSize, acl_stream));
    if (clear_err) return clear_err;
    if (diag == 0 || diag == 2) {
        auto launch = diag == 0 ? mf310p_v2_diag_mm_async
                                : mf310p_v2_diag_v1mm_async;
        for (int i = 0; i < 2; ++i) {
            const int rc = launch(x, w, y, s->chunk_rows, 0, acl_stream);
            if (rc) return rc;
        }
        const int werr = static_cast<int>(aclrtSynchronizeStream(acl_stream));
        if (werr) return werr;
        return launch(x, w, y, s->chunk_rows, 1, acl_stream);
    }
    if (diag == 7) {
        const SymmetricAddr a7 = symmetric_addr(s->layout, s->rank);
        auto l7 = [&](uint32_t en) -> int {
            return mf310p_v2_diag_loop_async(
                s->layout.pool_base, x, w, y, a7.ownC, a7.ownPeerC, a7.peerDst,
                a7.expectedRecvDst, reinterpret_cast<uint64_t>(s->ib),
                reinterpret_cast<uint64_t>(s->status), s->layout.ack_slot,
                s->layout.peer_ack_slot, s->layout.expected_credit_dst,
                s->chunk_rows, 1, 0, en, acl_stream);
        };
        for (int i = 0; i < 2; ++i) {
            const int rc = l7(0);
            if (rc) return rc;
        }
        const int werr7 = static_cast<int>(aclrtSynchronizeStream(acl_stream));
        if (werr7) return werr7;
        return l7(1);
    }
    if (diag == 6) {
        const SymmetricAddr a6 = symmetric_addr(s->layout, s->rank);
        auto l6 = [&](uint32_t en) -> int {
            return mf310p_v2_diag_ibdone16_async(
                s->layout.pool_base, x, w, y, a6.ownC, a6.ownPeerC, a6.peerDst,
                a6.expectedRecvDst, reinterpret_cast<uint64_t>(s->ib),
                reinterpret_cast<uint64_t>(s->status), s->layout.ack_slot,
                s->layout.peer_ack_slot, s->layout.expected_credit_dst,
                s->chunk_rows, 1, 0, en, acl_stream);
        };
        for (int i = 0; i < 2; ++i) {
            const int rc = l6(0);
            if (rc) return rc;
        }
        const int werr6 = static_cast<int>(aclrtSynchronizeStream(acl_stream));
        if (werr6) return werr6;
        return l6(1);
    }
    if (diag == 5) {
        /* 通过正式启动入口执行真正的 V2TrialKernel，使用与 IbDone 诊断相同的单 Chunk 串行形状。 */
        const SymmetricAddr a5 = symmetric_addr(s->layout, s->rank);
        for (int i = 0; i < 2; ++i) {
            const int rc = mf310p_v2_trial_launch_async(
                s->layout.pool_base, x, w, y, a5.ownC, a5.ownPeerC,
                a5.peerDst, a5.expectedRecvDst,
                reinterpret_cast<uint64_t>(s->ib),
                reinterpret_cast<uint64_t>(s->status),
                s->layout.ack_slot, s->layout.peer_ack_slot,
                s->layout.expected_credit_dst, s->chunk_rows, 1, 0, 0,
                acl_stream);
            if (rc) return rc;
        }
        const int werr5 = static_cast<int>(aclrtSynchronizeStream(acl_stream));
        if (werr5) return werr5;
        return mf310p_v2_trial_launch_async(
            s->layout.pool_base, x, w, y, a5.ownC, a5.ownPeerC, a5.peerDst,
            a5.expectedRecvDst, reinterpret_cast<uint64_t>(s->ib),
            reinterpret_cast<uint64_t>(s->status), s->layout.ack_slot,
            s->layout.peer_ack_slot, s->layout.expected_credit_dst,
            s->chunk_rows, 1, 0, 1, acl_stream);
    }
    if (diag == 1 || diag == 3 || diag == 4) {
        auto warm = [&](int rep) -> int {
            if (diag == 1)
                return mf310p_v2_diag_noib_async(
                    s->layout.pool_base, x, w, y, a.ownC, a.ownPeerC,
                    a.peerDst, a.expectedRecvDst,
                    reinterpret_cast<uint64_t>(s->status), s->chunk_rows,
                    rep, acl_stream);
            if (diag == 3)
                return mf310p_v2_diag_ib_async(
                    s->layout.pool_base, x, w, y, a.ownC, a.ownPeerC,
                    a.peerDst, a.expectedRecvDst,
                    reinterpret_cast<uint64_t>(s->ib),
                    reinterpret_cast<uint64_t>(s->status), s->chunk_rows,
                    rep, acl_stream);
            return mf310p_v2_diag_ibdone_async(
                s->layout.pool_base, x, w, y, a.ownC, a.ownPeerC, a.peerDst,
                a.expectedRecvDst, reinterpret_cast<uint64_t>(s->ib),
                reinterpret_cast<uint64_t>(s->status), s->layout.ack_slot,
                s->layout.peer_ack_slot, s->layout.expected_credit_dst,
                s->chunk_rows, rep, acl_stream);
        };
        for (int i = 0; i < 2; ++i) {
            const int rc = warm(0);
            if (rc) return rc;
        }
        const int werr = static_cast<int>(aclrtSynchronizeStream(acl_stream));
        if (werr) return werr;
        return warm(1);
    }
    return -1;
}

