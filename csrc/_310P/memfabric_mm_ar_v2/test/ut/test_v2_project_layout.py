# SPDX-License-Identifier: Apache-2.0
"""V2 CANN 直调工程目录合同测试，不需要导入硬件扩展。

只验证目录及构建路径的结构，不证明设备正确性。
从仓库根目录执行：
  pytest -q csrc/_310P/memfabric_mm_ar_v2/test/ut/
"""

from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[5]
V2 = ROOT / "csrc/_310P/memfabric_mm_ar_v2"
SRC = V2 / "src"
DOCS = V2 / "docs"
TEST = V2 / "test"
CMAKE = ROOT / "cmake/memfabric_310p_v2.cmake"


def test_v2_root_has_canonical_layout_and_agents() -> None:
    # evidence/ 是脱敏证据目录，AGENTS.md 是2026-10-25主线新增的工程助手合同。
    assert {p.name for p in V2.iterdir() if not p.name.startswith(".")} == {
        "docs",
        "src",
        "test",
        "evidence",
        "AGENTS.md",
    }
    assert (V2 / "AGENTS.md").is_file()
    assert (DOCS / "ARCHIVE_20261019_G3.md").is_file()
    assert (DOCS / "V2_G3_QWEN_E2E_REPORT.md").is_file()


def test_cann_direct_invoke_source_manifest_and_kernel_identity() -> None:
    manifest = (SRC / "CMakeLists.txt").read_text(encoding="utf-8")
    parent = CMAKE.read_text(encoding="utf-8")
    assert 'include("${_dir}/CMakeLists.txt")' in parent
    for rel in (
        "op_kernel/mf310p_v2_kernel.asc",
        "op_kernel/mf310p_v2_diagnostics.asc",
        "op_kernel/mf310p_v2_kernel_common.h",
        "op_host/mf310p_v2_trial_internal.h",
        "op_host/mf310p_v2_trial_diagnostics.cpp",
        "op_host/mf310p_v2_trial_diagnostics_api.h",
        "torch_adapter/mf310p_v2_serve_diagnostics.cpp",
        "torch_adapter/mf310p_v2_serve_internal.h",
        "op_host/mf310p_v2_trial_adapter.cpp",
        "op_host/mf310p_v2_trial_adapter_api.h",
        "torch_adapter/mf310p_v2_serve.cpp",
    ):
        assert (SRC / rel).is_file(), rel
    for stem in (
        "op_kernel/mf310p_v2_kernel.asc",
        "op_host/mf310p_v2_trial_adapter.cpp",
        "torch_adapter/mf310p_v2_serve.cpp",
    ):
        assert stem in manifest, stem
    assert "--npu-arch=dav-2002" in parent
    assert "MF310P_V2_KERNEL_SOURCES" in parent
    assert "VLLM_ASCEND_310P_V2_BUILD_DIAGNOSTICS" in manifest
    assert "mf310p_v2_diagnostics.asc" in manifest
    assert "MF310P_V2_HOST_SOURCES" in parent
    assert "npu_op_package(" not in manifest  # 有意保留已验证的直调 ABI


def test_host_relative_include_and_torch_abi_resolve() -> None:
    host = SRC / "op_host/mf310p_v2_trial_adapter.cpp"
    text = host.read_text(encoding="utf-8")
    assert '#include "mf310p_v2_trial_internal.h"' in text
    internal = (SRC / "op_host/mf310p_v2_trial_internal.h").read_text()
    assert '#include "../../../memfabric_mm_ar/memfabric310p_adapter_api.h"' in internal
    assert (host.parent / "../../../memfabric_mm_ar/memfabric310p_adapter_api.h").is_file()
    bridge = (SRC / "torch_adapter/mf310p_v2_serve.cpp").read_text()
    assert '#include "mf310p_v2_serve_internal.h"' in bridge
    assert (SRC / "op_host/mf310p_v2_trial_adapter_api.h").is_file()
    assert "memfabric_mm_ar_v2_allreduce(Tensor x, Tensor weight" in bridge


def test_v2_tests_live_locally_but_v1_shared_harness_is_untouched() -> None:
    assert (TEST / "ut/test_memfabric_mm_ar_v2_serve_source.py").is_file()
    assert (TEST / "e2e/tp2_v2_mixed_route_correctness.py").is_file()
    assert (TEST / "e2e/evaluate_v2_g3.py").is_file()
    assert (TEST / "g3_e2e_scripts/regression.sh").is_file()
    assert (TEST / "t1_nz_direct_trial.py").is_file()
    assert (TEST / "t2_ar_stress.py").is_file()
    for old in (
        "tests/ut/_310p/test_memfabric_mm_ar_v2_serve_source.py",
        "tests/e2e/_310p/tp2_v2_mixed_route_correctness.py",
        "tests/e2e/_310p/evaluate_v2_g3.py",
    ):
        assert not (ROOT / old).exists(), old
    assert (ROOT / "tests/e2e/_310p/test_memfabric_mm_ar_functional.py").is_file()
    assert (ROOT / "tests/e2e/_310p/memfabric_mm_ar_serve_lib.py").is_file()
