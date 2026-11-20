#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
MF_ROOT="$(cd ../.. && pwd)"
: "${ASCEND_HOME_PATH:?Load CANN environment}"
mkdir -p build
bisheng --npu-arch=dav-2002 -O2 -std=c++17 -x asc main.asc -x none -o build/tp2_compare \
 -I"$ASCEND_HOME_PATH/include" -I"$MF_ROOT/src/smem/include/host" \
 -I"$MF_ROOT/src/smem/include/device" -L"$MF_ROOT/build/src/smem/csrc" -lmf_smem \
 -L"$ASCEND_HOME_PATH/lib64" -lhccl -lascendcl -lruntime -ldl -pthread
