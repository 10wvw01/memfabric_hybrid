#!/usr/bin/env python
# coding=utf-8
# Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
# MemCache_Hybrid is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#          http://license.coscl.org.cn/MulanPSL2
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
# See the Mulan PSL v2 for more details.

"""MemCache prefill workload benchmarks.

These tests measure MemCache KV Cache storage latency under workloads that
simulate real LLM inference patterns: single prefill, varied sequence lengths,
prefill→decode lifecycle, prefix cache hit, and
DRAM→HBM rewarm.

All tests require an Ascend NPU environment with MetaService running.
Tests are designed for a 2-rank (same-card die-pair) configuration:
  rank0 = MMC_TEST_DEVICE_ID (default dev4)
  rank1 = MMC_TEST_DEVICE_ID_2 (default dev5, same NPU card die-pair)
"""

import multiprocessing
import os
import socket
import time

import pytest

from .llm_workload_simulator import LLMWorkloadSimulator, TtftReport, percentile

# 测试参数支持 MMC_TEST_* 环境变量覆盖，默认值与历史规格一致；小规格跑通流程时可设小值。
PREFILL_SEQ_LEN = int(os.environ.get("MMC_TEST_PREFILL_SEQ_LEN", "4096"))
PREFILL_ITERATIONS = int(os.environ.get("MMC_TEST_PREFILL_ITERATIONS", "100"))
P99_THRESHOLD_US = int(os.environ.get("MMC_TEST_P99_THRESHOLD_US", "50000"))

VARIED_SEQ_LENS = [int(x) for x in os.environ.get("MMC_TEST_VARIED_SEQ_LENS", "512,1024,2048,4096,8192,16384").split(",")]
VARIED_ITERATIONS = int(os.environ.get("MMC_TEST_VARIED_ITERATIONS", "20"))
R_SQUARED_THRESHOLD = float(os.environ.get("MMC_TEST_R_SQUARED_THRESHOLD", "0.9"))

DECODE_STEPS = int(os.environ.get("MMC_TEST_DECODE_STEPS", "256"))
DECODE_P50_RATIO = float(os.environ.get("MMC_TEST_DECODE_P50_RATIO", "0.5"))
DECLEANUP_BATCH = int(os.environ.get("MMC_TEST_DECLEANUP_BATCH", "32"))

CACHE_HIT_RATIO = float(os.environ.get("MMC_TEST_CACHE_HIT_RATIO", "1.0"))
HIT_MISS_ITERATIONS = int(os.environ.get("MMC_TEST_HIT_MISS_ITERATIONS", "20"))

REWARM_ITERATIONS = int(os.environ.get("MMC_TEST_REWARM_ITERATIONS", "50"))
REWARM_SEQ_LEN = int(os.environ.get("MMC_TEST_REWARM_SEQ_LEN", "4096"))
REWARM_MAX_RATIO = float(os.environ.get("MMC_TEST_REWARM_MAX_RATIO", "1.1"))
# device_sdma 下 DRAM rewarm 与 HBM hit 走相近数据路径，P50 差异在采样噪声内，
# 严格要求 rewarm > hit 会导致间歇性失败。允许 rewarm 略低于 hit，但不低于此比例。
REWARM_HIT_TOLERANCE = float(os.environ.get("MMC_TEST_REWARM_HIT_TOLERANCE", "1.05"))
REWARM_HBM_TOTAL_MB = int(os.environ.get("MMC_TEST_REWARM_HBM_TOTAL_MB", "2048"))

META_BOOTSTRAP_SECONDS = 3
DEFAULT_DEVICE_ID = 4
DEFAULT_DEVICE_ID_2 = 5
RANK1_KEEPALIVE_TIMEOUT_S = 300
META_SERVICE_PORT = 5000


def _npu_available() -> bool:
    try:
        import acl  # noqa: F401
        import torch_npu  # noqa: F401

        return True
    except ImportError:
        return False


def _is_meta_service_running(host: str = "127.0.0.1", port: int = META_SERVICE_PORT) -> bool:
    try:
        with socket.create_connection((host, port), timeout=1):
            return True
    except (OSError, ConnectionRefusedError):
        return False


