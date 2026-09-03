#!/bin/bash
# 310P3 BM Test — run script
#
# Usage: ./run.sh <mode> [rank_size] [store_ip] [store_port]
#   mode:       hbm | dram | lifecycle (default: hbm)
#   rank_size:  number of ranks (default: 2; HBM: 2/4; DRAM: 2/4/8)
#   store_ip:   rank0 IP (default: 127.0.0.1)
#   store_port: TCP port (default: 24000)
#
# Prerequisites:
#   - MemFabric built and installed (bash script/build_and_pack_run.sh)
#   - ASCEND_HOME_PATH set to CANN installation
#   - MPI installed (mpirun)
#   - For HBM test: dev0+dev1 (2-rank same-card) or dev0..dev3 (4-rank cross-card)

set -e

CURRENT_DIR=$(cd $(dirname ${BASH_SOURCE:-$0}) && pwd)

MODE="${1:-hbm}"
RANK_SIZE="${2:-2}"
STORE_IP="${3:-127.0.0.1}"
STORE_PORT="${4:-24000}"
STORE_URL="tcp://${STORE_IP}:${STORE_PORT}"

MF_HOME="${MEMFABRIC_HYBRID_HOME_PATH:-$(cd ${CURRENT_DIR}/../.. && pwd)/output}"
export MEMFABRIC_HYBRID_HOME_PATH="${MF_HOME}"

export LD_LIBRARY_PATH="${MF_HOME}/smem/lib64:${MF_HOME}/hybm/lib64:${LD_LIBRARY_PATH:-}"
export ASCEND_MF_STORE_URL="${STORE_URL}"
export MF_LOG_LEVEL=1

echo "[run] mode=${MODE} rankSize=${RANK_SIZE} storeUrl=${STORE_URL}"
echo "[run] LD_LIBRARY_PATH=${LD_LIBRARY_PATH}"

BIN="${CURRENT_DIR}/310p3_bm_test"
if [ ! -f "${BIN}" ]; then
    echo "[run] binary not found, building..."
    bash ${CURRENT_DIR}/build.sh
fi

# Launch all ranks with a single mpirun
# Each rank defaults deviceId = mpi rank (dev0, dev1, dev2, ...)
mpirun --allow-run-as-root -np ${RANK_SIZE} \
    -x ASCEND_MF_STORE_URL \
    -x MF_LOG_LEVEL \
    -x LD_LIBRARY_PATH \
    -x MEMFABRIC_HYBRID_HOME_PATH \
    -x ASCEND_HOME_PATH \
    ${BIN} ${STORE_URL} ${MODE}

echo "[run] done"
