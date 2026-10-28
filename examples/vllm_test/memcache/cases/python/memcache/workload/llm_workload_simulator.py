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

"""LLM workload simulator for TTFT benchmark testing.

Simulates four KV Cache access patterns found in real LLM inference:
  - prefill:           write full-sequence KV cache to MemCache (L2G)
  - decode_step:       append single-block KV cache (L2G)
  - prefix_cache_hit:  read existing KV cache from MemCache (G2L)
  - rewarm_from_dram:  evict target to DRAM then read it back to HBM (G2L)

The KV cache structure (61 layers, K=128KB, V=16KB) reuses the DeepSeek R1
W8A8 parameters from ``example/benchmark/mutil_process.py``.
"""

import math
import time
from typing import Any, Dict, List, Optional, Tuple

import torch

try:
    import torch_npu

    _HAS_TORCH_NPU = True
except ImportError:
    torch_npu = None
    _HAS_TORCH_NPU = False

try:
    from memcache_hybrid import L2G, G2L

    _HAS_MEMCACHE = True
except ImportError:
    L2G = 0
    G2L = 1
    _HAS_MEMCACHE = False


# --- Model structure constants (DeepSeek R1 W8A8, mutil_process.py:24-26) ---
DEFAULT_NUM_LAYERS = 61
DEFAULT_K_SIZE = 128 * 1024
DEFAULT_V_SIZE = 16 * 1024
DEFAULT_BLOCK_SIZE = 128
DEFAULT_MAX_BLOCKS = 128

# --- Tensor allocation constants ---
TENSOR_ALIGNMENT_BYTES = 2 * 1024 * 1024
NPU_DEVICE = "npu"

# --- Timing constants ---
MICROSECONDS_PER_SECOND = 1_000_000

# --- Rewarm constants ---
DEFAULT_HBM_TOTAL_MB = 1024
EVICTION_SAFETY_MARGIN = 1

# --- Key prefixes ---
PREFILL_KEY_PREFIX = "kv_"
DECODE_KEY_PREFIX = "kv_decode_"
DUMMY_KEY_PREFIX = "dummy_"


def _allocate_aligned_tensor(shape, dtype=torch.uint8, alignment=TENSOR_ALIGNMENT_BYTES):
    """Allocate a 2 MB-aligned NPU tensor (reuses mutil_process.py:52-69 pattern)."""
    num_elements = torch.prod(torch.tensor(shape)).item()
    if dtype.is_floating_point:
        element_size = torch.finfo(dtype).bits // 8
    else:
        element_size = torch.iinfo(dtype).bits // 8
    total_bytes = num_elements * element_size
    padding = alignment - 1
    buffer = torch.randint(0, 256, (total_bytes + padding,), dtype=dtype, device=NPU_DEVICE)
    address = buffer.data_ptr()
    aligned_address = (address + alignment - 1) & ~(alignment - 1)
    offset = (aligned_address - address) // element_size
    aligned_tensor = buffer[offset : offset + num_elements].view(*shape)
    return aligned_tensor


