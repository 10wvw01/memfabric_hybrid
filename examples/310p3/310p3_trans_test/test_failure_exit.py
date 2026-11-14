#!/usr/bin/env python3
"""验证310P TRANS示例的单rank失败退出，固定使用设备0/1。

需要已构建的示例、MemFabric库及CANN环境。注册结果不一致的用例会先完成两端真实注册，
再仅向选定rank返回失败，以检查不同阶段的MPI等待能否及时结束。
数据校验用例跳过指定方向的传输并返回成功，检查示例能否发现数据未更新。
"""

import argparse
import json
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import time
import unittest

INJECTOR_SOURCE = r"""

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dlfcn.h>
#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "smem_trans.h"
#include "hybm.h"

static int ShouldFail(const char *stage)
{
    const char *mode = getenv("TRANS_TEST_MODE");
    const char *selected = getenv("TRANS_TEST_RANK");
    const char *rank = getenv("OMPI_COMM_WORLD_RANK");
    return mode && selected && rank && strcmp(mode, stage) == 0 &&
           (atoi(selected) == -1 || atoi(selected) == atoi(rank));
}

static int32_t Failure(const char *stage)
{
    fprintf(stderr, "[INJECT rank=%s] %s return=-1\n", getenv("OMPI_COMM_WORLD_RANK"), stage);
    fflush(stderr);
    return -1;
}

static int32_t AfterCall(const char *stage, int32_t ret)
{
    return ret == 0 && ShouldFail(stage) ? Failure(stage) : ret;
}

static int SkipTransfer(const char *stage)
{
    if (!ShouldFail(stage)) {
        return 0;
    }
    fprintf(stderr, "[INJECT rank=%s] %s: skip transfer, return=0\n", getenv("OMPI_COMM_WORLD_RANK"), stage);
    fflush(stderr);
    return 1;
}

int32_t smem_trans_init(const smem_trans_config_t *config)
{
    if (ShouldFail("init")) {
        return Failure("init");
    }
    auto real_init = reinterpret_cast<decltype(&smem_trans_init)>(dlsym(RTLD_NEXT, "smem_trans_init"));
    return real_init(config);
}

smem_trans_t smem_trans_create(const char *store, const char *id, const smem_trans_config_t *config)
{
    if (ShouldFail("create")) {
        Failure("create");
        return NULL;
    }
    auto real_create = reinterpret_cast<decltype(&smem_trans_create)>(dlsym(RTLD_NEXT, "smem_trans_create"));
    return real_create(store, id, config);
}

void *hybm_alloc_vmm_buffer(uint32_t device, uint32_t world, size_t size)
{
    if (ShouldFail("alloc")) {
        Failure("alloc");
        return NULL;
    }
    auto real_alloc = reinterpret_cast<decltype(&hybm_alloc_vmm_buffer)>(dlsym(RTLD_NEXT, "hybm_alloc_vmm_buffer"));
    return real_alloc(device, world, size);
}

int32_t smem_trans_register_mem(smem_trans_t handle, void *address, size_t size, uint32_t flags)
{
    if (ShouldFail("register_before")) {
        return Failure("register_before");
    }
    auto real_register =
        reinterpret_cast<decltype(&smem_trans_register_mem)>(dlsym(RTLD_NEXT, "smem_trans_register_mem"));
    int32_t ret = real_register(handle, address, size, flags);
    const char *mode = getenv("TRANS_TEST_MODE");
    if (mode && strcmp(mode, "register_after") == 0 && ret == 0) {
        /* 两端均完成底层注册后，再改变一端向上层报告的结果。 */
        PMPI_Barrier(MPI_COMM_WORLD);
        return AfterCall("register_after", ret);
    }
    return ret;
}

int32_t smem_trans_write(smem_trans_t handle, const void *local, const char *peer, void *remote, size_t size,
                         uint32_t flags)
{
    if (SkipTransfer("skip_write")) {
        return 0;
    }
    auto real_write = reinterpret_cast<decltype(&smem_trans_write)>(dlsym(RTLD_NEXT, "smem_trans_write"));
    return AfterCall("write", real_write(handle, local, peer, remote, size, flags));
}

int32_t smem_trans_write_submit(smem_trans_t handle, const void *local, const char *peer, void *remote, size_t size,
                                void *stream, uint32_t flags)
{
    if (SkipTransfer("skip_write_submit")) {
        return 0;
    }
    auto real_write = reinterpret_cast<decltype(&smem_trans_write_submit)>(dlsym(RTLD_NEXT, "smem_trans_write_submit"));
    return AfterCall("write_submit", real_write(handle, local, peer, remote, size, stream, flags));
}

int32_t smem_trans_read(smem_trans_t handle, void *local, const char *peer, const void *remote, size_t size,
                        uint32_t flags)
{
    if (SkipTransfer("skip_read")) {
        return 0;
    }
    auto real_read = reinterpret_cast<decltype(&smem_trans_read)>(dlsym(RTLD_NEXT, "smem_trans_read"));
    return AfterCall("read", real_read(handle, local, peer, remote, size, flags));
}

int32_t smem_trans_read_submit(smem_trans_t handle, void *local, const char *peer, const void *remote, size_t size,
                               void *stream, uint32_t flags)
{
    if (SkipTransfer("skip_read_submit")) {
        return 0;
    }
    auto real_read = reinterpret_cast<decltype(&smem_trans_read_submit)>(dlsym(RTLD_NEXT, "smem_trans_read_submit"));
    return AfterCall("read_submit", real_read(handle, local, peer, remote, size, stream, flags));
}

int32_t smem_trans_deregister_mem(smem_trans_t handle, void *address)
{
    auto real_deregister =
        reinterpret_cast<decltype(&smem_trans_deregister_mem)>(dlsym(RTLD_NEXT, "smem_trans_deregister_mem"));
    return AfterCall("deregister", real_deregister(handle, address));
}
"""


class TransFailureExitTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="trans-exit-")
        cls.addClassCleanup(cls.temp.cleanup)
        cls.injector = Path(cls.temp.name) / "injector.so"
        home = ARGS.mf_home
        if (home / "smem/include/host").is_dir():
            includes = [home / "smem/include/host", home / "hybm/include"]
        else:
            includes = [home / "include/smem/host", home / "include/hybm"]
        command = ["mpicxx", "-std=c++17", "-Wall", "-Werror", "-shared", "-fPIC", "-x", "c++", "-"]
        command += ["-I" + str(path) for path in includes]
        command += ["-o", str(cls.injector), "-ldl"]
        subprocess.run(command, input=INJECTOR_SOURCE, text=True, check=True)

    def run_case(self, mode, rank=None, expected=1):
        env = os.environ.copy()
        for name in ("ASCEND_RT_VISIBLE_DEVICES", "DEVICE_ID", "TRANS_TEST_RANK"):
            env.pop(name, None)
        if mode != "normal":
            env["TRANS_TEST_MODE"] = mode
            env["LD_PRELOAD"] = str(self.injector) + (":" + env["LD_PRELOAD"] if env.get("LD_PRELOAD") else "")
        if rank is not None:
            env["TRANS_TEST_RANK"] = str(rank)
        # 同一测试进程中的用例顺序执行，沿用相同配置存储地址。
        command = ["mpirun"] + (["--allow-run-as-root"] if os.geteuid() == 0 else [])
        command += ["-np", "2", str(ARGS.binary), "tcp://127.0.0.1:" + str(ARGS.port)]
        code, output, timed_out, seconds = self.execute_mpi(command, env)
        label = mode + ("_rank" + str(rank) if rank is not None else "_control")
        if ARGS.log_dir:
            (ARGS.log_dir / (label + ".log")).write_text(output)
            (ARGS.log_dir / (label + ".json")).write_text(
                json.dumps(
                    {"exit": code, "timeout": timed_out, "seconds": seconds, "mode": mode, "rank": rank}, indent=2
                )
            )
        self.assertFalse(timed_out, output)
        self.assertEqual(code, expected, output)
        if expected:
            self.assertIn("[INJECT rank=", output)
            self.assertIn("结束MPI任务", output)
        else:
            self.assertEqual(output.count("ALL TESTS PASS"), 2, output)
            self.assertEqual(output.count("verify PASS"), 8, output)
        return output

    def execute_mpi(self, command, env):
        start = time.monotonic()
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
            output, _ = process.communicate(timeout=ARGS.timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
            os.killpg(process.pid, signal.SIGTERM)
            try:
                output, _ = process.communicate(timeout=10)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                output, _ = process.communicate()
        return process.returncode, output, timed_out, round(time.monotonic() - start, 3)

    def test_register_rank0(self):
        self.run_case("register_after", 0)

    def test_register_rank1(self):
        self.run_case("register_after", 1)

    def test_register_both_fail(self):
        self.run_case("register_before", -1)

    def test_register_control(self):
        self.run_case("register_after", expected=0)

    def test_init_failure(self):
        self.run_case("init", 0)

    def test_create_failure(self):
        self.run_case("create", 1)

    def test_allocation_failure(self):
        self.run_case("alloc", 0)

    def test_write_rank0(self):
        self.run_case("write", 0)

    def test_write_rank1(self):
        self.run_case("write", 1)

    def test_write_submit_failure(self):
        self.run_case("write_submit", 0)

    def test_read_failure(self):
        self.run_case("read", 1)

    def test_read_submit_failure(self):
        self.run_case("read_submit", 1)

    def test_deregister_failure(self):
        self.run_case("deregister", 0)

    def test_missing_transfer(self):
        for operation in ("write", "write_submit", "read", "read_submit"):
            for rank in (0, 1):
                with self.subTest(operation=operation, rank=rank):
                    output = self.run_case("skip_" + operation, rank)
                    self.assertIn("skip transfer, return=0", output)
                    self.assertIn("verify FAIL total_errors:4194304", output)
                    self.assertNotIn("ALL TESTS PASS", output)

    def test_completed(self):
        self.run_case("normal", expected=0)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--mf-home", type=Path, default=Path(__file__).resolve().parents[3] / "output")
    parser.add_argument("--log-dir", type=Path)
    parser.add_argument("--port", type=int, default=29610)
    parser.add_argument("--timeout", type=int, default=30)
    ARGS, remaining = parser.parse_known_args()
    ARGS.binary = ARGS.binary.resolve(strict=True)
    ARGS.mf_home = ARGS.mf_home.resolve(strict=True)
    if ARGS.log_dir:
        ARGS.log_dir.mkdir(parents=True, exist_ok=True)
    unittest.main(argv=[__file__, *remaining], verbosity=2)
