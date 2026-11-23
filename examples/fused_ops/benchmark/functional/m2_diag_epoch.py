#!/usr/bin/env python3
"""Diagnostic twin of m2_xops_test.py: first-wave dirty-flag hypothesis.

NOTE (2026-11-19 T-065): STALE — written against the pre-T-065 ABI (explicit
ib/status/trace pointers, 8-attr operator call). The 7-attr era exposes the
same capabilities via the xops session facade (SmemSession.status/.trace and
launch); the raw aclnn call here no longer matches the operator schema and
will fail at launch. Kept for the cold-start investigation record (坑 93).

Two-wave experiment in ONE process (ar kernel, 8-attr ABI, no prewarm gate):
  0. Dump the pool control region (peerRdy/ready slots) BEFORE any launch,
     via raw aclrtMemcpy — pool memory has no zeroing guarantee (T-047 note),
     so a stale value equal to the first wave's epoch would false-positive
     the lanes' peerRdy gate and let them read scratch before the peer copy
     lands.
  A. Wave A with epoch=EPOCH_BASE+1 (1001) — the standard M2 first wave.
  B. Wave B with epoch=EPOCH_BASE+2 (1002) — slots now hold 1001 from wave A's
     real signals, so wave B must wait properly.

Prediction under the dirty-flag hypothesis:
  - Wave A mismatches on the rank whose peerRdy slot was dirty with 1001;
  - Wave B is bit-exact on both ranks;
  - Wave A trace lane spin counts ~0 on the raced rank (gate opened instantly).

Not wired into run_m2.sh; invoked manually for diagnosis only.
"""
import argparse
import ctypes
import os
import sys

import numpy as np

SDK = os.environ.get("MIXFABRIC_SDK_OUT", "/home/USER/310P_2/memfabric_hybrid/output")
CANN = os.environ["ASCEND_HOME_PATH"]

import torch  # noqa: E402
import torch_npu  # noqa: E402

K = 2048
N = 2048
POOL_BYTES = 64 << 20
DATA_BYTES = 16 << 20
EPOCH_BASE = 1000

