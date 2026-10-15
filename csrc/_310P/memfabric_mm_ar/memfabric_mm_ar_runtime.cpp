/*
 * 310P3 TP=2 MemFabric runtime for the ABI v7 mm_ar batch pipeline.
 *
 * One wave owns a bounded send/recv arena. Each batch is a baseM-aligned
 * 8-core cooperative MM. Core0 sends the completed batch, then later batches
 * are enqueued before wait/reduce of earlier batches so SDMA can overlap both
 * subsequent MM and earlier reduction. Wave credit remains the arena reuse
 * guard; MemFabric transport internals stay opaque.
 */
#include <ATen/ATen.h>
#include <c10/util/Exception.h>
#include <c10/util/string_view.h>
#include <torch/extension.h>

#ifdef ASCEND_PLATFORM_310P
#include <acl/acl.h>
#include <acl/acl_rt.h>
#include <torch_npu/csrc/aten/common/from_blob.h>
#include <torch_npu/csrc/core/npu/NPUStream.h>
#include <torch_npu/csrc/core/npu/NPUGraphsUtils.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

#include "memfabric310p_adapter_api.h"

namespace vllm_ascend {

/* Defined below (both build variants); forward-declared for the atexit hook. */
void memfabric_mm_ar_shutdown();

#ifdef VLLM_ASCEND_ENABLE_310P_MEMFABRIC_MM_AR
namespace {

constexpr int64_t kMmArWidth = 2048;
constexpr uint32_t kBaseM = 256;
constexpr uint32_t kMaxArenaRows = 8192;
/*
 * Eager ready-cell generations start here: every graph capture bakes the
 * small wave-invariant `batch + 1` (1..VLLM_ASCEND_MF310P_MAX_BATCHES) into
 * the captured launch, so eager values must live in a disjoint range.
 */
constexpr uint32_t kEagerGenerationBase = 0x40000000u;
constexpr uint64_t kMmArRowBytes =
    static_cast<uint64_t>(kMmArWidth) * sizeof(at::Half);
constexpr uint64_t kDefaultLocalPoolBytes = 96ULL * 1024ULL * 1024ULL;
constexpr uint64_t kPoolHeadroomBytes = 1ULL * 1024ULL * 1024ULL;
constexpr const char* kDefaultStoreUrl = "tcp://127.0.0.1:8581";
/*
 * Small-M exact path ceiling: M below min(batch_m, this) skips the padded
 * 256-row batch pipeline entirely and runs the N-split serial wave (eight
 * per-core 256-column weight slices, template stair {16,32,64,128,256}).
 */
constexpr int64_t kSmallPathMaxM = 256;
constexpr uint64_t kWeightSlicesBytes = 8ULL * 1024ULL * 1024ULL;
constexpr size_t kMaxWeightSliceEntries = 64;

/*
 * Kill switch for the small-M N-split path (default on). "0" restores the
 * padded batch pipeline for every M.
 */
bool small_path_enabled()
{
    static const bool enabled = [] {
        const char* v =
            std::getenv("VLLM_ASCEND_310P_MEMFABRIC_MM_AR_SMALL_M_PATH");
        return v == nullptr || *v == '\0' || *v != '0';
    }();
    return enabled;
}

uint32_t small_template_for(int64_t rows)
{
    if (rows <= 16) return 16;
    if (rows <= 32) return 32;
    if (rows <= 64) return 64;
    if (rows <= 128) return 128;
    return 256;
}

uint64_t parse_u64_env(const char* name, uint64_t fallback)
{
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return fallback;
    }
    errno = 0;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(value, &end, 0);
    TORCH_CHECK(
        errno == 0 && end != value && *end == '\0',
        "Invalid ", name, "=", value);
    return static_cast<uint64_t>(parsed);
}

/*
 * Optional host enqueue tracing. These timestamps are not device execution
 * timings; use the CANN profiler for MM/SDMA overlap measurements.
 */
bool trace_enabled()
{
    static const bool enabled = [] {
        const char* v = std::getenv("VLLM_ASCEND_310P_MEMFABRIC_MM_AR_TRACE");
        return v != nullptr && *v != '\0' && *v != '0';
    }();
    return enabled;
}

int64_t trace_now_us()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void trace_wave(
    int rank,
    int64_t rows,
    uint32_t batches,
    int64_t join_sync_us,
    int64_t join_barrier_us,
    int64_t warmup_us,
    int64_t enqueue_us,
    int64_t prejoin_us)
{
    std::fprintf(
        stderr,
        "[mf310p-trace] rank=%d rows=%lld batches=%u join_sync_us=%lld "
        "join_barrier_us=%lld warmup_us=%lld enqueue_us=%lld prejoin_us=%lld "
        "total_us=%lld\n",
        rank, static_cast<long long>(rows), batches,
        static_cast<long long>(join_sync_us),
        static_cast<long long>(join_barrier_us),
        static_cast<long long>(warmup_us),
        static_cast<long long>(enqueue_us),
        static_cast<long long>(prejoin_us),
        static_cast<long long>(join_sync_us + join_barrier_us + warmup_us +
                               enqueue_us + prejoin_us));
}

struct RuntimeState;

/*
 * Per-weight N-split slice cache: the [2048, 2048] NZ weight re-laid out as
 * eight contiguous per-core [256, 2048] NZ column slices. Weights are
 * process-lifetime in serving, so entries are keyed by device pointer and
 * never evicted; the entry cap bounds pathological callers (falling back to
 * the batch pipeline once exceeded).
 */
struct WeightSlicesEntry {
    uint64_t weight_ptr;
    uint64_t slices;
};

struct RuntimeState {
    std::mutex mutex;
    mf310p_context_t* ctx = nullptr;
    mf310p_layout_t layout{};
    int tp_rank = -1;
    int64_t batch_basem_count = 0;
    uint32_t batch_m = 0;
    bool poisoned = false;
    std::string failure_reason;
    uint64_t producer_scratch = 0; /* device VA, batch_m*2048*2 bytes */
    std::vector<WeightSlicesEntry> weight_slices;
    bool producer_warmed = false;
    bool waiter_warmed = false;
    bool add_warmed = false;
    bool small_warmed = false;
    bool protocol_warmed = false;
    bool protocol_initialized = false;
    aclrtStream eager_stream = nullptr;
    bool graph_capture_seen = false;
    /*
     * Eager ready-cell generation. Eager waves must never reuse a value a
     * previous wave (or a graph capture, which bakes the small constant
     * `batch + 1` into the captured launch) left in a cell, so eager
     * generations live in a disjoint high range and simply increase.
     */
    uint32_t wave_seq = 0;

