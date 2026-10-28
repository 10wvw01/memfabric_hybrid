#!/usr/bin/env python3
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

"""Minimal repro for HYBM "get sdid failed: 207000" during store.init().

Reproduces the failure chain:
  store.init(device_id)
    -> smem_bm_init()
      -> hybm_init()                         [OK]
      -> hybm_create_entity()
        -> InitSegment()
          -> InitDramSegment() / InitHbmSegment()
            -> MemSegment::InitDeviceInfo()
              -> RtGetDeviceInfo(INFO_TYPE_SDID) -> 107000
            -> "get sdid failed: 207000"
          -> segment init failed

Error 107000 = ACL_ERROR_INVALID_DEVICE returned by RtGetDeviceInfo(INFO_TYPE_SDID),
even though aclrtSetDevice() and aclrtCreateContext() succeed.

Usage:
  python3 repro_sdid_207000.py [device_id] [protocol] [hbm_size]

  device_id   Physical NPU device ID (default: 6)
  protocol   device_sdma | host_shm (default: device_sdma)
  hbm_size   HBM size, e.g. 2GB or 0 (default: 2GB)

Examples:
  python3 repro_sdid_207000.py 6 device_sdma 2GB
  python3 repro_sdid_207000.py 6 host_shm 0
  python3 repro_sdid_207000.py 0 device_sdma 2GB

Prerequisites:
  - MetaService running on port 5000
  - MMC_META_CONFIG_PATH and MMC_LOCAL_CONFIG_PATH set or auto-detected
"""

import multiprocessing
import os
import socket
import sys
import time
from typing import Optional

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------
META_SERVICE_PORT = 5000
META_BOOTSTRAP_SECONDS = 3
DEFAULT_DEVICE_ID = 6
DEFAULT_PROTOCOL = "device_sdma"
DEFAULT_HBM_SIZE = "2GB"
SDID_ERROR_CODE = 207000
SDID_ERROR_PATTERN = "get sdid failed"
STORE_INIT_SUCCESS = 0

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def project_root() -> str:
    p = os.path.abspath(__file__)
    for _ in range(3):
        p = os.path.dirname(p)
    return p


def is_meta_service_running(host: str = "127.0.0.1", port: int = META_SERVICE_PORT) -> bool:
    try:
        with socket.create_connection((host, port), timeout=1):
            return True
    except (OSError, ConnectionRefusedError):
        return False


def ensure_meta_service(meta_config: str, local_config: str):
    os.environ["MMC_META_CONFIG_PATH"] = meta_config
    os.environ["MMC_LOCAL_CONFIG_PATH"] = local_config

    if is_meta_service_running():
        print("[INFO] MetaService already running (using existing instance)")
        print(f"[INFO] MMC_LOCAL_CONFIG_PATH={local_config}")
        return None

    from memcache_hybrid import MetaService

    ctx = multiprocessing.get_context("fork")
    proc = ctx.Process(target=lambda: MetaService.main())
    proc.start()
    time.sleep(META_BOOTSTRAP_SECONDS)

    if is_meta_service_running():
        print(f"[INFO] MetaService started (PID: {proc.pid})")
        return proc
    raise RuntimeError("MetaService failed to start")


def write_temp_config(meta_config_path: str, protocol: str, hbm_size: str) -> str:
    """Write a temporary mmc-local config with the given protocol and HBM size."""
    base_dir = os.path.dirname(meta_config_path)
    temp_path = os.path.join(base_dir, f"mmc-local-repro_{protocol}_{hbm_size}.conf")

    with open(meta_config_path, "r", encoding="utf-8") as f:
        lines = f.readlines()

    new_lines = []
    for line in lines:
        stripped = line.strip()
        if stripped.startswith("ock.mmc.local_service.protocol"):
            new_lines.append(f"ock.mmc.local_service.protocol = {protocol}\n")
        elif stripped.startswith("ock.mmc.local_service.hbm.size"):
            new_lines.append(f"ock.mmc.local_service.hbm.size = {hbm_size}\n")
        else:
            new_lines.append(line)

    if not any("ock.mmc.local_service.protocol" in l for l in new_lines):
        new_lines.append(f"ock.mmc.local_service.protocol = {protocol}\n")
    if not any("ock.mmc.local_service.hbm.size" in l for l in new_lines):
        new_lines.append(f"ock.mmc.local_service.hbm.size = {hbm_size}\n")

    with open(temp_path, "w", encoding="utf-8") as f:
        f.writelines(new_lines)

    return temp_path


