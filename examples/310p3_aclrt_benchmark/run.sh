#!/bin/bash
# 310P3 Bare ACL Benchmark — run script (MPI cross-process)
#
# Usage: ./run.sh [devA] [devB]
#   devA: exporter device ID (rank0, default: 0)
#   devB: importer device ID (rank1, default: 1)
#
# Examples:
#   ./run.sh              # dev0 -> dev1 (same-card cross-die)
#   ./run.sh 0 2          # dev0 -> dev2 (cross-card)
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