    ~RuntimeState() = default;
};

RuntimeState& runtime_state()
{
    static RuntimeState state;
    return state;
}

void init_context_locked(
    RuntimeState& state,
    const at::Tensor& x,
    int64_t tp_rank,
    int64_t batch_basem_count)
{
    TORCH_CHECK(
        mf310p_adapter_abi_version() == VLLM_ASCEND_MF310P_ADAPTER_ABI_VERSION,
        "310P MemFabric internal adapter ABI mismatch: expected ",
        VLLM_ASCEND_MF310P_ADAPTER_ABI_VERSION,
        ", got ", mf310p_adapter_abi_version());

    TORCH_CHECK(
        batch_basem_count == 1 || batch_basem_count == 2 ||
            batch_basem_count == 4,
        "batch_basem_count must be one of {1,2,4}, got ",
        batch_basem_count);
    const uint32_t batch_m =
        kBaseM * static_cast<uint32_t>(batch_basem_count);

    if (state.ctx != nullptr) {
        TORCH_CHECK(state.tp_rank == tp_rank,
                    "MemFabric runtime rank changed from ", state.tp_rank,
                    " to ", tp_rank);
        TORCH_CHECK(
            state.batch_basem_count == batch_basem_count,
            "MemFabric runtime batch_basem_count changed from ",
            state.batch_basem_count, " to ", batch_basem_count,
            ". Restart the worker when changing communication batch size.");
        return;
    }

    TORCH_CHECK(tp_rank == 0 || tp_rank == 1,
                "MemFabric runtime only supports TP=2 rank 0/1, got ", tp_rank);

    const uint64_t local_pool_bytes = parse_u64_env(
        "VLLM_ASCEND_310P_MEMFABRIC_LOCAL_BYTES", kDefaultLocalPoolBytes);
    const uint64_t min_bytes =
        kPoolHeadroomBytes +
        2 * static_cast<uint64_t>(batch_m) * kMmArRowBytes + 8;
    TORCH_CHECK(
        local_pool_bytes > min_bytes,
        "VLLM_ASCEND_310P_MEMFABRIC_LOCAL_BYTES=", local_pool_bytes,
        " is too small for one send/recv batch plus pool headroom; need > ",
        min_bytes);

    const uint64_t rows_by_budget =
        (local_pool_bytes - kPoolHeadroomBytes - 8) /
        (2 * kMmArRowBytes);
    uint32_t arena_rows = static_cast<uint32_t>(
        std::min<uint64_t>(rows_by_budget, kMaxArenaRows));
    arena_rows = (arena_rows / batch_m) * batch_m;
    TORCH_CHECK(
        arena_rows >= batch_m,
        "MemFabric arena budget produced arena_rows=", arena_rows,
        " < batch_m=", batch_m);

    const char* store_url = std::getenv("VLLM_ASCEND_310P_MEMFABRIC_STORE_URL");
    if (store_url == nullptr || *store_url == '\0') {
        store_url = kDefaultStoreUrl;
    }

    const int ret = mf310p_create(
        static_cast<int>(tp_rank),
        2,
        store_url,
        local_pool_bytes,
        arena_rows,
        batch_m,
        &state.ctx);
    TORCH_CHECK(ret == 0 && state.ctx != nullptr,
                "mf310p_create failed with ret=", ret);

    const int layout_ret = mf310p_get_layout(state.ctx, &state.layout);
    TORCH_CHECK(layout_ret == 0,
                "mf310p_get_layout failed with ret=", layout_ret);
    TORCH_CHECK(state.layout.batch_m == batch_m,
                "MemFabric returned unexpected batch_m");
    TORCH_CHECK(state.layout.arena_rows == arena_rows,
                "MemFabric returned unexpected arena_rows");
    TORCH_CHECK(
        state.layout.batch_bytes ==
            static_cast<uint64_t>(batch_m) * kMmArRowBytes,
        "MemFabric returned unexpected batch_bytes");
    TORCH_CHECK(
        state.layout.max_batches == arena_rows / batch_m &&
            state.layout.max_batches > 0,
        "MemFabric returned invalid max_batches");
    TORCH_CHECK(state.layout.pool_base != 0,
                "MemFabric returned an invalid pool base");

    state.tp_rank = static_cast<int>(tp_rank);
    state.batch_basem_count = batch_basem_count;
    state.batch_m = batch_m;
    (void)x;

    static bool atexit_registered = false;
    if (!atexit_registered) {
        const int atexit_ret =
            std::atexit([]() { memfabric_mm_ar_shutdown(); });
        TORCH_CHECK(atexit_ret == 0,
                    "failed to register MemFabric shutdown hook");
        atexit_registered = true;
    }
}

void check_runtime_healthy(const RuntimeState& state)
{
    TORCH_CHECK(
        !state.poisoned,
        "310P MemFabric mm_ar runtime is poisoned after a previous failed wave. "
        "Restart both TP workers before reusing it. First failure: ",
        state.failure_reason);
}

/* ---- ABI v7 kernel warmup helpers ---- */

void warm_producer_locked(RuntimeState& state, aclrtStream stream)
{
    if (state.producer_warmed) return;
    for (int i = 0; i < 2; ++i) {
        const int ret = mf310p_warmup_producer_async(
            state.ctx, reinterpret_cast<void*>(stream));
        TORCH_CHECK(ret == 0,
                    "mf310p_warmup_producer_async failed with ret=", ret);
    }
    const aclError sync_ret = aclrtSynchronizeStream(stream);
    TORCH_CHECK(sync_ret == ACL_SUCCESS,
                "producer warmup sync failed, ret=", sync_ret);
    state.producer_warmed = true;
}

/*
 * Warm the small-path kernel symbols. Every N-split template instantiation
 * is a distinct symbol with the silent-no-op first-launch property, so any
 * eager fused call (small or batch) must warm all five templates plus the
 * blocked add before a later small wave - eager or captured - can run.
 */
void warm_small_locked(RuntimeState& state, aclrtStream stream)
{
    if (state.small_warmed) return;
    if (small_path_enabled()) {
        for (int i = 0; i < 2; ++i) {
            const int ret = mf310p_warmup_nsplit_producer_async(
                state.ctx, reinterpret_cast<void*>(stream));
            TORCH_CHECK(ret == 0,
                        "mf310p_warmup_nsplit_producer_async failed with "
                        "ret=",
                        ret);
        }
        for (int i = 0; i < 2; ++i) {
            const int ret = mf310p_warmup_add_blocked_async(
                state.ctx, reinterpret_cast<void*>(stream));
            TORCH_CHECK(ret == 0,
                        "mf310p_warmup_add_blocked_async failed with ret=",
                        ret);
        }
        const aclError sync_ret = aclrtSynchronizeStream(stream);
        TORCH_CHECK(sync_ret == ACL_SUCCESS,
                    "small-path warmup sync failed, ret=", sync_ret);
    }
    state.small_warmed = true;
}

/*
 * Find (or lazily build once per weight) the eight per-core NZ column
 * slices. The build is eight strided device-to-device copies enqueued on
 * the caller's stream, so it is ordered before the producer launch without
 * any extra synchronization. Graph capture must never build slices: the
 * eager warmup iterations before capture_begin do it (same contract as the
 * producer scratch).
 */
uint64_t ensure_weight_slices_locked(
    RuntimeState& state,
    uint64_t weight_ptr,
    aclrtStream stream,
    bool capturing)
{
    for (const auto& entry : state.weight_slices) {
        if (entry.weight_ptr == weight_ptr) {
            return entry.slices;
        }
    }
    TORCH_CHECK(
        !capturing,
        "310P MemFabric small-path weight slices must be built by eager "
        "warmup before ACL Graph capture.");
    TORCH_CHECK(
        state.weight_slices.size() < kMaxWeightSliceEntries,
        "310P MemFabric small-path weight slice cache is full (",
        kMaxWeightSliceEntries,
        " entries, ", kMaxWeightSliceEntries * kWeightSlicesBytes / (1024 * 1024),
        " MiB). Set VLLM_ASCEND_310P_MEMFABRIC_MM_AR_SMALL_M_PATH=0 to fall "
        "back to the batch pipeline.");

    void* slices = nullptr;
    const aclError malloc_ret = aclrtMalloc(
        &slices, kWeightSlicesBytes, ACL_MEM_MALLOC_HUGE_FIRST);
    TORCH_CHECK(malloc_ret == ACL_SUCCESS && slices != nullptr,
                "weight slices aclrtMalloc failed, ret=", malloc_ret);
    const int build_ret = mf310p_build_weight_slices(
        state.ctx, weight_ptr, reinterpret_cast<uint64_t>(slices),
        reinterpret_cast<void*>(stream));
    TORCH_CHECK(build_ret == 0,
                "mf310p_build_weight_slices failed with ret=", build_ret);
    state.weight_slices.push_back({weight_ptr,
                                   reinterpret_cast<uint64_t>(slices)});
    return reinterpret_cast<uint64_t>(slices);
}

void warm_waiter_locked(RuntimeState& state, aclrtStream stream)
{
    if (state.waiter_warmed) return;
    for (int i = 0; i < 2; ++i) {
        const int ret = mf310p_warmup_waiter_async(
            state.ctx, reinterpret_cast<void*>(stream));
        TORCH_CHECK(ret == 0,
                    "mf310p_warmup_waiter_async failed with ret=", ret);
    }
    if (!state.add_warmed) {
        for (int i = 0; i < 2; ++i) {
            const int ret = mf310p_warmup_add_async(
                state.ctx, reinterpret_cast<void*>(stream));
            TORCH_CHECK(ret == 0,
                        "mf310p_warmup_add_async failed with ret=", ret);
        }
        state.add_warmed = true;
    }
    if (!state.protocol_warmed) {
        for (int i = 0; i < 2; ++i) {
            const int ret = mf310p_warmup_protocol_async(
                state.ctx, reinterpret_cast<void*>(stream));
            TORCH_CHECK(ret == 0,
                        "mf310p_warmup_protocol_async failed with ret=", ret);
        }
        state.protocol_warmed = true;
    }
    const aclError sync_ret = aclrtSynchronizeStream(stream);
    TORCH_CHECK(sync_ret == ACL_SUCCESS,
                "waiter/add/protocol warmup sync failed, ret=", sync_ret);
    state.waiter_warmed = true;
}

bool is_current_stream_capturing()
{
    return c10_npu::currentStreamCaptureStatusMayInitCtx() ==
           c10_npu::CaptureStatus::Active;
}

void validate_execution_stream_locked(
    RuntimeState& state,
    aclrtStream stream,
    bool capturing)
{
    TORCH_CHECK(stream != nullptr, "MemFabric requires a valid ACL stream");

    /*
     * torch.npu.graph() intentionally switches to an internal non-default
     * capture stream. The graph context synchronizes the device before that
     * transition, so it is a framework-managed quiescent stream handoff, not
     * arbitrary concurrent eager use. Keep the eager contract strict while
     * allowing that capture side stream.
     */
    if (capturing) {
        state.graph_capture_seen = true;
        return;
    }

    if (state.eager_stream == nullptr) {
        state.eager_stream = stream;
        return;
    }
    TORCH_CHECK(
        state.eager_stream == stream,
        "310P MemFabric mm_ar eager execution is single-stream by contract. "
        "The first eager call used stream=",
        reinterpret_cast<void*>(state.eager_stream),
        ", current stream=",
        reinterpret_cast<void*>(stream),
        ". ACL Graph capture side streams are handled separately.");
}

void require_capture_ready_locked(const RuntimeState& state)
{
    TORCH_CHECK(
        state.ctx != nullptr && state.protocol_initialized &&
            state.producer_scratch != 0 && state.producer_warmed &&
            state.waiter_warmed && state.add_warmed &&
            (!small_path_enabled() || state.small_warmed) &&
            state.protocol_warmed,
        "310P MemFabric mm_ar entered ACL Graph capture before runtime "
        "initialization/warmup completed. Run the normal eager warmup/profile "
        "path before graph capture.");
}

void initialize_protocol_locked(RuntimeState& state, aclrtStream stream)
{
    if (state.protocol_initialized) return;

    warm_waiter_locked(state, stream);

    int ret = mf310p_prepare_wave_async(
        state.ctx, 1u, reinterpret_cast<void*>(stream));
    TORCH_CHECK(ret == 0,
                "mf310p_prepare_wave_async(init) failed with ret=", ret);

    ret = mf310p_control_barrier(state.ctx);
    TORCH_CHECK(ret == 0,
                "mf310p_control_barrier(init) failed with ret=", ret);

    ret = mf310p_exchange_geometry(state.ctx);
    TORCH_CHECK(ret == 0,
                "mf310p_exchange_geometry failed with ret=", ret);
    const int geo_ret = mf310p_get_layout(state.ctx, &state.layout);
    TORCH_CHECK(geo_ret == 0,
                "mf310p_get_layout(geometry refresh) failed with ret=",
                geo_ret);

    ret = mf310p_init_credit_async(
        state.ctx, reinterpret_cast<void*>(stream));
    TORCH_CHECK(ret == 0,
                "mf310p_init_credit_async failed with ret=", ret);

    const aclError sync_ret = aclrtSynchronizeStream(stream);
    TORCH_CHECK(sync_ret == ACL_SUCCESS,
                "initial MemFabric credit sync failed, ret=", sync_ret);
    state.protocol_initialized = true;
}

} // namespace

