#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
ROOT="$(cd ../../.. && pwd)"
mkdir -p build
mpicxx -DOMPI_SKIP_MPICXX -std=c++17 -O2 lifecycle.cpp -o build/lifecycle \
 -I"$ASCEND_HOME_PATH/include" -I"$ROOT/src/smem/include/host" \
 -I"$ROOT/src/smem/include/device" -I"$ROOT/src/hybm/ops/hybm_kernel" \
 -L"$ROOT/build/src/smem/csrc" -lmf_smem -L"$ROOT/build/src/hybm/csrc" -lmf_hybm_core -L"$ASCEND_HOME_PATH/lib64" -lascendcl -lhccl -pthread
export LD_LIBRARY_PATH="$ROOT/build/src/smem/csrc:$ROOT/build/src/hybm/csrc:$ASCEND_HOME_PATH/lib64:$LD_LIBRARY_PATH"
export MF_SDMA_ORCH_DIR="$ROOT/build/src/hybm/ops/hybm_kernel"
MODE=normal
if [[ $# -gt 0 ]]; then MODE="$1"; fi
if [[ "$MODE" == "--build-only" ]]; then exit 0; fi
if [[ "$MODE" == "--build-pypto" ]]; then
    mpicxx -DOMPI_SKIP_MPICXX -std=c++17 -O2 -fPIC -shared -DMF_PYPTO_CALLBACK lifecycle.cpp \
      -o build/libpypto_coexist.so -I"$ASCEND_HOME_PATH/include" -I"$ROOT/src/smem/include/host" \
      -I"$ROOT/src/smem/include/device" -I"$ROOT/src/hybm/ops/hybm_kernel" \
      -L"$ROOT/build/src/smem/csrc" -lmf_smem -L"$ROOT/build/src/hybm/csrc" -lmf_hybm_core \
      -L"$ASCEND_HOME_PATH/lib64" -lascendcl -lhccl -pthread
    exit 0
fi
RANKS=2
if [[ -v TEST_RANKS ]]; then RANKS="$TEST_RANKS"; fi
exec mpirun --allow-run-as-root --stdin none -np "$RANKS" ./build/lifecycle "$MODE" < /dev/null
