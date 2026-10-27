# SPDX-License-Identifier: Apache-2.0
"""Import-free source regressions for the 310P3 MemFabric mm_ar ABI v7 path."""

from __future__ import annotations

import ast
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
HELPER = ROOT / "vllm_ascend" / "_310p" / "ops" / "memfabric_mm_ar.py"
ENVS = ROOT / "vllm_ascend" / "envs.py"
MODELSLIM = ROOT / "vllm_ascend" / "_310p" / "quantization" / "modelslim_config.py"
CUSTOM_CSRC = ROOT / "csrc" / "_310P" / "memfabric_mm_ar"
BINDING = CUSTOM_CSRC / "memfabric_mm_ar_binding.cpp"
RUNTIME = CUSTOM_CSRC / "memfabric_mm_ar_runtime.cpp"
ADAPTER_API = CUSTOM_CSRC / "memfabric310p_adapter_api.h"
ADAPTER = CUSTOM_CSRC / "memfabric310p_adapter.cpp"
DEVICE = CUSTOM_CSRC / "memfabric310p_device.asc"
MEMFABRIC_CMAKE = ROOT / "cmake" / "memfabric_310p.cmake"

INTERNAL_MEMFABRIC_TOKENS = (
    "SMEM_SHM_SDMA_RS_",
    "SMEM_SHM_SDMA_RESERVED",
    "SdmaReadCell",
    "SdmaWriteCell",
    "smem_shm_sdma_reserved",
    "Mf310pPostSlotOrdered",
    "kMfRsReservedBytes",
    "kMfRsReqTailOff",
    "kMfRsArrStampOff",
    "debug_dump_sdma_rings_locked",
)


def _func(path: Path, name: str) -> ast.FunctionDef:
    for node in ast.parse(path.read_text()).body:
        if isinstance(node, ast.FunctionDef) and node.name == name:
            return node
    raise AssertionError(f"function {name} not found in {path}")


def _src(node: ast.AST) -> str:
    return ast.unparse(node)


def test_custom_310p_sources_are_isolated_under_marked_subproject() -> None:
    assert CUSTOM_CSRC.name == "memfabric_mm_ar"
    assert CUSTOM_CSRC.parent.name == "_310P"
    expected = {
        "memfabric_mm_ar_binding.cpp",
        "memfabric_mm_ar_runtime.cpp",
        "memfabric_mm_ar_torch_adpt.h",
        "memfabric310p_adapter_api.h",
        "memfabric310p_adapter.cpp",
        "memfabric310p_device.asc",
        "README.md",
    }
    assert expected.issubset({p.name for p in CUSTOM_CSRC.iterdir()})


def test_eligibility_remains_deliberately_narrow() -> None:
    src = _src(_func(HELPER, "should_enable_memfabric_mm_ar"))
    module = HELPER.read_text()
    fused_prefix = _src(_func(HELPER, "_is_fused_layer_prefix"))
    assert "VLLM_ASCEND_310P_ENABLE_MEMFABRIC_MM_AR" in src
    # Both fused layer families: full-attention o_proj and GDN out_proj.
    assert '".self_attn.o_proj": "full_attention"' in module
    assert '".linear_attn.out_proj": "linear_attention"' in module
    assert "layer_types[layer_idx] == _FUSED_LAYER_PREFIXES[suffix]" in fused_prefix
    assert "_EXPECTED_TP_SIZE" in src
    assert "_EXPECTED_INPUT_SIZE_PER_PARTITION" in src
    assert "_EXPECTED_OUTPUT_SIZE" in src
    assert "torch.float16" in src
    assert "AscendUnquantizedLinearMethod" in src


def test_fused_route_has_no_m_threshold() -> None:
    module = HELPER.read_text()
    apply_src = module[module.index("def apply(") :]
    assert "VLLM_ASCEND_310P_MEMFABRIC_MM_AR_MIN_M" not in module
    assert "VLLM_ASCEND_310P_MEMFABRIC_MM_AR_MIN_M" not in ENVS.read_text()
    # Once selected, every M is fused; only the opt-in warmup fallback keeps
    # a stock detour for engine-init dummy runs.
    assert "memfabric_mm_ar_warmup_fallback_active()" in apply_src
    assert "x.shape[0] <" not in apply_src


