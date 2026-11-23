#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
ROOT="$(cd ../.. && pwd)"
: "${ASCEND_HOME_PATH:?Load CANN environment}"
mkdir -p build
bisheng --npu-arch=dav-2002 -O2 -std=c++17 -x asc main.asc -x none -o build/api_perf \
 -I"$ASCEND_HOME_PATH/include" -I"$ROOT/src/smem/include/host" \
 -I"$ROOT/src/smem/include/device" -L"$ROOT/build/src/smem/csrc" -lmf_smem \
 -L"$ASCEND_HOME_PATH/lib64" -lascendcl -lruntime -ldl -pthread
