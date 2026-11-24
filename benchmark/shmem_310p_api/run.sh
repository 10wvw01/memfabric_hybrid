#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
ROOT="$(cd ../.. && pwd)"
: "${ASCEND_HOME_PATH:?Load CANN environment first}"
if [[ -n "${MPI_HOME:-}" ]]; then
    MPI_LIB="$MPI_HOME/lib"; MPI_RUN="$MPI_HOME/bin/mpirun"
elif [[ -x /usr/local/openmpi/bin/mpirun ]]; then
    MPI_LIB=/usr/local/openmpi/lib; MPI_RUN=/usr/local/openmpi/bin/mpirun
else
    MPI_LIB=/usr/lib/aarch64-linux-gnu/openmpi/lib; MPI_RUN=/usr/bin/mpirun
fi
export LD_LIBRARY_PATH="$ROOT/build/src/smem/csrc:$ROOT/build/src/hybm/csrc:$MPI_LIB:"\
"$ASCEND_HOME_PATH/lib64:${LD_LIBRARY_PATH:-}"
export MF_SDMA_ORCH_DIR="${MF_SDMA_ORCH_DIR:-$ROOT/build/src/hybm/ops/hybm_kernel}"
export LOG_DIR="${LOG_DIR:-$PWD/build/logs/$(date +%Y%m%d-%H%M%S)}"
mkdir -p "$LOG_DIR"
LOG_DIR="$(cd "$LOG_DIR" && pwd)"
export LOG_DIR
export DEVICE_BASE="${DEVICE_BASE:-6}"
bash build.sh
profile=()
if [[ "${PROFILE:-0}" == 1 ]]; then
 profile=(msprof --output="$LOG_DIR/profile" --ascendcl=on --runtime-api=on --task-time=l2 --aicpu=on --hccl=on --ai-core=on)
fi
timeout --kill-after=10s "${TIMEOUT:-1200}s" "$MPI_RUN" --allow-run-as-root --stdin none -np 2 \
    "${profile[@]}" build/api_perf > "$LOG_DIR/launcher.log" 2>&1
python3 summarize.py "$LOG_DIR"