void memfabric_mm_ar_shutdown()
{
#ifdef VLLM_ASCEND_ENABLE_310P_MEMFABRIC_MM_AR
    RuntimeState& state = runtime_state();
    std::lock_guard<std::mutex> guard(state.mutex);
    if (state.ctx == nullptr) return;

    if (state.graph_capture_seen) {
        /*
         * NPUGraph replay may execute the captured model on whichever stream
         * is current at replay time, and replay bypasses this host function.
         * We therefore cannot prove all replay streams quiescent here without
         * a device-wide synchronization (which this integration deliberately
         * avoids). Keep the external pool process-lifetime instead of risking
         * destruction with in-flight graph work.
         */
        std::fprintf(
            stderr,
            "[mf310p] graph-used context kept process-lifetime; skip unsafe "
            "MemFabric destroy\n");
        return;
    }

    if (state.eager_stream != nullptr) {
        /* The last healthy wave ends with an asynchronous credit signal.
         * Enqueue public quiet before the final stream sync so every local
         * MemFabric request, including that credit, is proven landed before
         * pool destruction. */
        const int quiet_ret = mf310p_quiet_async(
            state.ctx, reinterpret_cast<void*>(state.eager_stream));
        if (quiet_ret != 0) {
            state.poisoned = true;
            if (state.failure_reason.empty()) {
                state.failure_reason =
                    "shutdown quiet enqueue failed; MemFabric resources "
                    "intentionally leaked until process exit";
            }
            std::fprintf(
                stderr,
                "[mf310p] skip unsafe destroy after quiet enqueue ret=%d\n",
                quiet_ret);
            return;
        }

        const aclError sync_ret = aclrtSynchronizeStream(state.eager_stream);
        if (sync_ret != ACL_SUCCESS) {
            state.poisoned = true;
            if (state.failure_reason.empty()) {
                state.failure_reason =
                    "shutdown stream synchronization failed; MemFabric "
                    "resources intentionally leaked until process exit";
            }
            std::fprintf(
                stderr,
                "[mf310p] skip unsafe destroy after stream sync failure ret=%d\n",
                static_cast<int>(sync_ret));
            return;
        }
    }

    if (state.producer_scratch != 0) {
        (void)aclrtFree(reinterpret_cast<void*>(state.producer_scratch));
        state.producer_scratch = 0;
    }
    for (const auto& entry : state.weight_slices) {
        (void)aclrtFree(reinterpret_cast<void*>(entry.slices));
    }
    state.weight_slices.clear();
    const int destroy_ret = mf310p_destroy(state.ctx);
    if (destroy_ret != 0) {
        std::fprintf(stderr, "[mf310p] mf310p_destroy ret=%d\n", destroy_ret);
    }
    state.ctx = nullptr;
    state.eager_stream = nullptr;
    state.graph_capture_seen = false;
    state.protocol_initialized = false;
    state.protocol_warmed = false;
    state.waiter_warmed = false;
    state.add_warmed = false;
    state.small_warmed = false;
    state.producer_warmed = false;
    state.batch_basem_count = 0;
    state.batch_m = 0;
#endif
}

