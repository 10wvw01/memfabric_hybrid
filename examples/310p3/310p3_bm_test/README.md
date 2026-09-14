# 310P3 BM Functional Verification Demo

## Overview

This demo verifies MemFabric Big Memory (BM) functionality on **Atlas 300I Duo (Ascend 310P3)**.

## Test Modes

| Mode | Description | Topology | Copy Directions |
|------|-------------|----------|-----------------|
| `hbm` | HBM multi-rank pool | dev0 + dev1 + ... (cross-card) | L2G, G2L, G2G, G2H, H2G |
| `dram` | DRAM 2-rank HOST_SHM pool | 2 ranks | H2G, G2H, G2G |
| `lifecycle` | 100x create→join→copy→destroy cycle | dev0 + dev1 + ... | L2G, G2H |

## Prerequisites

- MemFabric built and installed: `bash script/build_and_pack_run.sh`
- CANN toolkit installed (`ASCEND_HOME_PATH` set)
- MPI installed (`mpirun` / `mpicxx`)
- Atlas 300I Duo (310P3) device

## Build

```bash
export MEMFABRIC_HYBRID_HOME_PATH=/path/to/memfabric_hybrid/output
export ASCEND_HOME_PATH=/usr/local/Ascend/ascend-toolkit/latest
bash examples/310p3/310p3_bm_test/build.sh
```

## Run

```bash
# HBM test (2/4 ranks; 2-rank same-card, 4-rank cross-card; 8-rank unsupported)
bash examples/310p3/310p3_bm_test/run.sh hbm 2 127.0.0.1 24000
bash examples/310p3/310p3_bm_test/run.sh hbm 4 127.0.0.1 24000

# DRAM test (2/4/8 ranks)
bash examples/310p3/310p3_bm_test/run.sh dram 2 127.0.0.1 24000
bash examples/310p3/310p3_bm_test/run.sh dram 4 127.0.0.1 24000
bash examples/310p3/310p3_bm_test/run.sh dram 8 127.0.0.1 24000

# Lifecycle test (100x create→join→copy→destroy; 2/4 ranks)
bash examples/310p3/310p3_bm_test/run.sh lifecycle 2 127.0.0.1 24000
bash examples/310p3/310p3_bm_test/run.sh lifecycle 4 127.0.0.1 24000
```

Usage: `./run.sh <mode> [rank_size] [store_ip] [store_port]`

| Parameter | Default | Description |
|-----------|---------|-------------|
| `mode` | `hbm` | `hbm` / `dram` / `lifecycle` |
| `rank_size` | `2` | Number of ranks (HBM: 2/4, max 4; lifecycle: 2/4; DRAM: 2/4/8) |
| `store_ip` | `127.0.0.1` | Rank0 IP for config store |
| `store_port` | `24000` | TCP port for config store |

## 310P3 Constraints

1. **HBM topology**: 2-rank same-card and 4-rank cross-card (2 cards) are supported. 8-rank cross-card (4 cards) is **not** supported: 310P3 Duo's two dies share 8 inbound P2P slots per card (`ascend_hal_base.h`); 8-rank full-mesh needs 2×(8-2)=12 inbound slots per card > 8. `TopologyPreflight` rejects this before join. Cross-card import requires the `aclrtMemAttr` ABI fix (commit `a2002aba`).
2. **DRAM+HBM**: Cannot be in the same handle. Use separate `smem_bm_create2` calls.
3. **HOST_SHM DRAM**: Requires `localDRAMSize == maxDramSize` and `maxHbmSize == 0`.
4. **BM entity id**: Must be < 64 (`HYBM_ENTITY_ID_SEGMENT_SIZE`).
5. **Post-join sync**: `smem_bm_join` returns before peer import completes. The demo adds `MPI_Barrier` + `sleep(1)`.
6. **Multi-rank race**: HBM and lifecycle tests use round-robin serialization (one writer per target per round) to avoid concurrent writes to the same GVA. Lifecycle cycles writer→target each loop (`writer=loop%worldSize`, `target=(writer+1)%worldSize`).
7. **Build**: Use `cmake -DBUILD_UT=OFF` (avoid GVA version detection failure).

## Verified Results

| Mode | rankSize | Topology | Result |
|------|----------|----------|--------|
| `hbm` | 2 | 1 card (dev0+dev1) | PASS |
| `hbm` | 4 | 2 cards (dev0-3) | PASS |
| `hbm` | 8 | 4 cards (dev0-7) | FAIL (preflight: 12 > 8 P2P slots/card) |
| `dram` | 2/4/8 | host shm | PASS |
| `lifecycle` | 2 | 1 card | PASS (100/100) |
| `lifecycle` | 4 | 2 cards | PASS (100/100) |

## Expected Output

```
[rank 0] start, mode:hbm deviceId:0 worldSize:4
[rank 0] smem_bm_init OK, socType=5 deviceId:0
[rank 0] smem_bm_join(HBM) OK
[rank 0] round targetRank=1 writer=2 writerTag=0xa2
[rank 0] L2G(target 1) OK
[rank 0] G2H(writer 1) verify OK
[rank 0] G2L(target 1) verify OK
[rank 0] G2G(writer 1) verify OK
[rank 0] H2G(target 1) OK
[rank 0] HBM test PASS
[rank 0] ALL TESTS PASSED
```

### Lifecycle (rankSize=4)

```
[rank 0] start, mode:lifecycle deviceId:0 worldSize:4
[rank 0] smem_bm_init OK, socType=5 deviceId:0
[rank 0] L2G: writing tag=0x80000000 to targetRank=1 targetGva=0x280080000000
[rank 0] L2G(target 1) OK
[rank 0] G2H(writer 3) firstWord=0x83000000 expected=0x83000000 OK
[rank 0] lifecycle loop 100/100 OK
[rank 0] Lifecycle test PASS (0/100 loops failed)
[rank 0] ALL TESTS PASSED
```

## Architecture

```
   Round-robin: for each target rank, one designated writer writes to it,
   then target verifies via G2H, writer verifies via G2L, target does G2G,
   finally writer writes host data via H2G and target verifies via G2H.

     target=0  writer=1     target=1  writer=2     target=2  writer=3     target=3  writer=0
     ┌──────┐               ┌──────┐               ┌──────┐               ┌──────┐
     │dev0  │ L2G→          │dev1  │ L2G→          │dev2  │ L2G→          │dev3  │ L2G→
     │GVA   │←G2H           │GVA   │←G2H           │GVA   │←G2H           │GVA   │←G2H
     └──────┘  G2L→         └──────┘  G2L→         └──────┘  G2L→         └──────┘  G2L→
               ↑G2G                   ↑G2G                   ↑G2G                   ↑G2G
               H2G→                   H2G→                   H2G→                   H2G→
               ←G2H                   ←G2H                   ←G2H                   ←G2H

   Each rank initializes its own GVA with its tag before the round-robin loop,
   so G2G copies meaningful content from the writer's GVA.
   G2G overwrites localGva with a sentinel (0xDE) before copy to detect no-op.
   H2G uses 0xB0 tag prefix to distinguish from L2G's 0xA0 prefix.
```