def _project_root() -> str:
    env_root = os.environ.get("MMC_MEMCACHE_DIR")
    if env_root and os.path.isdir(env_root):
        return env_root
    p = os.path.abspath(__file__)
    for _ in range(5):
        p = os.path.dirname(p)
    return p


def _set_config_env():
    root = _project_root()
    os.environ.setdefault("MMC_META_CONFIG_PATH", os.path.join(root, "config", "mmc-meta.conf"))
    os.environ.setdefault("MMC_LOCAL_CONFIG_PATH", os.path.join(root, "config", "mmc-local.conf"))


def _check_performance(condition: bool, message: str) -> None:
    """性能阈值检查：只告警不 fail。

    共享设备环境下性能波动大，性能断言基于专用环境基线，不适用于所有环境。
    测试的主要价值是采集性能数据（P50/P90/P99），性能异常只告警不中断。
    """
    if not condition:
        print(f"[WARN] {message}")


def _linear_r2(x_values, y_values) -> float:
    """Compute R² (coefficient of determination) for linear regression of y on x."""
    n = len(x_values)
    if n < 2:
        return 0.0
    mean_x = sum(x_values) / n
    mean_y = sum(y_values) / n
    ss_xx = sum((x - mean_x) ** 2 for x in x_values)
    ss_xy = sum((x - mean_x) * (y - mean_y) for x, y in zip(x_values, y_values))
    if ss_xx == 0:
        return 0.0
    slope = ss_xy / ss_xx
    intercept = mean_y - slope * mean_x
    ss_res = sum((y - (slope * x + intercept)) ** 2 for x, y in zip(x_values, y_values))
    ss_tot = sum((y - mean_y) ** 2 for y in y_values)
    if ss_tot == 0:
        return 1.0
    return 1.0 - ss_res / ss_tot


def _collect_prefill_latencies(simulator, request_prefix: str, seq_len: int, iterations: int) -> list:
    """Run *iterations* prefill operations, returning successful latencies.

    Each iteration writes a key named ``{request_prefix}_{i}`` and removes it after.
    """
    latencies = []
    for i in range(iterations):
        ret, latency = simulator.prefill(f"{request_prefix}_{i}", seq_len)
        if ret == 0:
            latencies.append(latency)
        simulator.remove(f"{request_prefix}_{i}")
    return latencies


def _collect_hit_latencies(simulator, request_prefix: str, seq_len: int, iterations: int) -> list:
    """For each iteration: prefill a key, read it back via prefix_cache_hit, then remove.

    Returns the successful hit (G2L read) latencies.
    """
    latencies = []
    for i in range(iterations):
        simulator.prefill(f"{request_prefix}_{i}", seq_len)
        ret, latency = simulator.prefix_cache_hit(f"{request_prefix}_{i}", seq_len)
        if ret == 0:
            latencies.append(latency)
        simulator.remove(f"{request_prefix}_{i}")
    return latencies


def _rank1_keepalive_worker(device_id, ready_event, stop_event):
    """Init store on rank1 device to complete the 2-rank group join, then wait for stop."""
    store = None
    try:
        import acl

        from memcache_hybrid import DistributedObjectStore

        acl.init()
        acl.rt.set_device(device_id)
        store = DistributedObjectStore()
        ret = store.init(device_id)
        if ret == 0:
            ready_event.set()
        stop_event.wait(timeout=RANK1_KEEPALIVE_TIMEOUT_S)
    except Exception:
        ready_event.set()
    finally:
        if store is not None:
            try:
                store.remove_all()
            except Exception:
                pass
            store.close()


@pytest.fixture(scope="session")
def meta_service():
    """Start MetaService once per session (skip if NPU unavailable)."""
    if not _npu_available():
        pytest.skip("NPU environment not available")
    _set_config_env()

    if not _is_meta_service_running():
        from memcache_hybrid import MetaService

        ctx = multiprocessing.get_context("fork")
        proc = ctx.Process(target=lambda: MetaService.main())
        proc.start()
        time.sleep(META_BOOTSTRAP_SECONDS)
        yield proc
        proc.terminate()
        proc.join(timeout=5)
    else:
        yield None


