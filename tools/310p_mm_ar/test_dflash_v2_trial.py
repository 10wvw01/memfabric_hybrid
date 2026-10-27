# SPDX-License-Identifier: Apache-2.0
"""Import-free / NPU-free contract regression for DFlash K7 vs V2 trial driver."""
from __future__ import annotations

import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import dflash_v2_trial as trial  # noqa: E402


def case(arm="stock", mode="eager", phase="smoke"):
    return trial.build_plan(
        arm=arm, mode=mode, phase=phase, devices="6,7",
        port=8123, store_port=8581, orch_json=(
            "/opt/safe/libmf_sdma_orch_v11.json" if arm == "v2" else None),
        output_dir=Path("/tmp/r1-dflash-k7-" + arm + "-" + mode + "-" + phase),
    )


class K7TrialContract(unittest.TestCase):
    def test_stock_disable_and_matching_dflash(self):
        p = case("stock")
        self.assertEqual(p["speculative"]["method"], "dflash")
        self.assertEqual(p["speculative"]["num_speculative_tokens"], 7)
        self.assertEqual(p["speculative"]["model"], trial.DRAFT)
        self.assertEqual(trial.DRAFT, "/home/models/Qwen/Qwen3.6-35B-A3B-DFlash")
        self.assertEqual(set(p["speculative"]), {"method", "model", "num_speculative_tokens"})
        self.assertEqual(trial.TARGET, "/home/models/Qwen/Qwen3.6-35B-A3B-w8a8")
        serve = p["server_command"]
        self.assertEqual(serve[serve.index("--host") + 1], "0.0.0.0")
        self.assertEqual(serve[serve.index("--max-num-seqs") + 1], "16")
        self.assertEqual(serve[serve.index("--gpu-memory-utilization") + 1], "0.90")
        self.assertNotIn("--safetensors-load-strategy", serve)
        self.assertEqual(p["TP"], 2)
        self.assertEqual(p["env_overrides"]["VLLM_ASCEND_310P_ENABLE_MEMFABRIC_MM_AR"], "0")
        self.assertEqual(p["env_overrides"]["VLLM_ASCEND_310P_MEMFABRIC_MM_AR_V2_SERVE"], "0")
        self.assertNotIn("MF_SDMA_ORCH_JSON", p["env_overrides"])

    def test_v2_explicit_kver_and_same_model(self):
        a, b = case("stock"), case("v2")
        self.assertEqual(a["speculative"], b["speculative"])
        self.assertEqual(a["benchmark_command"], b["benchmark_command"][:]
                         if a["output_dir"] == b["output_dir"] else
                         [a["output_dir"] if x == b["output_dir"] else x
                          for x in b["benchmark_command"]])
        self.assertEqual(b["env_overrides"]["VLLM_ASCEND_310P_ENABLE_MEMFABRIC_MM_AR"], "1")
        self.assertEqual(b["env_overrides"]["VLLM_ASCEND_310P_MEMFABRIC_MM_AR_V2_SERVE"], "1")
        self.assertIn("VLLM_ASCEND_310P_MEMFABRIC_V2_CHUNK_ROWS", b["env_overrides"])
        with self.assertRaisesRegex(ValueError, "KVER11"):
            trial.build_plan(arm="v2", mode="eager", phase="smoke", devices="6,7",
                             port=8123, store_port=8581, orch_json=None,
                             output_dir=Path("/tmp/new"))

    def test_graph_descriptor_multiple_of_eight_and_no_smuggling(self):
        p = case("v2", mode="graph")
        compiler = p["server_command"][p["server_command"].index("--compilation-config") + 1]
        loaded = json.loads(compiler)
        self.assertEqual(loaded["cudagraph_mode"], "FULL_DECODE_ONLY")
        self.assertTrue(all(size % 8 == 0 for size in loaded["cudagraph_capture_sizes"]))
        self.assertIn(80, loaded["cudagraph_capture_sizes"])
        self.assertNotIn(10, loaded["cudagraph_capture_sizes"])

    def test_smoke_vs_full_dataset_contract(self):
        smoke, full = case(), case(phase="full")
        self.assertEqual(smoke["expected_requests"], 2)
        self.assertEqual(full["expected_requests"], 50)
        for cmd, out_len in ((smoke["benchmark_command"], "32"),
                             (full["benchmark_command"], "2048")):
            self.assertEqual(cmd[cmd.index("--random-input-len") + 1], "4096")
            self.assertEqual(cmd[cmd.index("--random-output-len") + 1], out_len)
            self.assertIn("--ignore-eos", cmd)
        self.assertEqual(full["benchmark_command"][full["benchmark_command"].index("--max-concurrency") + 1], "10")
        self.assertEqual(full["benchmark_command"][full["benchmark_command"].index("--tokenizer") + 1], trial.TARGET)
        self.assertNotIn("--num-warmups", full["benchmark_command"])


    def test_bad_multiprocess_and_worktree_is_rejected(self):
        with self.assertRaises(ValueError):
            trial.build_plan(arm="stock", mode="eager", phase="smoke",
                             devices="0,0", port=8123, store_port=8581,
                             orch_json=None, output_dir=Path("/tmp/a"))
        with self.assertRaises(ValueError):
            trial.build_plan(arm="stock", mode="eager", phase="smoke",
                             devices="0,1", port=8123, store_port=8123,
                             orch_json=None, output_dir=Path("/tmp/a"))

    def test_stock_contamination_never_passes(self):
        m = case()
        clean = trial.classify(m, "normal stock serve", {"completed": 2})
        self.assertEqual(clean["verdict"], "STOCK_EAGER_SERVING_PASS")
        bad = trial.classify(m, "[mf310p-v2] V2 MM+MemFabric MemFabricMmArV2LinearMethod310",
                             {"completed": 2})
        self.assertEqual(bad["verdict"], "INVALID_STOCK_CONTAMINATED")

    def test_v2_requires_fused_route_and_fresh_correctness(self):
        m = case("v2")
        self.assertEqual(trial.classify(m, "only native stock", {"completed": 2})["verdict"],
                         "V2_NOT_PROVEN_STOCK_FALLBACK_POSSIBLE")
        true_log = ("Select MemFabricMmArV2LinearMethod310 "
                    "[mf310p-v2] M=4096 within [2048,4096]: V2 MM+MemFabric "
                    "[mf310p-v2] first fused call verified bit-exact")
        verdict = trial.classify(m, true_log, {"completed": 2})
        self.assertIn("ROUTE_HINT_ONLY", verdict["verdict"])
        self.assertEqual(verdict["actual_npu_fused_kernel_trace"], "NOT_PROVIDED")

    def test_v2_graph_must_not_claim_graph_fused(self):
        m = case("v2", mode="graph")
        log = ("Select MemFabricMmArV2LinearMethod310 "
               "[mf310p-v2] M=4096 V2 MM+MemFabric "
               "[mf310p-v2] first fused call verified bit-exact "
               "event=replay component=target rank=0")
        out = trial.classify(m, log, {"completed": 2})
        self.assertEqual(out["verdict"], "V2_PREFILL_HINT_ONLY_GRAPH_FUSED_REPLAY_NOT_PROVEN")

    def test_incomplete_and_device_error_are_fail_stop(self):
        m = case("v2")
        for log, bench in (("507014", {"completed": 2}), ("", {"completed": 1}), ("", {})):
            self.assertTrue(trial.classify(m, log, bench)["verdict"].startswith("FAIL"))

    def test_environment_drops_inherited_memfabric(self):
        from unittest.mock import patch
        with patch.dict("os.environ", {
            "VLLM_ASCEND_310P_MEMFABRIC_V2_CHUNK_ROWS": "256",
            "VLLM_ASCEND_310P_ENABLE_MEMFABRIC_MM_AR": "1",
            "MF_SDMA_ORCH_JSON": "/wrong.json",
            "DUMMY_UNRELATED": "safe",
        }):
            env = trial.clean_environment(case("stock")["env_overrides"])
        self.assertNotIn("MF_SDMA_ORCH_JSON", env)
        self.assertNotIn("VLLM_ASCEND_310P_MEMFABRIC_V2_CHUNK_ROWS", env)
        self.assertEqual(env["VLLM_ASCEND_310P_ENABLE_MEMFABRIC_MM_AR"], "0")
        self.assertEqual(env["DUMMY_UNRELATED"], "safe")

    def test_graph_requires_extra_readiness(self):
        genuine_g0 = {"mode": "STOCK_DFLASH_G0_READ_ONLY",
                      "num_speculative_tokens": 7,
                      "python": sys.executable,
                      "verdict": {"static_go": True, "blockers": []},
                      "model_paths": {"target": trial.TARGET, "draft": trial.DRAFT},
                      "probes": {"pip_check": {"classification": "PASS"},
                                 "isolated_import_probe": {"exit_code": 0}}}
        with self.assertRaisesRegex(RuntimeError, "Graph research"):
            trial.execute(case("stock", mode="graph"), True, True, True,
                          readonly_preflight=genuine_g0)

    def test_preflight_missing_and_bad_status_fail_stop(self):
        with self.assertRaisesRegex(RuntimeError, "wrong G0"):
            trial.validate_readonly_preflight({})
        valid = {"mode": "STOCK_DFLASH_G0_READ_ONLY",
                 "num_speculative_tokens": 7,
                 "verdict": {"static_go": True, "blockers": []},
                 "model_paths": {"target": trial.TARGET, "draft": trial.DRAFT},
                 "probes": {"pip_check": {"exit_code": 0, "classification": "PASS"},
                            "isolated_import_probe": {"exit_code": 0}}}
        trial.validate_readonly_preflight(valid)
        valid["probes"]["pip_check"]["classification"] = "KNOWN_SERVER_STATE_WAIVED"
        trial.validate_readonly_preflight(valid)
        valid["verdict"]["static_go"] = False
        with self.assertRaisesRegex(RuntimeError, "G0 not passed"):
            trial.validate_readonly_preflight(valid)


    def test_vllm_ascend_resolves_to_same_isolated_tree(self):
        self.assertFalse(trial.vllm_ascend_outside_this_worktree(
            "/wt/tools/310p_mm_ar/dflash_v2_trial.py", Path("/wt/vllm_ascend")))
        self.assertTrue(trial.vllm_ascend_outside_this_worktree(
            "/wt/tools/310p_mm_ar/dflash_v2_trial.py", Path("/elsewhere/vllm_ascend")))
        self.assertTrue(trial.vllm_ascend_outside_this_worktree(
            "/wt/tools/310p_mm_ar/dflash_v2_trial.py", None))

    def test_missing_v2_build_outputs_are_detected_without_import(self):
        from tempfile import TemporaryDirectory
        with TemporaryDirectory() as base:
            root = Path(base)
            pkg = root / "vllm_ascend"
            pkg.mkdir()
            self.assertEqual(len(trial.missing_v2_artifacts(root)), 3)
            (pkg / "libmf310p_v2_trial_device.so").touch()
            (pkg / "libmf310p_device.so").touch()
            (pkg / "vllm_ascend_C.cpython-312-aarch64-linux-gnu.so").touch()
            self.assertEqual(trial.missing_v2_artifacts(root), [])

    def test_g0_execution_requires_serve_order_import_and_paths(self):
        good = {"mode": "STOCK_DFLASH_G0_READ_ONLY",
                "num_speculative_tokens": 7,
                "verdict": {"static_go": True, "blockers": []},
                "model_paths": {"target": trial.TARGET, "draft": trial.DRAFT},
                "probes": {"pip_check": {"classification": "KNOWN_SERVER_STATE_WAIVED",
                                          "unexpected_lines": []},
                           "isolated_import_probe": {"exit_code": 0}}}
        trial.validate_readonly_preflight(good)
        good["probes"]["isolated_import_probe"]["exit_code"] = 1
        with self.assertRaisesRegex(RuntimeError, "import smoke"):
            trial.validate_readonly_preflight(good)
        good["probes"]["isolated_import_probe"]["exit_code"] = 0
        good["model_paths"]["draft"] = "/unverified/model"
        with self.assertRaisesRegex(RuntimeError, "model paths"):
            trial.validate_readonly_preflight(good)


    def test_console_script_belongs_to_same_python(self):
        from tempfile import TemporaryDirectory
        with TemporaryDirectory() as td:
            path = Path(td) / "vllm"
            path.write_text("#!" + sys.executable + "\nprint('stub')\n")
            self.assertTrue(trial.console_script_matches_python(str(path), sys.executable))
            path.write_text("#!/not-existing/python\n")
            self.assertFalse(trial.console_script_matches_python(str(path), sys.executable))
            path.write_text("not-a-shebang\n")
            self.assertFalse(trial.console_script_matches_python(str(path), sys.executable))

    def test_g0_version_source_and_console_binding(self):
        from unittest.mock import patch
        from tempfile import TemporaryDirectory
        with TemporaryDirectory() as td:
            script = Path(td) / "vllm"
            script.write_text("#!" + sys.executable + "\n")
            fake_root = Path(td) / "vllm_ascend"
            fake_root.mkdir()
            from types import SimpleNamespace
            wanted = {"vllm":"0.27.1+empty", "torch":"2.10.0",
                      "torch-npu":"2.10.0.post4","transformers":"5.17.0"}
            installed = dict(wanted)
            report = {"python": sys.executable, "package_roots": {
                "vllm_ascend":str(fake_root), "vllm":"/fake/vllm"},
                "versions":wanted}
            with (patch("importlib.metadata.version", side_effect=lambda k:installed[k]),
                  patch("importlib.util.find_spec",return_value=SimpleNamespace(
                      submodule_search_locations=["/fake/vllm"]))):
                trial.validate_runtime_identity(report, fake_root, str(script))
                report["versions"]["transformers"] = "5.13.0"
                with self.assertRaisesRegex(RuntimeError,"distribution changed"):
                    trial.validate_runtime_identity(report,fake_root,str(script))
                report["versions"]["transformers"] = "5.17.0"
                report["package_roots"]["vllm_ascend"] = "/wrong/tree"
                with self.assertRaisesRegex(RuntimeError,"import root changed"):
                    trial.validate_runtime_identity(report,fake_root,str(script))

    def test_execute_is_gated_before_file_or_server_work(self):
        with self.assertRaisesRegex(RuntimeError, "G0"):
            trial.execute(case("stock"), False, False, False)


if __name__ == "__main__":
    unittest.main()
