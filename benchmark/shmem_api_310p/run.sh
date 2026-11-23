#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
ROOT="$(cd ../.. && pwd)"
: "${ASCEND_HOME_PATH:?Load CANN environment}"
export LD_LIBRARY_PATH="$ROOT/build/src/smem/csrc:$ROOT/build/src/hybm/csrc:$ASCEND_HOME_PATH/lib64:${LD_LIBRARY_PATH:-}"
export MF_SDMA_ORCH_DIR="${MF_SDMA_ORCH_DIR:-$ROOT/build/src/hybm/ops/hybm_kernel}"
: "${LOG_DIR:?Set LOG_DIR to a directory outside the source tree}"
mkdir -p "$LOG_DIR"
bash build.sh
timeout --kill-after=10s "${TIMEOUT:-1800}s" build/api_perf "${DEVICE_BASE:-0}" "$LOG_DIR" \
 "${WARMUP:-10}" "${ROUNDS:-100}" "${STORE_URL:-tcp://127.0.0.1:18882}" > "$LOG_DIR/launcher.log" 2>&1
python3 summarize.py "$LOG_DIR"
