#!/usr/bin/env python3
"""TP2 functional (M2) test for the prd_code fused operators.

Per-rank process (RANK=0/1 in env); both ranks run the identical sequence:
pool bootstrap -> prewarm (enabled=0 waves) -> one real fused wave -> golden
bit-exact comparison. The aclnn launch goes through the xops wheel
(torch_npu.npu_smem_mm_ar_310p3 / npu_smem_mm_iar_310p3), validating the
EXEC_NPU_CMD_V1 -> aclnnSmemMm{Ar,Iar}310p3 -> kernel chain end to end.

Verification data: one-hot rows x, small-int weights w; golden AR = y0 + y1.
Both fused kernels write the AR result DIRECTLY into y — verify from the
tensor itself (bit-exact: the 2-rank fp16 sum of these integers is
order-independent).
"""
import argparse
import ctypes
import os
import sys

import numpy as np

SDK = os.environ.get("MIXFABRIC_SDK_OUT", "/home/w00804146/310P_2/memfabric_hybrid/output")
CANN = os.environ["ASCEND_HOME_PATH"]
# Packet size (bytes). Default 1MiB; 4MiB = single-packet-per-subgroup mode.
PKT = int(os.environ.get("SMEM_MM_310P3_PACKET", str(1024 * 1024)))

import torch  # noqa: E402
import torch_npu  # noqa: E402
# The xops wheel import is deferred until after the pool is built: its load
# chain is one of the last differences between the two ranks before meta
# allocation (lib load order pitfall).

K = 2048
N = 2048
POOL_BYTES = 64 << 20
DATA_BYTES = 16 << 20
EPOCH_BASE = 1000


def fail(msg):
    print(f"[rank{RANK}] FAIL: {msg}", flush=True)
    sys.exit(1)


def check(rc, name):
    if rc != 0:
        fail(f"{name} rc={rc}")
    return rc


class SmemConfig(ctypes.Structure):
    _fields_ = [
        ("shmInitTimeout", ctypes.c_uint32),
        ("shmCreateTimeout", ctypes.c_uint32),
        ("controlOperationTimeout", ctypes.c_uint32),
        ("startConfigStoreServer", ctypes.c_bool),
        ("flags", ctypes.c_uint32),
    ]


