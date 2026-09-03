# 310P3 BM Functional Verification Demo

## Overview

This demo verifies MemFabric Big Memory (BM) functionality on **Atlas 300I Duo (Ascend 310P3)**.

## Test Modes

| Mode | Description | Topology | Copy Directions |
|------|-------------|----------|-----------------|
| `hbm` | HBM 2-rank same-card pool | dev0 + dev1 (same card) | L2G, G2L, G2G, G2H, H2G |
| `dram` | DRAM 2-rank HOST_SHM pool | 2 ranks | H2G, G2H, G2G |
| `lifecycle` | 100x create→join→copy→destroy cycle | dev0 + dev1 | L2G, G2G |

## Prerequisites

- MemFabric built and installed: `bash script/build_and_pack_run.sh`
- CANN toolkit installed (`ASCEND_HOME_PATH` set)
- MPI installed (`mpirun` / `mpicxx`)
- Atlas 300I Duo (310P3) device

## Build

```bash
export MEMFABRIC_HYBRID_HOME_PATH=/path/to/memfabric_hybrid/output
export ASCEND_HOME_PATH=/usr/local/Ascend/ascend-toolkit/latest
bash examples/310p3_bm_test/build.sh
```

## Run

```bash
# HBM test (2 ranks on same card: dev0 + dev1)
bash examples/310p3_bm_test/run.sh hbm 2 127.0.0.1 24000

# DRAM test (2/4/8 ranks)
bash examples/310p3_bm_test/run.sh dram 2 127.0.0.1 24000
bash examples/310p3_bm_test/run.sh dram 4 127.0.0.1 24000
bash examples/310p3_bm_test/run.sh dram 8 127.0.0.1 24000

# Lifecycle test (100x create→join→copy→destroy)
bash examples/310p3_bm_test/run.sh lifecycle 2 127.0.0.1 24000
```

Usage: `./run.sh <mode> [rank_size] [store_ip] [store_port]`

| Parameter | Default | Description |
|-----------|---------|-------------|
| `mode` | `hbm` | `hbm` / `dram` / `lifecycle` |
| `rank_size` | `2` | Number of ranks (HBM: 2 same-card; DRAM: 2/4/8) |
| `store_ip` | `127.0.0.1` | Rank0 IP for config store |
| `store_port` | `24000` | TCP port for config store |

## 310P3 Constraints

1. **HBM topology**: Only 2-rank same-card (dev0+dev1) is supported. 4-rank cross-card HBM fails with ACL error 507899.
2. **DRAM+HBM**: Cannot be in the same handle. Use separate `smem_bm_create2` calls.
3. **HOST_SHM DRAM**: Requires `localDRAMSize == maxDramSize` and `maxHbmSize == 0`.
4. **BM entity id**: Must be < 64 (`HYBM_ENTITY_ID_SEGMENT_SIZE`).
5. **Post-join sync**: `smem_bm_join` returns before peer import completes. The demo adds `MPI_Barrier` + `sleep(1)`.
6. **Build**: Use `cmake -DBUILD_UT=OFF` (avoid GVA version detection failure).

## Expected Output

```
[rank 0] start, mode:hbm deviceId:0 worldSize:2
[rank 0] smem_bm_init OK, socType=5 deviceId:0
[rank 0] smem_bm_join(HBM) OK
[rank 0] localGva:0x... peerGva:0x... peerRank:1
[rank 0] L2G ret:0
[rank 0] G2L ret:0
[rank 0] G2G ret:0
[rank 0] G2H ret:0
[rank 0] H2G ret:0
[rank 0] HBM test PASSED
[rank 0] ALL TESTS PASSED
```

## Architecture

```
                +-----------+
  Rank 0 (dev0) | BM Pool   |
                | HBM 1GiB  |  <-- L2G / G2L / G2G / G2H / H2G
                +-----------+
                     |
                same card (310P3 Duo)
                     |
                +-----------+
  Rank 1 (dev1) | BM Pool   |
                | HBM 1GiB  |
                +-----------+
```
