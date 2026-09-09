#!/bin/bash
# 310P3 Bare ACL Benchmark — run script (MPI cross-process)
#
# Usage: ./run.sh [devA] [devB]
#   devA: rank0 device ID (exporter, default: 0)
#   devB: rank1 device ID (importer, default: 1)
#
# Test phases:
#   1. D2D test (rank1 only, single-sync vs batch-sync)
#   2. Single-rank H2D/D2H test (rank1 only, single-sync vs batch-sync)
#   3. Concurrent H2D/D2H test (both ranks, single-sync, for contention analysis)
#
# Examples:
#   ./run.sh              # dev0 + dev1 (same-card cross-die)
#   ./run.sh 0 2          # dev0 + dev2 (cross-card)
#
# Prerequisites:
#   - ASCEND_HOME_PATH set to CANN installation
#   - MPI installed (mpirun)
#   - devA + devB accessible

set -e

CURRENT_DIR=$(cd $(dirname ${BASH_SOURCE:-$0}) && pwd)

CANN_PATH="${ASCEND_HOME_PATH:-/usr/local/Ascend/ascend-toolkit/latest}"
export ASCEND_HOME_PATH="${CANN_PATH}"
export LD_LIBRARY_PATH="${CANN_PATH}/aarch64-linux/lib64:${LD_LIBRARY_PATH:-}"

DEV_A="${1:-0}"
DEV_B="${2:-1}"

BIN="${CURRENT_DIR}/310p3_aclrt_benchmark"
if [ ! -f "${BIN}" ]; then
    echo "[run] binary not found, building..."
    bash ${CURRENT_DIR}/build.sh
fi

echo "[run] MPI cross-process benchmark: dev${DEV_A} (exporter) -> dev${DEV_B} (importer)"
mpirun --allow-run-as-root -np 2 \
    -x ASCEND_HOME_PATH \
    -x LD_LIBRARY_PATH \
    ${BIN} ${DEV_A} ${DEV_B}
echo "[run] done"
