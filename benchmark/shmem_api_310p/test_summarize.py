"""Check that incomplete runs cannot become benchmark results."""

import tempfile
import unittest
from pathlib import Path

from summarize import EXPECTED, aggregate


class SummarizeTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        for rank in (0, 1):
            lines = ["CONFIG api_style=at api_filter=all warmup=0 rounds=1 timing=acl_event"]
            for api, size in sorted(EXPECTED):
                if rank == 0:
                    lines.append(f"SAMPLE api={api} bytes={size} iteration=0 event_us=10 host_us=20")
                lines.append(f"PASS rank={rank} api={api} bytes={size} full_payload_checked=1 rounds=1")
            lines.append(f"PASS rank={rank} cleanup_complete=1")
            (self.directory / f"rank{rank}.log").write_text("\n".join(lines))

    def change_rank0(self, old, new):
        path = self.directory / "rank0.log"
        path.write_text(path.read_text().replace(old, new, 1))

    def test_complete_run(self):
        rows = aggregate(self.directory)
        self.assertEqual(len(rows), len(EXPECTED))
        self.assertEqual(rows[0]["event_us"]["mean"], 10)

    def test_single_interface(self):
        for rank in (0, 1):
            path = self.directory / f"rank{rank}.log"
            lines = [line for line in path.read_text().splitlines() if "api=" not in line or "api=put " in line]
            path.write_text("\n".join(lines).replace("api_filter=all", "api_filter=put"))
        self.assertEqual(len(aggregate(self.directory)), 7)

    def test_missing_sample(self):
        self.change_rank0("SAMPLE", "REMOVED")
        with self.assertRaises(ValueError):
            aggregate(self.directory)

    def test_invalid_measurement(self):
        self.change_rank0("event_us=10", "event_us=nan")
        with self.assertRaises(ValueError):
            aggregate(self.directory)

    def test_missing_validation(self):
        path = self.directory / "rank1.log"
        path.write_text(path.read_text().replace("full_payload_checked=1", "full_payload_checked=0", 1))
        with self.assertRaises(ValueError):
            aggregate(self.directory)

    def test_incomplete_rounds(self):
        self.change_rank0("rounds=1", "rounds=2")
        with self.assertRaises(ValueError):
            aggregate(self.directory)


if __name__ == "__main__":
    unittest.main()
