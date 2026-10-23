/* Qwen TP=2 的 V2 实验服务桥接层。保持 V1 Runtime 与设备 Kernel 原样，通过独立 PyTorch Op 调用已实机验证的 V2 MM+SDMA+Add。Python LinearMethod 负责 M 区间路由。MTE3 首写保护只是工程规避：所有输出在建池前分配并在整个 Worker 生命周期固定存活；Eager 初始化逐槽执行真实两卡通信波与设备同步。新的设备内核组合仍必须实机验证首写正确性和双 Rank 启动偏斜。 */
#include <ATen/ATen.h>
#include <c10/util/Exception.h>
#include <torch/extension.h>
#include <torch/library.h>
#include <acl/acl_rt.h>
#include <torch_npu/csrc/core/npu/NPUStream.h>
#include <torch_npu/csrc/core/npu/NPUGraphsUtils.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <mutex>
#include <string>
#include <vector>

#include "mf310p_v2_serve_internal.h"

namespace vllm_ascend {
void memfabric_mm_ar_v2_shutdown();
namespace {

constexpr int64_t kWidth = 2048;
// 本服务的调度参数设置 --max-num-batched-tokens=4096。
// 因此每次 Forward 的 M≤4096；T0 独立 8192 行实测能力不受影响。
constexpr int64_t kMaxWindowRows = 4096;
constexpr int64_t kThreshold = 2048;
constexpr int kDefaultPoolSlots = 4;



V2ServeState& state()
{
    return v2_serve_state();
}

int configured_slot_count()
{
    const char* v = std::getenv("VLLM_ASCEND_310P_MEMFABRIC_V2_OUTPUT_SLOTS");
    if (v == nullptr || *v == '\0') return kDefaultPoolSlots;
    char* end = nullptr;
    const long n = std::strtol(v, &end, 10);
    TORCH_CHECK(end != v && *end == '\0' && n >= 2 && n <= 16,
                "V2 output slots must be within [2,16], got ", v);
    return static_cast<int>(n);
}

const char* configured_store_url()
{
    const char* v = std::getenv("VLLM_ASCEND_310P_MEMFABRIC_STORE_URL");
    return v && *v ? v : "tcp://127.0.0.1:8581";
}

void checked(int rc, const char* op)
{
    TORCH_CHECK(rc == 0, "V2 MemFabric serving ", op, " failed (rc=", rc,
                "). Restart BOTH TP=2 workers; a partially failed "
                "SDMA wave cannot be safely continued.");
}

void check_protocol(V2ServeState& s, const char* where)
{
    uint64_t status = ~0ULL;
    checked(mf310p_v2_trial_check_status(s.handle, &status), where);
    TORCH_CHECK(status == 0, "V2 serving ", where,
                " protocol status=", status);
}

void warm_outputs_locked(V2ServeState& s, const at::Tensor& x,
                          const at::Tensor& w, aclrtStream stream)
{
    // 服务 API 保证 M≥2048；初始化真实预热限制为两块，
    // 使用与稳定运行一致的 MM/SDMA 通信协议。
    const uint32_t warm_rows = 2u * s.chunk_rows;
    TORCH_CHECK(x.size(0) >= static_cast<int64_t>(warm_rows),
                "V2 bootstrap requires at least two complete chunks");
    const uint64_t input = reinterpret_cast<uint64_t>(x.const_data_ptr());
    const uint64_t weight = reinterpret_cast<uint64_t>(w.const_data_ptr());
    for (auto& slot : s.output_slots) {
        checked(mf310p_v2_trial_run(
                    s.handle, input, weight,
                    reinterpret_cast<uint64_t>(slot.data_ptr()),
                    warm_rows, 1, reinterpret_cast<void*>(stream)),
                "real-wave output prewarm");
        const aclError sync = aclrtSynchronizeStream(stream);
        TORCH_CHECK(sync == ACL_SUCCESS,
                    "V2 prewarm stream sync failed: ", sync);
        check_protocol(s, "prewarm");
    }
    // 初始化时还要覆盖非对齐尾行的暂存路径，
    // 首次 stage.copy_、补齐后的通信波和阶段缓冲区
    // 第一次被 MM 读取，不得发生于用户请求。2026-10-18
    // 冷池事件中 M=2049 首次输出损坏且 status=0，
    // 只有新部署 orchestrator Kernel 的最初若干次加载异常，
    // 因此在初始化时明确触发该路径。
    const int64_t tail_rows = static_cast<int64_t>(warm_rows) - 1;
    s.input_stage.narrow(0, 0, tail_rows)
        .copy_(x.narrow(0, 0, tail_rows));
    checked(mf310p_v2_trial_run(
                s.handle,
                reinterpret_cast<uint64_t>(s.input_stage.const_data_ptr()),
                weight,
                reinterpret_cast<uint64_t>(s.output_slots.front().data_ptr()),
                static_cast<uint32_t>(
                    std::max<int64_t>(kThreshold, warm_rows)),
                1, reinterpret_cast<void*>(stream)),
            "staged-tail prewarm");
    const aclError tail_sync = aclrtSynchronizeStream(stream);
    TORCH_CHECK(tail_sync == ACL_SUCCESS,
                "V2 staged-tail prewarm sync failed: ", tail_sync);
    check_protocol(s, "staged-tail prewarm");

    // 深度扫描：对 [2048,4096] 内所有 Chunk 整倍数
    // 逐一执行暂存通信波。2026-10-19 冷启动第 17 轮
    // 曾出现两块预热通过而首次三块输出损坏，status=0；
    // 说明新波深度也存在独立首用风险。暂存缓冲区
    // 采用 x 循环填充，不要求首次输入超过实际行数，
    // 且每次调用所需补齐行数都可由暂存区覆盖，
    // 传输机制不解析具体数据内容。
    const int64_t stage_rows = s.input_stage.size(0);
    for (int64_t off = 0; off < stage_rows; off += x.size(0)) {
        const int64_t n = std::min<int64_t>(x.size(0), stage_rows - off);
        s.input_stage.narrow(0, off, n).copy_(x.narrow(0, 0, n));
    }
    for (int64_t m = kThreshold; m <= kMaxWindowRows;
         m += static_cast<int64_t>(s.chunk_rows)) {
        checked(mf310p_v2_trial_run(
                    s.handle,
                    reinterpret_cast<uint64_t>(s.input_stage.const_data_ptr()),
                    weight,
                    reinterpret_cast<uint64_t>(s.output_slots.front().data_ptr()),
                    static_cast<uint32_t>(m), 1,
                    reinterpret_cast<void*>(stream)),
                "depth-sweep prewarm");
        const aclError sweep_sync = aclrtSynchronizeStream(stream);
        TORCH_CHECK(sweep_sync == ACL_SUCCESS,
                    "V2 depth-sweep prewarm sync failed: ", sweep_sync);
        check_protocol(s, "depth-sweep prewarm");
    }
}

void init_locked(V2ServeState& s, const at::Tensor& x,
                 const at::Tensor& w, int64_t tp_rank,
                 uint32_t chunk_rows, aclrtStream stream)
{
    if (s.handle != nullptr) {
        TORCH_CHECK(s.rank == tp_rank && s.chunk_rows == chunk_rows,
                    "V2 serving context geometry changed; restart workers");
        TORCH_CHECK(s.execution_stream == stream,
                    "V2 serving is single-stream only; mixed streams "
                    "must be explicitly synchronized and reinstrumented");
        return;
    }

    // 输出槽与补齐输入区都必须在 create() 之前分配。
    // 这是平台 MTE3 首写问题的缓解，不代表根因已修复。
    // 所有分配保持固定存活直至 Worker 退出。
    const int count = configured_slot_count();
    s.output_slots.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        s.output_slots.push_back(
            at::empty({kMaxWindowRows, kWidth}, x.options()));
    }
    s.input_stage = at::zeros({kMaxWindowRows, kWidth}, x.options());
    // 确保建池前的所有缓冲区初始化与准备都已经完成。
    const aclError init_sync = aclrtSynchronizeStream(stream);
    TORCH_CHECK(init_sync == ACL_SUCCESS,
                "V2 pre-pool allocation/init sync failed ", init_sync);
    s.rank = static_cast<int>(tp_rank);
    s.chunk_rows = chunk_rows;
    s.execution_stream = stream;

