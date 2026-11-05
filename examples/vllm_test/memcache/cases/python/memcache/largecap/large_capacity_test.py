#!/usr/bin/env python
# coding=utf-8
# Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
# MemCache_Hybrid is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#          http://license.coscl.cn/MulanPSL2
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE.
# See the Mulan PSL v2 for more details.

"""Layer 2 large-capacity MemCache workload tests (8GB HBM + 20GB DRAM, TP=4).

Tests large-scale KV Cache storage behavior that the 1GB+1GB config cannot
exercise:

  LC-1  HBM capacity fill       — fill 8GB HBM, verify all keys accessible
  LC-2  HBM→DRAM eviction       — overflow HBM, verify eviction to DRAM succeeds
  LC-3  DRAM large-scale rewarm — evict many keys to DRAM, read back, measure TTFT
  LC-4  Large-scale prefix hit  — pre-populate 50+ keys, measure hit TTFT distribution
  LC-5  Sustained write drain   — continuous L2G writes until DRAM fills, measure throughput

KV Cache structure (DeepSeek R1 W8A8):
  61 layers, K=128KB, V=16KB, block_size=128, seq_len=4096
  → 281MB per request per rank

Capacity math (per rank):
  HBM 8GB / 281MB ≈ 29 requests before eviction
  DRAM 20GB / 281MB ≈ 72 requests in DRAM
  Total ≈ 101 requests (HBM + DRAM)
"""

import math
import multiprocessing
import os
import socket
import sys
import time
from typing import Any, Dict, List, Optional, Tuple

import pytest

# ---------------------------------------------------------------------------
# Model structure constants (same as prefill_ttft_test.py)
# ---------------------------------------------------------------------------
NUM_LAYERS = 61
K_SIZE = 128 * 1024
V_SIZE = 16 * 1024
BLOCK_SIZE = 128
SEQ_LEN = 4096

KV_BUF_SIZE_BYTES = math.ceil(SEQ_LEN / BLOCK_SIZE) * (K_SIZE + V_SIZE) * NUM_LAYERS
KV_BUF_SIZE_MB = KV_BUF_SIZE_BYTES / (1024 * 1024)

# ---------------------------------------------------------------------------
# Large-capacity constants
# ---------------------------------------------------------------------------
HBM_SIZE_MB = 3 * 1024
DRAM_SIZE_MB = 10 * 1024

HBM_CAPACITY_REQUESTS = math.floor(HBM_SIZE_MB / KV_BUF_SIZE_MB)
DRAM_CAPACITY_REQUESTS = math.floor(DRAM_SIZE_MB / KV_BUF_SIZE_MB)
TOTAL_CAPACITY_REQUESTS = HBM_CAPACITY_REQUESTS + DRAM_CAPACITY_REQUESTS

EVICTION_TRIGGER_EXTRA = 5
LARGE_SCALE_PREPOPULATE = 40
SUSTAINED_WRITE_COUNT = 42

META_SERVICE_PORT = 5000
META_BOOTSTRAP_SECONDS = 3
RANK_KEEPALIVE_TIMEOUT_S = 600

DEFAULT_DEVICE_ID = 4
DEFAULT_DEVICE_ID_2 = 5
DEFAULT_DEVICE_ID_3 = 6
DEFAULT_DEVICE_ID_4 = 7
WORLD_SIZE = 4

L2G = 0
G2L = 1

TENSOR_ALIGNMENT_BYTES = 2 * 1024 * 1024
NPU_DEVICE = "npu"

# Thresholds
HBM_FILL_LATENCY_P99_MS = 50.0
EVICTION_SUCCESS_RATIO = 0.95
DRAM_REWARM_P99_MS = 100.0
PREFIX_HIT_P99_MS = 150.0
SUSTAINED_WRITE_MIN_THROUGHPUT_MBPS = 50.0


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
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


def _set_config_env():
    root = os.environ.get("MMC_MEMCACHE_DIR", "/home/l30023043/memcache")
    os.environ.setdefault("MMC_META_CONFIG_PATH", os.path.join(root, "config", "mmc-meta.conf"))
    test_config = os.path.join(os.path.dirname(__file__), "..", "..", "config", "mmc-local-largecap-4rank.conf")
    if not os.path.exists(test_config):
        test_config = os.path.join(root, "config", "mmc-local.conf")
    os.environ.setdefault("MMC_LOCAL_CONFIG_PATH", os.path.abspath(test_config))


