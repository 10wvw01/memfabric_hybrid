import argparse
import os
from pathlib import Path
import re
import signal
import socket
import subprocess
import tempfile
import time
import unittest

ARGS = None


class PoolFallbackTest(unittest.TestCase):
    def run_pool(self, devices, size, *, dram=False, reject=False, inject=None):
        with tempfile.TemporaryDirectory(prefix="bm-pool-") as directory:
            self.run_workers(Path(directory), devices, size, dram, reject, inject)

    def wait_workers(self, processes, log_dir):
        deadline = time.monotonic() + 90
        while any(process.poll() is None for process in processes):
            bad = [process.returncode for process in processes if process.poll() not in (None, 0)]
            self.assertFalse(bad, f"rank failed {bad}; see {log_dir}")
            self.assertLess(time.monotonic(), deadline, f"rank timeout; see {log_dir}")
            time.sleep(0.1)
        self.assertEqual([p.returncode for p in processes], [0] * len(processes), str(log_dir))

    def run_workers(self, directory, devices, size, dram, reject, inject):
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            port = listener.getsockname()[1]
        log_dir = ARGS.log_dir / self.id().split(".")[-1]
        log_dir.mkdir(parents=True, exist_ok=True)
        environment = dict(os.environ)
        environment.update(
            ASCEND_RT_VISIBLE_DEVICES="0,1,2,3", LD_PRELOAD=str(ARGS.injector), MF_GROUP_JOIN_MAX_TIMEOUT="30"
        )
        environment.pop("MF_TEST_P2P_OOM_DEVICE", None)
        if inject is not None:
            environment["MF_TEST_P2P_OOM_DEVICE"] = str(inject)
        processes, outputs = [], []
        try:
            for rank, device in enumerate(devices):
                output = (log_dir / f"rank-{rank}.log").open("w")
                outputs.append(output)
                command = [
                    str(ARGS.binary),
                    str(rank),
                    str(len(devices)),
                    str(device),
                    f"tcp://127.0.0.1:{port}",
                    "dram" if dram else "hbm",
                    str(size),
                    "alloc-reject" if reject else "success",
                    str(directory),
                ]
                processes.append(
                    subprocess.Popen(
                        command,
                        stdout=output,
                        stderr=subprocess.STDOUT,
                        env=environment,
                        start_new_session=True,
                    )
                )
            self.wait_workers(processes, log_dir)
        finally:
            for process in processes:
                if process.poll() is None:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait()
            for output in outputs:
                output.close()
        logs = "\n".join(path.read_text(errors="replace") for path in sorted(log_dir.glob("rank-*.log")))
        self.check_logs(logs, len(devices), reject, inject, log_dir)

    def check_logs(self, logs, ranks, reject, inject, log_dir):
        creates = re.findall(r"CREATE_RESULT rank=(\d+) ok=(\d+)", logs)
        self.assertEqual(len(creates), ranks, str(log_dir))
        if reject:
            self.assertTrue(any(int(ok) == 0 for _, ok in creates), str(log_dir))
            self.assertIn("超过2个rank不支持回退普通设备DDR", logs)
            self.assertNotIn("JOIN_RESULT", logs)
            self.assertNotRegex(logs, r"TEST_POOL_CREATE [^\n]*type=1 ")
        else:
            self.assertTrue(all(int(ok) == 1 for _, ok in creates), str(log_dir))
            results = re.findall(r"JOIN_RESULT rank=(\d+) ret=(-?\d+)", logs)
            self.assertEqual(len(results), ranks, str(log_dir))
            self.assertTrue(all(int(ret) == 0 for _, ret in results), str(log_dir))
            self.assertEqual(logs.count("COPY_OK"), ranks, str(log_dir))
            if inject is not None or "4g_ddr" in self.id():
                self.assertRegex(logs, r"TEST_POOL_CREATE [^\n]*type=1 ret=0")
        if inject is not None:
            self.assertIn("INJECT_P2P_OOM", logs)

    def test_single_rank_4g_ddr(self):
        self.run_pool([0], 4 << 30)

    def test_single_rank_injected_fallback(self):
        self.run_pool([0], 64 << 20, inject="all")

    def test_four_rank_4g_allocation_rejected(self):
        self.run_pool([0, 1, 2, 3], 4 << 30, reject=True)

    def test_four_rank_injected_allocation_rejected(self):
        self.run_pool([0, 1, 2, 3], 64 << 20, reject=True, inject="all")

    def test_same_card_4g_ddr(self):
        self.run_pool([0, 1], 4 << 30)

    def test_four_rank_3g_p2p(self):
        self.run_pool([0, 1, 2, 3], 3 << 30)

    def test_same_card_injected_fallback(self):
        self.run_pool([0, 1], 64 << 20, inject="all")

    def test_four_rank_mixed_allocation_rejected(self):
        self.run_pool([0, 1, 2, 3], 64 << 20, reject=True, inject=0)

    def test_four_rank_p2p(self):
        self.run_pool([0, 1, 2, 3], 64 << 20)

    def test_four_rank_5g_host_dram(self):
        self.run_pool([0, 1, 2, 3], 5 << 30, dram=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--injector", type=Path, required=True)
    parser.add_argument("--log-dir", type=Path, required=True)
    ARGS, tests = parser.parse_known_args()
    ARGS.binary = ARGS.binary.resolve(strict=True)
    ARGS.injector = ARGS.injector.resolve(strict=True)
    ARGS.log_dir = ARGS.log_dir.resolve()
    unittest.main(argv=[__file__, *tests])
