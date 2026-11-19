#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
ROOT="$(cd ../.. && pwd)"
: "${ASCEND_HOME_PATH:?Load CANN environment}"
export LD_LIBRARY_PATH="$ROOT/build/src/smem/csrc:$ROOT/build/src/hybm/csrc:"\
"$ASCEND_HOME_PATH/lib64:${LD_LIBRARY_PATH:-}"
export MF_SDMA_ORCH_DIR="${MF_SDMA_ORCH_DIR:-$ROOT/build/src/hybm/ops/hybm_kernel}"
export HCCL_BUFFSIZE="${HCCL_BUFFSIZE:-64}"
export HCCL_INTRA_PCIE_ENABLE=1
export HCCL_INTRA_ROCE_ENABLE=0
LOG_DIR="${LOG_DIR:-$PWD/build/logs/$(date +%Y%m%d-%H%M%S)}"
mkdir -p "$LOG_DIR"
bash build.sh
backends="${BACKEND:-both}"
if [[ "$backends" == both ]]; then backends="mf-signal hccl"; fi
for backend in $backends; do
 mkdir -p "$LOG_DIR/$backend"
 timeout --kill-after=10s "${TIMEOUT:-600}s" build/tp2_compare "$backend" "${DEVICE_BASE:-0}" \
  "$LOG_DIR/$backend" "${WARMUP:-10}" "${ROUNDS:-100}" "${STORE_URL:-tcp://127.0.0.1:18881}" \
  > "$LOG_DIR/$backend/launcher.log" 2>&1
done
python3 summarize.py "$LOG_DIR"