def _percentile(values: List[float], pct: float) -> float:
    if not values:
        return 0.0
    sorted_values = sorted(values)
    n = len(sorted_values)
    rank = pct * (n - 1)
    lower = int(rank)
    upper = min(lower + 1, n - 1)
    fraction = rank - lower
    return sorted_values[lower] * (1 - fraction) + sorted_values[upper] * fraction


def _allocate_aligned_tensor(total_bytes: int, alignment: int = TENSOR_ALIGNMENT_BYTES):
    import torch
    padding = alignment - 1
    buffer = torch.randint(0, 256, (total_bytes + padding,), dtype=torch.uint8, device=NPU_DEVICE)
    address = buffer.data_ptr()
    aligned_address = (address + alignment - 1) & ~(alignment - 1)
    offset = (aligned_address - address)
    return buffer[offset:offset + total_bytes]


def _rank_keepalive_worker(device_id: int, ready_event, stop_event):
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
        else:
            print(f"[WARN] rank dev={device_id} store.init returned {ret}")
            ready_event.set()
        stop_event.wait(timeout=RANK_KEEPALIVE_TIMEOUT_S)
    except Exception as e:
        print(f"[ERROR] rank keepalive dev={device_id}: {e}")
        ready_event.set()
    finally:
        if store is not None:
            try:
                store.remove_all()
            except Exception:
                pass
            store.close()


# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------
@pytest.fixture(scope="session")
def meta_service():
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
def store_and_buffers(meta_service):
    import acl
    import torch_npu
    from memcache_hybrid import DistributedObjectStore

    rank0_dev = int(os.environ.get("MMC_TEST_DEVICE_ID", str(DEFAULT_DEVICE_ID)))
    rank1_dev = int(os.environ.get("MMC_TEST_DEVICE_ID_2", str(DEFAULT_DEVICE_ID_2)))
    rank2_dev = int(os.environ.get("MMC_TEST_DEVICE_ID_3", str(DEFAULT_DEVICE_ID_3)))
    rank3_dev = int(os.environ.get("MMC_TEST_DEVICE_ID_4", str(DEFAULT_DEVICE_ID_4)))

    ctx = multiprocessing.get_context("fork")
    rank_procs = []
    rank_events = []
    rank_stops = []
    for dev in [rank1_dev, rank2_dev, rank3_dev]:
        ready = ctx.Event()
        stop = ctx.Event()
        proc = ctx.Process(target=_rank_keepalive_worker, args=(dev, ready, stop))
        proc.start()
        rank_procs.append(proc)
        rank_events.append(ready)
        rank_stops.append(stop)

    time.sleep(2)

    acl.init()
    ret = acl.rt.set_device(rank0_dev)
    assert ret == 0, f"set_device({rank0_dev}) failed, ret={ret}"

    store = DistributedObjectStore()
    ret = store.init(rank0_dev)
    assert ret == 0, f"store.init({rank0_dev}) failed, ret={ret}"

    for i, evt in enumerate(rank_events):
        assert evt.wait(timeout=120), f"rank{i+1} (dev={[rank1_dev, rank2_dev, rank3_dev][i]}) did not join in time"

    try:
        store.remove_all()
    except Exception:
        pass

    num_blocks = math.ceil(SEQ_LEN / BLOCK_SIZE)
    k_total = num_blocks * K_SIZE * NUM_LAYERS
    v_total = num_blocks * V_SIZE * NUM_LAYERS

    k_pool = _allocate_aligned_tensor(k_total)
    v_pool = _allocate_aligned_tensor(v_total)
    torch_npu.npu.current_stream().synchronize()

    ret = store.register_buffer(k_pool.data_ptr(), k_total)
    assert ret == 0, f"register_buffer(K) failed, ret={ret}"
    ret = store.register_buffer(v_pool.data_ptr(), v_total)
    assert ret == 0, f"register_buffer(V) failed, ret={ret}"

    layer_ptrs = []
    layer_sizes = []
    k_per_layer = num_blocks * K_SIZE
    v_per_layer = num_blocks * V_SIZE
    for i in range(NUM_LAYERS):
        layer_ptrs.append(k_pool[i].data_ptr())
        layer_ptrs.append(v_pool[i].data_ptr())
        layer_sizes.append(k_per_layer)
        layer_sizes.append(v_per_layer)

    yield store, layer_ptrs, layer_sizes, k_pool, v_pool

    try:
        store.remove_all()
    except Exception:
        pass
    store.close()
    for stop in rank_stops:
        stop.set()
    for proc in rank_procs:
        proc.join(timeout=30)
        if proc.is_alive():
            proc.terminate()
            proc.join(timeout=5)


