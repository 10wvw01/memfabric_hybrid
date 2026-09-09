#!/bin/bash
# 310P3 Bare ACL Benchmark — build script
set -e

CURRENT_DIR=$(cd $(dirname ${BASH_SOURCE:-$0}) && pwd)

CANN_PATH="${ASCEND_HOME_PATH:-/usr/local/Ascend/ascend-toolkit/latest}"
export ASCEND_HOME_PATH="${CANN_PATH}"

echo "[build] ASCEND_HOME_PATH=${ASCEND_HOME_PATH}"

rm -rf ${CURRENT_DIR}/build
mkdir -p ${CURRENT_DIR}/build
cmake -S ${CURRENT_DIR} -B ${CURRENT_DIR}/build
cmake --build ${CURRENT_DIR}/build -j
echo "[build] done: ${CURRENT_DIR}/310p3_aclrt_benchmark"