def test_python_plan_is_base_m_batch_based() -> None:
    module = HELPER.read_text()
    plan = module[module.index("class MemFabricMmArPlan") : module.index("def _target_text_config")]
    assert "_BASE_M = 256" in module
    assert "batch_basem_count" in plan
    assert "def batch_m" in plan
    assert "def batch_bytes" in plan
    assert "VLLM_ASCEND_310P_MEMFABRIC_MM_AR_BATCH_BASEM_COUNT" in plan
    assert "q not in (1, 2, 4)" in plan
    assert "VLLM_ASCEND_310P_MEMFABRIC_MM_AR_TILE_M" not in module
    assert "_MAX_CHUNKS_PER_WAVE" not in module


def test_fused_op_is_single_python_route_and_passes_batch_count() -> None:
    src = _src(_func(HELPER, "memfabric_mm_ar_allreduce"))
    assert "memfabric_mm_ar_allreduce" in src
    assert "layer.weight.data" in src
    assert "plan.batch_basem_count" in src
    assert "F.linear" not in src


def test_cpp_binding_registers_v7_fused_op_and_shutdown() -> None:
    src = BINDING.read_text()
    assert "TORCH_LIBRARY_FRAGMENT(_C_ascend, ops)" in src
    assert "memfabric_mm_ar_allreduce" in src
    assert "int batch_basem_count" in src
    assert "int tile_m" not in src
    assert "memfabric_mm_ar_shutdown" in src
    assert "torch::kPrivateUse1" in src


def test_memfabric_transport_internals_never_escape_into_vllm_sources() -> None:
    for path in (RUNTIME, ADAPTER_API, ADAPTER, DEVICE):
        src = path.read_text()
        for token in INTERNAL_MEMFABRIC_TOKENS:
            assert token not in src, f"{token} leaked into {path}"


def test_device_uses_only_public_memfabric_data_plane() -> None:
    src = DEVICE.read_text()
    assert '#include "smem_shm_aicore_base_sdma.h"' in src
    assert "smem_shm_sdma_signal(" in src
    assert "smem_shm_sdma_wait(" in src
    assert "smem_shm_sdma_quiet(" in src
    for private_variant in (
        "smem_shm_sdma_signal_at",
        "smem_shm_sdma_wait_at",
        "smem_shm_sdma_quiet_at",
    ):
        assert private_variant not in src


def test_device_uses_eight_core_cooperative_mm_with_core0_signal_owner() -> None:
    src = DEVICE.read_text()
    producer = src[src.index("Mf310pDirectProducerKernel") : src.index("mf310pWaitBatchKernel")]

    assert "MF310P_MATMUL_BASE_N = 256" in src
    assert "MF310P_MATMUL_BASE_K = 64" in src
    assert "MF310P_COOPERATIVE_CORES = 8" in src
    assert "rt.usedCoreNum = MF310P_COOPERATIVE_CORES" in producer
    assert "mm.IterateAll(cGm)" in producer
    # Classic Matmul is single-core semantics: cooperation must be expressed
    # through explicit per-block M-partitions over dense row-sliced A/C views.
    assert "kRowsPerCore = BATCH_M / MF310P_COOPERATIVE_CORES" in producer
    assert "rowOff * MF310P_MM_AR_K" in producer
    assert "rowOff * MF310P_MM_AR_N" in producer
    # CONFIG_MDL measurably faults (on-chip MTE overrun) at these per-block
    # M sizes; the tiling must stay on the measured-valid CONFIG_NORM.
    assert "GetMMConfig<MatmulConfigMode::CONFIG_NORM>" in src
    assert "GetMMConfig<MatmulConfigMode::CONFIG_MDL>" not in src
    # Ownership-scoped cache visibility: each block cleans only the
    # contiguous C rows it wrote (not the whole batch).
    clean = producer[producer.index("mm.IterateAll(cGm)") : producer.index("Mf310pWriteReady(")]
    assert "Mf310pCleanRegion(" in clean
    assert "rowOff * MF310P_MM_AR_N * sizeof(float16_t)" in clean
    assert "Mf310pWriteReady(" in producer
    assert "if (block != 0)" in producer
    assert "smem_shm_sdma_signal(" in producer
    assert producer.index("mm.IterateAll(cGm)") < producer.index("Mf310pWriteReady(")
    assert producer.index("Mf310pWriteReady(") < producer.index("if (block != 0)")
    assert producer.index("if (block != 0)") < producer.index("smem_shm_sdma_signal(")

    launcher = src[src.index("#define MF310P_PRODUCER_LAUNCH_CASE") :]
    assert "<<<MF310P_COOPERATIVE_CORES, 0, stream>>>" in launcher
    assert "MF310P_PRODUCER_WORKERS" not in src
    assert "worker_count + 1" not in src
    assert "rt.usedCoreNum = 1" not in src