def _put_kv(store, key: str, layer_ptrs: List[int], layer_sizes: List[int]) -> int:
    import torch_npu
    torch_npu.npu.current_stream().synchronize()
    ret = store.put_from_layers(key, layer_ptrs, layer_sizes, L2G)
    torch_npu.npu.current_stream().synchronize()
    return ret


def _get_kv(store, key: str, layer_ptrs: List[int], layer_sizes: List[int]) -> int:
    import torch_npu
    torch_npu.npu.current_stream().synchronize()
    ret = store.get_into_layers(key, layer_ptrs, layer_sizes, G2L)
    torch_npu.npu.current_stream().synchronize()
    return ret


def _put_kv_timed(store, key: str, layer_ptrs: List[int], layer_sizes: List[int]) -> Tuple[int, float]:
    import torch_npu
    torch_npu.npu.current_stream().synchronize()
    start = time.perf_counter()
    ret = store.put_from_layers(key, layer_ptrs, layer_sizes, L2G)
    torch_npu.npu.current_stream().synchronize()
    end = time.perf_counter()
    return ret, (end - start) * 1000.0


def _get_kv_timed(store, key: str, layer_ptrs: List[int], layer_sizes: List[int]) -> Tuple[int, float]:
    import torch_npu
    torch_npu.npu.current_stream().synchronize()
    start = time.perf_counter()
    ret = store.get_into_layers(key, layer_ptrs, layer_sizes, G2L)
    torch_npu.npu.current_stream().synchronize()
    end = time.perf_counter()
    return ret, (end - start) * 1000.0


# ---------------------------------------------------------------------------
# LC-1: HBM capacity fill — fill 8GB HBM, verify all keys accessible
# ---------------------------------------------------------------------------
@pytest.mark.largecap
def test_hbm_capacity_fill_8gb(store_and_buffers):
    store, layer_ptrs, layer_sizes, _, _ = store_and_buffers
    try:
        store.remove_all()
    except Exception:
        pass

    num_keys = HBM_CAPACITY_REQUESTS
    print(f"\n=== LC-1: HBM capacity fill ===")
    print(f"  HBM: {HBM_SIZE_MB}MB, KV buf: {KV_BUF_SIZE_MB:.1f}MB")
    print(f"  Keys to fill HBM: {num_keys}")

    latencies: List[float] = []
    failures = 0
    for i in range(num_keys):
        key = f"lc1_hbm_fill_{i}"
        ret, lat_ms = _put_kv_timed(store, key, layer_ptrs, layer_sizes)
        if ret == 0:
            latencies.append(lat_ms)
        else:
            failures += 1
            print(f"  [{i+1}/{num_keys}] {key}: FAIL(ret={ret})")
        if (i + 1) % 10 == 0:
            print(f"  [{i+1}/{num_keys}] written, avg={sum(latencies)/len(latencies):.2f}ms" if latencies else "")

    p50 = _percentile(latencies, 0.50) if latencies else 0.0
    p99 = _percentile(latencies, 0.99) if latencies else 0.0
    avg = sum(latencies) / len(latencies) if latencies else 0.0

    verify_failures = 0
    for i in range(num_keys):
        key = f"lc1_hbm_fill_{i}"
        if store.is_exist(key) != 1:
            verify_failures += 1

    print(f"\n  Summary: {num_keys - failures}/{num_keys} writes succeeded, {failures} failed")
    print(f"  Write latency: avg={avg:.2f}ms P50={p50:.2f}ms P99={p99:.2f}ms")
    print(f"  Verify: {num_keys - verify_failures}/{num_keys} keys exist")

    assert failures == 0, f"{failures} HBM fill writes failed"
    assert verify_failures == 0, f"{verify_failures} keys not found after HBM fill"
    assert p99 < HBM_FILL_LATENCY_P99_MS, f"HBM fill P99 {p99:.2f}ms exceeds {HBM_FILL_LATENCY_P99_MS}ms"

    try:
        store.remove_all()
    except Exception:
        pass


