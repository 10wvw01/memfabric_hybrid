#!/bin/bash
# 310P3 BM Test — build script
set -e

CURRENT_DIR=$(cd $(dirname ${BASH_SOURCE:-$0}) && pwd)

# Default: use the build output directory directly
MF_HOME="${MEMFABRIC_HYBRID_HOME_PATH:-$(cd ${CURRENT_DIR}/../../.. && pwd)/output}"
export MEMFABRIC_HYBRID_HOME_PATH="${MF_HOME}"

CANN_PATH="${ASCEND_HOME_PATH:-/usr/local/Ascend/ascend-toolkit/latest}"
export ASCEND_HOME_PATH="${CANN_PATH}"

echo "[build] MF_HOME=${MF_HOME}"
echo "[build] ASCEND_HOME_PATH=${ASCEND_HOME_PATH}"

rm -rf ${CURRENT_DIR}/build
mkdir -p ${CURRENT_DIR}/build
cmake -S ${CURRENT_DIR} -B ${CURRENT_DIR}/build
cmake --build ${CURRENT_DIR}/build -j
echo "[build] done: ${CURRENT_DIR}/310p3_bm_test"