class LLMWorkloadSimulator:
    """Simulates LLM inference KV Cache access patterns for TTFT measurement."""

    def __init__(self, store: Any, device_id: int, model_config: Optional[Dict] = None):
        self._store = store
        self._device_id = device_id
        config = model_config or {}
        self._num_layers = config.get("num_layers", DEFAULT_NUM_LAYERS)
        self._k_size = config.get("k_size", DEFAULT_K_SIZE)
        self._v_size = config.get("v_size", DEFAULT_V_SIZE)
        self._block_size = config.get("block_size", DEFAULT_BLOCK_SIZE)
        self._max_blocks = config.get("max_blocks", DEFAULT_MAX_BLOCKS)
        self._k_pool: Optional[torch.Tensor] = None
        self._v_pool: Optional[torch.Tensor] = None
        self._registered_buffers: List[Tuple[int, int]] = []
        self._alloc_kv_tensors(self._max_blocks)

    # ------------------------------------------------------------------
    # 2.2  K/V interleaved layer sizes
    # ------------------------------------------------------------------
    def _kv_layer_sizes(self, num_blocks: int) -> List[int]:
        """Return K/V interleaved sizes for *num_blocks* blocks.

        Length == 2 * num_layers, alternating K/V per layer.
        """
        k_total = num_blocks * self._k_size
        v_total = num_blocks * self._v_size
        sizes: List[int] = []
        for _ in range(self._num_layers):
            sizes.append(k_total)
            sizes.append(v_total)
        return sizes

    # ------------------------------------------------------------------
    # 2.3  Allocate NPU tensors and register buffers
    # ------------------------------------------------------------------
    def _alloc_kv_tensors(self, num_blocks: int) -> List[torch.Tensor]:
        """Pre-allocate K/V NPU tensor pools and register them with the store.

        Returns a list of 2 * num_layers layer-view tensors (interleaved K/V).
        """
        k_pool = _allocate_aligned_tensor((self._num_layers, num_blocks, self._k_size))
        v_pool = _allocate_aligned_tensor((self._num_layers, num_blocks, self._v_size))
        self._sync_stream()

        k_total_bytes = self._k_size * self._num_layers * num_blocks
        v_total_bytes = self._v_size * self._num_layers * num_blocks
        ret = self._store.register_buffer(k_pool.data_ptr(), k_total_bytes)
        if ret != 0:
            raise RuntimeError(f"register_buffer(K pool) failed, ret={ret}")
        ret = self._store.register_buffer(v_pool.data_ptr(), v_total_bytes)
        if ret != 0:
            raise RuntimeError(f"register_buffer(V pool) failed, ret={ret}")

        self._k_pool = k_pool
        self._v_pool = v_pool
        self._registered_buffers.append((k_pool.data_ptr(), k_total_bytes))
        self._registered_buffers.append((v_pool.data_ptr(), v_total_bytes))

        tensors: List[torch.Tensor] = []
        for i in range(self._num_layers):
            tensors.append(k_pool[i])
            tensors.append(v_pool[i])
        return tensors

    # ------------------------------------------------------------------
    # Helpers
    # ------------------------------------------------------------------
    def _sync_stream(self):
        if _HAS_TORCH_NPU:
            torch_npu.npu.current_stream().synchronize()

    def _layer_ptrs(self) -> List[int]:
        """Return 2 * num_layers data pointers (interleaved K/V) from the pool."""
        ptrs: List[int] = []
        for i in range(self._num_layers):
            ptrs.append(self._k_pool[i].data_ptr())
            ptrs.append(self._v_pool[i].data_ptr())
        return ptrs

    def _ensure_capacity(self, num_blocks: int):
        if num_blocks > self._max_blocks:
            self._free_pools()
            self._max_blocks = num_blocks
            self._alloc_kv_tensors(num_blocks)

    def _put_kv(self, key: str, num_blocks: int) -> int:
        ptrs = self._layer_ptrs()
        sizes = self._kv_layer_sizes(num_blocks)
        return self._store.put_from_layers(key, ptrs, sizes, L2G)

    def _get_kv(self, key: str, num_blocks: int) -> int:
        ptrs = self._layer_ptrs()
        sizes = self._kv_layer_sizes(num_blocks)
        return self._store.get_into_layers(key, ptrs, sizes, G2L)

    # ------------------------------------------------------------------
    # 2.4  Prefill
    # ------------------------------------------------------------------
    def prefill(self, request_id: str, seq_len: int) -> Tuple[int, float]:
        """Simulate prefill: write seq_len tokens of KV cache via put_from_layers (L2G)."""
        num_blocks = math.ceil(seq_len / self._block_size)
        self._ensure_capacity(num_blocks)
        key = f"{PREFILL_KEY_PREFIX}{request_id}"
        self._sync_stream()
        start = time.perf_counter()
        ret = self._put_kv(key, num_blocks)
        self._sync_stream()
        end = time.perf_counter()
        latency_us = (end - start) * MICROSECONDS_PER_SECOND
        return ret, latency_us

    # ------------------------------------------------------------------
    # 2.5  Decode step
    # ------------------------------------------------------------------
    def decode_step(self, request_id: str, step: int) -> Tuple[int, float]:
        """Simulate single-token decode: append 1 block of KV cache via put_from_layers (L2G)."""
        num_blocks = 1
        key = f"{DECODE_KEY_PREFIX}{request_id}_{step}"
        self._sync_stream()
        start = time.perf_counter()
        ret = self._put_kv(key, num_blocks)
        self._sync_stream()
        end = time.perf_counter()
        latency_us = (end - start) * MICROSECONDS_PER_SECOND
        return ret, latency_us

    # ------------------------------------------------------------------
    # 2.6  Prefix cache hit
    # ------------------------------------------------------------------
    def prefix_cache_hit(self, request_id: str, prefix_len: int) -> Tuple[int, float]:
        """Simulate prefix cache hit: read existing KV cache via get_into_layers (G2L)."""
        num_blocks = math.ceil(prefix_len / self._block_size)
        key = f"{PREFILL_KEY_PREFIX}{request_id}"
        self._sync_stream()
        start = time.perf_counter()
        ret = self._get_kv(key, num_blocks)
        self._sync_stream()
        end = time.perf_counter()
        latency_us = (end - start) * MICROSECONDS_PER_SECOND
        return ret, latency_us

    # ------------------------------------------------------------------
    # 2.7  DRAM → HBM rewarm
    # ------------------------------------------------------------------
    def rewarm_from_dram(
        self, request_id: str, seq_len: int, hbm_total_mb: int = DEFAULT_HBM_TOTAL_MB
    ) -> Tuple[int, float]:
        """Simulate DRAM→HBM rewarm: evict target to DRAM, then read back via G2L."""
        num_blocks = math.ceil(seq_len / self._block_size)
        self._ensure_capacity(num_blocks)
        target_key = f"{PREFILL_KEY_PREFIX}{request_id}"

        # Step 1 — write target KV cache to HBM
        ret = self._put_kv(target_key, num_blocks)
        if ret != 0:
            return ret, 0.0

        # Step 2 — fill HBM with dummy keys to trigger LRU eviction
        kv_buf_size_bytes = num_blocks * (self._k_size + self._v_size) * self._num_layers
        kv_buf_size_mb = kv_buf_size_bytes / (1024 * 1024)
        key_num = math.ceil(hbm_total_mb / kv_buf_size_mb) + EVICTION_SAFETY_MARGIN
        for i in range(key_num):
            dummy_key = f"{DUMMY_KEY_PREFIX}{request_id}_{i}"
            ret = self._put_kv(dummy_key, num_blocks)
            if ret != 0:
                break

        # Step 3 — verify target key survived (evicted HBM→DRAM)
        exist = self._store.is_exist(target_key)
        if exist != 1:
            return -1, 0.0

        # Step 4 — rewarm: read target from DRAM back to HBM (G2L)
        self._sync_stream()
        start = time.perf_counter()
        ret = self._get_kv(target_key, num_blocks)
        self._sync_stream()
        end = time.perf_counter()
        latency_us = (end - start) * MICROSECONDS_PER_SECOND

        # Step 5 — cleanup dummy keys to avoid store exhaustion across iterations
        for i in range(key_num):
            dummy_key = f"{DUMMY_KEY_PREFIX}{request_id}_{i}"
            try:
                self._store.remove(dummy_key)
            except Exception:
                pass

        return ret, latency_us

    # ------------------------------------------------------------------
    # 2.8  Cleanup
    # ------------------------------------------------------------------
    def _free_pools(self):
        for ptr, size in self._registered_buffers:
            try:
                self._store.unregister_buffer(ptr, size)
            except Exception:
                pass
        self._registered_buffers.clear()
        self._k_pool = None
        self._v_pool = None

    def cleanup(self):
        """Release NPU tensors and unregister buffers."""
        self._free_pools()

    def remove(self, request_id: str):
        """Remove a prefill key from the store to free capacity."""
        key = f"{PREFILL_KEY_PREFIX}{request_id}"
        try:
            if self._store.is_exist(key) == 1:
                self._store.remove(key)
                self._sync_stream()
        except Exception:
            pass

    def remove_decode_keys(self, request_id: str, max_step: int):
        """Remove decode keys [0, max_step) for *request_id* to free capacity."""
        for step in range(max_step):
            key = f"{DECODE_KEY_PREFIX}{request_id}_{step}"
            try:
                if self._store.is_exist(key) == 1:
                    self._store.remove(key)
            except Exception:
                pass