# ---------------------------------------------------------------------------
# LC-2: HBM→DRAM eviction — overflow HBM, verify eviction to DRAM
# ---------------------------------------------------------------------------
@pytest.mark.largecap
def test_hbm_to_dram_eviction_large(store_and_buffers):
    store, layer_ptrs, layer_sizes, _, _ = store_and_buffers
    try:
        store.remove_all()
    except Exception:
        pass

    total_keys = HBM_CAPACITY_REQUESTS + EVICTION_TRIGGER_EXTRA
    print(f"\n=== LC-2: HBM→DRAM eviction ===")
    print(f"  HBM capacity: {HBM_CAPACITY_REQUESTS} keys")
    print(f"  Total keys to write: {total_keys} (overflow by {EVICTION_TRIGGER_EXTRA})")

    results: List[Tuple[int, int, float]] = []
    for i in range(total_keys):
        key = f"lc2_evict_{i}"
        ret, lat_ms = _put_kv_timed(store, key, layer_ptrs, layer_sizes)
        results.append((i, ret, lat_ms))
        status = "OK" if ret == 0 else f"FAIL(ret={ret})"
        evict = " ← eviction" if i >= HBM_CAPACITY_REQUESTS else ""
        print(f"  [{i+1}/{total_keys}] {key}: {status} ({lat_ms:.2f}ms){evict}")

    successes = sum(1 for _, ret, _ in results if ret == 0)
    failures = [(i, ret) for i, ret, _ in results if ret != 0]
    evict_latencies = [lat for i, _, lat in results if i >= HBM_CAPACITY_REQUESTS and results[i][1] == 0]
    hbm_latencies = [lat for i, _, lat in results if i < HBM_CAPACITY_REQUESTS and results[i][1] == 0]

    evict_p50 = _percentile(evict_latencies, 0.50) if evict_latencies else 0.0
    hbm_p50 = _percentile(hbm_latencies, 0.50) if hbm_latencies else 0.0

    verify_count = 0
    for i in range(total_keys):
        key = f"lc2_evict_{i}"
        if store.is_exist(key) == 1:
            verify_count += 1

    print(f"\n  Summary: {successes}/{total_keys} succeeded, {len(failures)} failed")
    print(f"  HBM write P50: {hbm_p50:.2f}ms, Eviction write P50: {evict_p50:.2f}ms")
    print(f"  Verify: {verify_count}/{total_keys} keys still accessible after eviction")
    if failures:
        print(f"  FAILED: {failures}")

    success_ratio = successes / total_keys
    assert success_ratio >= EVICTION_SUCCESS_RATIO, (
        f"Eviction success ratio {success_ratio:.2%} < {EVICTION_SUCCESS_RATIO:.0%}"
    )
    assert verify_count >= total_keys * EVICTION_SUCCESS_RATIO, (
        f"Only {verify_count}/{total_keys} keys accessible after eviction"
    )

    try:
        store.remove_all()
    except Exception:
        pass


