#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
: "${ASCEND_HOME_PATH:?Load CANN set_env.sh first}"
LOG_DIR="${LOG_DIR:-/workspace/my-logs-dir/aicpu-retry/$(date +%Y%m%d-%H%M%S-%N)}"
mkdir -p "$LOG_DIR"
LOG_DIR="$(realpath "$LOG_DIR")"
KERNEL_DIR="$ROOT/build/src/hybm/ops/hybm_kernel"
export LD_LIBRARY_PATH="$ROOT/build/src/smem/csrc:$ROOT/build/src/hybm/csrc:$ASCEND_HOME_PATH/lib64:${LD_LIBRARY_PATH:-}"
unset ASCEND_RT_VISIBLE_DEVICES
read -r -a mpi_compile <<< "$("${MPI_CXX:-mpicxx}" --showme:compile)"
read -r -a mpi_link <<< "$("${MPI_CXX:-mpicxx}" --showme:link)"
cd "$LOG_DIR"
bisheng --npu-arch=dav-2002 -O2 -std=c++17 -DOMPI_SKIP_MPICXX -x asc \
 "$ROOT/test/integration/310p_aicpu_deploy/retry.asc" -x none -o "$LOG_DIR/retry" \
 "${mpi_compile[@]}" "${mpi_link[@]}" -I"$ASCEND_HOME_PATH/include" \
 -I"$ROOT/src/smem/include/host" -I"$ROOT/src/smem/include/device" \
 -L"$ROOT/build/src/smem/csrc" -lmf_smem -L"$ROOT/build/src/hybm/csrc" -lmf_hybm_core \
 -L"$ASCEND_HOME_PATH/lib64" -lascendcl -lruntime -ldl -pthread
cc -shared -fPIC "$ROOT/test/integration/310p_aicpu_deploy/retry_inject.c" -o "$LOG_DIR/channel-fail.so" -ldl
mpi_run=("${MPI_RUN:-mpirun}")
if [[ "$(id -u)" == 0 ]]; then mpi_run+=(--allow-run-as-root); fi
export RETRY_BINARY="$LOG_DIR/retry" RETRY_KERNEL_DIR="$KERNEL_DIR"
export RETRY_BASE="${DEVICE_BASE:-4}" RETRY_STORE="${STORE_URL:-tcp://127.0.0.1:18821}"
export RETRY_INJECTOR="$LOG_DIR/channel-fail.so"
for mode in missing-so invalid-json channel verify; do
    export TEST_CASE_DIR="$LOG_DIR/$mode" TEST_CASE_MODE="$mode"
    mkdir -p "$TEST_CASE_DIR"/{rank0,rank1}
    for rank in 0 1; do
        for asset in libmf_sdma_orch.so libmf_sdma_orch.json libmf_sdma_orch_launch.json; do
            cp "$KERNEL_DIR/$asset" "$TEST_CASE_DIR/rank$rank/"
        done
    done
    if [[ "$mode" == missing-so ]]; then rm "$TEST_CASE_DIR/rank0/libmf_sdma_orch.so"; fi
    if [[ "$mode" == invalid-json ]]; then
        echo '{"kernelSo":"../invalid.so"}' > "$TEST_CASE_DIR/rank0/libmf_sdma_orch_launch.json"
    fi
    export EXPECT_FAILURE=1
    launch_task()
    {
        timeout --kill-after=10s 180s "${mpi_run[@]}" -np 2 bash -c '
            export MF_SDMA_ORCH_DIR="$TEST_CASE_DIR/rank$OMPI_COMM_WORLD_RANK"
            if [[ ( "$TEST_CASE_MODE" == channel || "$TEST_CASE_MODE" == verify ) && "$EXPECT_FAILURE" == 1 ]]; then
                export LD_PRELOAD="$RETRY_INJECTOR${LD_PRELOAD:+:$LD_PRELOAD}"
                if [[ "$OMPI_COMM_WORLD_RANK" == 0 ]]; then
                    if [[ "$TEST_CASE_MODE" == channel ]]; then export TEST_DEPLOY_CHANNEL_FAIL=1;
                    else export TEST_DEPLOY_VERIFY_FAIL=1; fi
                fi
            fi
            exec "$RETRY_BINARY" "$RETRY_KERNEL_DIR" "$RETRY_STORE" "$RETRY_BASE" "$EXPECT_FAILURE"
        '
    }
    ret=0
    launch_task > "$LOG_DIR/$mode-failure.log" 2>&1 || ret=$?
    if [[ "$ret" != 2 ]]; then
        echo "ERROR expected creation failure, actual task exit=$ret"; exit 1
    fi
    for rank in 0 1; do
        grep -q "EXPECTED_FAILURE rank=$rank failed=1 allfailed=2" "$LOG_DIR/$mode-failure.log"
        for asset in libmf_sdma_orch.so libmf_sdma_orch.json libmf_sdma_orch_launch.json; do
            cp "$KERNEL_DIR/$asset" "$TEST_CASE_DIR/rank$rank/"
        done
    done
    if [[ "$mode" == verify ]]; then
        grep -q 'deploy copy/verify failed after retries' "$LOG_DIR/$mode-failure.log"
        grep -q 'automatic deploy file removed' "$LOG_DIR/$mode-failure.log"
    fi
    export EXPECT_FAILURE=0
    launch_task > "$LOG_DIR/$mode-restart.log" 2>&1
    for rank in 0 1; do
        grep -q "PASS rank=$rank restart_success=1" "$LOG_DIR/$mode-restart.log"
    done
    echo "PASS $mode"
done
echo "Logs: $LOG_DIR"
