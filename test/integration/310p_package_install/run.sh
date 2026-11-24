#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
: "${ASCEND_HOME_PATH:?Load CANN set_env.sh first}"
RUN_PACKAGE="${RUN_PACKAGE:-$ROOT/output/memfabric_hybrid-$(tr -d '[:space:]' < "$ROOT/VERSION")_linux_aarch64.run}"
AICPU_KERNEL_RUN_PACKAGE="${AICPU_KERNEL_RUN_PACKAGE:-$ROOT/output/memfabric_hybrid_aicpu_kernel.run}"
WHEEL_PACKAGE="${WHEEL_PACKAGE:-$(find "$ROOT/output/memfabric_hybrid/wheel" -name '*.whl' -print -quit)}"
LOG_DIR="${LOG_DIR:-/workspace/my-logs-dir/package-install/$(date +%Y%m%d-%H%M%S-%N)}"
mkdir -p "$LOG_DIR"
LOG_DIR="$(realpath "$LOG_DIR")"
RUN_PACKAGE="$(realpath "$RUN_PACKAGE")"
WHEEL_PACKAGE="$(realpath "$WHEEL_PACKAGE")"
unset PYTHONPATH MF_SDMA_ORCH_DIR MEMFABRIC_HYBRID_HOME_PATH MEMFABRIC_HYBRID_EXTEND_LIB_PATH
export LD_LIBRARY_PATH="$(python3 - <<'PY'
import os
print(':'.join(p for p in os.environ.get('LD_LIBRARY_PATH', '').split(':') if p and
               not any(part in p for part in ('memfabric', 'my-worktree', 'output_ut'))))
PY
)"
CANN_LIBRARY_PATH="$LD_LIBRARY_PATH"
pids=()
trap 'for pid in "${pids[@]}"; do kill "$pid" 2>/dev/null || true; done' EXIT
cd "$LOG_DIR"
for mode in run wheel; do
    python3 -m venv --system-site-packages "$LOG_DIR/$mode-venv"
    export PATH="$LOG_DIR/$mode-venv/bin:$PATH"
    export LD_LIBRARY_PATH="$CANN_LIBRARY_PATH"
    unset MEMFABRIC_HYBRID_HOME_PATH MEMFABRIC_HYBRID_EXTEND_LIB_PATH
    if [[ "$mode" == run ]]; then
        bash "$RUN_PACKAGE" --install-path="$LOG_DIR/run-install" > "$LOG_DIR/install-run.log" 2>&1
        source "$LOG_DIR/run-install/memfabric_hybrid/set_env.sh"
        bash "$AICPU_KERNEL_RUN_PACKAGE" --install --force > "$LOG_DIR/install-aicpu-kernel.log" 2>&1
        # rank 加载 venv 内两级布局 .so, FindInstalledJson 上溯两级到包目录找 cust_aicpu_kernel/,
        # 故 install.sh 装到 venv 包内 memfabric_hybrid/cust_aicpu_kernel/, 在此验证同位置。
        pkg_dir=$(python -c 'import memfabric_hybrid as m, os; print(os.path.dirname(os.path.abspath(m.__file__)))')
        for asset in libmf_sdma_orch.so libmf_sdma_orch.json libmf_sdma_orch_launch.json; do
            test -s "$pkg_dir/cust_aicpu_kernel/$asset"
        done
        include="$MEMFABRIC_HYBRID_HOME_PATH/include"
        host="$include/smem/host"
        device="$include/smem/device"
    else
        # wheel 模式: pip 装 wheel 测 Python 包可用性, 但 310P aicpu 产物不再随 wheel 分发,
        # 需装主 run 包提供 MEMFABRIC_HYBRID_HOME_PATH, 再装 aicpu kernel run 让 FindInstalledJson 发现。
        bash "$RUN_PACKAGE" --install-path="$LOG_DIR/wheel-install" > "$LOG_DIR/install-wheel-run.log" 2>&1
        source "$LOG_DIR/wheel-install/memfabric_hybrid/set_env.sh"
        python -m pip install --no-deps --force-reinstall "$WHEEL_PACKAGE" > "$LOG_DIR/install-wheel.log" 2>&1
        bash "$AICPU_KERNEL_RUN_PACKAGE" --install --force > "$LOG_DIR/install-aicpu-kernel.log" 2>&1
        pkg_dir=$(python -c 'import memfabric_hybrid as m, os; print(os.path.dirname(os.path.abspath(m.__file__)))')
        for asset in libmf_sdma_orch.so libmf_sdma_orch.json libmf_sdma_orch_launch.json; do
            test -s "$pkg_dir/cust_aicpu_kernel/$asset"
        done
        include="$(python -c 'import memfabric_hybrid as m; print(m.get_include_path())')"
        host="$include/smem/host"
        device="$include/smem/device"
    fi
    bisheng --npu-arch=dav-2002 -O2 -std=c++17 -fPIC -shared -x asc \
        "$ROOT/test/integration/310p_package_install/exchange.asc" -o "$LOG_DIR/$mode-exchange.so" \
        -I"$ASCEND_HOME_PATH/include" -I"$host" -I"$device" \
        -L"$ASCEND_HOME_PATH/lib64" -lascendcl -lruntime > "$LOG_DIR/$mode-compile.log" 2>&1
    pids=()
    for rank in 0 1; do
        timeout --kill-after=10s 240s python "$ROOT/test/integration/310p_package_install/rank.py" \
            "$rank" "$((${DEVICE_BASE:-2} + rank))" "${STORE_URL:-tcp://127.0.0.1:18812}" \
            "$LOG_DIR/$mode-exchange.so" "$LOG_DIR/$mode-venv" > "$LOG_DIR/$mode-rank$rank.log" 2>&1 &
        pids+=("$!")
    done
    ret=0
    for pid in "${pids[@]}"; do wait "$pid" || ret=1; done
    if ((ret)); then cat "$LOG_DIR/$mode"-rank*.log; exit 1; fi
    for rank in 0 1; do
        for text in 'IMPORT package=' 'automatic deploy begin' 'kernel deployed to CP1 search path' \
                    'automatic deploy file removed' 'full_payload_checked=1'; do
            if ! grep -q "$text" "$LOG_DIR/$mode-rank$rank.log"; then
                echo "ERROR missing $text in $mode rank $rank"; exit 1
            fi
        done
        grep -E 'IMPORT package=|automatic deploy|PASS' "$LOG_DIR/$mode-rank$rank.log"
    done
done
echo "Logs: $LOG_DIR"
