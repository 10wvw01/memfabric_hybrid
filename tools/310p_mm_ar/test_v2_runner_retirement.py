"""Regression: 2026-10-26 project scope retired V2 Runner reconstruction.

Pure stdlib checks. Runtime/device safety still requires separate validation.
"""
from pathlib import Path
import unittest

ROOT=Path(__file__).resolve().parents[2]
D=ROOT/"csrc/_310P/memfabric_mm_ar_v2/docs"

class TestV2RunnerRetirement(unittest.TestCase):
    def test_experimental_triton_waiver_is_gone(self):
        s=(ROOT/"vllm_ascend/patch/platform/patch_use_v2_model_runner.py").read_text()
        self.assertNotIn("EXPERIMENTAL_V2_NO_TRITON",s)
        self.assertNotIn("_patched_validate_v2_model_runner",s)
        self.assertNotIn("VllmConfig._validate_v2_model_runner =",s)
        self.assertIn("VLLM_USE_V2_MODEL_RUNNER",s)
    def test_unsupported_v2_still_rejects_before_device_init(self):
        s=(ROOT/"vllm_ascend/_310p/worker_310p.py").read_text()
        self.assertLess(s.index("if self.vllm_config.use_v2_model_runner:"),
                        s.index("self.device = self._init_device()"))
        self.assertIn("refusing to execute the V1 worker as V2",s)
    def test_former_r2_runner_plan_is_not_active(self):
        self.assertFalse((D/"V2_RUNNER_TRITON_FREE_MASTER_PLAN_20261025.md").exists())
        root=(ROOT/"PROJECT_310P_MEMFABRIC.md").read_text()
        self.assertNotIn("[R2 V2 Runner无Triton全量任务锁定]",root)
        self.assertIn("ADR-013",root)
    def test_fusion_serving_assets_untouched(self):
        s=(ROOT/"vllm_ascend/_310p/ops/memfabric_mm_ar_v2_serve.py").read_text()
        self.assertIn("memfabric_mm_ar_v2_allreduce",s)
        self.assertIn("tensor_model_parallel_all_reduce",s)
        self.assertIn("v2_threshold_route",s)
        self.assertIn("return out",s)
        self.assertIn("2048",s)
    def test_no_new_r2_runtime_modules_on_main(self):
        self.assertFalse((ROOT/"vllm_ascend/_310p/v2/hybrid_state_device_copy.py").exists())
        self.assertFalse((ROOT/"vllm_ascend/_310p/v2/model_runner.py").exists())
if __name__=="__main__":
    unittest.main()
