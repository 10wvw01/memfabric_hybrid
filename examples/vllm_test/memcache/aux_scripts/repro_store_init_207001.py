#!/usr/bin/env python3
# coding=utf-8
# Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
# MemCache_Hybrid is licensed under Mulan PSL v2.
#
# Minimal reproduction for store.init() ret=-1 on 310P3 with device_sdma.
#
# Root cause (identified 2026-09-24):
#   Stale Python processes from previous test runs hold config store TCP
#   connections (rankId:N reconnect:1). When the MetaService restarts, these
#   stale connections are accepted and fill up aliveRankSet_ before the
#   actual rank0 can join. The new rank0 gets "AutoRanking failed,
#   dataSize: 0" because aliveRankSet_.size() >= worldSize.
#
#   Secondary issue: dev4/dev5 had zombie VLLM::Worker_TP processes using
#   all HBM, causing AclrtMallocPhysical ret:207001 (RT_ERROR_DRV_OUT_MEMORY).
#
# Fix:
#   1. Run debug/test/clean_stale_env.sh before each test to kill stale
#      processes and clean IPC resources.
#   2. HybmVmmBasedSegment::MallocFromDevice (310P branch) still lacks
#      P2P_HUGE -> HBM_MEM_HUGE fallback on 207001 (tracked separately).
#
# Usage:
#   bash debug/test/clean_stale_env.sh
#   MMC_TEST_DEVICE_ID=6 MMC_TEST_DEVICE_ID_2=7 python3 repro_store_init_207001.py

import multiprocessing
import os
import socket
import sys
import time

META_SERVICE_PORT = 5000
META_BOOTSTRAP_SECONDS = 3


def is_port_listening(port):
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=1):
            return True
    except OSError:
        return False


def rank1_keepalive_worker(device_id, ready_event, stop_event):
    """Init store on rank1 to complete the 2-rank group join, then wait."""
    store = None
    try:
        import acl
        from memcache_hybrid import DistributedObjectStore

        acl.init()
        acl.rt.set_device(device_id)
        store = DistributedObjectStore()
        ret = store.init(device_id)
        print(f"[rank1] store.init({device_id}) ret={ret}")
        if ret == 0:
            ready_event.set()
        stop_event.wait(timeout=300)
    except Exception as e:
        print(f"[rank1] exception: {e}")
        ready_event.set()
    finally:
        if store is not None:
            try:
                store.remove_all()
            except Exception:
                pass
            store.close()


def main():
    rank0_dev = int(os.environ.get("MMC_TEST_DEVICE_ID", "6"))
    rank1_dev = int(os.environ.get("MMC_TEST_DEVICE_ID_2", "7"))

    print(f"=== repro_store_init_207001: rank0=dev{rank0_dev} rank1=dev{rank1_dev} ===")
    print(f"    MMC_LOCAL_CONFIG_PATH={os.environ.get('MMC_LOCAL_CONFIG_PATH', '<unset>')}")
    print()

    # Start MetaService if not running
    if not is_port_listening(META_SERVICE_PORT):
        from memcache_hybrid import MetaService

        ctx = multiprocessing.get_context("fork")
        proc = ctx.Process(target=lambda: MetaService.main())
        proc.start()
        time.sleep(META_BOOTSTRAP_SECONDS)
        print(f"[meta] MetaService started (PID={proc.pid})")
    else:
        proc = None
        print("[meta] MetaService already running")

    try:
        import acl
        from memcache_hybrid import DistributedObjectStore

        # Start rank1 keepalive
        ctx = multiprocessing.get_context("fork")
        rank1_ready = ctx.Event()
        rank1_stop = ctx.Event()
        rank1_proc = ctx.Process(
            target=rank1_keepalive_worker,
            args=(rank1_dev, rank1_ready, rank1_stop),
        )
        rank1_proc.start()
        time.sleep(1)

        # Init rank0
        acl.init()
        acl.rt.set_device(rank0_dev)
        print(f"[rank0] acl.rt.set_device({rank0_dev}) OK")

        store = DistributedObjectStore()
        ret = store.init(rank0_dev)
        print(f"[rank0] store.init({rank0_dev}) ret={ret}")

        if ret != 0:
            print()
            print("=== REPRODUCED: store.init failed ===")
            print("Root cause: HybmVmmBasedSegment::MallocFromDevice 310P branch")
            print("  hardcodes ACL_MEM_P2P_HUGE with no fallback to ACL_HBM_MEM_HUGE")
            print("  on error 207001 (RT_ERROR_DRV_OUT_MEMORY).")
            print()
            print("Fix: Add P2P_HUGE -> HBM_MEM_HUGE fallback in MallocFromDevice,")
            print("  matching HybmAclVmmSegment::MallocPhysicalWithFallback pattern.")
            print()
            print("Check /var/log/memcache_hybrid/logs/mmc-meta.log for:")
            print('  "AclrtMallocPhysical failed, ret:207001"')

            rank1_stop.set()
            rank1_proc.join(timeout=10)
            sys.exit(1)

        print("[rank0] store.init SUCCESS!")
        store.remove_all()
        store.close()

        rank1_stop.set()
        rank1_proc.join(timeout=10)
        print()
        print("=== PASSED: store.init succeeded ===")
        sys.exit(0)

    finally:
        if proc is not None:
            proc.terminate()
            proc.join(timeout=5)


if __name__ == "__main__":
    main()
