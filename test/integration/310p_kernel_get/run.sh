#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
: "${ASCEND_HOME_PATH:?Load CANN set_env.sh first}"
LOG_DIR="${LOG_DIR:-/workspace/my-logs-dir/310p-kernel-get/$(date +%Y%m%d-%H%M%S-%N)}"
mkdir -p "$LOG_DIR"
LOG_DIR="$(realpath "$LOG_DIR")"
echo "Logs: $LOG_DIR"
RUN_PACKAGE="${RUN_PACKAGE:-$ROOT/output/memfabric_hybrid-$(tr -d '[:space:]' < "$ROOT/VERSION")_linux_aarch64.run}"
python3 -m venv --system-site-packages "$LOG_DIR/venv"
export PATH="$LOG_DIR/venv/bin:$PATH"
unset PYTHONPATH MF_SDMA_ORCH_DIR MEMFABRIC_HYBRID_HOME_PATH MEMFABRIC_HYBRID_EXTEND_LIB_PATH
bash "$RUN_PACKAGE" --install-path="$LOG_DIR/install" > "$LOG_DIR/install.log" 2>&1
source "$LOG_DIR/install/memfabric_hybrid/set_env.sh"
include="$MEMFABRIC_HYBRID_HOME_PATH/include"
bisheng --npu-arch=dav-2002 -O2 -std=c++17 -fPIC -shared -x asc \
    "$ROOT/test/integration/310p_kernel_get/exchange.asc" -o "$LOG_DIR/exchange.so" \
    -I"$ASCEND_HOME_PATH/include" -I"$include/smem/host" -I"$include/smem/device" \
    -L"$ASCEND_HOME_PATH/lib64" -lascendcl -lruntime > "$LOG_DIR/compile.log" 2>&1
pids=()
trap 'for pid in "${pids[@]}"; do kill "$pid" 2>/dev/null || true; done' EXIT
for rank in 0 1; do
    device="${DEVICE0:-2}"
    if ((rank)); then device="${DEVICE1:-3}"; fi
    timeout --kill-after=10s 240s python "$ROOT/test/integration/310p_kernel_get/rank.py" \
        "$rank" "$device" "${STORE_URL:-tcp://127.0.0.1:18832}" "$LOG_DIR/exchange.so" \
        "${ROUNDS:-100}" "${BYTES:-131072}" > "$LOG_DIR/rank$rank.log" 2>&1 &
    pids+=("$!")
done
ret=0
for pid in "${pids[@]}"; do wait "$pid" || ret=1; done
if ((ret)); then cat "$LOG_DIR"/rank*.log; exit 1; fi
for rank in 0 1; do
    grep 'PASS rank=' "$LOG_DIR/rank$rank.log"
done
echo "Logs: $LOG_DIR"
