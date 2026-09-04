#!/bin/bash
# 310P3 SHM Kernel Test — build script
set -e

CURRENT_DIR=$(cd $(dirname ${BASH_SOURCE:-$0}) && pwd)

MF_HOME="${MEMFABRIC_HYBRID_HOME_PATH:-$(cd ${CURRENT_DIR}/../.. && pwd)/output}"
export MEMFABRIC_HYBRID_HOME_PATH="${MF_HOME}"

CANN_PATH="${ASCEND_HOME_PATH:-/usr/local/Ascend/cann-9.1.0}"
export ASCEND_HOME_PATH="${CANN_PATH}"

echo "[build] MF_HOME=${MF_HOME}"
echo "[build] ASCEND_HOME_PATH=${ASCEND_HOME_PATH}"

BUILD_DIR="${CURRENT_DIR}/build_kernel"
rm -rf ${BUILD_DIR}
mkdir -p ${BUILD_DIR}

# Swap CMakeLists.txt for kernel build
if [ -f "${CURRENT_DIR}/CMakeLists.txt" ]; then
    mv ${CURRENT_DIR}/CMakeLists.txt ${CURRENT_DIR}/CMakeLists_host.txt.bak
fi
cp ${CURRENT_DIR}/CMakeLists_kernel.txt ${CURRENT_DIR}/CMakeLists.txt

cmake -S ${CURRENT_DIR} -B ${BUILD_DIR} -DSOC_VERSION=Ascend310P3
cmake --build ${BUILD_DIR} -j

# Restore original CMakeLists.txt
mv ${CURRENT_DIR}/CMakeLists_host.txt.bak ${CURRENT_DIR}/CMakeLists.txt

echo "[build] done: ${CURRENT_DIR}/310p3_shm_kernel_test"