/*
 * Debug-only application snapshot. MemFabric internals are intentionally
 * opaque: report only public pool geometry, vLLM-owned arenas and data words.
 */
at::Tensor memfabric_mm_ar_debug_snapshot()
{
    constexpr int64_t kWords = 24;
    at::Tensor out = at::zeros({kWords}, at::TensorOptions().dtype(at::kLong));
    int64_t* w = out.data_ptr<int64_t>();

#ifdef VLLM_ASCEND_ENABLE_310P_MEMFABRIC_MM_AR
    RuntimeState& state = runtime_state();
    std::lock_guard<std::mutex> guard(state.mutex);
    if (state.ctx == nullptr) {
        return out;
    }
    const mf310p_layout_t& layout = state.layout;

    auto arena_word = [](uint64_t addr, bool* ok) -> uint64_t {
        uint64_t v = 0;
        const aclError ret = aclrtMemcpy(
            &v, sizeof(v), reinterpret_cast<void*>(addr), sizeof(v),
            ACL_MEMCPY_DEVICE_TO_HOST);
        if (ret != ACL_SUCCESS && ok != nullptr) {
            *ok = false;
        }
        return v;
    };

    bool own_ok = true;
    bool peer_ok = true;
    w[0] = 0x4D465037; /* MFP7 */
    w[1] = state.tp_rank;
    w[2] = static_cast<int64_t>(layout.pool_base);
    w[3] = static_cast<int64_t>(layout.symmetric_size);
    w[4] = static_cast<int64_t>(layout.local_size);
    w[5] = static_cast<int64_t>(layout.send_arena);
    w[6] = static_cast<int64_t>(layout.recv_arena);
    w[7] = static_cast<int64_t>(layout.peer_recv_arena);
    w[8] = static_cast<int64_t>(layout.ack_slot);
    w[9] = static_cast<int64_t>(layout.peer_ack_slot);
    w[10] = static_cast<int64_t>(layout.arena_bytes);
    w[11] = static_cast<int64_t>(layout.batch_bytes);
    w[12] = state.protocol_initialized ? 1 : 0;
    w[13] = static_cast<int64_t>(arena_word(layout.send_arena, &own_ok));
    w[14] = static_cast<int64_t>(arena_word(layout.recv_arena, &own_ok));
    w[15] = static_cast<int64_t>(arena_word(layout.peer_recv_arena, &peer_ok));
    w[16] = own_ok ? 1 : 0;
    w[17] = peer_ok ? 1 : 0;
    uint64_t protocol_status = 0;
    const int status_ret =
        mf310p_debug_protocol_status(state.ctx, &protocol_status);
    w[18] = static_cast<int64_t>(protocol_status);
    w[19] = status_ret == 0 ? 1 : 0;
    w[20] = static_cast<int64_t>(layout.expected_credit_dst);
    w[21] = static_cast<int64_t>(layout.expected_recv_base);
    w[22] = layout.arena_rows;
    w[23] = layout.batch_m;
#endif
    return out;
}