def main():
    global RANK
    p = argparse.ArgumentParser()
    p.add_argument("--devices", required=True)
    p.add_argument("--store-url", required=True)
    p.add_argument("--m", type=int, choices=[1280, 2048, 3840, 4096], default=2048)
    p.add_argument("--op", choices=["ar", "iar"], required=True,
                   help="ar = smem_mm_ar_310p3 (local reduction); "
                        "iar = smem_mm_iar_310p3 (hardware reduction)")
    a = p.parse_args()
    RANK = int(os.environ["RANK"])
    devs = [int(x) for x in a.devices.split(",")]
    dev = devs[RANK]
    M = a.m

    torch.npu.set_device(dev)
    device = torch.device(f"npu:{dev}")
    torch.npu.empty_cache()
    # Exact EnsureLibs replication: libmf_smem first, then libascendcl, both
    # RTLD_NOW|RTLD_GLOBAL (glibc: RTLD_NOW=0x2; ctypes defaults to LAZY).
    # Pool before any tensor (M2 discipline, matches the op-package init order).
    smem = ctypes.CDLL(f"{SDK}/smem/lib64/libmf_smem.so",
                       mode=ctypes.RTLD_GLOBAL | 0x2)
    ctypes.CDLL("libascendcl.so", mode=ctypes.RTLD_GLOBAL | 0x2)
    smem.smem_init.argtypes = [ctypes.c_uint32]
    smem.smem_shm_config_init.argtypes = [ctypes.POINTER(SmemConfig)]
    smem.smem_shm_init.argtypes = [ctypes.c_char_p, ctypes.c_uint32, ctypes.c_uint32,
                                   ctypes.c_uint16, ctypes.POINTER(SmemConfig)]
    smem.smem_shm_create.argtypes = [ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint32,
                                     ctypes.c_uint64, ctypes.c_uint32, ctypes.c_uint32,
                                     ctypes.POINTER(ctypes.c_void_p)]
    smem.smem_shm_create.restype = ctypes.c_void_p
    smem.smem_shm_sdma_get_workspace.argtypes = [ctypes.c_void_p]
    smem.smem_shm_sdma_get_workspace.restype = ctypes.c_void_p
    smem.smem_shm_get_symmetric_size.argtypes = [ctypes.c_void_p]
    smem.smem_shm_get_symmetric_size.restype = ctypes.c_uint64
    smem.smem_shm_control_allgather.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint32,
                                                ctypes.c_void_p, ctypes.c_uint32]
    smem.smem_shm_control_barrier.argtypes = [ctypes.c_void_p]
    smem.smem_shm_destroy.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    smem.smem_shm_uninit.argtypes = [ctypes.c_uint32]
    smem.smem_uninit.argtypes = []
    smem.smem_uninit.restype = None

    check(smem.smem_init(0), "smem_init")
    cfg = SmemConfig()
    check(smem.smem_shm_config_init(ctypes.byref(cfg)), "smem_shm_config_init")
    cfg.startConfigStoreServer = RANK == 0
    check(smem.smem_shm_init(a.store_url.encode(), 2, RANK, dev, ctypes.byref(cfg)), "smem_shm_init")
    base = ctypes.c_void_p()
    pool = smem.smem_shm_create(0, 2, RANK, POOL_BYTES, 2, 0, ctypes.byref(base))
    if not pool or not base.value or not smem.smem_shm_sdma_get_workspace(pool):
        fail("SDMA pool not ready (orch deploy failed?)")
    gva = base.value
    symm = smem.smem_shm_get_symmetric_size(pool)
    if symm < DATA_BYTES + 4096:
        fail(f"symmetric geometry {symm}")

    geo = np.zeros((2, 4), dtype=np.uint64)
    mine = np.array([gva, symm, 0, symm], dtype=np.uint64)
    check(smem.smem_shm_control_allgather(pool, mine.ctypes.data, 32, geo.ctypes.data, 64),
          "geometry allgather")
    peer_gva = int(geo[1 - RANK][0])
    if peer_gva == 0 or int(geo[1 - RANK][1]) != symm:
        fail(f"peer geometry {geo}")
    print(f"[rank{RANK}] pool gva=0x{gva:x} peer=0x{peer_gva:x} symm=0x{symm:x}", flush=True)

    # Deferred xops import: pool before operator registration (load-order pitfall).
    if a.op == "ar":
        import xops_ar_310p3  # noqa: F401  # registers torch_npu.npu_smem_mm_ar_310p3
    else:
        import xops_iar_310p3  # noqa: F401  # registers torch_npu.npu_smem_mm_iar_310p3

    # Deterministic integer test data: one-hot rows, small-int weight.
    wi = (np.arange(N)[:, None] * 31 + np.arange(K)[None, :] * 7) % 13 - 6  # [N,K]
    w_nd = torch.from_numpy(wi.astype(np.float16)).to(device)
    w_nz = torch_npu.npu_format_cast(w_nd.clone(), 29)
    col = np.arange(M) % K
    val = (1 + np.arange(M) % 7) if RANK == 0 else (2 + np.arange(M) % 5)
    x_np = np.zeros((M, K), dtype=np.float16)
    x_np[np.arange(M), col] = val.astype(np.float16)
    x = torch.from_numpy(x_np).to(device)
    y = torch.zeros(M, N, dtype=torch.float16, device=device)
    ib = torch.zeros(16384, dtype=torch.uint8, device=device)
    status = torch.zeros(64, dtype=torch.uint8, device=device)
    trace = torch.zeros(512, dtype=torch.int64, device=device)

    def launch(enabled, epoch):
        status.zero_()
        trace.zero_()
        if a.op == "ar":
            torch_npu.npu_smem_mm_ar_310p3(
                x, w_nz, y,
                gva, peer_gva, symm,
                ib.data_ptr(), status.data_ptr(), trace.data_ptr(),
                RANK, epoch, PKT, M, 1024, 8, enabled)
        else:
            torch_npu.npu_smem_mm_iar_310p3(
                x, w_nz, y,
                gva, peer_gva, symm,
                ib.data_ptr(), status.data_ptr(), trace.data_ptr(),
                RANK, epoch, PKT, M, 1024, 8, enabled)
        torch.npu.synchronize()

    # Cold-first-write prewarm: two no-op launches, sync, barrier.
    launch(0, EPOCH_BASE)
    launch(0, EPOCH_BASE)
    check(smem.smem_shm_control_barrier(pool), "prewarm barrier")
    print(f"[rank{RANK}] prewarm (enabled=0) via xops OK", flush=True)

    # Real wave via the PTA layer.
    launch(1, EPOCH_BASE + 1)
    check(smem.smem_shm_control_barrier(pool), "final barrier")

    st = status.cpu().numpy().view(np.uint64)[0]
    if st != 0:
        fail(f"kernel status=0x{st:x}")

    # Both operators write the AR result DIRECTLY into y — verify from the tensor.
    got = y.cpu().numpy().astype(np.int32)

    yr = (val[:, None] * wi[:, col].T.astype(np.int32)).astype(np.int16)
    other_val = (2 + np.arange(M) % 5) if RANK == 0 else (1 + np.arange(M) % 7)
    yo = (other_val[:, None] * wi[:, col].T.astype(np.int32)).astype(np.int16)
    golden = (yr + yo).astype(np.int32)
    bad = int(np.count_nonzero(got != golden))
    print(f"[rank{RANK}] {'PASS' if bad == 0 else 'FAIL'} data mismatches={bad}/{M * N}",
          flush=True)

    check(smem.smem_shm_control_barrier(pool), "exit barrier")
    check(smem.smem_shm_destroy(pool, 0), "smem_shm_destroy")
    smem.smem_shm_uninit(0)
    smem.smem_uninit()
    if bad != 0:
        sys.exit(1)


if __name__ == "__main__":
    main()
