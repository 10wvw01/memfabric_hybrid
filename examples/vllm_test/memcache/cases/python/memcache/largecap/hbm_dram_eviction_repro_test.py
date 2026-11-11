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

"""Minimal repro for 107002 (ACL_ERROR_RT_CONTEXT_NULL) on HBM→DRAM eviction.

This test isolates the HBM→DRAM eviction path observed in vLLM integration tests:
  - Writes enough KV cache data to fill HBM (1GB per rank)
  - Continues writing to trigger LRU eviction (HBM→DRAM CopyBlob)
  - Checks whether eviction succeeds or fails with 107002

Test variants:
  A) Eviction on the init thread (same thread that called acl.rt.set_device + store.init)
  B) Eviction on a worker thread (simulates vLLM worker thread scenario)

If A passes but B fails, the root cause is thread-local ACL context not being
set on the worker thread when CopyD2H runs.
"""

import math
import multiprocessing
import os
import socket
import sys
import time

import pytest

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------
NUM_LAYERS = 61
K_SIZE = 128 * 1024
V_SIZE = 16 * 1024
BLOCK_SIZE = 128
SEQ_LEN = 4096

KV_BUF_SIZE_BYTES = math.ceil(SEQ_LEN / BLOCK_SIZE) * (K_SIZE + V_SIZE) * NUM_LAYERS
KV_BUF_SIZE_MB = KV_BUF_SIZE_BYTES / (1024 * 1024)

HBM_TOTAL_MB = 512
NUM_KEYS_TO_FILL_HBM = math.ceil(HBM_TOTAL_MB / KV_BUF_SIZE_MB)
NUM_EXTRA_KEYS = 5

META_SERVICE_PORT = 5000
META_BOOTSTRAP_SECONDS = 3
RANK1_KEEPALIVE_TIMEOUT_S = 300

DEFAULT_DEVICE_ID = 4
DEFAULT_DEVICE_ID_2 = 5

L2G = 0
G2L = 1

TENSOR_ALIGNMENT_BYTES = 2 * 1024 * 1024
NPU_DEVICE = "npu"


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
def _npu_available():
    try:
        import acl
        import torch_npu
        return True
    except ImportError:
        return False


def _is_meta_service_running(host="127.0.0.1", port=META_SERVICE_PORT):
    try:
        with socket.create_connection((host, port), timeout=1):
            return True
    except (OSError, ConnectionRefusedError):
        return False


def _set_config_env():
    root = os.environ.get("MMC_MEMCACHE_DIR", "/home/l30023043/memcache")
    os.environ.setdefault("MMC_META_CONFIG_PATH", os.path.join(root, "config", "mmc-meta.conf"))
    test_config = os.path.join(os.path.dirname(__file__), "..", "..", "config", "mmc-local-eviction-repro.conf")
    if not os.path.exists(test_config):
        test_config = os.path.join(os.path.dirname(__file__), "..", "..", "config", "mmc-local-integration.conf")
    os.environ.setdefault("MMC_LOCAL_CONFIG_PATH", os.path.abspath(test_config))


def _allocate_aligned_tensor(total_bytes, alignment=TENSOR_ALIGNMENT_BYTES):
    import torch
    padding = alignment - 1
    buffer = torch.randint(0, 256, (total_bytes + padding,), dtype=torch.uint8, device=NPU_DEVICE)
    address = buffer.data_ptr()
    aligned_address = (address + alignment - 1) & ~(alignment - 1)
    offset = (aligned_address - address)
    return buffer[offset:offset + total_bytes]


def _rank1_keepalive_worker(device_id, ready_event, stop_event):
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
    for i in range(NUM_LAYERS):
        layer_ptrs.append(k_pool[i].data_ptr())
        layer_ptrs.append(v_pool[i].data_ptr())

    layer_sizes = []
    k_per_layer = num_blocks * K_SIZE
    v_per_layer = num_blocks * V_SIZE
    for _ in range(NUM_LAYERS):
        layer_sizes.append(k_per_layer)
        layer_sizes.append(v_per_layer)

    yield store, layer_ptrs, layer_sizes

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


