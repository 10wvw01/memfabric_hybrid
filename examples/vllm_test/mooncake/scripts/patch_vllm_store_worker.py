#!/usr/bin/env python3
"""Patch vLLM MooncakeStoreWorker for hybrid attention models on 310P3.

Applies two fixes:
  12. register_kv_caches: per-group segment assignment
  13. save_put / load_get: SDMA transfer retry

Idempotent: checks for marker strings before applying.
"""
import sys
import re

def patch_file(path: str) -> None:
    with open(path) as f:
        src = f.read()

    changed = False

    # --- Fix 12: per-group segment assignment in register_kv_caches ---
    marker_12 = "per_group_addrs"
    if marker_12 in src:
        print("Fix 12 (per-group segments) already applied")
    else:
        old_12 = """        seen_ptrs: set[int] = set()
        addrs: list[int] = []
        block_lens: list[int] = []

        for value in kv_caches.values():
            cache = _repr_tensor(value)
            cache_storage = cache.untyped_storage()
            base_addr = cache_storage.data_ptr()
            if base_addr in seen_ptrs:
                continue
            seen_ptrs.add(base_addr)
            region_len = cache_storage.nbytes()

            ret = self.store.register_buffer(base_addr, region_len)
            if ret != 0:
                logger.error(
                    "register_buffer failed for addr %#x len %d: %d",
                    base_addr,
                    region_len,
                    ret,
                )

            # Detect layout via stride: a dim whose byte-stride exceeds
            # page_size_bytes is an outer segment dim (e.g. the K/V dim of
            # FlashAttn's (2, num_blocks, ...)). FlashInfer/MLA's blocks-
            # outermost layout has no such dim and yields a single segment.
            el = cache.element_size()
            page_size_bytes = region_len // self.num_blocks
            outer_dims = [
                d for d in range(cache.ndim) if cache.stride(d) * el > page_size_bytes
            ]
            if not outer_dims:
                # Blocks-first layout (FlashInfer / MLA): one segment.
                addrs.append(base_addr)
                block_lens.append(page_size_bytes)
            else:
                # K/V-first layout (FlashAttn / ROCm): split segments.
                seg_stride = cache.stride(outer_dims[0]) * el
                for idx in range(cache.shape[outer_dims[0]]):
                    addrs.append(base_addr + idx * seg_stride)
                    block_lens.append(seg_stride // self.num_blocks)

        logger.info(
            "Registered KV caches: num_groups=%d, num_segments=%d, num_blocks=%d",
            len(self.token_dbs),
            len(addrs),
            self.num_blocks,
        )

        for db in self.token_dbs:
            db.set_kv_caches_base_addr(addrs)
            db.set_block_len(block_lens)"""

        new_12 = """        seen_ptrs: set[int] = set()
        # Per-group segment lists so each token_db only uses its own layers'
        # addresses (critical for hybrid models where groups have different
        # KV cache layouts and block sizes).
        per_group_addrs: list[list[int]] = [[] for _ in self.token_dbs]
        per_group_lens: list[list[int]] = [[] for _ in self.token_dbs]

        layer_to_group: dict[str, int] = {}
        for g_idx, g in enumerate(self._kv_cache_groups):
            for ln in g.layer_names:
                layer_to_group[ln] = g_idx

        for layer_name, value in kv_caches.items():
            cache = _repr_tensor(value)
            cache_storage = cache.untyped_storage()
            storage_base = cache_storage.data_ptr()
            region_len = cache_storage.nbytes()

            # Register the entire storage once (deduped by base pointer).
            if storage_base not in seen_ptrs:
                seen_ptrs.add(storage_base)
                ret = self.store.register_buffer(storage_base, region_len)
                if ret != 0:
                    logger.error(
                        "register_buffer failed for addr %#x len %d: %d",
                        storage_base,
                        region_len,
                        ret,
                    )

            # Compute per-layer segments using the tensor's own data_ptr
            # (which accounts for storage_offset) so layers sharing the
            # same underlying storage are each mapped to their own group.
            g_idx = layer_to_group.get(layer_name, 0)
            if layer_name not in layer_to_group:
                logger.warning(
                    "Layer %s not found in any KV cache group, "
                    "assigning to group 0",
                    layer_name,
                )
            el = cache.element_size()
            layer_bytes = cache.numel() * el
            page_size_bytes = layer_bytes // self.num_blocks
            outer_dims = [
                d for d in range(cache.ndim) if cache.stride(d) * el > page_size_bytes
            ]
            if not outer_dims:
                # Blocks-first layout (FlashInfer / MLA / Mamba): one segment.
                per_group_addrs[g_idx].append(cache.data_ptr())
                per_group_lens[g_idx].append(page_size_bytes)
            else:
                # K/V-first layout (FlashAttn / ROCm): split segments.
                seg_stride = cache.stride(outer_dims[0]) * el
                for idx in range(cache.shape[outer_dims[0]]):
                    per_group_addrs[g_idx].append(
                        cache.data_ptr() + idx * seg_stride
                    )
                    per_group_lens[g_idx].append(seg_stride // self.num_blocks)

        total_segments = sum(len(a) for a in per_group_addrs)
        logger.info(
            "Registered KV caches: num_groups=%d, num_segments=%d, num_blocks=%d",
            len(self.token_dbs),
            total_segments,
            self.num_blocks,
        )
        for g_idx, db in enumerate(self.token_dbs):
            db.set_kv_caches_base_addr(per_group_addrs[g_idx])
            db.set_block_len(per_group_lens[g_idx])"""

        if old_12 in src:
            src = src.replace(old_12, new_12)
            changed = True
            print("Fix 12 (per-group segments) applied")
        else:
            print("WARNING: Fix 12 pattern not found, skipping", file=sys.stderr)

    # --- Fix 13a: save_put retry ---
    marker_13a = "Retrying save_put"
    if marker_13a in src:
        print("Fix 13a (save_put retry) already applied")
    else:
        old_13a = """            try:
                res = self.store.batch_put_from_multi_buffers(
                    keys,
                    addrs,
                    sizes,
                    self.replicate_config,
                )
                failed = [i for i, v in enumerate(res) if v < 0]
                self._record_operation(
                    "save_put",
                    put_start,
                    len(keys),
                    num_bytes=batch_bytes,
                    status="partial_failure" if failed else "ok",
                    num_failed_keys=len(failed),
                )
                if failed:"""

        new_13a = """            try:
                res = self.store.batch_put_from_multi_buffers(
                    keys,
                    addrs,
                    sizes,
                    self.replicate_config,
                )
                failed = [i for i, v in enumerate(res) if v < 0]

                # Retry failed transfers (310P3 NPU SDMA may fail on first
                # attempt due to HBM access timing after NPU compute).
                if failed:
                    for attempt in range(2):
                        time.sleep(0.05 * (attempt + 1))
                        retry_indices = failed
                        retry_keys = [keys[i] for i in retry_indices]
                        retry_addrs = [addrs[i] for i in retry_indices]
                        retry_sizes = [sizes[i] for i in retry_indices]
                        logger.info(
                            "Retrying save_put for %d failed keys "
                            "(attempt %d)", len(retry_keys), attempt + 1,
                        )
                        retry_res = self.store.batch_put_from_multi_buffers(
                            retry_keys,
                            retry_addrs,
                            retry_sizes,
                            self.replicate_config,
                        )
                        retry_failed = [
                            i for i, v in enumerate(retry_res) if v < 0
                        ]
                        if not retry_failed:
                            for i, orig_idx in enumerate(retry_indices):
                                res[orig_idx] = retry_res[i]
                            failed = []
                            break
                        else:
                            failed = [retry_indices[i]
                                      for i in retry_failed]

                self._record_operation(
                    "save_put",
                    put_start,
                    len(keys),
                    num_bytes=batch_bytes,
                    status="partial_failure" if failed else "ok",
                    num_failed_keys=len(failed),
                )
                if failed:"""

        if old_13a in src:
            src = src.replace(old_13a, new_13a)
            changed = True
            print("Fix 13a (save_put retry) applied")
        else:
            print("WARNING: Fix 13a pattern not found, skipping", file=sys.stderr)

    # --- Fix 13b: load_get retry ---
    marker_13b = "Retrying load_get"
    if marker_13b in src:
        print("Fix 13b (load_get retry) already applied")
    else:
        old_13b = """                res = self.store.batch_get_into_multi_buffers(
                    batch_keys, batch_addrs, batch_sizes
                )
                if tiers_by_key is not None:
                    _log_mooncake_load_tier_summary(
                        req_id, batch_keys, res, tiers_by_key
                    )
                failed = [
                    (key, value, block_id)
                    for key, value, block_id in zip(
                        batch_keys, res, batch_block_ids, strict=True
                    )
                    if value < 0
                ]"""

        new_13b = """                res = self.store.batch_get_into_multi_buffers(
                    batch_keys, batch_addrs, batch_sizes
                )
                failed = [
                    (key, value, block_id)
                    for key, value, block_id in zip(
                        batch_keys, res, batch_block_ids, strict=True
                    )
                    if value < 0
                ]

                # Retry failed loads (310P3 NPU SDMA may fail on first
                # attempt due to HBM access timing).
                if failed:
                    failed_keys = [k for k, _, _ in failed]
                    failed_indices = [i for i, v in enumerate(res) if v < 0]
                    for attempt in range(2):
                        time.sleep(0.05 * (attempt + 1))
                        retry_addrs = [batch_addrs[i] for i in failed_indices]
                        retry_sizes = [batch_sizes[i] for i in failed_indices]
                        logger.info(
                            "Retrying load_get for %d failed keys "
                            "(attempt %d)", len(failed_keys), attempt + 1,
                        )
                        retry_res = self.store.batch_get_into_multi_buffers(
                            failed_keys, retry_addrs, retry_sizes
                        )
                        retry_failed = [
                            i for i, v in enumerate(retry_res) if v < 0
                        ]
                        if not retry_failed:
                            for i, orig_idx in enumerate(failed_indices):
                                res[orig_idx] = retry_res[i]
                            failed = []
                            break
                        else:
                            failed_indices = [
                                failed_indices[i] for i in retry_failed
                            ]
                            failed = [
                                (batch_keys[i], res[i], batch_block_ids[i])
                                for i in failed_indices
                            ]"""

        if old_13b in src:
            src = src.replace(old_13b, new_13b)
            changed = True
            print("Fix 13b (load_get retry) applied")
        else:
            print("WARNING: Fix 13b pattern not found, skipping", file=sys.stderr)

    if changed:
        with open(path, 'w') as f:
            f.write(src)
        print(f"Patches written to {path}")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <worker.py path>", file=sys.stderr)
        sys.exit(1)
    patch_file(sys.argv[1])
