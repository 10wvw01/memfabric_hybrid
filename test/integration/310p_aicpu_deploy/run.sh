#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
: "${ASCEND_HOME_PATH:?Load CANN set_env.sh first}"
LOG_DIR="${LOG_DIR:-/workspace/my-logs-dir/aicpu-deploy/$(date +%Y%m%d-%H%M%S-%N)}"
mkdir -p "$LOG_DIR"
export LD_LIBRARY_PATH="$ROOT/build/src/smem/csrc:$ROOT/build/src/hybm/csrc:$ASCEND_HOME_PATH/lib64:${LD_LIBRARY_PATH:-}"
export MF_SDMA_ORCH_DIR="${MF_SDMA_ORCH_DIR:-$ROOT/build/src/hybm/ops/hybm_kernel}"
export MF_LOG_LEVEL=1
bisheng --npu-arch=dav-2002 -O2 -std=c++17 -x asc "$ROOT/test/integration/310p_aicpu_deploy/main.asc" \
 -o "$LOG_DIR/shmem_d2d" -I"$ASCEND_HOME_PATH/include" -I"$ROOT/src/smem/include/host" \
 -I"$ROOT/src/smem/include/device" -L"$ROOT/build/src/smem/csrc" -lmf_smem \
 -L"$ROOT/build/src/hybm/csrc" -lmf_hybm_core -L"$ASCEND_HOME_PATH/lib64" -lascendcl -lruntime -ldl -pthread
g++ -O2 -std=c++17 "$ROOT/test/integration/310p_aicpu_deploy/inspect.cpp" -o "$LOG_DIR/inspect" \
 -I"$ASCEND_HOME_PATH/include" -I"$ROOT/tools/aicpu_kernel_deploy" \
 -I"$ROOT/src/hybm/ops/hybm_kernel" -I"$ROOT/src/smem/include/device" \
 -L"$ASCEND_HOME_PATH/lib64" -lascendcl -lruntime
export DEPLOY_INSPECT_SECONDS="${DEPLOY_INSPECT_SECONDS:-15}"
pids=()
coproc INSPECTOR { timeout --kill-after=10s 300s "$LOG_DIR/inspect" "${DEVICE_BASE:-2}" \
    "$MF_SDMA_ORCH_DIR/libmf_sdma_orch.so" 2> "$LOG_DIR/inspect.log"; }
inspector_pid=$INSPECTOR_PID
trap 'for pid in "${pids[@]}" "$inspector_pid"; do kill "$pid" 2>/dev/null || true; done' EXIT
read -r -t 45 reply <&"${INSPECTOR[0]}"
if [[ "$reply" != READY ]]; then echo "ERROR inspector not ready"; exit 1; fi
probe()
{
    printf '%s\n' "$1" >&"${INSPECTOR[1]}"
    read -r -t 45 reply <&"${INSPECTOR[0]}"
    if [[ "$reply" != 0 ]]; then echo "ERROR device inspection failed: $1 ret=$reply"; exit 1; fi
}
for run in 1 2; do
    pids=()
    for rank in 0 1; do
        timeout --kill-after=10s 180s "$LOG_DIR/shmem_d2d" "$rank" "$((${DEVICE_BASE:-2} + rank))" \
          "${STORE_URL:-tcp://127.0.0.1:18792}" "${BYTES:-131072}" "${ROUNDS:-100}" \
          > "$LOG_DIR/run$run-rank$rank.log" 2>&1 &
        pids+=("$!")
    done
    for rank in 0 1; do
        log="$LOG_DIR/run$run-rank$rank.log"
        for attempt in $(seq 1 60); do
            if grep -q 'kernel deployed to CP1 search path' "$log"; then break; fi
            sleep 1
        done
        path="$(sed -n 's/.*kernel deployed to CP1 search path.* dst: //p' "$log" | head -1)"
        if [[ ! "$path" =~ ^/usr/lib64/aicpu_kernels/0/aicpu_kernels_device/libmf_sdma_orch_[0-9]+_[a-zA-Z0-9]+\.so$ ]]; then
            echo "ERROR invalid deployed path: $path"; exit 1
        fi
        nonce="$(sed -n 's/.*kernel deployed to CP1 search path.* nonce: \([0-9]*\).*/\1/p' "$log" | head -1)"
        probe "present $path $nonce"
    done
    ret=0
    for pid in "${pids[@]}"; do wait "$pid" || ret=1; done
    if ((ret)); then cat "$LOG_DIR"/run"$run"-rank*.log; exit 1; fi
    for rank in 0 1; do
        log="$LOG_DIR/run$run-rank$rank.log"
        for text in 'automatic deploy begin' 'kernel deployed to CP1 search path' \
                    'deployment files absence verified' 'automatic deploy file removed' \
                    'full_payload_checked=1' 'cleanup_complete=1'; do
            if ! grep -q "$text" "$log"; then echo "ERROR missing $text in $log"; exit 1; fi
        done
        path="$(sed -n 's/.*kernel deployed to CP1 search path.* dst: //p' "$log" | head -1)"
        nonce="$(sed -n 's/.*kernel deployed to CP1 search path.* nonce: \([0-9]*\).*/\1/p' "$log" | head -1)"
        probe "absent $path $nonce"
        grep -E 'automatic deploy|byte-for-byte verified|absence verified|PASS' "$log"
    done
done
printf 'exit\n' >&"${INSPECTOR[1]}"
wait "$inspector_pid"
echo "Logs: $LOG_DIR"
