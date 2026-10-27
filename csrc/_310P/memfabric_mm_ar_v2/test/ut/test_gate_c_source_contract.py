# SPDX-License-Identifier: Apache-2.0
"""Gate C CPU-only 源码合同测试：不需要 CANN / torch_npu。"""

import json
import runpy
from pathlib import Path

V2 = Path(__file__).resolve().parents[2]
SRC = V2 / "src"
PERF = V2 / "test/perf"


def test_gate_c_is_optional_diag_only_and_old_abi_preserved():
    api = (SRC / "op_host/mf310p_v2_trial_diagnostics_api.h").read_text()
    host = (SRC / "op_host/mf310p_v2_trial_diagnostics.cpp").read_text()
    production = (SRC / "op_host/mf310p_v2_trial_adapter.cpp").read_text()
    prod_kernel = (SRC / "op_kernel/mf310p_v2_kernel.asc").read_text()
    cmake = (SRC / "CMakeLists.txt").read_text()
    assert "mf310p_v2_trial_diag_loop4(" in api
    assert "mf310p_v2_trial_diag_loop4(" in host
    assert "mf310p_v2_trial_diag(" in api  # 旧接口保留
    assert "mf310p_v2_trial_diag_loop4(" not in production
    assert "mf310p_v2_trial_diag_loop4(" not in prod_kernel
    assert "VLLM_ASCEND_310P_V2_BUILD_DIAGNOSTICS" in cmake
    assert "if(VLLM_ASCEND_310P_V2_BUILD_DIAGNOSTICS)" in cmake
    assert "mf310p_v2_trial_diagnostics.cpp" in cmake


def test_loop4_lock_and_no_fake_multichunk_ibdone16():
    host = (SRC / "op_host/mf310p_v2_trial_diagnostics.cpp").read_text()
    section = host.split('extern "C" int mf310p_v2_trial_diag_loop4(', 1)[1]
    kernel = (SRC / "op_kernel/mf310p_v2_diagnostics.asc").read_text()
    assert "kGateCChunks = 4" in section
    assert "kGateCChunkRows = 1024" in section
    assert "s->chunk_rows != kGateCChunkRows" in section
    assert "kGateCChunkRows, kGateCChunks, 0, enable" in section
    assert "aclrtMemsetAsync(" in section
    assert "aclrtSynchronizeDevice()" in section
    assert "gatec_loop4_real_launched" in section
    assert "mf310p_v2_diag_loop_async(" in section
    assert "(void)overlap" in kernel
    assert "V2DiagLoopKernel" in kernel
    # diag6 只处理第一个 chunk，不能写成全形状 4 Chunk 等价探针。
    assert "diag == 6" in host
    assert "s->chunk_rows, 1, 0, en, acl_stream" in host


def test_driver_validates_status_and_bit_exact_before_profile():
    runner = (PERF / "gate_c_loop4.py").read_text()
    assert "choices=CASES" in runner
    assert '"formal", "loop4"' in runner
    assert "WORLD_SIZE" in runner
    assert "torch.equal(y, ref)" in runner
    assert "mf310p_v2_trial_check_status" in runner
    assert "must(lib.mf310p_v2_trial_ib_probe" in runner
    assert "with profile(" in runner
    assert "FileExistsError" in runner


def test_offline_trace_parser_filters_exact_symbol_and_kernel_count(tmp_path):
    # runpy 不触发 main()；无需导入 torch。
    mod = runpy.run_path(str(PERF / "analyze_gate_c_trace.py"))
    file = tmp_path / "trace.json"
    file.write_text(
        json.dumps(
            {
                "traceEvents": [
                    {"ph": "X", "name": "_ZN12_GLOBAL__V2TrialKernel_impl", "dur": 2571},
                    {"ph": "X", "name": "_ZN12_GLOBAL__V2DiagLoopKernel_impl", "dur": 3100},
                    {"ph": "X", "name": "HybmSdmaOrchEpoch", "dur": 15000000},
                ]
            }
        )
    )
    assert mod["collect"](file, "V2TrialKernel") == [2571.0]
    assert mod["collect"](file, "V2DiagLoopKernel") == [3100.0]


def test_gatec_source_shape_and_chunk_not_confused():
    runner = (PERF / "gate_c_loop4.py").read_text()
    assert "M = 4096" in runner
    assert "CHUNK = 1024" in runner
    assert 'is_single = a.case.startswith("diag")' in runner
    assert 'overlap": 1 if a.case == "formal" else 0' in runner


def test_aic_metrics_optin_and_pipe_parser_fail_closed(tmp_path):
    runner = (PERF / "gate_c_loop4.py").read_text()
    # 管道计数器必须是 --profile 的附加 opt-in，不改变默认采集路径。
    assert "--aic-metrics" in runner
    assert "requires --profile" in runner
    assert "PipeUtilization" in runner
    assert "ASCEND_WORK_PATH" in runner
    mod = runpy.run_path(str(PERF / "analyze_gate_c_trace.py"))
    # 数量不符必须拒绝给结论（fail-closed），与 Kernel 计数同一纪律。
    bad = tmp_path / "prof_rank0" / "profiling_data" / "w_ascend_pt" / "PROF_x" / "mindstudio_profiler_output"
    bad.mkdir(parents=True)
    (bad / "op_summary_1.csv").write_text(
        "Op Name,Task Start Time(us),mte2_time(us)\nV2TrialKernel,1,1.0\nV2TrialKernel,2,2.0\n"
    )
    try:
        mod["collect_pipe"](tmp_path / "prof_rank0", "V2TrialKernel", 3)
        raise AssertionError("expected fail-closed ValueError")
    except ValueError as exc:
        assert "expected 3" in str(exc)
    # legacy diag 包装器：每三元组末位为真实发射，数量必须整除。
    cols = (
        "Op Name,Task Start Time(us),Task Duration(us),aicore_time(us),"
        "mte2_time(us),scalar_time(us),mac_time(us),vec_time(us),"
        "mte1_time(us),mte3_time(us),total_cycles\n"
    )
    mk = lambda t, d, m: (f"V2DiagIbKernel,{t},{d},{d},{m},{m},{m},{m},{m},{m},{int(t) * 100}\n")
    (bad / "op_summary_1.csv").write_text(
        cols
        + "".join(
            [mk(1, 0.5, 0.5), mk(2, 0.6, 0.6), mk(3, 7.0, 7.0), mk(4, 0.5, 0.5), mk(5, 0.6, 0.6), mk(6, 8.0, 8.0)]
        )
    )
    got = mod["collect_pipe"](tmp_path / "prof_rank0", "V2DiagIbKernel", 2, 3)
    assert got["mte2_time(us)"]["p50"] == 7.5
    try:
        mod["collect_pipe"](tmp_path / "prof_rank0", "V2DiagIbKernel", 2, 4)
        raise AssertionError("expected divisibility ValueError")
    except ValueError as exc:
        assert "not divisible" in str(exc)
