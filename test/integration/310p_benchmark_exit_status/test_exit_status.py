#!/usr/bin/env python3
# Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
# MemFabric_Hybrid is licensed under Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
# http://license.coscl.org.cn/MulanPSL2
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
# See the Mulan PSL v2 for more details.

"""检查310P benchmark的主进程退出码。

用法：python3 test_exit_status.py --binary /path/to/bm_perf_benchmark_310p
需要C编译器及benchmark依赖库。故障在aclInit入口注入，测试使用设备参数0/1。
"""

import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

INJECTOR_SOURCE = r"""
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>

int aclInit(const char *config)
{
    (void)config;
    if (strcmp(getenv("MF_BM_TEST_MODE"), "signal") == 0) {
        raise(SIGTERM);
    }
    return 107001;
}

pid_t waitpid(pid_t pid, int *status, int options)
{
    static int interrupted = 0;
    const char *mode = getenv("MF_BM_TEST_MODE");
    if (strcmp(mode, "eintr") == 0 && !interrupted) {
        interrupted = 1;
        errno = EINTR;
        return -1;
    }
    pid_t (*real_waitpid)(pid_t, int *, int) = dlsym(RTLD_NEXT, "waitpid");
    pid_t result = real_waitpid(pid, status, options);
    if (strcmp(mode, "wait_error") == 0 && result > 0) {
        errno = ECHILD;
        return -1;
    }
    return result;
}
"""


class BenchmarkExitStatusTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="bm-310p-exit-")
        cls.injector = Path(cls.temp.name) / "injector.so"
        subprocess.run(
            ["cc", "-shared", "-fPIC", "-x", "c", "-", "-o", str(cls.injector), "-ldl"],
            input=INJECTOR_SOURCE,
            text=True,
            check=True,
        )

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_failure(self, mode):
        env = os.environ.copy()
        env["MF_BM_TEST_MODE"] = mode
        env["LD_PRELOAD"] = str(self.injector) + (":" + env["LD_PRELOAD"] if env.get("LD_PRELOAD") else "")
        command = [
            str(ARGS.binary),
            "-bw",
            "-t",
            "all",
            "-s",
            "4096",
            "-cc",
            "1",
            "-d",
            "0",
            "-ws",
            "2",
            "-lrs",
            "2",
            "-ip",
            "tcp://127.0.0.1:29731",
            "-hbm-size",
            "268435456",
            "-dram-size",
            "268435456",
        ]
        result = subprocess.run(command, env=env, text=True, capture_output=True, timeout=30)
        output = result.stdout + result.stderr
        if ARGS.log_dir:
            (ARGS.log_dir / (mode + ".log")).write_text(output)
            (ARGS.log_dir / (mode + ".json")).write_text(
                json.dumps({"command": command, "exit": result.returncode}, indent=2)
            )
        self.assertGreater(result.returncode, 0, output)
        return output

    def test_initialization_failure(self):
        output = self.run_failure("init_error")
        self.assertEqual(output.count("acl init failed"), 2, output)

    def test_child_terminated_by_signal(self):
        output = self.run_failure("signal")
        self.assertIn("signal", output)

    def test_waitpid_retries_interruption(self):
        output = self.run_failure("eintr")
        self.assertIn("exitCode=1", output)
        self.assertNotIn("waitpid failed", output)

    def test_waitpid_failure(self):
        output = self.run_failure("wait_error")
        self.assertIn("waitpid failed", output)

    def test_help_succeeds(self):
        result = subprocess.run([str(ARGS.binary), "--help"], text=True, capture_output=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--log-dir", type=Path)
    ARGS = parser.parse_args()
    ARGS.binary = ARGS.binary.resolve(strict=True)
    if ARGS.log_dir:
        ARGS.log_dir.mkdir(parents=True, exist_ok=True)
    unittest.main(argv=[__file__], verbosity=2)
