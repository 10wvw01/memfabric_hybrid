import unittest
from unittest.mock import MagicMock, patch

import data_op_all_path_test as paths


class DataOpEntrypointTest(unittest.TestCase):
    def test_child_failures_reach_main_exit(self):
        for codes, expected in [((0, 0), 0), ((0, 1), 1), ((-15, 0), 1)]:
            with self.subTest(codes=codes):
                children = [MagicMock(exitcode=code) for code in codes]
                with (
                    patch.object(paths, "BigMemoryTest"),
                    patch.object(paths.multiprocessing, "Process", side_effect=children),
                ):
                    self.assertEqual(paths.main_process(), expected)

    def test_copy_error_is_reported(self):
        src = paths.BigMemoryBlock(paths.RankMemType(paths.MemType.LH, paths.RankType.LOCAL), 1, 4, None)
        dst = paths.BigMemoryBlock(paths.RankMemType(paths.MemType.GH, paths.RankType.REMOTE), 2, 4, None)
        context = MagicMock()
        context.get_handle.return_value.copy_data.return_value = -1
        with self.assertRaisesRegex(RuntimeError, "copy_data failed"):
            paths.copy_data(src, dst, 4, context)


if __name__ == "__main__":
    unittest.main()
