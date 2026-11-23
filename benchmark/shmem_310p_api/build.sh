#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
ROOT="$(cd ../.. && pwd)"
: "${ASCEND_HOME_PATH:?Load CANN environment first}"
if [[ -n "${MPI_HOME:-}" ]]; then
    MPI_INC="$MPI_HOME/include"; MPI_LIB="$MPI_HOME/lib"
elif [[ -x /usr/local/openmpi/bin/mpirun ]]; then
    MPI_INC=/usr/local/openmpi/include; MPI_LIB=/usr/local/openmpi/lib
else
    MPI_INC=/usr/lib/aarch64-linux-gnu/openmpi/include
    MPI_LIB=/usr/lib/aarch64-linux-gnu/openmpi/lib
fi
mkdir -p build
bisheng --npu-arch=dav-2002 -O2 -std=c++17 -DOMPI_SKIP_MPICXX -x asc main.asc \
    -o build/api_perf -I"$MPI_INC" -I"$ASCEND_HOME_PATH/include" \
    -I"$ROOT/src/smem/include/host" -I"$ROOT/src/smem/include/device" \
    -L"$ROOT/build/src/smem/csrc" -lmf_smem -L"$ASCEND_HOME_PATH/lib64" -lascendcl -lruntime \
    -L"$MPI_LIB" -lmpi -pthread