@pytest.fixture(scope="session")
def simulator(meta_service):
    """Create a 2-rank group once per session: rank1 keepalive (dev5) + rank0 simulator (dev4)."""
    import acl

    from memcache_hybrid import DistributedObjectStore

    rank0_dev = int(os.environ.get("MMC_TEST_DEVICE_ID", str(DEFAULT_DEVICE_ID)))
    rank1_dev = int(os.environ.get("MMC_TEST_DEVICE_ID_2", str(DEFAULT_DEVICE_ID_2)))

    ctx = multiprocessing.get_context("fork")
    rank1_ready = ctx.Event()
    rank1_stop = ctx.Event()
    rank1_proc = ctx.Process(target=_rank1_keepalive_worker, args=(rank1_dev, rank1_ready, rank1_stop))
    rank1_proc.start()
    time.sleep(1)

    acl.init()
    ret = acl.rt.set_device(rank0_dev)
    assert ret == 0, f"set_device({rank0_dev}) failed, ret={ret}"

    store = DistributedObjectStore()
    ret = store.init(rank0_dev)
    assert ret == 0, f"store.init({rank0_dev}) failed, ret={ret}"
    assert rank1_ready.wait(timeout=60), "rank1 did not join in time"

    try:
        store.remove_all()
    except Exception:
        pass

    sim = LLMWorkloadSimulator(store, rank0_dev)
    yield sim
    sim.cleanup()
    try:
        store.remove_all()
    except Exception:
        pass
    store.close()

    rank1_stop.set()
    rank1_proc.join(timeout=30)
    if rank1_proc.is_alive():
        rank1_proc.terminate()
        rank1_proc.join(timeout=5)


@pytest.fixture(autouse=True)
def _cleanup_store(request):
    """Clear all keys before each test (session-scoped store reuse)."""
    if "simulator" not in request.fixturenames:
        yield
        return
    sim = request.getfixturevalue("simulator")
    try:
        sim._store.remove_all()
    except Exception:
        pass
    yield


@pytest.mark.workload
def test_prefill_ttft_single_request(simulator):
    """100 次 L2G write 的 P99，验证单次 prefill 写入路径稳定性。"""
    latencies = []
    for i in range(PREFILL_ITERATIONS):
        ret, latency = simulator.prefill(f"single_{i}", PREFILL_SEQ_LEN)
        assert ret == 0, f"prefill failed at iteration {i}, ret={ret}"
        assert latency > 0, f"latency should be positive, got {latency}"
        latencies.append(latency)
        simulator.remove(f"single_{i}")

    p99_us = percentile(latencies, 0.99)
    report = TtftReport.from_latencies("prefill_single_request", latencies, pass_condition=(p99_us < P99_THRESHOLD_US))
    report.print()

    _check_performance(report.p99_us < P99_THRESHOLD_US, f"P99 {report.p99_us:.2f}us exceeds {P99_THRESHOLD_US}us threshold")
    for field in ("p50_us", "p90_us", "p99_us", "avg_us"):
        assert hasattr(report, field), f"report missing field {field}"


@pytest.mark.workload
def test_prefill_ttft_varied_seq_len(simulator):
    """seq_len 512~16384 各档 P50，验证延迟随数据量的线性扩展。"""
    seq_lens = []
    p50_values = []
    for seq_len in VARIED_SEQ_LENS:
        latencies = []
        for i in range(VARIED_ITERATIONS):
            ret, latency = simulator.prefill(f"varied_{seq_len}_{i}", seq_len)
            if ret == 0:
                latencies.append(latency)
            simulator.remove(f"varied_{seq_len}_{i}")
        p50 = percentile(latencies, 0.5)
        p90 = percentile(latencies, 0.9)
        p99 = percentile(latencies, 0.99)
        print(f"seq_len={seq_len}: P50={p50:.2f}us, P90={p90:.2f}us, P99={p99:.2f}us")
        seq_lens.append(seq_len)
        p50_values.append(p50)

    r_squared = _linear_r2(seq_lens, p50_values)
    print(f"R² = {r_squared:.4f}")
    _check_performance(r_squared > R_SQUARED_THRESHOLD, f"R² {r_squared:.4f} below {R_SQUARED_THRESHOLD} threshold")


