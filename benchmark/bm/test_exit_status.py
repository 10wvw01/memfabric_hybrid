"""Run parent failure checks: python3 test_exit_status.py --binary PATH."""

import argparse
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

INJECTOR = r"""
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
int aclInit(const char *config)
{
    (void)config;
    return strcmp(getenv("MF_GENERIC_BM_MODE"), "peer_wait") == 0 ? 0 : 107001;
}
int aclrtSetDevice(int device)
{
    if (device == 0) return 107001;
    for (;;) pause();
}
"""


class ParentExitTest(unittest.TestCase):
    def run_failure(self, mode):
        with tempfile.TemporaryDirectory() as work:
            library = Path(work) / "injector.so"
            subprocess.run(
                ["cc", "-shared", "-fPIC", "-x", "c", "-", "-o", str(library)],
                input=INJECTOR,
                text=True,
                check=True,
            )
            result = subprocess.run(
                [
                    str(ARGS.binary),
                    "-bw",
                    "-ot",
                    "sdma",
                    "-t",
                    "all",
                    "-s",
                    "4096",
                    "-d",
                    "0",
                    "-ws",
                    "2",
                    "-lrs",
                    "2",
                    "-ip",
                    "tcp://127.0.0.1:29731",
                ],
                env={**os.environ, "LD_PRELOAD": str(library), "MF_GENERIC_BM_MODE": mode},
                text=True,
                capture_output=True,
                timeout=10,
            )
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("subprocess failed", result.stdout + result.stderr)

    def test_failed_initialization_reaches_parent(self):
        self.run_failure("init_error")

    def test_failure_terminates_waiting_peer(self):
        self.run_failure("peer_wait")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, required=True)
    ARGS, remaining = parser.parse_known_args()
    ARGS.binary = ARGS.binary.resolve()
    unittest.main(argv=[__file__, *remaining])
