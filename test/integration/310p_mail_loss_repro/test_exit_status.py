#!/usr/bin/env python3
"""检查接收通知复现测试的退出码，使用310P3设备0/1。

需要加载CANN环境，并提供编译后的测试程序和AICPU kernel启动配置。
故障在真实stream同步完成后或host读取状态时注入。
"""

import argparse
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import tempfile
import unittest

INJECTOR_SOURCE = r"""
#define _GNU_SOURCE
#include <dlfcn.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "acl/acl_rt.h"

/* 与Ex09Stat开头字段对应，用于改变host读回的计数和状态。 */
struct TestStatePrefix {
    uint64_t magic, progress, gensDone, mailsSent, mailsRecv;
    uint64_t waitPollsTotal, waitPollsMax, waitPollsLast;
    uint32_t state;
};

/* 根据rank和调用地址定位注入点，避免改变库内部的初始化。 */
static int FromTest(void *caller)
{
    const char *rank = getenv("OMPI_COMM_WORLD_RANK");
    const char *selected = getenv("EX09_TEST_RANK");
    if (selected && (!rank || strcmp(selected, rank) != 0)) {
        return 0;
    }
    Dl_info info;
    struct stat source, executable;
    return dladdr(caller, &info) && stat(info.dli_fname, &source) == 0 &&
           stat("/proc/self/exe", &executable) == 0 &&
           source.st_dev == executable.st_dev && source.st_ino == executable.st_ino;
}

aclError aclrtCreateStream(aclrtStream *stream)
{
    if (FromTest(__builtin_return_address(0)) && strcmp(getenv("EX09_TEST_MODE"), "create_error") == 0) {
        return 507018;
    }
    aclError (*real_create)(aclrtStream *) = dlsym(RTLD_NEXT, "aclrtCreateStream");
    return real_create(stream);
}

aclError aclrtDestroyStream(aclrtStream stream)
{
    aclError (*real_destroy)(aclrtStream) = dlsym(RTLD_NEXT, "aclrtDestroyStream");
    aclError ret = real_destroy(stream);
    if (ret == ACL_SUCCESS && FromTest(__builtin_return_address(0)) &&
        strcmp(getenv("EX09_TEST_MODE"), "destroy_error") == 0) {
        return 507018;
    }
    return ret;
}

aclError aclrtSynchronizeStream(aclrtStream stream)
{
    aclError (*real_sync)(aclrtStream) = dlsym(RTLD_NEXT, "aclrtSynchronizeStream");
    /* 真实任务完成后再改变host收到的返回值。 */
    aclError ret = real_sync(stream);
    if (ret == ACL_SUCCESS && FromTest(__builtin_return_address(0))) {
        const char *mode = getenv("EX09_TEST_MODE");
        if (strcmp(mode, "sync_error") == 0) {
            return 507018;
        }
        if (strcmp(mode, "interrupt") == 0) {
            raise(SIGINT);
        }
    }
    return ret;
}

aclError aclrtMemcpy(void *dst, size_t capacity, const void *src, size_t count, aclrtMemcpyKind kind)
{
    aclError (*real_copy)(void *, size_t, const void *, size_t, aclrtMemcpyKind) =
        dlsym(RTLD_NEXT, "aclrtMemcpy");
    aclError ret = real_copy(dst, capacity, src, count, kind);
    if (ret == ACL_SUCCESS && kind == ACL_MEMCPY_DEVICE_TO_HOST &&
        count >= sizeof(struct TestStatePrefix) && FromTest(__builtin_return_address(0))) {
        struct TestStatePrefix *state = dst;
        const char *mode = getenv("EX09_TEST_MODE");
        if (strcmp(mode, "stat_error") == 0) {
            return 507018;
        }
        if (strcmp(mode, "stall") == 0) {
            memset(dst, 0, count);
        } else if (strcmp(mode, "early_complete") == 0) {
            state->state = 5;
        } else if (strcmp(mode, "wrong_count") == 0) {
            state->mailsSent = 0;
        } else if (strcmp(mode, "wrong_recv_count") == 0) {
            state->mailsRecv = 0;
        } else if (strcmp(mode, "device_error") == 0) {
            state->state = 1;
        }
    }
    return ret;
}
"""


class MailLossExitStatusTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="mail-loss-exit-")
        cls.injector = Path(cls.temp.name) / "injector.so"
        subprocess.run(
            [
                "cc",
                "-shared",
                "-fPIC",
                "-x",
                "c",
                "-",
                "-I" + os.environ["ASCEND_HOME_PATH"] + "/include",
                "-o",
                str(cls.injector),
                "-ldl",
            ],
            input=INJECTOR_SOURCE,
            text=True,
            check=True,
        )

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_case(self, mode, expected, gens=7, rank=None):
        env = os.environ.copy()
        env.pop("ASCEND_RT_VISIBLE_DEVICES", None)
        env.pop("EX09_TEST_RANK", None)
        env.update(
            EX09_DEVICE_BASE="0",
            EX09_TOTAL_GENS=str(gens),
            EX09_CHUNKS="4",
            EX09_BATCH_GENS="1",
            EX09_GEN_DELAY_US="0",
            EX09_STALL_S="0" if mode == "stall" else "60",
            MF_SDMA_ORCH_JSON=str(ARGS.kernel_json),
            MF_SDMA_ORCH_EPOCH_BUDGET_S="1",
        )
        if mode != "normal":
            env["EX09_TEST_MODE"] = mode
            env["LD_PRELOAD"] = str(self.injector) + (":" + env["LD_PRELOAD"] if env.get("LD_PRELOAD") else "")
        if rank is not None:
            env["EX09_TEST_RANK"] = str(rank)
        command = ["mpirun"] + (["--allow-run-as-root"] if os.geteuid() == 0 else [])
        command += ["-np", "2", str(ARGS.binary)]
        returncode, output = self.execute_mpi(command, env)
        if ARGS.log_dir:
            label = mode + ("_rank" + str(rank) if rank is not None else "") + "_g" + str(gens)
            (ARGS.log_dir / (label + ".log")).write_text(output)
            (ARGS.log_dir / (label + ".json")).write_text(
                json.dumps({"exit": returncode, "mode": mode, "gens": gens, "rank": rank}, indent=2)
            )
        self.assertEqual(returncode, expected, output)
        if expected:
            self.assertNotIn("本次运行未检测到异常", output)
        return output

    def execute_mpi(self, command, env):
        process = subprocess.Popen(
            command,
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            start_new_session=True,
        )
        try:
            output, _ = process.communicate(timeout=45)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGTERM)
            try:
                output, _ = process.communicate(timeout=10)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                output, _ = process.communicate()
            self.fail("MPI任务未在45秒内结束\n" + output)
        return process.returncode, output

    def test_stream_creation_failure(self):
        self.run_case("create_error", 1)

    def test_stream_destruction_failure(self):
        self.run_case("destroy_error", 1)

    def test_status_copy_failure(self):
        self.run_case("stat_error", 1)

    def test_stream_sync_failure(self):
        self.run_case("sync_error", 1)

    def test_progress_timeout(self):
        self.run_case("stall", 1)

    def test_interrupted(self):
        self.run_case("interrupt", 1)

    def test_early_complete_state(self):
        self.run_case("early_complete", 1)

    def test_incorrect_send_count(self):
        self.run_case("wrong_count", 1, gens=1)

    def test_incorrect_receive_count(self):
        self.run_case("wrong_recv_count", 1, gens=1)

    def test_existing_device_error(self):
        self.run_case("device_error", 1)

    def test_single_rank_sync_failure(self):
        self.run_case("sync_error", 1, rank=0)

    def test_completed(self):
        output = self.run_case("normal", 0)
        counters = re.findall(r"终态 stat: state=0 gen=\d+ gensDone=(\d+) sent=(\d+) recv=(\d+)", output)
        self.assertEqual(counters, [("7", "28", "28")] * 2, output)

    def test_zero_rounds(self):
        output = self.run_case("normal", 0, gens=0)
        self.assertEqual(output.count("gensDone=0 sent=0 recv=0"), 2, output)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--kernel-json", type=Path, required=True)
    parser.add_argument("--log-dir", type=Path)
    ARGS, remaining = parser.parse_known_args()
    ARGS.binary = ARGS.binary.resolve(strict=True)
    ARGS.kernel_json = ARGS.kernel_json.resolve(strict=True)
    if ARGS.log_dir:
        ARGS.log_dir.mkdir(parents=True, exist_ok=True)
    unittest.main(argv=[__file__, *remaining], verbosity=2)