# ---------------------------------------------------------------------------
# Test A: Eviction on init thread (should work, same as MemCache workload tests)
# ---------------------------------------------------------------------------
def test_eviction_on_init_thread(store_and_buffers):
    store, layer_ptrs, layer_sizes = store_and_buffers
    torch_npu = __import__("torch_npu")

    total_keys = NUM_KEYS_TO_FILL_HBM + NUM_EXTRA_KEYS
    print(f"\n=== Test A: Eviction on init thread ===")
    print(f"  KV buf size: {KV_BUF_SIZE_MB:.1f} MB")
    print(f"  HBM: {HBM_TOTAL_MB} MB, keys to fill: {NUM_KEYS_TO_FILL_HBM}")
    print(f"  Total keys to write: {total_keys}")

    results = []
    for i in range(total_keys):
        key = f"evict_init_{i}"
        torch_npu.npu.current_stream().synchronize()
        ret = store.put_from_layers(key, layer_ptrs, layer_sizes, L2G)
        torch_npu.npu.current_stream().synchronize()
        status = "OK" if ret == 0 else f"FAIL(ret={ret})"
        results.append((i, key, ret, status))
        print(f"  [{i+1}/{total_keys}] {key}: {status}")

    failures = [r for r in results if r[2] != 0]
    print(f"\n  Summary: {len(results) - len(failures)}/{len(results)} succeeded, {len(failures)} failed")
    if failures:
        print(f"  FAILED keys: {[(r[0], r[3]) for r in failures]}")

    assert len(failures) == 0, f"{len(failures)} eviction operations failed with 107002"


# ---------------------------------------------------------------------------
# Test B: Eviction on worker thread (simulates vLLM worker thread scenario)
# ---------------------------------------------------------------------------
def _worker_thread_eviction(store, layer_ptrs, layer_sizes, total_keys, results, tid):
    try:
        import torch_npu
        for i in range(total_keys):
            key = f"evict_worker_{i}"
            torch_npu.npu.current_stream().synchronize()
            ret = store.put_from_layers(key, layer_ptrs, layer_sizes, L2G)
            torch_npu.npu.current_stream().synchronize()
            results.append((i, key, ret))
    except Exception as e:
        results.append((-1, str(e), -999))


def test_eviction_on_worker_thread(store_and_buffers):
    import threading
    store, layer_ptrs, layer_sizes = store_and_buffers
    torch_npu = __import__("torch_npu")

    total_keys = NUM_KEYS_TO_FILL_HBM + NUM_EXTRA_KEYS
    print(f"\n=== Test B: Eviction on worker thread ===")
    print(f"  KV buf size: {KV_BUF_SIZE_MB:.1f} MB")
    print(f"  HBM: {HBM_TOTAL_MB} MB (2 ranks), keys to fill: {NUM_KEYS_TO_FILL_HBM}")
    print(f"  Total keys to write: {total_keys}")

    try:
        store.remove_all()
    except Exception:
        pass

    results = []
    t = threading.Thread(
        target=_worker_thread_eviction,
        args=(store, layer_ptrs, layer_sizes, total_keys, results, 0),
    )
    t.start()
    t.join(timeout=120)

    if t.is_alive():
        print("  [TIMEOUT] Worker thread did not finish in 120s")

    for idx, key, ret in sorted(results, key=lambda x: x[0]):
        status = "OK" if ret == 0 else f"FAIL(ret={ret})"
        print(f"  [{idx+1}/{total_keys}] {key}: {status}")

    failures = [r for r in results if r[2] != 0]
    print(f"\n  Summary: {len(results) - len(failures)}/{len(results)} succeeded, {len(failures)} failed")
    if failures:
        print(f"  FAILED keys: {[(r[0], r[2]) for r in failures]}")

    assert len(failures) == 0, f"{len(failures)} eviction operations failed on worker thread"
