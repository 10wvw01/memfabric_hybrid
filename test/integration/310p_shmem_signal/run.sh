#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
ROOT="$(cd ../../.. && pwd)"
: "${ASCEND_HOME_PATH:?Load CANN environment first}"
if [[ -n "${MPI_HOME:-}" ]]; then
 MPI_INC="$MPI_HOME/include"; MPI_LIB="$MPI_HOME/lib"; MPI_RUN="$MPI_HOME/bin/mpirun"
elif [[ -x /usr/local/openmpi/bin/mpirun ]]; then
 MPI_INC=/usr/local/openmpi/include; MPI_LIB=/usr/local/openmpi/lib; MPI_RUN=/usr/local/openmpi/bin/mpirun
else
 MPI_INC=/usr/lib/aarch64-linux-gnu/openmpi/include
 MPI_LIB=/usr/lib/aarch64-linux-gnu/openmpi/lib
 MPI_RUN=/usr/bin/mpirun
fi
mkdir -p build
bisheng --npu-arch=dav-2002 -O2 -std=c++17 -DOMPI_SKIP_MPICXX -x asc signal_api.asc -x none \
 -o build/signal_api -I"$MPI_INC" -I"$ASCEND_HOME_PATH/include" \
 -I"$ROOT/src/smem/include/host" -I"$ROOT/src/smem/include/device" \
 -L"$ROOT/build/src/smem/csrc" -lmf_smem -L"$ASCEND_HOME_PATH/lib64" -lascendcl -lruntime \
 -L"$MPI_LIB" -lmpi -pthread
export LD_LIBRARY_PATH="$ROOT/build/src/smem/csrc:$ROOT/build/src/hybm/csrc:"\
"$MPI_LIB:"\
"$ASCEND_HOME_PATH/lib64:${LD_LIBRARY_PATH:-}"
export MF_SDMA_ORCH_DIR="$ROOT/build/src/hybm/ops/hybm_kernel"
if [[ "${1:-}" == --build-only ]]; then exit 0; fi
if [[ $# -ne 0 ]]; then echo "ERROR: expected no arguments or --build-only" >&2; exit 1; fi
"$MPI_RUN" --allow-run-as-root --stdin none -np 2 ./build/signal_api < /dev/null