def test_ready_cells_are_per_batch_per_core_and_generation_tagged() -> None:
    device = DEVICE.read_text()
    adapter = ADAPTER.read_text()
    assert "MF310P_READY_STRIDE_BYTES = 64" in device
    assert "batchIndex) * MF310P_COOPERATIVE_CORES + core" in device
    assert "== generation" in device
    assert "kProducerReadyStrideBytes = 64ULL" in adapter
    assert "VLLM_ASCEND_MF310P_COOPERATIVE_CORES" in adapter
    assert "kProducerControlBytes" in adapter
    assert "aclrtMemsetAsync(" in adapter


def test_wait_one_batch_is_strict_and_does_not_quiet() -> None:
    src = DEVICE.read_text()
    waiter = src[src.index("mf310pWaitBatchKernel") : src.index("mf310pAddKernel")]
    assert "smem_shm_sdma_wait(gva)" in waiter
    assert "smem_shm_sdma_quiet(gva)" not in waiter
    assert "SMEM_SHM_SDMA_MAIL_OK" in waiter
    assert "expectedRecvBase + static_cast<uint64_t>(batchIndex) * batchBytes" in waiter
    assert "m.len != batchBytes" in waiter
    assert "m.imm != batchIndex" in waiter
    assert "MF310P_STATUS_MAIL_MISMATCH" in waiter


def test_adapter_abi_v7_is_batch_oriented_application_only() -> None:
    src = ADAPTER_API.read_text()
    assert "VLLM_ASCEND_MF310P_ADAPTER_ABI_VERSION 7u" in src
    assert "VLLM_ASCEND_MF310P_MAX_BATCHES 64u" in src
    assert "VLLM_ASCEND_MF310P_COOPERATIVE_CORES 8u" in src
    for field in (
        "arena_bytes",
        "batch_bytes",
        "arena_rows",
        "batch_m",
        "max_batches",
    ):
        assert field in src
    assert "mf310p_wait_batch_async" in src
    assert "mf310p_add_batch_async" in src
    assert "chunk_bytes" not in src
    assert "max_chunks" not in src
    assert "producer_control" not in src
    assert "debug_flags" not in src


def test_adapter_decouples_arena_from_batch_and_clears_ready_per_wave() -> None:
    src = ADAPTER.read_text()
    create = src[src.index('extern "C" int mf310p_create') : src.index('extern "C" int mf310p_destroy')]
    prepare = src[
        src.index('extern "C" int mf310p_prepare_wave_async') : src.index('extern "C" int mf310p_init_credit_async')
    ]
    producer = src[
        src.index('extern "C" int mf310p_direct_producer_async') : src.index('extern "C" int mf310p_wait_batch_async')
    ]
    assert "arena_rows" in create
    assert "arena_bytes =" in create
    assert "batch_bytes =" in create
    assert "arena_rows / batch_m" in create
    assert "VLLM_ASCEND_MF310P_MAX_BATCHES" in create
    assert "aclrtMemsetAsync(" in prepare
    assert "ctx->producer_control" in prepare
    assert "aclrtMemsetAsync(" not in producer