# ---------------------------------------------------------------------------
# LC-3: DRAM large-scale rewarm — evict many keys, read back from DRAM
# ---------------------------------------------------------------------------
@pytest.mark.largecap
def test_dram_rewarm_large_scale(store_and_buffers):
    store, layer_ptrs, layer_sizes, _, _ = store_and_buffers
    try:
        store.remove_all()
    except Exception:
        pass

    target_count = 10
    total_fill = HBM_CAPACITY_REQUESTS + target_count

    print(f"\n=== LC-3: DRAM large-scale rewarm ===")
    print(f"  Target keys: {target_count} (will be evicted to DRAM)")
    print(f"  Fill keys: {total_fill} (overflow HBM by {target_count})")

    print(f"  Phase 1: Writing target keys...")
    for i in range(target_count):
        key = f"lc3_target_{i}"
        ret = _put_kv(store, key, layer_ptrs, layer_sizes)
        assert ret == 0, f"Write target {i} failed, ret={ret}"

    print(f"  Phase 2: Filling HBM with dummy keys to trigger eviction...")
    for i in range(HBM_CAPACITY_REQUESTS):
        key = f"lc3_dummy_{i}"
        ret = _put_kv(store, key, layer_ptrs, layer_sizes)
        if ret != 0:
            print(f"  [{i+1}/{HBM_CAPACITY_REQUESTS}] dummy write FAIL(ret={ret}), stopping fill")
            break
        if (i + 1) % 10 == 0:
            print(f"  [{i+1}/{HBM_CAPACITY_REQUESTS}] dummy written")

    print(f"  Phase 3: Verifying targets evicted to DRAM (is_exist)...")
    survived = 0
    for i in range(target_count):
        key = f"lc3_target_{i}"
        if store.is_exist(key) == 1:
            survived += 1
    print(f"  {survived}/{target_count} targets survived in store (evicted to DRAM)")

    print(f"  Phase 4: Rewarming targets from DRAM→HBM (G2L)...")
    rewarm_latencies: List[float] = []
    rewarm_failures = 0
    for i in range(target_count):
        key = f"lc3_target_{i}"
        ret, lat_ms = _get_kv_timed(store, key, layer_ptrs, layer_sizes)
        if ret == 0:
            rewarm_latencies.append(lat_ms)
            print(f"  [{i+1}/{target_count}] {key}: rewarm {lat_ms:.2f}ms")
        else:
            rewarm_failures += 1
            print(f"  [{i+1}/{target_count}] {key}: FAIL(ret={ret})")

    p50 = _percentile(rewarm_latencies, 0.50) if rewarm_latencies else 0.0
    p99 = _percentile(rewarm_latencies, 0.99) if rewarm_latencies else 0.0
    avg = sum(rewarm_latencies) / len(rewarm_latencies) if rewarm_latencies else 0.0

    print(f"\n  Summary: {len(rewarm_latencies)}/{target_count} rewarm succeeded, {rewarm_failures} failed")
    print(f"  Rewarm latency: avg={avg:.2f}ms P50={p50:.2f}ms P99={p99:.2f}ms")

    assert rewarm_failures == 0, f"{rewarm_failures} rewarm operations failed"
    assert p99 < DRAM_REWARM_P99_MS, f"Rewarm P99 {p99:.2f}ms exceeds {DRAM_REWARM_P99_MS}ms"

    try:
        store.remove_all()
    except Exception:
        pass


# ---------------------------------------------------------------------------
# LC-4: Large-scale prefix cache hit — pre-populate 50+ keys, measure hit TTFT
# ---------------------------------------------------------------------------
@pytest.mark.largecap
def test_large_scale_prefix_cache_hit(store_and_buffers):
    store, layer_ptrs, layer_sizes, _, _ = store_and_buffers
    try:
        store.remove_all()
    except Exception:
        pass

    num_keys = LARGE_SCALE_PREPOPULATE
    print(f"\n=== LC-4: Large-scale prefix cache hit ===")
    print(f"  Pre-populating {num_keys} keys ({num_keys * KV_BUF_SIZE_MB:.0f}MB total)")
    print(f"  HBM capacity: {HBM_CAPACITY_REQUESTS} keys, DRAM capacity: {DRAM_CAPACITY_REQUESTS} keys")

    print(f"  Phase 1: Pre-populating...")
    write_latencies: List[float] = []
    write_failures = 0
    for i in range(num_keys):
        key = f"lc4_prepop_{i}"
        ret, lat_ms = _put_kv_timed(store, key, layer_ptrs, layer_sizes)
        if ret == 0:
            write_latencies.append(lat_ms)
        else:
            write_failures += 1
        if (i + 1) % 10 == 0:
            print(f"  [{i+1}/{num_keys}] written, avg={sum(write_latencies)/len(write_latencies):.2f}ms" if write_latencies else "")

    write_p50 = _percentile(write_latencies, 0.50) if write_latencies else 0.0
    print(f"  Write: {num_keys - write_failures}/{num_keys} succeeded, P50={write_p50:.2f}ms")

    print(f"  Phase 2: Reading all keys back (G2L)...")
    read_latencies: List[float] = []
    read_failures = 0
    for i in range(num_keys):
        key = f"lc4_prepop_{i}"
        ret, lat_ms = _get_kv_timed(store, key, layer_ptrs, layer_sizes)
        if ret == 0:
            read_latencies.append(lat_ms)
        else:
            read_failures += 1
        if (i + 1) % 10 == 0:
            avg = sum(read_latencies) / len(read_latencies) if read_latencies else 0.0
            print(f"  [{i+1}/{num_keys}] read, avg={avg:.2f}ms")

    read_p50 = _percentile(read_latencies, 0.50) if read_latencies else 0.0
    read_p90 = _percentile(read_latencies, 0.90) if read_latencies else 0.0
    read_p99 = _percentile(read_latencies, 0.99) if read_latencies else 0.0
    read_avg = sum(read_latencies) / len(read_latencies) if read_latencies else 0.0

    in_hbm = min(num_keys, HBM_CAPACITY_REQUESTS)
    in_dram = max(0, num_keys - HBM_CAPACITY_REQUESTS)

    print(f"\n  Summary: {len(read_latencies)}/{num_keys} reads succeeded, {read_failures} failed")
    print(f"  Expected: {in_hbm} in HBM, {in_dram} in DRAM (evicted)")
    print(f"  Read latency: avg={read_avg:.2f}ms P50={read_p50:.2f}ms P90={read_p90:.2f}ms P99={read_p99:.2f}ms")

    assert read_failures == 0, f"{read_failures} read operations failed"
    assert read_p99 < PREFIX_HIT_P99_MS, f"Read P99 {read_p99:.2f}ms exceeds {PREFIX_HIT_P99_MS}ms"

    try:
        store.remove_all()
    except Exception:
        pass


