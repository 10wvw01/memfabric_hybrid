# Graph custom-op 方案的未合入历史快照，不属于当前正式回归。
# SPDX-License-Identifier: Apache-2.0
"""未导入硬件扩展的 G3 V2 Serving 源码回归（Graph custom-op 未合入快照）。"""

from __future__ import annotations

import ast
from pathlib import Path

ROOT = Path(__file__).resolve().parents[6]
V2PY = ROOT / "vllm_ascend/_310p/ops/memfabric_mm_ar_v2_serve.py"
V1PY = ROOT / "vllm_ascend/_310p/ops/memfabric_mm_ar.py"
ROUTER = ROOT / "vllm_ascend/_310p/quantization/modelslim_config.py"
HOOKS = ROOT / "vllm_ascend/_310p/ops/memfabric_mm_ar_runner_hooks.py"
V2SRC = ROOT / "csrc/_310P/memfabric_mm_ar_v2/src"
V2CPP = V2SRC / "torch_adapter/mf310p_v2_serve.cpp"
V2ADAPTER = V2SRC / "op_host/mf310p_v2_trial_adapter.cpp"
V2BUILD = ROOT / "cmake/memfabric_310p_v2.cmake"


def test_v2_route_boundary_is_exact_and_uses_stock_outside_fused_interval():
    module = ast.parse(V2PY.read_text())
    func = next(node for node in module.body if isinstance(node, ast.FunctionDef) and node.name == "v2_threshold_route")
    namespace = {"MIN_FUSED_M": 2048, "MAX_FUSED_M": 4096}
    exec(compile(ast.fix_missing_locations(ast.Module(body=[func], type_ignores=[])), str(V2PY), "exec"), namespace)
    route = namespace["v2_threshold_route"]
    assert [(m, route(m)) for m in (0, 1, 10, 1024, 2047, 2048, 2049, 3073, 4096, 4097, 5000, 8192, 8193)] == [
        (0, "stock"),
        (1, "stock"),
        (10, "stock"),
        (1024, "stock"),
        (2047, "stock"),
        (2048, "v2"),
        (2049, "v2"),
        (3073, "v2"),
        (4096, "v2"),
        (4097, "stock"),
        (5000, "stock"),
        (8192, "stock"),
        (8193, "stock"),
    ]
    try:
        route(-1)
    except ValueError:
        pass
    else:
        raise AssertionError("negative M must fail")
    # 运行时 M>4096 必须回退普通 Stock，不能
    # 抛异常或暗中拆成多个 V2 通信波。
    assert all(route(m) == "stock" for m in (4097, 5000, 8192, 8193))


def test_all_stock_shapes_restore_original_tp_reduce_exactly_once():
    code = V2PY.read_text()
    # 路由逻辑放在不透明 custom-op 内，避免 torch.compile
    # 把分支固定到单一符号化 M 的 backbone 编译产物。
    body = code[code.index("def memfabric_v2_routed_linear(") :]
    assert 'v2_threshold_route(int(x.shape[0])) == "stock"' in body
    # Stock 数学路径等价于 AscendUnquantizedLinearMethod.apply
    # （unquantized_gemm）再执行恰好一次 TP AllReduce。
    assert "partial = torch.ops.vllm.unquantized_gemm(x, weight, None)" in body
    assert "return tensor_model_parallel_all_reduce(partial)" in body
    assert "memfabric_mm_ar_v2_allreduce" in body
    assert "return out" in body
    assert "tensor_model_parallel_all_reduce(out)" not in body
    assert "MIN_FUSED_M = 2048" in code
    assert "MAX_FUSED_M = 4096" in code


def test_v1_route_is_unchanged_without_opt_in():
    router = ROUTER.read_text()
    assert "VLLM_ASCEND_310P_MEMFABRIC_MM_AR_V2_SERVE" in router
    assert "make_memfabric_mm_ar_v2_linear_method" in router
    assert "make_memfabric_mm_ar_linear_method()" in router
    assert '"MemFabricMmArV2LinearMethod310"' in V1PY.read_text()
    hook = HOOKS.read_text()
    assert "memfabric_mm_ar_pool_started() or v2_pool_started()" in hook


def test_v2_native_op_is_separately_compiled_and_graph_fail_closed():
    cpp = V2CPP.read_text()
    cmake = V2BUILD.read_text()
    assert "mf310p_v2_serve.cpp" in (V2SRC / "CMakeLists.txt").read_text()
    assert "memfabric_mm_ar_v2_allreduce(Tensor x, Tensor weight" in cpp
    assert "currentStreamCaptureStatusMayInitCtx" in cpp
    assert "FULL_DECODE_ONLY" in cpp
    assert "MTE3" in cpp
    assert "mf310p_v2_trial_run(" in cpp
    assert "mf310p_v2_trial_run_ar_only(" not in cpp


