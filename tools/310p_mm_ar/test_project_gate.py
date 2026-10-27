# SPDX-License-Identifier: Apache-2.0
"""CPU-only tests for the change classification and project file gate."""
from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import project_gate as guard  # noqa: E402


class ProjectGateTests(unittest.TestCase):
    def test_static_pass_and_missing_file(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for path in guard.REQUIRED:
                f = root / path
                f.parent.mkdir(parents=True, exist_ok=True)
                f.write_text("", encoding="utf-8")
            (root / "PROJECT_310P_MEMFABRIC.md").write_text(
                "dev/mainline\n" + "\n".join(guard.ENTRY_LINKS), encoding="utf-8")
            (root / guard.CONTROL).write_text("Stock 4096 2048 TP Gate E", encoding="utf-8")
            self.assertEqual(guard.check_static(root), [])
            (root / guard.DECISIONS).unlink()
            self.assertTrue(any("DECISIONS.md" in e for e in guard.check_static(root)))

    def test_code_changes_need_control_update(self):
        source = guard.PROJECT + "src/op_kernel/mf310p_v2_kernel.asc"
        self.assertTrue(guard.check_changes([source]))
        self.assertEqual(guard.check_changes([source, guard.CONTROL]), [])

    def test_evidence_needs_both_control_and_performance_ledger(self):
        evidence = guard.PROJECT + "evidence/new_run/rank0.json"
        self.assertEqual(len(guard.check_changes([evidence])), 2)
        self.assertEqual(guard.check_changes([evidence, guard.CONTROL, guard.PERF]), [])

    def test_report_needs_performance_ledger(self):
        report = guard.DOCS + "V2_EXAMPLE_ONDEVICE_REPORT.md"
        self.assertEqual(len(guard.check_changes([report])), 1)
        self.assertEqual(guard.check_changes([report, guard.PERF, guard.CONTROL]), [])

    def test_decision_needs_control_but_plain_docs_do_not(self):
        self.assertTrue(guard.check_changes([guard.DECISIONS]))
        self.assertEqual(guard.check_changes([guard.DECISIONS, guard.CONTROL]), [])
        self.assertEqual(guard.check_changes([guard.DOCS + "README.md"]), [])

    def test_validate_git_sha(self):
        with self.assertRaisesRegex(ValueError, "invalid base"):
            guard.git_changed_paths(Path("."), "main;echo injected", "a" * 40)


if __name__ == "__main__":
    unittest.main()
