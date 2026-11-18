#!/usr/bin/env python3
"""检查310P SDMA稳定性测试的错误退出。

需要加载CANN环境，并提供编译后的测试程序和AICPU kernel启动配置。
通过临时预加载库改变host收到的接口返回值，检查失败时的退出码。
"""

import argparse
import json
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import unittest

INJECTOR_SOURCE = r"""
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "acl/acl.h"

/* 根据rank和调用地址定位测试程序发起的接口调用。 */
static int ShouldInject(void *caller, const char *mode)
{
    const char *rank = getenv("OMPI_COMM_WORLD_RANK");
    const char *selected = getenv("STABILITY_TEST_RANK");
    if (selected && (!rank || strcmp(selected, rank) != 0)) {
        return 0;
    }
    Dl_info info;
    struct stat source, executable;
    return strcmp(getenv("STABILITY_TEST_MODE"), mode) == 0 &&
           dladdr(caller, &info) && stat(info.dli_fname, &source) == 0 &&
           stat("/proc/self/exe", &executable) == 0 &&
           source.st_dev == executable.st_dev && source.st_ino == executable.st_ino;
}

aclError aclrtCreateStream(aclrtStream *stream)
{
    if (ShouldInject(__builtin_return_address(0), "create_error")) {
        return 507018;
    }
    aclError (*real_create)(aclrtStream *) = dlsym(RTLD_NEXT, "aclrtCreateStream");
    return real_create(stream);
}

aclError aclrtDestroyStream(aclrtStream stream)
{
    aclError (*real_destroy)(aclrtStream) = dlsym(RTLD_NEXT, "aclrtDestroyStream");
    aclError ret = real_destroy(stream);
    return ret == ACL_SUCCESS && ShouldInject(__builtin_return_address(0), "destroy_error") ? 507018 : ret;
}

aclError aclrtSynchronizeStream(aclrtStream stream)
{
    aclError (*real_sync)(aclrtStream) = dlsym(RTLD_NEXT, "aclrtSynchronizeStream");
    aclError ret = real_sync(stream);
    return ret == ACL_SUCCESS && ShouldInject(__builtin_return_address(0), "sync_error") ? 507018 : ret;
}

aclError aclrtSynchronizeDeviceWithTimeout(int32_t timeout)
{
    aclError (*real_sync)(int32_t) = dlsym(RTLD_NEXT, "aclrtSynchronizeDeviceWithTimeout");
    aclError ret = real_sync(timeout);
    return ret == ACL_SUCCESS && ShouldInject(__builtin_return_address(0), "device_sync_error") ? 507018 : ret;
}

aclError aclrtMemcpy(void *dst, size_t capacity, const void *src, size_t count, aclrtMemcpyKind kind)
{
    aclError (*real_copy)(void *, size_t, const void *, size_t, aclrtMemcpyKind) =
        dlsym(RTLD_NEXT, "aclrtMemcpy");
    aclError ret = real_copy(dst, capacity, src, count, kind);
    if (ret == ACL_SUCCESS && kind == ACL_MEMCPY_DEVICE_TO_HOST && count == 4 * sizeof(uint32_t)) {
        if (ShouldInject(__builtin_return_address(0), "copy_error")) {
            return 507018;
        }
        if (ShouldInject(__builtin_return_address(0), "data_mismatch")) {
            memset(dst, 0, count);
        }
    }
    return ret;
}

aclError aclrtResetDevice(int32_t device)
{
    aclError (*real_reset)(int32_t) = dlsym(RTLD_NEXT, "aclrtResetDevice");
    aclError ret = real_reset(device);
    return ret == ACL_SUCCESS && ShouldInject(__builtin_return_address(0), "reset_error") ? 507018 : ret;
}

aclError aclFinalize(void)
{
    aclError (*real_finalize)(void) = dlsym(RTLD_NEXT, "aclFinalize");
    aclError ret = real_finalize();
    return ret == ACL_SUCCESS && ShouldInject(__builtin_return_address(0), "finalize_error") ? 507018 : ret;
}
"""


class StabilityExitStatusTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="stability-exit-")
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

    def run_case(self, mode, expected, rank=None):
        env = os.environ.copy()
        env.pop("MF_SDMA_ORCH_DIR", None)
        for name in ("DEVICE_ID", "STABILITY_TEST_RANK", "STABILITY_TEST_MODE"):
            env.pop(name, None)
        env.update(
            TEST_DEVICE_BASE=str(ARGS.device_base),
            STRESS_ROUNDS="7",
            MF_SDMA_ORCH_JSON=str(ARGS.kernel_json),
            MF_SDMA_ORCH_EPOCH_BUDGET_S="1",
        )
        if mode != "normal":
            env["STABILITY_TEST_MODE"] = mode
            env["LD_PRELOAD"] = str(self.injector) + (":" + env["LD_PRELOAD"] if env.get("LD_PRELOAD") else "")
        if rank is not None:
            env["STABILITY_TEST_RANK"] = str(rank)
        command = ["mpirun"] + (["--allow-run-as-root"] if os.geteuid() == 0 else [])
        command += ["-np", "2", str(ARGS.binary)]
        returncode, output, timed_out = self.execute_mpi(command, env)
        if ARGS.log_dir:
            label = mode + ("_rank" + str(rank) if rank is not None else "")
            (ARGS.log_dir / (label + ".log")).write_text(output)
            (ARGS.log_dir / (label + ".json")).write_text(
                json.dumps(
                    {
                        "exit": returncode,
                        "mode": mode,
                        "rank": rank,
                        "timed_out": timed_out,
                        "device_base": ARGS.device_base,
                        "visible_devices": env.get("ASCEND_RT_VISIBLE_DEVICES"),
                    },
                    indent=2,
                )
            )
        self.assertFalse(timed_out, "MPI任务未在45秒内结束\n" + output)
        self.assertEqual(returncode, expected, output)
        if expected == 0:
            for rank_id in (0, 1):
                self.assertIn("r" + str(rank_id) + " PASS ===", output)
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
        timed_out = False
        try:
            output, _ = process.communicate(timeout=45)
        except subprocess.TimeoutExpired:
            timed_out = True
            os.killpg(process.pid, signal.SIGTERM)
            try:
                output, _ = process.communicate(timeout=10)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                output, _ = process.communicate()
        return process.returncode, output, timed_out

    def test_stream_creation_failure(self):
        self.run_case("create_error", 1)

    def test_stream_destruction_failure(self):
        self.run_case("destroy_error", 1)

    def test_result_copy_failure(self):
        self.run_case("copy_error", 1)

    def test_device_reset_failure(self):
        self.run_case("reset_error", 1)

    def test_acl_finalize_failure(self):
        self.run_case("finalize_error", 1)

    def test_stream_sync_failure(self):
        self.run_case("sync_error", 1)

    def test_device_sync_failure(self):
        self.run_case("device_sync_error", 1)

    def test_data_mismatch(self):
        self.run_case("data_mismatch", 1)

    def test_single_rank_creation_failure(self):
        self.run_case("create_error", 1, rank=0)

    def test_single_rank_sync_failure(self):
        self.run_case("sync_error", 1, rank=0)

    def test_single_rank_copy_failure(self):
        self.run_case("copy_error", 1, rank=0)

    def test_completed(self):
        self.run_case("normal", 0)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--kernel-json", type=Path, required=True)
    parser.add_argument("--log-dir", type=Path)
    parser.add_argument(
        "--device-base",
        type=int,
        default=os.environ.get("TEST_DEVICE_BASE", "0"),
        help="rank 0的设备索引，rank 1使用下一索引；默认读取TEST_DEVICE_BASE，未设置时为0",
    )
    ARGS, remaining = parser.parse_known_args()
    if not 0 <= ARGS.device_base < 65535:
        parser.error("--device-base须在0到65534之间")
    ARGS.binary = ARGS.binary.resolve(strict=True)
    ARGS.kernel_json = ARGS.kernel_json.resolve(strict=True)
    if ARGS.log_dir:
        ARGS.log_dir.mkdir(parents=True, exist_ok=True)
    unittest.main(argv=[__file__, *remaining], verbosity=2)
