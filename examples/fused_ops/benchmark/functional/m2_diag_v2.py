#!/usr/bin/env python3
"""Diagnostic v2: per-wave unique data + direct work/scratch content check.

NOTE (2026-11-19 T-065): STALE — written against the pre-T-065 ABI (explicit
ib/status/trace pointers, 8-attr operator call). The 7-attr era exposes the
same capabilities via the xops session facade (SmemSession.status/.trace and
launch); the raw aclnn call here no longer matches the operator schema and
will fail at launch. Kept for the cold-start investigation record (坑 93).

Three waves in ONE process, each with a DIFFERENT x value pattern so that
reading stale content from a previous wave can never mask a race:

  wave k: epoch = EPOCH_BASE + 1 + k, val = base_pattern + 1000*(k+1)

After each wave we verify three things per rank:
  1. y == yr + yo (bit-exact golden, recomputed per wave);
  2. pool work region rows [0, M) == yr (own MM output actually landed);
  3. pool scratch region rows [0, M) == yo (peer copy actually landed) —
     read via raw aclrtMemcpy from the pool.

If wave 1 passes on both data-freshness and content, steady-state protocol
ordering is sound and the wave-0 anomaly is a first-wave (cold session) issue.
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

MIB = 1 << 20
K_SCRATCH_OFF = 16 * MIB
K_CTRL_SRC_OFF = K_SCRATCH_OFF + 16 * MIB
K_PEER_RDY_OFF = K_CTRL_SRC_OFF + 512
K_TRACE_SCHED_BASE = 16 + 8 * 16


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
    p.add_argument("--waves", type=int, default=3)
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

    work_base = int(gva) + symm * RANK

    def pool_read(off, nbytes):
        buf = np.zeros(nbytes, dtype=np.uint8)
        rc = acl.aclrtMemcpy(buf.ctypes.data, nbytes,
                             ctypes.c_void_p(work_base + off), nbytes, 2)
        if rc != 0:
            return None
        return buf

    if a.op == "ar":
        import xops_ar_310p3  # noqa: F401
    else:
        import xops_iar_310p3  # noqa: F401

    # Fixed weights; x pattern varies per wave.
    wi = (np.arange(N)[:, None] * 31 + np.arange(K)[None, :] * 7) % 13 - 6
    w_nd = torch.from_numpy(wi.astype(np.float16)).to(device)
    w_nz = torch_npu.npu_format_cast(w_nd.clone(), 29)
    col = np.arange(M) % K

    y = torch.zeros(M, N, dtype=torch.float16, device=device)
    ib = torch.zeros(16384, dtype=torch.uint8, device=device)
    status = torch.zeros(64, dtype=torch.uint8, device=device)
    trace = torch.zeros(512, dtype=torch.int64, device=device)

    row_bytes = N * 2
    m_bytes = M * row_bytes

    def wave(tag, k):
        # Unique per-wave values: small offsets keep every val*wi product
        # <= 2048 so fp16 is EXACT (one-hot rows => single product, no sum).
        # NOTE: other_val MUST mirror the peer's actual val formula (the peer
        # uses the same +3*k wave offset on ITS base pattern).
        val = ((1 + np.arange(M) % 7) if RANK == 0 else (2 + np.arange(M) % 5)) + 3 * k
        other_val = ((2 + np.arange(M) % 5) if RANK == 0 else (1 + np.arange(M) % 7)) + 3 * k
        x_np = np.zeros((M, K), dtype=np.float16)
        x_np[np.arange(M), col] = val.astype(np.float16)
        x = torch.from_numpy(x_np).to(device)
        yr = (val[:, None] * wi[:, col].T.astype(np.int32)).astype(np.int16)
        yo = (other_val[:, None] * wi[:, col].T.astype(np.int32)).astype(np.int16)
        golden = (yr + yo).astype(np.int32)

        epoch = EPOCH_BASE + 1 + k
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
        check(smem.smem_shm_control_barrier(pool), f"{tag} barrier")
        st = status.cpu().numpy().view(np.uint64)[0]
        if st != 0:
            fail(f"{tag} kernel status=0x{st:x}")

        got = y.cpu().numpy().astype(np.int32)
        bad = int(np.count_nonzero(got != golden))

        # Direct content check: work rows and scratch rows vs expected.
        wbuf = pool_read(0, m_bytes)
        sbuf = pool_read(K_SCRATCH_OFF, m_bytes)
        wbad = sbad = -1
        if wbuf is not None:
            wgot = wbuf.view(np.int16).reshape(M, N).astype(np.int32)
            wbad = int(np.count_nonzero(wgot != yr.astype(np.int32)))
        if sbuf is not None:
            sgot = sbuf.view(np.int16).reshape(M, N).astype(np.int32)
            sbad = int(np.count_nonzero(sgot != yo.astype(np.int32)))

        tr = trace.cpu().numpy()
        lane_spins = [int(tr[K_TRACE_SCHED_BASE + 64 + l]) for l in range(4)]
        print(f"[rank{RANK}] {tag} epoch={epoch}: "
              f"{'PASS' if bad == 0 else 'FAIL'} y_mis={bad}/{M * N} "
              f"work_mis={wbad}/{M * N} scratch_mis={sbad}/{M * N} "
              f"laneSpins={lane_spins}", flush=True)
        if bad != 0:
            wfirst = wbuf.view(np.int16)[:8] if wbuf is not None else None
            sfirst = sbuf.view(np.int16)[:8] if sbuf is not None else None
            print(f"[rank{RANK}]   y[0,:6]={got[0, :6].tolist()} "
                  f"golden[0,:6]={golden[0, :6].tolist()} "
                  f"yr[0,:4]={yr[0, :4].astype(np.int32).tolist()}", flush=True)
            print(f"[rank{RANK}]   work[:8]={None if wfirst is None else wfirst.tolist()} "
                  f"scratch[:8]={None if sfirst is None else sfirst.tolist()}", flush=True)
        return bad

    total = 0
    for k in range(a.waves):
        total += wave(f"wave{k}", k)

    check(smem.smem_shm_control_barrier(pool), "exit barrier")
    check(smem.smem_shm_destroy(pool, 0), "smem_shm_destroy")
    smem.smem_shm_uninit(0)
    smem.smem_uninit()
    if total != 0:
        sys.exit(1)


if __name__ == "__main__":
    main()