def test_adapter_uses_public_host_readiness_and_real_device_id() -> None:
    src = ADAPTER.read_text()
    assert "smem_shm_sdma_get_workspace(ctx->shm) == nullptr" in src
    assert "sdma_workspace" not in src
    assert "smem_shm_get_symmetric_size" in src
    assert "aclrtGetDevice(&device_id)" in src
    assert "static_cast<uint16_t>(device_id)" in src
    assert "static_cast<uint16_t>(rank)" not in src


def test_runtime_pipelines_batches_then_quiets_and_acks_wave() -> None:
    src = RUNTIME.read_text()
    impl = src[src.index("memfabric_mm_ar_allreduce_impl") :]
    loop = impl[impl.index("enqueue_producer(0)") : impl.index("if (trace_enabled())")]
    assert "enqueue_producer(batch);" in loop
    assert "enqueue_wait_add(batch - 1);" in loop
    assert "enqueue_wait_add(batches - 1);" in loop
    assert loop.index("enqueue_producer(batch);") < loop.index("enqueue_wait_add(batch - 1);")
    assert loop.index("enqueue_wait_add(batches - 1);") < loop.index("mf310p_quiet_async(")
    assert loop.index("mf310p_quiet_async(") < loop.index("mf310p_ack_async(")
    assert "mf310p_wait_mails_async" not in impl
    assert "mf310p_add_async" not in impl


def test_runtime_arena_and_tail_are_batch_based_and_bounds_safe() -> None:
    src = RUNTIME.read_text()
    impl = src[src.index("memfabric_mm_ar_allreduce_impl") :]
    assert "kBaseM = 256" in src
    assert "kMaxArenaRows = 8192" in src
    assert "rows_by_budget" in src
    assert "arena_rows = (arena_rows / batch_m) * batch_m" in src
    assert "state.layout.arena_rows" in impl
    assert "producer_scratch" in impl
    assert "tail scratch copy failed" in impl
    assert "valid_rows" in impl
    assert "VLLM_ASCEND_MF310P_MAX_CHUNKS" not in src


def test_runtime_tail_scratch_is_zeroed_once_not_per_tail() -> None:
    src = RUNTIME.read_text()
    impl = src[src.index("memfabric_mm_ar_allreduce_impl") :]
    producer = impl[impl.index("auto enqueue_producer") : impl.index("auto enqueue_wait_add")]
    alloc = impl[impl.index("state.producer_scratch == 0") : impl.index("at::Tensor output")]
    # One-time zero at allocation, on the same stream as every later use.
    assert "aclrtMemsetAsync(" in alloc
    assert "producer scratch init zero failed" in alloc
    # The per-tail path copies only the valid rows; no per-tail memset.
    assert "aclrtMemcpyAsync(" in producer
    assert "aclrtMemsetAsync(" not in producer
    assert "tail scratch memset failed" not in impl


def test_runtime_has_graph_safe_fixed_credit_and_stream_contract() -> None:
    src = RUNTIME.read_text()
    assert "protocol_initialized" in src
    assert "mf310p_init_credit_async(" in src
    assert "mf310p_prepare_wave_async(" in src
    assert "mf310p_gate_async(" in src
    assert "mf310p_ack_async(" in src
    assert "eager execution is single-stream by contract" in src
    assert "currentStreamCaptureStatusMayInitCtx" in src
    assert "graph_capture_seen" in src
    assert "require_capture_ready_locked" in src
    assert "context must be created by eager warmup" in src
    assert "aclrtSynchronizeStream(state.eager_stream)" in src
    assert "graph-used context kept process-lifetime" in src
    assert "poisoned" in src