    checked(mf310p_v2_trial_create(s.rank, configured_store_url(),
                                   chunk_rows, &s.handle),
            "pool create / geometry exchange");
    static bool shutdown_hook_registered = false;
    if (!shutdown_hook_registered) {
        TORCH_CHECK(std::atexit(&memfabric_mm_ar_v2_shutdown) == 0,
                    "V2 failed to register worker shutdown hook");
        shutdown_hook_registered = true;
    }
    checked(mf310p_v2_trial_ib_probe(s.handle, 1000u,
                                     reinterpret_cast<void*>(stream)),
            "8-core IB probe");
    warm_outputs_locked(s, x, w, stream);
}

at::Tensor reserve_slot_locked(V2ServeState& s)
{
    for (auto& slot : s.output_slots) {
        // torch 2.10 空闲槽 StorageImpl 的引用计数恰好是 1，
        // 仅由状态对象里的 Tensor 持有；TensorBase::storage() 返回
        // const Storage&，不会增加临时引用。返回的 narrow/reshape
        // 视图会使引用数至少为 2，因此大于 1 表示还在使用。
        // 超过 1 个引用的输出槽仍处于在途状态。
        // 即使跨不同层调用，也绝不可复用仍被引用的槽。
        if (slot.storage().use_count() == 1) {
            return slot;
        }
    }
    TORCH_CHECK(false,
        "V2 pooled outputs exhausted: all ", s.output_slots.size(),
        " slots are still referenced by model tensors. Increase "
        "VLLM_ASCEND_310P_MEMFABRIC_V2_OUTPUT_SLOTS (max 16), "
        "or reduce in-flight requests. Refusing unsafe alias/reuse.");
    return at::Tensor();  // TORCH_CHECK 已终止流程；此返回值仅用于满足 C++ 编译诊断。
}

void run_window_locked(V2ServeState& s, const at::Tensor& x,
                       const at::Tensor& w, int64_t start, int64_t rows,
                       at::Tensor& output, aclrtStream stream)
{
    const int64_t padded = std::max<int64_t>(
        kThreshold,
        ((rows + static_cast<int64_t>(s.chunk_rows) - 1) /
         s.chunk_rows) * s.chunk_rows);
    TORCH_CHECK(padded >= kThreshold && padded <= kMaxWindowRows,
                "V2 invalid padded window size ", padded);
    // 若输入行数已对齐且连续，则跳过暂存。
    // 非对齐尾块使用常驻暂存 Tensor；MM 以行为单位独立，
    // 有效行之后的旧内容永远不返回调用方。
    at::Tensor source;
    if (rows == padded) {
        source = x.narrow(0, start, rows);
    } else {
        // 这里使用 torch NPU 向量拷贝，不直接调用 aclrtMemcpyAsync，
        // 因为 T2 实测后者在 310P3 可读到过时的上游数据。
        // 暂存与 V2 Kernel 在同一 Stream 上串行。
        s.input_stage.narrow(0, 0, rows).copy_(
            x.narrow(0, start, rows));
        source = s.input_stage;
    }
    checked(mf310p_v2_trial_run(
                s.handle,
                reinterpret_cast<uint64_t>(source.const_data_ptr()),
                reinterpret_cast<uint64_t>(w.const_data_ptr()),
                reinterpret_cast<uint64_t>(output.data_ptr()),
                static_cast<uint32_t>(padded), 1,
                reinterpret_cast<void*>(stream)),
            "MM+MemFabric overlap");

    // 设备端失败会异步暴露在协议状态中。
    // 逐调用执行 Host 同步会破坏交叠性能，
    // 也不利于 Graph 运行；测试压力阶段及退出前必须
    // 主动同步并检查协议 status。
}

}  // 匿名命名空间