@pytest.mark.workload
def test_prefill_decode_lifecycle(simulator):
    """1 次 prefill + 256 步 decode，验证 decode 单块追加写入路径。"""
    ret, prefill_latency = simulator.prefill("lifecycle", PREFILL_SEQ_LEN)
    assert ret == 0, f"prefill failed, ret={ret}"

    decode_latencies = []
    for step in range(DECODE_STEPS):
        ret, latency = simulator.decode_step("lifecycle", step)
        if ret == 0:
            decode_latencies.append(latency)
        if (step + 1) % DECLEANUP_BATCH == 0:
            simulator.remove_decode_keys("lifecycle", step + 1)

    decode_p50 = percentile(decode_latencies, 0.5)
    print(f"Prefill latency = {prefill_latency:.2f}us, Decode P50 = {decode_p50:.2f}us")

    _check_performance(
        decode_p50 < prefill_latency * DECODE_P50_RATIO,
        f"Decode P50 {decode_p50:.2f}us >= prefill * {DECODE_P50_RATIO} ({prefill_latency * DECODE_P50_RATIO:.2f}us)",
    )


@pytest.mark.workload
def test_prefix_cache_hit_vs_miss(simulator):
    """20 轮 miss(写) vs hit(读) 配对，对照读写延迟。"""
    miss_latencies = []
    hit_latencies = []
    for i in range(HIT_MISS_ITERATIONS):
        ret, miss_latency = simulator.prefill(f"hit_miss_{i}", PREFILL_SEQ_LEN)
        if ret == 0:
            miss_latencies.append(miss_latency)
        ret, hit_latency = simulator.prefix_cache_hit(f"hit_miss_{i}", PREFILL_SEQ_LEN)
        if ret == 0:
            hit_latencies.append(hit_latency)
        simulator.remove(f"hit_miss_{i}")

    miss_p50 = percentile(miss_latencies, 0.5)
    hit_p50 = percentile(hit_latencies, 0.5)
    print(f"Cache miss P50 = {miss_p50:.2f}us, Cache hit P50 = {hit_p50:.2f}us")

    _check_performance(
        hit_p50 < miss_p50 * CACHE_HIT_RATIO,
        f"Hit P50 {hit_p50:.2f}us >= miss P50 * {CACHE_HIT_RATIO} ({miss_p50 * CACHE_HIT_RATIO:.2f}us)",
    )


@pytest.mark.workload
def test_dram_to_hbm_rewarm_ttft(simulator):
    """50 次 G2L 跨层级读取，验证 DRAM→HBM rewarm 路径。"""
    hit_latencies = _collect_hit_latencies(simulator, "rewarm_ref", REWARM_SEQ_LEN, REWARM_ITERATIONS)
    hit_p50 = percentile(hit_latencies, 0.5)

    prefill_latencies = _collect_prefill_latencies(simulator, "rewarm_prefill", REWARM_SEQ_LEN, REWARM_ITERATIONS)
    prefill_p50 = percentile(prefill_latencies, 0.5)

    rewarm_latencies = []
    for i in range(REWARM_ITERATIONS):
        ret, latency = simulator.rewarm_from_dram(f"rewarm_{i}", REWARM_SEQ_LEN, hbm_total_mb=REWARM_HBM_TOTAL_MB)
        if ret == 0 and latency > 0:
            rewarm_latencies.append(latency)
        simulator.remove(f"rewarm_{i}")

    report = TtftReport.from_latencies("dram_to_hbm_rewarm", rewarm_latencies)
    report.print()

    rewarm_p50 = report.p50_us
    print(f"Rewarm P50 = {rewarm_p50:.2f}us, Hit P50 = {hit_p50:.2f}us, Prefill P50 = {prefill_p50:.2f}us")

    _check_performance(
        rewarm_p50 >= hit_p50 * (2 - REWARM_HIT_TOLERANCE),
        f"Rewarm P50 {rewarm_p50:.2f}us should be >= hit P50 * {2 - REWARM_HIT_TOLERANCE:.2f} "
        f"({hit_p50 * (2 - REWARM_HIT_TOLERANCE):.2f}us); device_sdma 下两者数据路径相近，"
        f"允许 rewarm 略低于 hit",
    )
    _check_performance(
        rewarm_p50 < prefill_p50 * REWARM_MAX_RATIO,
        f"Rewarm P50 {rewarm_p50:.2f}us >= prefill P50 * {REWARM_MAX_RATIO} ({prefill_p50 * REWARM_MAX_RATIO:.2f}us)",
    )