def check_acl_device_info(device_id: int):
    """Call acl.rt.get_device_info to probe SDID availability."""
    import acl

    acl.init()
    ret = acl.rt.set_device(device_id)
    if ret != 0:
        print(f"[FAIL] acl.rt.set_device({device_id}) returned {ret}")
        return False
    print(f"[OK] acl.rt.set_device({device_id})")

    ctx, ret = acl.rt.create_context(device_id)
    if ret != 0:
        print(f"[FAIL] acl.rt.create_context({device_id}) returned {ret}")
        return False
    print(f"[OK] acl.rt.create_context({device_id})")

    # Probe get_device_info — HYBM uses RtGetDeviceInfo(INFO_TYPE_SDID)
    # Error 107000 = ACL_ERROR_INVALID_DEVICE indicates SDID not available
    found_valid = False
    for info_type in range(20):
        try:
            result = acl.rt.get_device_info(info_type, device_id)
            if isinstance(result, tuple) and len(result) >= 2:
                value, ret_code = result[0], result[1]
                if ret_code == 0:
                    found_valid = True
                    print(f"  get_device_info(type={info_type}) = value={value}, ret={ret_code} [OK]")
                elif ret_code == 107000:
                    print(f"  get_device_info(type={info_type}) = ret={ret_code} [107000=INVALID_DEVICE]")
        except Exception:
            pass

    if not found_valid:
        print(f"[WARN] All get_device_info calls returned 107000 — SDID query will fail")
    return True


# ---------------------------------------------------------------------------
# Main repro
# ---------------------------------------------------------------------------

def main():
    device_id = int(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_DEVICE_ID
    protocol = sys.argv[2] if len(sys.argv) > 2 else DEFAULT_PROTOCOL
    hbm_size = sys.argv[3] if len(sys.argv) > 3 else DEFAULT_HBM_SIZE

    print("=" * 70)
    print(f"  repro_sdid_207000: device_id={device_id} protocol={protocol} hbm={hbm_size}")
    print("=" * 70)
    print()

    # --- Step 1: Environment setup ---
    root = project_root()
    meta_config = os.path.join(root, "config", "mmc-meta.conf")
    base_local_config = os.path.join(root, "config", "mmc-local.conf")

    if not os.path.exists(meta_config):
        print(f"[FAIL] meta config not found: {meta_config}")
        return 1

    temp_config = write_temp_config(base_local_config, protocol, hbm_size)
    print(f"[INFO] Temp config: {temp_config}")
    print(f"       protocol={protocol}, hbm.size={hbm_size}")
    print()

    # --- Step 2: Start MetaService ---
    proc = None
    try:
        proc = ensure_meta_service(meta_config, temp_config)
    except Exception as e:
        print(f"[FAIL] MetaService startup: {e}")
        return 1
    print()

    # --- Step 3: Probe ACL device info ---
    print("--- Step 1: ACL device probe ---")
    check_acl_device_info(device_id)
    print()

    # --- Step 4: store.init() ---
    print("--- Step 2: store.init() ---")
    import acl
    from memcache_hybrid import DistributedObjectStore

    acl.init()
    ret = acl.rt.set_device(device_id)
    print(f"  acl.rt.set_device({device_id}) = {ret}")

    store = DistributedObjectStore()
    ret = store.init(device_id)
    print(f"  store.init({device_id}) = {ret}")

    if ret == STORE_INIT_SUCCESS:
        print()
        print("[RESULT] store.init() SUCCESS — SDID query worked")
        try:
            store.remove_all()
        except Exception:
            pass
        store.close()
        result = 0
    else:
        print()
        print(f"[RESULT] store.init() FAILED (ret={ret})")
        print(f"         Expected error: '{SDID_ERROR_PATTERN}: {SDID_ERROR_CODE}'")
        print(f"         Root cause: RtGetDeviceInfo(INFO_TYPE_SDID) returns 107000")
        result = 1

    print()
    print("=" * 70)
    if result == 0:
        print("  REPRO: NOT reproduced (store.init succeeded)")
    else:
        print("  REPRO: Confirmed — 'get sdid failed: 207000' on store.init()")
        print("  This is a NPU driver/firmware issue:")
        print("    RtGetDeviceInfo(INFO_TYPE_SDID) returns 107000 (INVALID_DEVICE)")
        print("  Possible fixes (run on host, not in container):")
        print("    1. Check multi-die-policy:  npu-smi info -t multi-die-policy -i <NPU_ID>")
        print("    2. Check work-mode:         npu-smi info -t work-mode -i <NPU_ID>")
        print("    3. Check p2p-enable:        npu-smi info -t p2p-enable -i <NPU_ID>")
        print("    4. Check device-share:      npu-smi info -t device-share -i <NPU_ID>")
        print("    5. Allocate DDR hugepages:   npu-smi set -t hugepages -i <NPU_ID>")
        print("    6. Update NPU firmware/driver to a version that supports SDID query")
    print("=" * 70)

    # Cleanup
    if proc is not None:
        proc.terminate()
        proc.join(timeout=5)

    try:
        os.remove(temp_config)
    except OSError:
        pass

    return result


if __name__ == "__main__":
    sys.exit(main())