V2ServeState& v2_serve_state()
{
    static V2ServeState s;
    return s;
}

at::Tensor memfabric_mm_ar_v2_allreduce(
    const at::Tensor& x, const at::Tensor& weight, int64_t tp_rank,
    int64_t chunk_rows)
{
    TORCH_CHECK(x.is_privateuseone() && weight.is_privateuseone(),
                "V2 requires NPU activation and weight");
    TORCH_CHECK(x.scalar_type() == at::kHalf &&
                    weight.scalar_type() == at::kHalf,
                "V2 only supports FP16");
    TORCH_CHECK(x.dim() == 2 && x.size(1) == kWidth &&
                weight.dim() == 2 && weight.size(0) == kWidth &&
                weight.size(1) == kWidth,
                "V2 geometry requires X[M,2048], W_NZ[2048,2048]");
    TORCH_CHECK(x.is_contiguous() && weight.is_contiguous(),
                "V2 requires contiguous X and NZ W");
    TORCH_CHECK(x.size(0) >= kThreshold,
                "V2 invoked on small M: ", x.size(0),
                " (small M must use native MM + HCCL)");
    TORCH_CHECK(x.size(0) <= kMaxWindowRows,
                "V2 serving requires M <=4096 per Forward, but got ",
                x.size(0), ". Configure vLLM --max-num-batched-tokens "
                "4096 or lower. This limit is separate from the "
                "per-request max input tokens.");
    TORCH_CHECK(tp_rank == 0 || tp_rank == 1, "V2 requires TP=2 rank 0/1");
    TORCH_CHECK(chunk_rows == 256 || chunk_rows == 512 ||
                    chunk_rows == 1024,
                "V2 chunk rows must be 256/512/1024");
    TORCH_CHECK(c10_npu::currentStreamCaptureStatusMayInitCtx() !=
                    c10_npu::CaptureStatus::Active,
                "V2 large-M prefill during ACL Graph capture is not yet "
                "supported. Configure FULL_DECODE_ONLY with capture "
                "sizes below M=2048; decode uses Stock HCCL.");
    const auto stream = c10_npu::getCurrentNPUStream().stream();
    auto& s = state();
    std::lock_guard<std::mutex> guard(s.mutex);
    TORCH_CHECK(!s.poisoned, "V2 serving runtime poisoned: ", s.failure);
    try {
        init_locked(s, x, weight, tp_rank,
                    static_cast<uint32_t>(chunk_rows), stream);
        const int64_t rows = x.size(0);
        // 本服务固定预算内的所有请求都能在一个 T0 波内执行。
        // 非对齐 M 由 run_window_locked() 补齐，
        // 返回时再裁剪到精确的有效行数，无需多段调度。
        at::Tensor slot = reserve_slot_locked(s);
        run_window_locked(s, x, weight, 0, rows, slot, stream);
        return slot.narrow(0, 0, rows);
    } catch (const std::exception& e) {
        s.poisoned = true;
        s.failure = e.what();
        throw;
    }
}

void memfabric_mm_ar_v2_shutdown()
{
    auto& s = state();
    std::lock_guard<std::mutex> guard(s.mutex);
    if (!s.handle) return;
    if (s.execution_stream != nullptr)
        (void)aclrtSynchronizeStream(s.execution_stream);
    (void)mf310p_v2_trial_destroy(s.handle);
    s.handle = nullptr;
    s.input_stage.reset();
    s.output_slots.clear();
}

}  // vllm_ascend 命名空间

TORCH_LIBRARY_FRAGMENT(_C_ascend, ops)
{
    ops.def(
        "memfabric_mm_ar_v2_allreduce(Tensor x, Tensor weight, "
        "int tp_rank, int chunk_rows) -> Tensor");
    ops.impl("memfabric_mm_ar_v2_allreduce", torch::kPrivateUse1,
             &vllm_ascend::memfabric_mm_ar_v2_allreduce);
    ops.def("memfabric_mm_ar_v2_shutdown() -> ()");
    ops.impl("memfabric_mm_ar_v2_shutdown",
             c10::DispatchKey::CompositeExplicitAutograd,
             &vllm_ascend::memfabric_mm_ar_v2_shutdown);
}
