# SPDX-License-Identifier: Apache-2.0
"""CPU-only test of the Stock+DFlash environment classifier; no vllm import."""
from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import dflash_stock_preflight as m  # noqa: E402


def good():
    versions = dict(m.PINNED)
    versions.update({key: "installed" for key in m.OBSERVE})
    return versions


class TestDflashStockPreflight(unittest.TestCase):
    def test_nominal_exact_contract(self):
        got = m.verdict(good(), {"vllm/dflash": True},
                        {flag: "0" for flag in m.STOCK_DISABLED_FLAGS},
                        {"target": True, "draft": True})
        self.assertTrue(got["static_go"])

    def test_version_or_api_missing_fails(self):
        v = good()
        v["vllm"] = "0.23.0"
        got = m.verdict(v, {"vllm/v1/spec_decode/dflash.py": False},
                        {}, {"target": True, "draft": True})
        self.assertFalse(got["static_go"])
        self.assertTrue(any("PIN_MISMATCH:vllm" in x for x in got["blockers"]))
        self.assertTrue(any("MISSING_SOURCE_API" in x for x in got["blockers"]))

    def test_stock_must_disable_fused(self):
        got = m.verdict(good(), {}, {m.STOCK_DISABLED_FLAGS[0]: "YES"},
                        {"target": True, "draft": True})
        self.assertFalse(got["static_go"])
        self.assertTrue(any("STOCK_CONTAMINATED" in x for x in got["blockers"]))

    def test_missing_model_config_is_not_missing_python_dependency(self):
        got = m.verdict(good(), {}, {}, {"target": True, "draft": False})
        self.assertEqual(got["blockers"], ["MODEL_CONFIG_MISSING:draft"])

    def test_optional_upstream_declaration_not_hard_failure(self):
        v = good()
        v["triton-ascend"] = None
        got = m.verdict(v, {}, {}, {"target": True, "draft": True})
        self.assertTrue(got["static_go"])
        self.assertTrue(any("triton-ascend" in x for x in got["observations"]))

    def test_pip_check_waives_only_exact_known_server_state(self):
        known_missing = (
            "vllm-ascend 0.1.dev requires triton-ascend, which is not installed.\n"
            "vllm-ascend 0.1.dev requires quart, which is not installed.\n"
            "ms-service-profiler 26.0.0 requires pandas, which is not installed.\n")
        self.assertEqual(m.classify_pip_check(1, known_missing), "KNOWN_SERVER_STATE_WAIVED")
        stale_conflict = (
            "vllm-ascend 0.1.dev has requirement torch-npu==2.10.0.post2, "
            "but you have torch-npu 2.10.0.post4.\n")
        self.assertEqual(m.classify_pip_check(1, stale_conflict), "KNOWN_SERVER_STATE_WAIVED")
        self.assertEqual(m.classify_pip_check(1, known_missing + stale_conflict
                                              + "te 0.4.0 is not supported on this platform\n"),
                         "KNOWN_SERVER_STATE_WAIVED")
        # Unknown missing package, any triton version conflict and foreign
        # conflicts stay hard blockers.
        self.assertEqual(
            m.classify_pip_check(1, known_missing + "vllm 0.24.0 requires torch, which is not installed."),
            "UNEXPECTED_DEPENDENCY_BREAKAGE")
        self.assertEqual(
            m.classify_pip_check(1, "vllm requires triton>=3.2, but you have triton 3.1."),
            "UNEXPECTED_DEPENDENCY_BREAKAGE")
        self.assertEqual(
            m.classify_pip_check(1, stale_conflict
                                 + "vllm 0.27.1 has requirement torch==2.10.0, but you have torch 2.9.0."),
            "UNEXPECTED_DEPENDENCY_BREAKAGE")
        self.assertEqual(m.classify_pip_check(0, "No broken requirements found."), "PASS")
        self.assertEqual(m.classify_pip_check(1, ""), "UNREADABLE")
        # The same missing optional package declared by ANOTHER distribution
        # must never be silently waived as an unrelated host-tool optional.
        self.assertEqual(m.classify_pip_check(1,
            "vllm 0.27.1 requires pandas, which is not installed."),
            "UNEXPECTED_DEPENDENCY_BREAKAGE")
        self.assertEqual(m.classify_pip_check(1,
            "vllm-ascend 0.1.dev has requirement transformers==5.13.0, "
            "but you have transformers 5.14.0."),
            "UNEXPECTED_DEPENDENCY_BREAKAGE")
        self.assertEqual(m.classify_pip_check(1,
            "vllm-ascend 0.1.dev has requirement fastapi<0.124.0, "
            "but you have fastapi 0.136.3."),
            "KNOWN_SERVER_STATE_WAIVED")

    def test_probe_imports_follow_real_serve_order(self):
        # The cold-order probe defect (circular device_op import, missing FLA
        # alias) was found on-device 2026-10-25; the probe must mirror serve.
        self.assertIn("import vllm_ascend.platform", m.PROBE_IMPORTS)
        self.assertIn("pre_register_and_update(None)", m.PROBE_IMPORTS)
        self.assertIn("import vllm_ascend._310p.model_runner_310p", m.PROBE_IMPORTS)
        self.assertLess(m.PROBE_IMPORTS.index("pre_register_and_update"),
                        m.PROBE_IMPORTS.index("model_runner_310p"))

    def test_source_api_scan_does_not_import_external_package(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            (root / "v1/worker").mkdir(parents=True)
            (root / "v1/worker/cp_utils.py").write_text(
                "def get_total_cp_world_size(): return 1\n", encoding="utf-8")
            got = m.check_api({"vllm": root, "vllm_ascend": None})
            self.assertTrue(got["vllm/cp_utils.get_total_cp_world_size"])
            self.assertFalse(got["vllm/model_executor/models/qwen3_dflash.py"])

    def test_cp_utils_v027_lane_requires_our_compat_shim(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            (root / "v1/worker").mkdir(parents=True)
            (root / "v1/worker/cp_utils.py").write_text(
                "from vllm.distributed import get_dcp_group\n", encoding="utf-8")
            no_shim = m.check_api({"vllm": root, "vllm_ascend": None})
            self.assertFalse(no_shim["vllm/cp_utils.get_total_cp_world_size"])
            ascend = root / "ascend"
            (ascend / "_310p").mkdir(parents=True)
            (ascend / "_310p/model_runner_310p.py").write_text(
                "try:\n    from vllm.v1.worker.cp_utils import get_total_cp_world_size\n"
                "except ImportError:\n    from vllm.v1.worker.cp_utils import get_dcp_group\n",
                encoding="utf-8")
            with_shim = m.check_api({"vllm": root, "vllm_ascend": ascend})
            self.assertTrue(with_shim["vllm/cp_utils.get_total_cp_world_size"])


if __name__ == "__main__":
    unittest.main()
