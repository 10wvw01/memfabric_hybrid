#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
ROOT="$(cd ../../.. && pwd)"
mkdir -p build
OUTPUT="${OUTPUT:-build/gm_demo}"
bisheng --npu-arch=dav-2002 -O2 -std=c++17 -x asc main.asc -o "$OUTPUT" \
 -I"$ASCEND_HOME_PATH/include" -I"$ROOT/src/smem/include/host" -I"$ROOT/src/smem/include/device" \
 -L"$ROOT/build/src/smem/csrc" -lmf_smem \
 -L"$ASCEND_HOME_PATH/lib64" -lhccl -lascendcl -lruntime -pthread