def test_env_exposes_batch_count_not_tile_size() -> None:
    src = ENVS.read_text()
    assert "VLLM_ASCEND_310P_MEMFABRIC_MM_AR_BATCH_BASEM_COUNT" in src
    assert '"1"' in src
    assert "VLLM_ASCEND_310P_MEMFABRIC_MM_AR_TILE_M" not in src
    assert "96 * 1024 * 1024" in src


def test_env_exposes_small_m_path_kill_switch() -> None:
    src = ENVS.read_text()
    assert "VLLM_ASCEND_310P_MEMFABRIC_MM_AR_SMALL_M_PATH" in src
    # Default on; "0" restores the padded batch pipeline for every M.
    assert 'os.getenv("VLLM_ASCEND_310P_MEMFABRIC_MM_AR_SMALL_M_PATH", "1")' in src
    # The runtime reads the switch directly in C++ (like the trace env), so
    # the entry documents it for visibility rather than routing in Python.
    runtime = RUNTIME.read_text()
    assert '"VLLM_ASCEND_310P_MEMFABRIC_MM_AR_SMALL_M_PATH"' in runtime


def test_device_nsplit_producer_uses_template_stair_and_column_slices() -> None:
    src = DEVICE.read_text()
    for template in (16, 32, 64, 128, 256):
        assert f"MF310P_NSPLIT_TILING({template}, {template})" in src
    nsplit = src[src.index("Mf310pNsplitProducerKernel") : src.index("mf310pWaitBatchKernel")]
    # Shared staged A window, per-core 256-column B slice, contiguous
    # [T, 256] C block per core: the whole slot stays one SDMA payload.
    assert "MF310P_NSPLIT_SLICE_ELEMS * sizeof(float16_t)" in nsplit
    assert "block) * T * MF310P_NSPLIT_N" in nsplit
    assert "SetOrgShape(\n        T, MF310P_NSPLIT_N, MF310P_MM_AR_K" in nsplit
    assert "mm.IterateAll(cGm)" in nsplit
    assert "Mf310pCleanRegion(" in nsplit
    # Core0 remains the sole signal owner after the 8-core ready rendezvous.
    assert "Mf310pWaitReady(" in nsplit
    assert "smem_shm_sdma_signal(" in nsplit
    assert nsplit.index("Mf310pWriteReady(") < nsplit.index("smem_shm_sdma_signal(")
    # All five template instantiations are distinct kernel symbols and the
    # first launch of a symbol is a silent no-op: warmups must cover them all.
    warmup = src[src.index("mf310p_device_warmup_nsplit_producer_async") :]
    for template in (16, 32, 64, 128, 256):
        assert f"MF310P_NSPLIT_WARMUP_CASE({template})" in warmup
    assert "generation == 0" in src


def test_device_blocked_add_reduces_exactly_valid_rows() -> None:
    src = DEVICE.read_text()
    blocked = src[
        src.index("mf310pAddBlockedKernel") : src.index('extern "C" int mf310p_device_launch_nsplit_producer_async')
    ]
    # out[m, n] = send[(n/256)*strideRows*256 + m*256 + (n%256)]: every
    # 256-element segment is exactly one (row, N-block) pair, so it never
    # straddles a 256-column block boundary of the blocked slot layout.
    assert "nBlock) * strideRows * MF310P_NSPLIT_N" in blocked
    assert "row * MF310P_NSPLIT_N" in blocked
    assert "validRows) * outCols" in blocked
    assert "MF310P_NSPLIT_N; /* 256 */" in blocked
    # valid_rows == 0 is the side-effect-free warmup shape.
    assert "valid_rows == 0" in src


