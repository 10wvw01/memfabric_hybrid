import tempfile
import unittest
from pathlib import Path

from summarize import expected_cases, summarize


class SummaryTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name)
        for rank in (0, 1):
            lines = []
            for mode, context, size in sorted(expected_cases()):
                case = f"rank={rank} mode={mode} context={context} bytes={size}"
                for iteration in range(2 if rank == 0 else 0):
                    value = rank + iteration + 1
                    lines.append(
                        f"SAMPLE {case} iteration={iteration} completed_us={value} event_us={value} host_us={value}"
                    )
                lines.append(f"PASS {case} full_payload_checked=1 rounds=2")
            lines.append(f"PASS rank={rank} cleanup_complete=1")
            (self.path / f"rank{rank}.log").write_text("\n".join(lines))

    def change_rank(self, transform):
        p = self.path / "rank0.log"
        p.write_text(transform(p.read_text()))

    def test_single_active_rank_statistics_cover_all_interfaces(self):
        result = summarize(self.path)
        self.assertEqual(len(result), 78)
        self.assertTrue(all(row["samples"] == 2 for row in result))
        self.assertEqual(result[0]["completed_us"]["mean"], 1.5)
        self.assertEqual(result[0]["event_us"]["p99"], 2)

    def test_passive_rank_must_not_emit_timed_samples(self):
        p = self.path / "rank1.log"
        sample = (self.path / "rank0.log").read_text().splitlines()[0].replace("rank=0", "rank=1")
        p.write_text(p.read_text() + "\n" + sample)
        with self.assertRaises(ValueError):
            summarize(self.path)

    def test_missing_case_is_rejected(self):
        self.change_rank(lambda s: "\n".join(line for line in s.splitlines() if "mode=12 " not in line))
        with self.assertRaises(ValueError):
            summarize(self.path)

    def test_missing_iteration_is_rejected(self):
        self.change_rank(lambda s: s.replace("iteration=1", "iteration=2", 1))
        with self.assertRaises(ValueError):
            summarize(self.path)

    def test_duplicate_sample_is_rejected(self):
        self.change_rank(lambda s: s + "\n" + s.splitlines()[0])
        with self.assertRaises(ValueError):
            summarize(self.path)

    def test_invalid_timing_is_rejected(self):
        self.change_rank(lambda s: s.replace("completed_us=2", "completed_us=nan", 1))
        with self.assertRaises(ValueError):
            summarize(self.path)

    def test_inconsistent_event_boundaries_are_rejected(self):
        self.change_rank(lambda s: s.replace("event_us=2", "event_us=50", 1))
        with self.assertRaises(ValueError):
            summarize(self.path)

    def test_unfinished_run_is_rejected(self):
        self.change_rank(lambda s: s.replace("cleanup_complete=1", "cleanup_complete=0"))
        with self.assertRaises(ValueError):
            summarize(self.path)


if __name__ == "__main__":
    unittest.main()
