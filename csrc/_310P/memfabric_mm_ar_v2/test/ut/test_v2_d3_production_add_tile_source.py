# SPDX-License-Identifier: Apache-2.0
"""D3 C2 CPU-only 源码/几何合同：不代表 310P3 BiSheng 编译或实机 PASS。"""
from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PROD = ROOT / "src/op_kernel/mf310p_v2_kernel.asc"
COMMON = ROOT / "src/op_kernel/mf310p_v2_kernel_common.h"
DIAG = ROOT / "src/op_kernel/mf310p_v2_diagnostics.asc"


def test_d3_only_production_tile_changes_diagnostics_stay_baseline():
    prod, common, diag = (p.read_text(encoding="utf-8") for p in (PROD, COMMON, DIAG))
    assert "constexpr uint32_t kProductionAddTile = 2048;" in prod
    assert "constexpr uint32_t kAddTile = 1024;" in common
    assert "kProductionAddTile" not in common
    assert "kProductionAddTile" not in diag
    assert "kAddTile" in diag
    assert "V2TrialKernel" in prod
    assert "V2DiagLoopKernel" in diag


def test_d3_both_add_paths_consume_full_tile():
    s = PROD.read_text(encoding="utf-8")
    assert s.count("off += kProductionAddTile") == 2
    assert s.count("DataCopy(la, a[off], kProductionAddTile)") == 2
    assert s.count("DataCopy(lb, b[off], kProductionAddTile)") == 2
    assert s.count("Add(res, va, vb, kProductionAddTile)") == 2
    assert s.count("DataCopy(dst[off], result, kProductionAddTile)") == 2
    assert s.count("pipe.InitBuffer(in0, 1, kProductionAddBytes)") == 1
    assert s.count("pipe.InitBuffer(in1, 1, kProductionAddBytes)") == 1
    assert s.count("pipe.InitBuffer(outQ, 1, kProductionAddBytes)") == 1
    assert "pipe.InitBuffer(mmUb, TOTAL_UB_SIZE - kProductionUBForVector - kUBForIB)" in s
    assert "constexpr uint32_t kProductionUBForVector = 3 * kProductionAddBytes;" in s
    assert "static_assert(TOTAL_UB_SIZE > kUBForIB + kProductionUBForVector" in s


def test_d3_tile_geometry_all_supported_chunks_and_baseline():
    n, cores, bytes_per_fp16 = 2048, 8, 2
    tile_before, tile_after = 1024, 2048
    assert tile_after * bytes_per_fp16 == 4096
    assert 3 * tile_before * bytes_per_fp16 == 6144
    assert 3 * tile_after * bytes_per_fp16 == 12288
    assert (3 * tile_after - 3 * tile_before) * bytes_per_fp16 == 6144
    for chunk in (256, 512, 1024):
        assert chunk % cores == 0
        fp16_elems_per_core = chunk * n // cores
        assert fp16_elems_per_core % tile_before == 0
        assert fp16_elems_per_core % tile_after == 0
        assert fp16_elems_per_core // tile_after == (
            fp16_elems_per_core // tile_before
        ) // 2
    # M4096 CHUNK1024: per-core 4*128*2048 FP16
    assert (4096 * n // cores) // tile_before == 1024
    assert (4096 * n // cores) // tile_after == 512


def test_d3_protocol_and_cache_visibility_unmodified_by_optimization():
    s = PROD.read_text(encoding="utf-8")
    assert s.index("mm.IterateAll(cg);") < s.index(
        "CleanLines(ownC + start + coreOff, perCoreBytes);"
    ) < s.index("WaitReady(ibGm, ibUb, core);")
    assert s.count("CleanLines(ownC + start + coreOff, perCoreBytes);") == 1
    assert s.count("CleanLines(ownPeerC + base, perCoreBytes);") == 2
    assert s.count("smem_shm_sdma_signal_at(") == 2
    assert s.count("smem_shm_sdma_wait_at(") == 3  # 前块/收尾/DONE
    assert "if (core == 0)" in s
    for code in ("WaitReady(", "ReleaseAdd(", "WaitAddDone(",
                 "smem_shm_sdma_quiet_at(", "kTagDone",
                 "CheckOrTrap(", "overlap == 0 || k > 0"):
        assert code in s


def test_d3_static_ub_is_only_necessary_not_sufficient():
    s = PROD.read_text(encoding="utf-8")
    assert "kProductionAddBytes % 32 == 0" in s
    assert "Matmul SetLocalWorkspace" in s
    assert "BiSheng" in s
    # MatmulGetTmpBufSize/CANN Tiling 必须在 310P3 构建资源审计中确认，
    # 仅凭 TOTAL_UB_SIZE 正数不能假装已验证 Matmul 临时空间充足。
