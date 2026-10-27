// 正式 T0 Host 运行时：建池、IB 自检、融合发射、协议状态和销毁。
#include "mf310p_v2_trial_internal.h"
#include <cstdlib>
#include <new>

extern "C" int mf310p_v2_trial_launch_async(
    uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
    uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
    uint64_t, uint32_t, uint32_t, uint32_t, uint32_t, aclrtStream);
extern "C" int mf310p_v2_ib_probe_async(
    uint64_t, uint64_t, uint32_t, uint32_t, aclrtStream);
extern "C" int mf310p_v2_ib_fan_probe_async(
    uint64_t, uint64_t, uint32_t, uint32_t, aclrtStream);
extern "C" uint32_t mf310p_v2_trial_ib_workspace_bytes();

namespace {
void free_allocations(mf310p_v2_trial* state)
{
    if (state->ib) aclrtFree(state->ib);
    if (state->status) aclrtFree(state->status);
    if (state->probe_result) aclrtFree(state->probe_result);
    if (state->v1_pool) mf310p_destroy(state->v1_pool);
}
}
extern "C" int mf310p_v2_trial_create(
    int rank, const char* store_url, uint32_t chunk_rows, mf310p_v2_trial_t** out)
{
    if (!out || !store_url || (rank != 0 && rank != 1) ||
        (chunk_rows != 256 && chunk_rows != 512 && chunk_rows != 1024))
        return -1;
    *out = nullptr;
    auto* s = new (std::nothrow) mf310p_v2_trial();
    if (!s) return -2;
    s->rank = static_cast<uint32_t>(rank);
    s->chunk_rows = chunk_rows;
    int err = mf310p_create(rank, 2, store_url, kPoolSize, kWindowRows,
                           chunk_rows, &s->v1_pool);
    if (!err) err = mf310p_control_barrier(s->v1_pool);
    if (!err) err = mf310p_exchange_geometry(s->v1_pool);
    /* exchange_geometry 完成之后才可以读取最新布局；expected_recv_base 和 expected_credit_dst 是交换过程中填入的对端 GVA，遵循 V1 交换后的布局刷新纪律。 */
    if (!err) err = mf310p_get_layout(s->v1_pool, &s->layout);
    if (!err) {
        err = static_cast<int>(aclrtMalloc(
            &s->ib, mf310p_v2_trial_ib_workspace_bytes(),
            ACL_MEM_MALLOC_HUGE_FIRST));
    }
    if (!err) err = static_cast<int>(aclrtMalloc(
        &s->status, kStatusSize, ACL_MEM_MALLOC_HUGE_FIRST));
    if (!err) err = static_cast<int>(aclrtMalloc(
        &s->probe_result, kProbeResultBytes, ACL_MEM_MALLOC_HUGE_FIRST));
    if (!err) err = static_cast<int>(aclrtMemset(
        s->ib, mf310p_v2_trial_ib_workspace_bytes(), 0,
        mf310p_v2_trial_ib_workspace_bytes()));
    if (!err) err = static_cast<int>(aclrtMemset(
        s->status, kStatusSize, 0, kStatusSize));
    if (!err) err = static_cast<int>(aclrtMemset(
        s->probe_result, kProbeResultBytes, 0, kProbeResultBytes));
    if (!err) err = static_cast<int>(aclrtMemset(
        reinterpret_cast<void*>(s->layout.ack_slot), 8, 0, 8));
    if (err) {
        free_allocations(s);
        delete s;
        return err;
    }
    *out = s;
    return 0;
}

extern "C" int mf310p_v2_trial_ib_probe(
    mf310p_v2_trial_t* s, uint32_t repetitions, void* stream)
{
    if (!s || !stream || repetitions == 0 || repetitions > 100000)
        return -1;
    aclrtStream acl_stream = static_cast<aclrtStream>(stream);
    int err = mf310p_v2_ib_probe_async(
        reinterpret_cast<uint64_t>(s->ib),
        reinterpret_cast<uint64_t>(s->probe_result), 0, 0, acl_stream);
    if (!err) err = mf310p_v2_ib_probe_async(
        reinterpret_cast<uint64_t>(s->ib),
        reinterpret_cast<uint64_t>(s->probe_result), 0, 0, acl_stream);
    if (!err) err = mf310p_v2_ib_probe_async(
        reinterpret_cast<uint64_t>(s->ib),
        reinterpret_cast<uint64_t>(s->probe_result),
        repetitions, 1, acl_stream);
    if (!err) err = static_cast<int>(aclrtSynchronizeStream(acl_stream));
    uint32_t slots[8 * 16] = {};  /* 8 个核各占 64B，即每核 16 个 uint32。 */
    if (!err) err = static_cast<int>(aclrtMemcpy(
        slots, sizeof(slots), s->probe_result, sizeof(slots),
        ACL_MEMCPY_DEVICE_TO_HOST));
    if (!err && (slots[0] != repetitions || slots[16] != repetitions))
        return -2;
    // 真实 MM Kernel 执行 8→1 就绪、1→7 释放、7→1 完成会合。
    // 启用前需单独对该设备符号进行两次预热。
    if (!err) err = mf310p_v2_ib_fan_probe_async(
        reinterpret_cast<uint64_t>(s->ib),
        reinterpret_cast<uint64_t>(s->probe_result), 0, 0, acl_stream);
    if (!err) err = mf310p_v2_ib_fan_probe_async(
        reinterpret_cast<uint64_t>(s->ib),
        reinterpret_cast<uint64_t>(s->probe_result), 0, 0, acl_stream);
    if (!err) err = mf310p_v2_ib_fan_probe_async(
        reinterpret_cast<uint64_t>(s->ib),
        reinterpret_cast<uint64_t>(s->probe_result),
        repetitions, 1, acl_stream);
    if (!err) err = static_cast<int>(aclrtSynchronizeStream(acl_stream));
    if (!err) err = static_cast<int>(aclrtMemcpy(
        slots, sizeof(slots), s->probe_result, sizeof(slots),
        ACL_MEMCPY_DEVICE_TO_HOST));
    bool all_ok = err == 0;
    for (uint32_t c = 0; c < 8; ++c) {
        all_ok = all_ok && slots[16 * c] == repetitions;
    }
    s->ib_probed = all_ok;
    return err ? err : (all_ok ? 0 : -3);
}