def test_pool_outputs_are_preallocated_and_live_views_not_overwritten():
    cpp = V2CPP.read_text()
    before = cpp.index("s.output_slots.reserve(")
    create = cpp.index("mf310p_v2_trial_create(")
    assert before < create
    # torch 2.10 的 TensorBase::storage() 返回 const Storage&，
    # 不会额外增加临时引用，空闲槽 StorageImpl 计数为 1，
    # 状态向量拥有唯一引用；模型视图会增加计数。
    assert "slot.storage().use_count() == 1" in cpp
    assert "use_count() == 2" not in cpp
    assert "V2 pooled outputs exhausted" in cpp
    assert "warm_outputs_locked(s, x, w, stream)" in cpp
    assert 'check_protocol(s, "prewarm")' in cpp
    # 非对齐尾块暂存路径必须在初始化时预热，
    # 冷 orchestrator 事件首先损坏的是首个暂存通信波。
    assert 'check_protocol(s, "staged-tail prewarm")' in cpp
    assert "s.input_stage.narrow(0, 0, tail_rows)" in cpp
    assert "std::atexit(&memfabric_mm_ar_v2_shutdown)" in cpp
    assert "output_slots.clear()" in cpp
    assert "memfabric_mm_ar_v2_shutdown()" in cpp
    adapter = V2ADAPTER.read_text()
    assert "trial_warmed_ys[64]" in adapter
    assert "trial_warmed_y_next" in adapter


def test_first_fused_call_is_numerically_verified_per_shape_class():
    code = V2PY.read_text()
    assert "_verified_aligned = False" in code
    assert "_verified_padded = False" in code
    assert "torch.equal(out, reference)" in code
    assert "first fused call verification FAILED" in code
    # Stock 参考验证每种形状仅重算一次原生 MM+HCCL，
    # 不能变成每调用都支付的固定开销。
    assert code.count("needs_verify") >= 2
    assert "tensor_model_parallel_all_reduce(torch.ops.vllm.unquantized_gemm(x, weight, None))" in code


def test_routing_is_an_opaque_custom_op_safe_for_compile_and_capture():
    code = V2PY.read_text()
    # custom-op 边界对 dynamo 不透明，编译后的 Prefill
    # 仍逐次执行其 Body；FULL_DECODE_ONLY 捕获期间
    # 则只记录 unquantized_gemm+HCCL，保持安全。
    assert 'torch.library.custom_op("vllm_ascend::memfabric_v2_routed_linear", mutates_args=())' in code
    assert "@memfabric_v2_routed_linear.register_fake" in code
    assert "return torch.empty_like(x)" in code
    apply = code[code.index("        def apply(") : code.index("    return MemFabricMmArV2LinearMethod310")]
    assert "return memfabric_v2_routed_linear(" in apply
    # Eager dummy/profile 必须回退 Stock，不允许
    # 通过启动虚拟输入提前创建 MemFabric 池。
    assert "memfabric_mm_ar_warmup_fallback_active() or v2_threshold_route" in code


def test_g3_tail_is_padded_and_multi_window_is_out_of_scope():
    cpp = V2CPP.read_text()
    assert "kMaxWindowRows = 4096" in cpp
    assert "V2 serving requires M <=4096 per Forward" in cpp
    assert "std::max<int64_t>(" in cpp
    assert "kThreshold," in cpp
    assert "s.input_stage.narrow(0, 0, rows).copy_(" in cpp
    assert "for (int64_t start = 0; start < rows; start += kMaxWindowRows)" not in cpp
    assert "return slot.narrow(0, 0, rows);" in cpp
    assert "src.narrow(0, 0, rows)" not in cpp
    assert "--max-num-batched-tokens" in cpp


def test_v2_opt_in_is_disabled_by_default():
    envs = (ROOT / "vllm_ascend/envs.py").read_text()
    assert "VLLM_ASCEND_310P_MEMFABRIC_MM_AR_V2_SERVE" in envs
    assert "VLLM_ASCEND_310P_MEMFABRIC_V2_CHUNK_ROWS" in envs
    assert "VLLM_ASCEND_310P_MEMFABRIC_V2_OUTPUT_SLOTS" in envs
    assert 'os.getenv("VLLM_ASCEND_310P_MEMFABRIC_MM_AR_V2_SERVE", "0")' in envs