at::Tensor memfabric_mm_ar_allreduce_impl(
    const at::Tensor& x,
    const at::Tensor& weight,
    int64_t tp_rank,
    int64_t batch_basem_count)
{
    const int64_t t_entry = trace_enabled() ? trace_now_us() : 0;
    TORCH_CHECK(
        batch_basem_count == 1 || batch_basem_count == 2 ||
            batch_basem_count == 4,
        "direct MemFabric mm_ar requires batch_basem_count in {1,2,4}, got ",
        batch_basem_count);
    TORCH_CHECK(x.is_contiguous(),
                "direct MemFabric mm_ar requires contiguous x, got strides ",
                x.strides());
    TORCH_CHECK(weight.is_contiguous(),
                "direct MemFabric mm_ar requires contiguous weight");

    const int64_t num_tokens = x.size(0);
    if (num_tokens == 0) {
        return at::empty({0, kMmArWidth}, x.options());
    }

    RuntimeState& state = runtime_state();
    std::lock_guard<std::mutex> guard(state.mutex);
    check_runtime_healthy(state);

    const auto npu_stream = c10_npu::getCurrentNPUStream();
    aclrtStream stream = npu_stream.stream();
    const bool capturing = is_current_stream_capturing();

    if (capturing) {
        TORCH_CHECK(
            state.ctx != nullptr,
            "310P MemFabric context must be created by eager warmup before "
            "ACL Graph capture.");
    }
    init_context_locked(state, x, tp_rank, batch_basem_count);
    validate_execution_stream_locked(state, stream, capturing);

    const uint64_t scratch_bytes =
        static_cast<uint64_t>(state.batch_m) * kMmArRowBytes;
    if (state.producer_scratch == 0) {
        TORCH_CHECK(
            !capturing,
            "310P MemFabric producer scratch must be allocated by eager "
            "warmup before ACL Graph capture.");
        void* scratch = nullptr;
        const aclError malloc_ret = aclrtMalloc(
            &scratch, scratch_bytes, ACL_MEM_MALLOC_HUGE_FIRST);
        TORCH_CHECK(malloc_ret == ACL_SUCCESS && scratch != nullptr,
                    "producer scratch aclrtMalloc failed, ret=", malloc_ret);
        /*
         * Zero the tail padding region exactly once, on the same stream as
         * every later use. Tail batches below only overwrite their valid
         * prefix; rows past valid_rows keep this zero (or stale rows from a
         * previous, larger tail). Stale padded rows are computed and
         * exchanged but never reduced: add_batch_async consumes exactly
         * valid_rows rows, and matmul output rows depend only on their own
         * input row, so garbage stays row-local. This removes the measured
         * ~0.6 ms per-tail 2 MiB engine memset that dominated small-M
         * latency now that every M takes the fused path.
         */
        const aclError zero_ret = aclrtMemsetAsync(
            scratch, scratch_bytes, 0, scratch_bytes, stream);
        TORCH_CHECK(zero_ret == ACL_SUCCESS,
                    "producer scratch init zero failed, ret=", zero_ret);
        state.producer_scratch = reinterpret_cast<uint64_t>(scratch);
    }

    at::Tensor output =
        at::empty({num_tokens, kMmArWidth}, x.options());
    const uint64_t weight_ptr =
        reinterpret_cast<uint64_t>(weight.const_data_ptr());

    if (capturing) {
        require_capture_ready_locked(state);
    } else {
        warm_producer_locked(state, stream);
        warm_small_locked(state, stream);
        initialize_protocol_locked(state, stream);
    }

    /*
     * Small-M exact path: M < min(batch_m, 256) skips the padded batch
     * pipeline and runs one serial N-split wave against the per-weight
     * column slices. Routing is identical for eager and captured waves: the
     * template/length/generation are all derived from M, which graph
     * captures keep fixed per captured graph.
     */
    const bool small_path = small_path_enabled() &&
        num_tokens < std::min<int64_t>(state.batch_m, kSmallPathMaxM);
    if (small_path) {
        try {
            const bool small_trace = trace_enabled();
            const int64_t t_enqueue0 = small_trace ? trace_now_us() : 0;
            const uint32_t valid_rows = static_cast<uint32_t>(num_tokens);
            const uint32_t t_rows = small_template_for(num_tokens);

            int ret = mf310p_prepare_wave_async(
                state.ctx, capturing ? 1u : 0u,
                reinterpret_cast<void*>(stream));
            TORCH_CHECK(ret == 0,
                        "mf310p_prepare_wave_async failed with ret=", ret);
            ret = mf310p_gate_async(
                state.ctx, reinterpret_cast<void*>(stream));
            TORCH_CHECK(ret == 0,
                        "mf310p_gate_async failed with ret=", ret);

            const uint64_t slices = ensure_weight_slices_locked(
                state, weight_ptr, stream, capturing);

            /*
             * Stage exactly the M valid rows into the once-zeroed scratch
             * shared with the batch tail path (same stream, never
             * concurrent). Rows [M, T) keep zeros/stale data: computed and
             * exchanged but never reduced by the blocked add.
             */
            const aclError copy_ret = aclrtMemcpyAsync(
                reinterpret_cast<void*>(state.producer_scratch),
                scratch_bytes,
                x.const_data_ptr(),
                static_cast<uint64_t>(valid_rows) * kMmArRowBytes,
                ACL_MEMCPY_DEVICE_TO_DEVICE,
                stream);
            TORCH_CHECK(copy_ret == ACL_SUCCESS,
                        "small-path staging copy failed, ret=", copy_ret);

            uint32_t generation = 0;
            if (!capturing) {
                ++state.wave_seq;
                if (state.wave_seq < kEagerGenerationBase ||
                    state.wave_seq == 0u) {
                    state.wave_seq = kEagerGenerationBase;
                }
                generation = state.wave_seq;
            } else {
                /* Wave-invariant batch-0 generation, cleared in-stream. */
                generation = 1;
            }

            ret = mf310p_small_producer_async(
                state.ctx,
                state.producer_scratch,
                slices,
                valid_rows,
                t_rows,
                generation,
                reinterpret_cast<void*>(stream));
            TORCH_CHECK(
                ret == 0,
                "mf310p_small_producer_async failed with ret=", ret);
            ret = mf310p_wait_small_async(
                state.ctx, t_rows, reinterpret_cast<void*>(stream));
            TORCH_CHECK(
                ret == 0, "mf310p_wait_small_async failed with ret=", ret);
            ret = mf310p_add_small_async(
                state.ctx,
                reinterpret_cast<uint64_t>(output.mutable_data_ptr()),
                valid_rows,
                t_rows,
                reinterpret_cast<void*>(stream));
            TORCH_CHECK(
                ret == 0, "mf310p_add_small_async failed with ret=", ret);
            ret = mf310p_quiet_async(
                state.ctx, reinterpret_cast<void*>(stream));
            TORCH_CHECK(ret == 0,
                        "mf310p_quiet_async failed with ret=", ret);
            ret = mf310p_ack_async(
                state.ctx, reinterpret_cast<void*>(stream));
            TORCH_CHECK(ret == 0, "mf310p_ack_async failed with ret=", ret);

            if (small_trace) {
                trace_wave(
                    state.tp_rank,
                    num_tokens,
                    0 /* batches=0 marks the small N-split wave */,
                    0,
                    0,
                    0,
                    trace_now_us() - t_enqueue0,
                    0);
            }
        } catch (const std::exception& exc) {
            state.poisoned = true;
            if (state.failure_reason.empty()) {
                state.failure_reason = exc.what();
            }
            throw;
        }
        return output;
    }

    const int64_t max_rows_per_wave = state.layout.arena_rows;
    for (int64_t wave_start = 0; wave_start < num_tokens;
         wave_start += max_rows_per_wave) {
        const int64_t wave_rows =
            std::min<int64_t>(max_rows_per_wave, num_tokens - wave_start);
        const uint32_t batches = static_cast<uint32_t>(
            (wave_rows + state.batch_m - 1) / state.batch_m);
        TORCH_CHECK(
            batches > 0 && batches <= state.layout.max_batches,
            "invalid batch count ", batches, " for max_batches=",
            state.layout.max_batches);

        try {
            const int64_t t_warm0 = trace_now_us();
            if (capturing) {
                require_capture_ready_locked(state);
            }
            const int64_t t_warm = trace_now_us();

            const int64_t t_join0 = t_warm;
            /*
             * Eager waves skip the 32 KiB ready-cell clear: their monotonic
             * generation cannot match a stale cell. Graph captures keep the
             * clear (their generation is the wave-invariant batch + 1) and
             * bake it into the captured stream.
             */
            int ret = mf310p_prepare_wave_async(
                state.ctx, capturing ? 1u : 0u, reinterpret_cast<void*>(stream));
            TORCH_CHECK(ret == 0,
                        "mf310p_prepare_wave_async failed with ret=", ret);
            ret = mf310p_gate_async(
                state.ctx, reinterpret_cast<void*>(stream));
            TORCH_CHECK(ret == 0,
                        "mf310p_gate_async failed with ret=", ret);
            const int64_t t_join2 = trace_now_us();

            const uint64_t x_wave =
                reinterpret_cast<uint64_t>(x.const_data_ptr()) +
                static_cast<uint64_t>(wave_start) * kMmArRowBytes;

            auto valid_rows_for = [&](uint32_t batch) -> uint32_t {
                const int64_t batch_begin =
                    static_cast<int64_t>(batch) * state.batch_m;
                return static_cast<uint32_t>(
                    std::min<int64_t>(
                        state.batch_m, wave_rows - batch_begin));
            };

            /*
             * Ready-cell generation for this wave: graph captures bake the
             * wave-invariant `batch + 1` (cleared inside the captured
             * stream); eager waves use a monotonic counter from a high
             * range disjoint from every captured value, which makes the
             * 32 KiB cell clear unnecessary (see prepare_wave above).
             */
            uint32_t wave_generation = 0;
            if (!capturing) {
                ++state.wave_seq;
                if (state.wave_seq < kEagerGenerationBase ||
                    state.wave_seq == 0u) {
                    state.wave_seq = kEagerGenerationBase;
                }
                wave_generation = state.wave_seq;
            }

            auto enqueue_producer = [&](uint32_t batch) {
                const uint32_t valid_rows = valid_rows_for(batch);
                uint64_t x_batch =
                    x_wave + static_cast<uint64_t>(batch) *
                                 state.batch_m * kMmArRowBytes;
                if (valid_rows != state.batch_m) {
                    /*
                     * Tail batch: copy only the valid rows into the
                     * once-zeroed scratch (see the allocation-time zero).
                     * No per-tail memset: padded rows keep zeros or stale
                     * tail data, which is computed and exchanged but never
                     * reduced into the output.
                     */
                    const aclError copy_ret = aclrtMemcpyAsync(
                        reinterpret_cast<void*>(state.producer_scratch),
                        scratch_bytes,
                        reinterpret_cast<const void*>(x_batch),
                        static_cast<uint64_t>(valid_rows) * kMmArRowBytes,
                        ACL_MEMCPY_DEVICE_TO_DEVICE,
                        stream);
                    TORCH_CHECK(
                        copy_ret == ACL_SUCCESS,
                        "tail scratch copy failed, ret=", copy_ret);
                    x_batch = state.producer_scratch;
                }

                const int producer_ret = mf310p_direct_producer_async(
                    state.ctx,
                    x_batch,
                    weight_ptr,
                    batch,
                    capturing ? (batch + 1) : wave_generation,
                    reinterpret_cast<void*>(stream));
                TORCH_CHECK(
                    producer_ret == 0,
                    "batch ", batch,
                    " mf310p_direct_producer_async failed, ret=",
                    producer_ret);
            };

            auto enqueue_wait_add = [&](uint32_t batch) {
                const uint32_t valid_rows = valid_rows_for(batch);
                int batch_ret = mf310p_wait_batch_async(
                    state.ctx, batch, reinterpret_cast<void*>(stream));
                TORCH_CHECK(
                    batch_ret == 0,
                    "mf310p_wait_batch_async failed for batch ", batch,
                    " with ret=", batch_ret);

                const uint64_t out_batch =
                    reinterpret_cast<uint64_t>(output.mutable_data_ptr()) +
                    static_cast<uint64_t>(
                        wave_start +
                        static_cast<int64_t>(batch) * state.batch_m) *
                        kMmArRowBytes;
                batch_ret = mf310p_add_batch_async(
                    state.ctx,
                    out_batch,
                    batch,
                    valid_rows,
                    reinterpret_cast<void*>(stream));
                TORCH_CHECK(
                    batch_ret == 0,
                    "mf310p_add_batch_async failed for batch ", batch,
                    " with ret=", batch_ret);
            };

            /*
             * Lookahead=1. P(n+1) is queued before W/A(n), so SDMA(n) can
             * overlap the next cooperative MM. After P(n+1) signals, W/A(n)
             * can run while SDMA(n+1) progresses.
             */
            enqueue_producer(0);
            for (uint32_t batch = 1; batch < batches; ++batch) {
                enqueue_producer(batch);
                enqueue_wait_add(batch - 1);
            }
            enqueue_wait_add(batches - 1);

            /* Drain all local outbound SDMA only once per wave, then publish
             * the wave credit after every local recv read has been enqueued. */
            ret = mf310p_quiet_async(
                state.ctx, reinterpret_cast<void*>(stream));
            TORCH_CHECK(ret == 0,
                        "mf310p_quiet_async failed with ret=", ret);
            ret = mf310p_ack_async(
                state.ctx, reinterpret_cast<void*>(stream));
            TORCH_CHECK(ret == 0, "mf310p_ack_async failed with ret=", ret);

            if (trace_enabled()) {
                const int64_t t_enq = trace_now_us();
                trace_wave(
                    state.tp_rank,
                    wave_rows,
                    batches,
                    0,
                    t_join2 - t_join0,
                    t_warm - t_warm0,
                    t_enq - t_join2,
                    wave_start == 0 ? t_warm0 - t_entry : 0);
            }
        } catch (const std::exception& exc) {
            state.poisoned = true;
            if (state.failure_reason.empty()) {
                state.failure_reason = exc.what();
            }
            throw;
        }
    }

    return output;
}

