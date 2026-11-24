import tempfile
import unittest
from pathlib import Path

from summarize import summarize


class SummaryTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name)
        for rank in (0, 1):
            rows = []
            for dim in (256, 512, 1024, 2048, 4096):
                rows += [f"SAMPLE rank={rank} dim={dim} iteration=0 host_us={10+rank} "
                         f"event_us={5-rank} enqueue_us=1", f"PASS rank={rank} dim={dim} full_payload_checked=1"]
            rows.append(f"PASS rank={rank} cleanup_complete=1")
            (self.path / f"rank{rank}.log").write_text("\n".join(rows))

    def test_uses_slowest_rank_for_each_metric(self):
        row = summarize(self.path)[256]
        self.assertEqual(row["samples"], 1)
        self.assertEqual(row["host_us"]["mean"], 11)
        self.assertEqual(row["event_us"]["mean"], 5)

    def test_rejects_incomplete_correctness(self):
        p = self.path / "rank1.log"
        p.write_text(p.read_text().replace("full_payload_checked=1", "unchecked"))
        with self.assertRaises(ValueError):
            summarize(self.path)

    def test_rejects_missing_rank_sample(self):
        p = self.path / "rank1.log"
        p.write_text("\n".join(line for line in p.read_text().splitlines()
                               if not line.startswith("SAMPLE rank=1 dim=256")))
        with self.assertRaises(ValueError):
            summarize(self.path)


if __name__ == "__main__":
    unittest.main()