# Mirror of the kernel pool layout constants (smem_mm_ar310p3.cpp).
MIB = 1 << 20
K_SCRATCH_OFF = 16 * MIB
K_CTRL_SRC_OFF = K_SCRATCH_OFF + 16 * MIB          # 32 MiB
K_PEER_RDY_OFF = K_CTRL_SRC_OFF + 512
K_READY_OFF = K_CTRL_SRC_OFF + 1024
K_TRACE_SCHED_BASE = 16 + 8 * 16                    # 144


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
    p.add_argument("--m", type=int, choices=[1280, 2048, 3840, 4096], default=1280)
    p.add_argument("--op", choices=["ar", "iar"], default="ar")
    a = p.parse_args()
    RANK = int(os.environ["RANK"])
    devs = [int(x) for x in a.devices.split(",")]
    dev = devs[RANK]
    M = a.m

    torch.npu.set_device(dev)
    device = torch.device(f"npu:{dev}")
    torch.npu.empty_cache()
    smem = ctypes.CDLL(f"{SDK}/smem/lib64/libmf_smem.so",
                       mode=ctypes.RTLD_GLOBAL | 0x2)
    acl = ctypes.CDLL("libascendcl.so", mode=ctypes.RTLD_GLOBAL | 0x2)
    acl.aclrtMemcpy.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p,
                                ctypes.c_size_t, ctypes.c_int]
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
    if peer_gva == 0 or int(geo[1 - RANK][1]) != symm or peer_gva != gva:
        fail(f"peer geometry {geo} (expected same-base symmetric window)")
    print(f"[rank{RANK}] pool gva=0x{gva:x} peer=0x{peer_gva:x} symm=0x{symm:x}", flush=True)

    # ---- Step 0: dump the dirty control region BEFORE any kernel launch ----
    # NOTE: each rank reads ITS OWN half (work = gva + symm*rank) — the peerRdy
    # slots polled by MY lanes live in MY work region, written by the PEER.
    work_base = int(gva) + symm * RANK

    def pool_read(off, nbytes=256):
        buf = np.zeros(nbytes // 8, dtype=np.uint64)
        rc = acl.aclrtMemcpy(buf.ctypes.data, nbytes,
                             ctypes.c_void_p(work_base + off), nbytes, 2)
        if rc != 0:
            return None
        return buf

    ctrl = pool_read(K_CTRL_SRC_OFF, 4096)
    if ctrl is None:
        print(f"[rank{RANK}] WARN: raw pool read failed", flush=True)
    else:
        nz = {int(i * 8): int(v) for i, v in enumerate(ctrl) if v != 0}
        print(f"[rank{RANK}] dirty control region: {len(nz)} non-zero u64 "
              f"-> {{offset_from_ctrl_src: value}} = {nz}", flush=True)

    if a.op == "ar":
        import xops_ar_310p3  # noqa: F401
    else:
        import xops_iar_310p3  # noqa: F401

    # Deterministic integer test data (identical to m2_xops_test.py).
    wi = (np.arange(N)[:, None] * 31 + np.arange(K)[None, :] * 7) % 13 - 6
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

    yr = (val[:, None] * wi[:, col].T.astype(np.int32)).astype(np.int16)
    other_val = (2 + np.arange(M) % 5) if RANK == 0 else (1 + np.arange(M) % 7)
    yo = (other_val[:, None] * wi[:, col].T.astype(np.int32)).astype(np.int16)
    golden = (yr + yo).astype(np.int32)

    def launch(epoch):
        status.zero_()
        trace.zero_()
        if a.op == "ar":
            torch_npu.npu_smem_mm_ar_310p3(
                x, w_nz, y, gva, symm,
                ib.data_ptr(), status.data_ptr(), trace.data_ptr(),
                RANK, epoch, M)
        else:
            torch_npu.npu_smem_mm_iar_310p3(
                x, w_nz, y, gva, symm,
                ib.data_ptr(), status.data_ptr(), trace.data_ptr(),
                RANK, epoch, M)
        torch.npu.synchronize()

    def wave(tag, epoch):
        launch(epoch)
        check(smem.smem_shm_control_barrier(pool), f"{tag} barrier")
        st = status.cpu().numpy().view(np.uint64)[0]
        if st != 0:
            fail(f"{tag} kernel status=0x{st:x}")
        got = y.cpu().numpy().astype(np.int32)
        bad_map = got != golden
        bad = int(np.count_nonzero(bad_map))
        # Per 256-row block mismatch counts: shows WHERE the damage sits.
        blocks = bad_map.reshape(M // 256, 256, N).sum(axis=(1, 2))
        tr = trace.cpu().numpy()
        lane_spins = [int(tr[K_TRACE_SCHED_BASE + 64 + l]) for l in range(4)]
        sched_hdr = [int(v) for v in tr[K_TRACE_SCHED_BASE:K_TRACE_SCHED_BASE + 4]]
        print(f"[rank{RANK}] {tag} epoch={epoch}: "
              f"{'PASS' if bad == 0 else 'FAIL'} mismatches={bad}/{M * N} "
              f"per256rowblock={blocks.tolist()} laneSpins={lane_spins} "
              f"schedWaitSpins(g0,g1..)={sched_hdr}", flush=True)
        return bad

    bad_a = wave("waveA", EPOCH_BASE + 1)
    bad_b = wave("waveB", EPOCH_BASE + 2)

    # Post-wave slot dump: after a completed wave both schedulers must have
    # written epoch into every peerRdy slot.
    ctrl2 = pool_read(K_CTRL_SRC_OFF, 4096)
    if ctrl2 is not None:
        nz = {int(i * 8): int(v) for i, v in enumerate(ctrl2) if v != 0}
        print(f"[rank{RANK}] post-wave control region: {nz}", flush=True)

    check(smem.smem_shm_control_barrier(pool), "exit barrier")
    check(smem.smem_shm_destroy(pool, 0), "smem_shm_destroy")
    smem.smem_shm_uninit(0)
    smem.smem_uninit()
    if bad_a != 0 or bad_b != 0:
        sys.exit(1)


if __name__ == "__main__":
    main()
