#!/bin/bash
# 310P3 TRANS Test — run script
#
# Usage: ./run.sh [rank_size] [store_ip] [store_port]
#   rank_size:  number of ranks (default: 2; supports 2/4)
#   store_ip:   rank0 IP (default: 127.0.0.1)
#   store_port: TCP port (default: 24000)
#
# Prerequisites:
#   - MemFabric built and installed (bash script/build_and_pack_run.sh)
#   - ASCEND_HOME_PATH set to CANN installation
#   - MPI installed (mpirun)
#   - For 2-rank: dev0+dev1 (same-card die pair)
#   - For 4-rank: dev0..dev3 (cross-card)

set -e

CURRENT_DIR=$(cd $(dirname ${BASH_SOURCE:-$0}) && pwd)

RANK_SIZE="${1:-2}"
STORE_IP="${2:-127.0.0.1}"
STORE_PORT="${3:-24000}"
STORE_URL="tcp://${STORE_IP}:${STORE_PORT}"

MF_HOME="${MEMFABRIC_HYBRID_HOME_PATH:-$(cd ${CURRENT_DIR}/../../.. && pwd)/output}"
export MEMFABRIC_HYBRID_HOME_PATH="${MF_HOME}"

export LD_LIBRARY_PATH="${MF_HOME}/smem/lib64:${MF_HOME}/hybm/lib64:${LD_LIBRARY_PATH:-}"
export MF_CONFIG_STORE_URL="${STORE_URL}"
export MF_LOG_LEVEL=1

echo "[run] rankSize=${RANK_SIZE} storeUrl=${STORE_URL}"
echo "[run] LD_LIBRARY_PATH=${LD_LIBRARY_PATH}"

BIN="${CURRENT_DIR}/310p3_trans_test"
if [ ! -f "${BIN}" ]; then
    echo "[run] binary not found, building..."
    bash ${CURRENT_DIR}/build.sh
fi

mpirun --allow-run-as-root -np ${RANK_SIZE} \
    -x MF_CONFIG_STORE_URL \
    -x MF_LOG_LEVEL \
    -x LD_LIBRARY_PATH \
    -x MEMFABRIC_HYBRID_HOME_PATH \
    -x ASCEND_HOME_PATH \
    ${BIN} ${STORE_URL}

echo "[run] done"
