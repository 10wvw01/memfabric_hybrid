# SPDX-License-Identifier: Apache-2.0
"""核查正式算子与诊断/验证能力已从各层实现文件中隔离。

仅检查源码和构建分组；不能替代 bisheng、310P3 正确性或稳定性测试。
"""
from __future__ import annotations

from pathlib import Path

SRC = Path(__file__).resolve().parents[2] / "src"


def _read(path: str) -> str:
    return (SRC / path).read_text(encoding="utf-8")


def test_production_kernel_excludes_t1_t2_and_stage_diagnostics():
    prod = _read("op_kernel/mf310p_v2_kernel.asc")
    diag = _read("op_kernel/mf310p_v2_diagnostics.asc")
    common = _read("op_kernel/mf310p_v2_kernel_common.h")
    assert 'mf310p_v2_trial_launch_async(' in prod
    assert 'mf310p_v2_ib_probe_async(' in prod
    assert 'mf310p_v2_ib_fan_probe_async(' in prod
    assert 'V2TrialKernel<' in prod
    for forbidden in ("V2DiagMmKernel", "V2NzDirectSmallKernel",
                      "V2MemfabricArOnlyKernel", "V2ArStageKernel",
                      "mf310p_v2_ar_only_launch_async"):
        assert forbidden not in prod, forbidden
    for required in ("V2DiagMmKernel", "V2NzDirectSmallKernel",
                     "V2MemfabricArOnlyKernel", "V2ArStageKernel",
                     "mf310p_v2_ar_only_launch_async"):
        assert required in diag, required
    assert 'CleanLines(' in common
    assert 'WaitReady(' in common
    assert '#include "mf310p_v2_kernel_common.h"' in prod
    assert '#include "mf310p_v2_kernel_common.h"' in diag


def test_production_host_exports_no_t1_t2_or_address_probes():
    prod = _read("op_host/mf310p_v2_trial_adapter.cpp")
    diag = _read("op_host/mf310p_v2_trial_diagnostics.cpp")
    internal = _read("op_host/mf310p_v2_trial_internal.h")
    api = _read("op_host/mf310p_v2_trial_adapter_api.h")
    diag_api = _read("op_host/mf310p_v2_trial_diagnostics_api.h")
    for symbol in (
        "mf310p_v2_trial_create(", "mf310p_v2_trial_ib_probe(",
        "mf310p_v2_trial_run(", "mf310p_v2_trial_check_status(",
        "mf310p_v2_trial_destroy(",
    ):
        assert symbol in prod and symbol in api
    for symbol in (
        "mf310p_v2_trial_run_nz_direct_small(",
        "mf310p_v2_trial_run_ar_only(",
        "mf310p_v2_trial_read_send_arena(",
        "mf310p_v2_trial_diag(",
    ):
        assert symbol not in prod
        assert symbol not in api
        assert symbol in diag and symbol in diag_api
    assert "struct mf310p_v2_trial {" in internal
    assert "trial_warmed_ys[64]" in internal


def test_serving_torch_op_is_independent_from_optional_debug_status():
    prod = _read("torch_adapter/mf310p_v2_serve.cpp")
    diag = _read("torch_adapter/mf310p_v2_serve_diagnostics.cpp")
    internal = _read("torch_adapter/mf310p_v2_serve_internal.h")
    assert 'memfabric_mm_ar_v2_allreduce(Tensor x, Tensor weight' in prod
    assert 'memfabric_mm_ar_v2_shutdown() -> ()' in prod
    assert 'memfabric_mm_ar_v2_debug_status' not in prod
    assert 'memfabric_mm_ar_v2_debug_status() -> int' in diag
    assert "v2_serve_state()" in diag
    assert "struct V2ServeState" in internal


def test_build_supports_diagnostics_on_and_off():
    src = _read("CMakeLists.txt")
    parent = (SRC.parents[3] / "cmake/memfabric_310p_v2.cmake").read_text()
    assert 'option(VLLM_ASCEND_310P_V2_BUILD_DIAGNOSTICS' in src
    assert 'op_kernel/mf310p_v2_kernel.asc' in src
    assert 'op_host/mf310p_v2_trial_adapter.cpp' in src
    assert 'torch_adapter/mf310p_v2_serve.cpp' in src
    begin = src.index("if(VLLM_ASCEND_310P_V2_BUILD_DIAGNOSTICS)")
    extra = src[begin:]
    for file in ("op_kernel/mf310p_v2_diagnostics.asc",
                 "op_host/mf310p_v2_trial_diagnostics.cpp",
                 "torch_adapter/mf310p_v2_serve_diagnostics.cpp"):
        assert file in extra
        assert file not in src[:begin]
    assert 'MF310P_V2_KERNEL_SOURCES' in parent
    assert 'MF310P_V2_HOST_SOURCES' in parent