extern "C" int mf310p_v2_trial_run(
    mf310p_v2_trial_t* s, uint64_t x, uint64_t w, uint64_t y,
    uint32_t m, uint32_t overlap, void* stream)
{
    if (!s || !s->ib_probed || !stream || !x || !w || !y ||
        !m || m > kWindowRows || m % s->chunk_rows || overlap > 1) {
        return -1;
    }
    auto acl_stream = static_cast<aclrtStream>(stream);
    const auto& l = s->layout;
    const bool rank0 = s->rank == 0;
    // 逻辑对称输出：C0 从偏移 0 开始，C1 从 arena_bytes 开始。
    const uint64_t ownC = rank0 ? l.send_arena : l.recv_arena;
    const uint64_t peerC = rank0 ? l.recv_arena : l.send_arena;
    const uint64_t peerDst = rank0 ? l.peer_segment : l.peer_recv_arena;
    const uint64_t expectedRecvDst =
        rank0 ? l.expected_recv_base : l.expected_recv_base - l.arena_bytes;
    /* T0 BUG#3 首写保护及 2026-10-17 实测补充：建池后新分配缓冲区的首次 MTE3 输出可能静默丢失；只有 aclrtSynchronizeDevice 能缓解，Stream 同步和 enable=0 预热无效。此外，V2TrialKernel 的首次真实发射即使写入长期预热的 y 也可能丢失结果，因此每个符号首次真实执行也需设备同步。建池后全设备同步可能耗时 0～15 秒，双 Rank 偏斜曾引发 AICore 等待超时。T1 小 M 路径使用开销较小的 Stream 级守护。 */
    const bool first_real_symbol_launch = !s->trial_symbol_launched;
    if (!s->trial_symbol_warmed) {
        for (int i = 0; i < 2; ++i) {
            const int rc = mf310p_v2_trial_launch_async(
                l.pool_base, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                s->chunk_rows, 1, 0, 0, acl_stream);
            if (rc != 0) return rc;
        }
        s->trial_symbol_warmed = true;
    }
    bool output_warmed = false;
    for (uint64_t known : s->trial_warmed_ys) {
        if (known == y) {
            output_warmed = true;
            break;
        }
    }
    if (getenv("MF310P_V2_NOWARMUP") == nullptr &&
        (first_real_symbol_launch || !output_warmed)) {
        const int sync_err = static_cast<int>(aclrtSynchronizeDevice());
        if (sync_err) return sync_err;
        // 服务为该上下文的整个生命周期固定输出分配。
        // 试验调用方也必须保证已标记为预热的地址保持存活，
        // 释放后重新分配到相同地址的情况不能
        // 由该地址缓存识别。
        if (!output_warmed) {
            s->trial_warmed_ys[s->trial_warmed_y_next] = y;
            s->trial_warmed_y_next =
                (s->trial_warmed_y_next + 1) % 64;
        }
    }
    s->warmed_y = y;
    s->trial_symbol_launched = true;
    const int clear_err = static_cast<int>(aclrtMemsetAsync(
        s->status, kStatusSize, 0, kStatusSize, acl_stream));
    if (clear_err) return clear_err;
    return mf310p_v2_trial_launch_async(
        l.pool_base, x, w, y, ownC, peerC, peerDst, expectedRecvDst,
        reinterpret_cast<uint64_t>(s->ib),
        reinterpret_cast<uint64_t>(s->status),
        l.ack_slot, l.peer_ack_slot, l.expected_credit_dst,
        s->chunk_rows, m / s->chunk_rows, overlap, 1, acl_stream);
}


extern "C" int mf310p_v2_trial_check_status(
    mf310p_v2_trial_t* s, uint64_t* status)
{
    if (!s || !status) return -1;
    return static_cast<int>(aclrtMemcpy(
        status, sizeof(*status), s->status, sizeof(*status),
        ACL_MEMCPY_DEVICE_TO_HOST));
}

extern "C" int mf310p_v2_trial_destroy(mf310p_v2_trial_t* s)
{
    if (!s) return 0;
    free_allocations(s);
    delete s;
    return 0;
}
