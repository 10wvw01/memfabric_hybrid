"""CPU-only regression checks for Gate-A arithmetic and clock-sign conventions."""
import unittest

from audit_prefill_gate_a import audit_rows, corrected_clock


class GateAAuditTest(unittest.TestCase):
    def test_clock_sign(self):
        # offset r0-r1 = +100: rank1=900 maps to rank0=1000
        self.assertEqual(corrected_clock(1000, 900, 100), 0)
        self.assertEqual(corrected_clock(1020, 900, 100), 20)

    def test_clean_step_not_diluted_by_outlier(self):
        rows = []
        for step in range(5):
            for k in range(40):
                rows.append(dict(
                    site_idx=step * 40 + k, step=step,
                    kernel_start_us_rel=step * 100000 + k * 1000,
                    kernel_dur_us=74000 if (step == 0 and k == 8) else 2500,
                ))
        res = audit_rows(rows, "v2_r0")
        self.assertEqual(len(res["outliers_us"]), 1)
        self.assertEqual(res["stable_complete_steps"], [1, 2, 3, 4])
        self.assertEqual(res["stable_kernel_sum_ms_per_step"], 100.0)

    def test_reject_missing_site(self):
        rows = [dict(site_idx=k, step=0, mm_start_us_rel=k * 1000,
                     mm_dur_us=800, ar_dur_us=1000) for k in range(39)]
        with self.assertRaisesRegex(ValueError, "40 sites"):
            audit_rows(rows, "stock_r0")


if __name__ == "__main__":
    unittest.main()
