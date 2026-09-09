#!/bin/bash
# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
BM_310P_EXAMPLE_DIR=$(dirname $(readlink -f "$0"))
PROJ_DIR=${BM_310P_EXAMPLE_DIR}/../../../

cd ${BM_310P_EXAMPLE_DIR}
mkdir build
cmake . -B build
make -C build
mkdir ${PROJ_DIR}/output/bm_perf_benchmark_310p -p
cp -rf ${BM_310P_EXAMPLE_DIR}/bm_perf_benchmark_310p ${PROJ_DIR}/output/bm_perf_benchmark_310p/

rm -rf ${BM_310P_EXAMPLE_DIR}/bm_perf_benchmark_310p
rm -rf ${BM_310P_EXAMPLE_DIR}/build