# ---------------------------------------------------------------------------
# LC-5: Sustained write drain — continuous L2G writes, measure throughput
# ---------------------------------------------------------------------------
@pytest.mark.largecap
def test_sustained_write_drain(store_and_buffers):
    store, layer_ptrs, layer_sizes, _, _ = store_and_buffers
    try:
        store.remove_all()
    except Exception:
        pass

    num_keys = SUSTAINED_WRITE_COUNT
    print(f"\n=== LC-5: Sustained write drain ===")
    print(f"  Writing {num_keys} keys continuously ({num_keys * KV_BUF_SIZE_MB:.0f}MB total)")
    print(f"  HBM capacity: {HBM_CAPACITY_REQUESTS} keys, DRAM capacity: {DRAM_CAPACITY_REQUESTS} keys")
    print(f"  Expected: first {HBM_CAPACITY_REQUESTS} in HBM, rest evicted to DRAM")

    latencies: List[float] = []
    failures = 0
    total_bytes = 0
    start_time = time.perf_counter()

    for i in range(num_keys):
        key = f"lc5_drain_{i}"
        ret, lat_ms = _put_kv_timed(store, key, layer_ptrs, layer_sizes)
        if ret == 0:
            latencies.append(lat_ms)
            total_bytes += KV_BUF_SIZE_BYTES
        else:
            failures += 1
            print(f"  [{i+1}/{num_keys}] FAIL(ret={ret})")
        if (i + 1) % 20 == 0:
            elapsed = time.perf_counter() - start_time
            throughput_mbps = (total_bytes / (1024 * 1024)) / elapsed if elapsed > 0 else 0
            avg_lat = sum(latencies) / len(latencies) if latencies else 0.0
            tier = "HBM" if i < HBM_CAPACITY_REQUESTS else "DRAM(evict)"
            print(f"  [{i+1}/{num_keys}] {tier} avg={avg_lat:.2f}ms throughput={throughput_mbps:.1f}MB/s")

    elapsed = time.perf_counter() - start_time
    total_mb = total_bytes / (1024 * 1024)
    throughput_mbps = total_mb / elapsed if elapsed > 0 else 0.0

    hbm_lats = [lat for i, lat in enumerate(latencies) if i < HBM_CAPACITY_REQUESTS]
    dram_lats = [lat for i, lat in enumerate(latencies) if i >= HBM_CAPACITY_REQUESTS]

    hbm_p50 = _percentile(hbm_lats, 0.50) if hbm_lats else 0.0
    dram_p50 = _percentile(dram_lats, 0.50) if dram_lats else 0.0

    print(f"\n  Summary: {num_keys - failures}/{num_keys} succeeded, {failures} failed")
    print(f"  Total: {total_mb:.0f}MB in {elapsed:.1f}s = {throughput_mbps:.1f}MB/s")
    print(f"  HBM phase P50: {hbm_p50:.2f}ms, DRAM(evict) phase P50: {dram_p50:.2f}ms")

    assert failures == 0, f"{failures} sustained write operations failed"
    assert throughput_mbps >= SUSTAINED_WRITE_MIN_THROUGHPUT_MBPS, (
        f"Throughput {throughput_mbps:.1f}MB/s < {SUSTAINED_WRITE_MIN_THROUGHPUT_MBPS}MB/s"
    )

    try:
        store.remove_all()
    except Exception:
        pass