#else  // VLLM_ASCEND_ENABLE_310P_MEMFABRIC_MM_AR

namespace {
[[noreturn]] void memfabric_not_built()
{
    TORCH_CHECK(
        false,
        "vllm_ascend_C was built without the 310P customized MemFabric mm_ar "
        "runtime. Install wgm-dev-310p MemFabric, source its set_env.sh, "
        "enable VLLM_ASCEND_310P_ENABLE_MEMFABRIC_MM_AR=1, then rebuild "
        "vLLM-Ascend.");
    std::abort();
}
} // namespace

at::Tensor memfabric_mm_ar_allreduce_impl(
    const at::Tensor&,
    const at::Tensor&,
    int64_t,
    int64_t)
{
    memfabric_not_built();
}

void memfabric_mm_ar_shutdown()
{
    /* Feature-off builds never create the context; nothing to tear down. */
}

at::Tensor memfabric_mm_ar_debug_snapshot()
{
    /* Feature-off builds have no pool; mirror the feature-on no-context
     * result (all-zero words) so the always-registered debug op resolves. */
    return at::zeros({24}, at::TensorOptions().dtype(at::kLong));
}

#endif // VLLM_ASCEND_ENABLE_310P_MEMFABRIC_MM_AR

} // namespace vllm_ascend
#endif // ASCEND_PLATFORM_310P