def test_adapter_builds_weight_slices_with_strided_copies() -> None:
    src = ADAPTER.read_text()
    build = src[
        src.index('extern "C" int mf310p_build_weight_slices') : src.index('extern "C" int mf310p_small_producer_async')
    ]
    assert "aclrtMemcpy2dAsync(" in build
    # Full NZ [k_f=128][n_f=128][512B]; slice c = n_f fractals [16c, 16c+16):
    # 8 KiB contiguous run per 64 KiB k_f row, re-laid out per core.
    assert "kNzFullKfRowBytes" in src
    assert "kNzSliceKfRowBytes" in src
    assert "kWeightSliceBytes" in src
    assert "kNsplitCores" in build
    api = ADAPTER_API.read_text()
    for fn in (
        "mf310p_build_weight_slices",
        "mf310p_small_producer_async",
        "mf310p_wait_small_async",
        "mf310p_add_small_async",
        "mf310p_warmup_nsplit_producer_async",
        "mf310p_warmup_add_blocked_async",
    ):
        assert fn in api


def test_runtime_routes_small_m_before_batch_wave() -> None:
    src = RUNTIME.read_text()
    impl = src[src.index("memfabric_mm_ar_allreduce_impl") :]
    assert "kSmallPathMaxM = 256" in src
    assert "small_template_for" in src
    assert "std::min<int64_t>(state.batch_m, kSmallPathMaxM)" in impl
    small = impl[impl.index("const bool small_path") : impl.index("const int64_t max_rows_per_wave")]
    # One serial wave: producer -> wait -> blocked add, then the shared
    # quiet/ack credit handshake.
    first = small.index("mf310p_small_producer_async(")
    assert first < small.index("mf310p_wait_small_async(")
    assert small.index("mf310p_wait_small_async(") < small.index("mf310p_add_small_async(")
    assert small.index("mf310p_add_small_async(") < small.index("mf310p_quiet_async(")
    assert small.index("mf310p_quiet_async(") < small.index("mf310p_ack_async(")
    # Staging copies exactly the M valid rows into the once-zeroed scratch.
    assert "small-path staging copy failed" in small
    # Batch-0 mail with explicit template length through the existing wait
    # kernel; generation contract matches the batch path (eager monotonic,
    # captured wave-invariant 1).
    assert "Wave-invariant batch-0 generation" in small
    assert "state.wave_seq" in small


def test_runtime_small_path_warmup_and_slice_cache_contracts() -> None:
    src = RUNTIME.read_text()
    assert "warm_small_locked" in src
    # Any eager fused call warms the small-path symbols, so a later small
    # capture never launches a cold symbol (whose first launch is a no-op).
    eager = src[
        src.index("warm_producer_locked(state, stream);") : src.index("initialize_protocol_locked(state, stream);")
    ]
    assert "warm_small_locked(state, stream);" in eager
    assert "(!small_path_enabled() || state.small_warmed)" in src
    # Slices are built eagerly before capture and cached per weight pointer.
    assert "ensure_weight_slices_locked" in src
    assert "must be built by eager" in src
    assert "kMaxWeightSliceEntries" in src
    assert "WeightSlicesEntry" in src
    # The entries are freed at shutdown alongside the producer scratch.
    assert "state.weight_slices.clear();" in src


def test_build_consumes_current_memfabric_public_package() -> None:
    src = MEMFABRIC_CMAKE.read_text()
    assert "MEMFABRIC_HYBRID_HOME_PATH" in src
    assert "libmf_smem.so" in src
    assert "smem_shm_aicore_base_sdma.h" in src
    assert "--npu-arch=dav-2002" in src
    assert "memfabric310p_adapter.cpp" in src
    assert "memfabric310p_device.asc" in src
    assert "VLLM_ASCEND_ENABLE_310P_MEMFABRIC_MM_AR" in src
    assert "find_program(BISHENG_COMPILER" in src
    assert "add_custom_command(" in src
    assert "mf310p_device_lib" in src


def test_modelslim_router_still_dispatches_eligible_float_layer() -> None:
    src = MODELSLIM.read_text()
    assert "should_enable_memfabric_mm_ar(layer)" in src
    assert "make_memfabric_mm_ar_linear_method" in src
    assert "return AscendUnquantizedLinearMethod()" in src
