#!/usr/bin/env bash
# run.sh — 310P SDMA 长稳压测一键编译与运行(封装 bisheng 长命令/环境自检/MPI 探测/双 rank 拉起)
#
# 用法:
#   bash run.sh                # 编译测试并 mpirun -np 2 运行(默认 30000 轮约 46s)
#   bash run.sh --build-only   # 仅编译(产物 /tmp/mf_310p_sdma_stability, 不运行)
#   bash run.sh --run-only     # 仅运行(要求已编译)
#
# 旋钮(环境变量):
#   STRESS_ROUNDS=30000        长稳轮数
#   TEST_DEVICE_BASE=0         起始设备号(DEVICE_ID = base + rank; 空闲 die 对可避开占用设备)
#
# 依赖与 08 示例一致: CANN(ASCEND_HOME_PATH)/bisheng/MPI(两套自动识别同源使用)

set -euo pipefail

# ---------- 全局路径常量 ----------
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)" # test/integration/<本目录> 上溯三级即仓库根
KERNEL_DIR="$ROOT/build/src/hybm/ops/hybm_kernel"
EX_SRC="$SCRIPT_DIR/310p_sdma_stability.asc"
EX_BIN="/tmp/mf_310p_sdma_stability"
BUILD_ONLY=0
RUN_ONLY=0

MAIN_LIBS=(
    "$ROOT/build/src/smem/csrc/libmf_smem.a"
    "$ROOT/build/src/hybm/csrc/libmf_hybm_core.a"
    "$ROOT/build/src/acc_links/csrc/libacc_tcp_net.a"
    "$ROOT/build/src/acc_offload/csrc/libmf_acc_offload.a"
)

# ---------- 参数解析 ----------
for arg in "$@"; do
    case "$arg" in
        --build-only) BUILD_ONLY=1 ;;
        --run-only) RUN_ONLY=1 ;;
        *) echo "FAIL: 未知参数: $arg (支持 --build-only / --run-only)"; exit 1 ;;
    esac
done
if [ "$BUILD_ONLY" -eq 1 ] && [ "$RUN_ONLY" -eq 1 ]; then
    echo "FAIL: --build-only 与 --run-only 互斥"
    exit 1
fi

# ---------- 环境自检: CANN ----------
if [ -z "${ASCEND_HOME_PATH:-}" ]; then
    for cand in /usr/local/Ascend/ascend-toolkit/latest /usr/local/Ascend/cann*; do
        if [ -d "$cand" ]; then
            ASCEND_HOME_PATH="$cand"
            break
        fi
    done
fi
if [ -z "${ASCEND_HOME_PATH:-}" ]; then
    echo "FAIL: 未设置 ASCEND_HOME_PATH(例: /usr/local/Ascend/cann-9.1.0)"
    exit 1
fi
CANN_INC="$ASCEND_HOME_PATH/include"
CANN_LIB="$ASCEND_HOME_PATH/aarch64-linux/lib64"

# ---------- 环境自检: bisheng ----------
if ! command -v bisheng >/dev/null 2>&1; then
    echo "FAIL: bisheng 不在 PATH(CANN 工具链 bin 下)"
    exit 1
fi

# ---------- MPI 探测(自装优先, 编译与启动同源) ----------
if [ -x /usr/local/openmpi/bin/mpirun ]; then
    MPI_INC="/usr/local/openmpi/include"
    MPI_LIB="/usr/local/openmpi/lib"
    MPI_RUN="/usr/local/openmpi/bin/mpirun"
elif [ -x /usr/bin/mpirun ] && [ -d /usr/lib/aarch64-linux-gnu/openmpi ]; then
    MPI_INC="/usr/lib/aarch64-linux-gnu/openmpi/include"
    MPI_LIB="/usr/lib/aarch64-linux-gnu/openmpi/lib"
    MPI_RUN="/usr/bin/mpirun"
else
    echo "FAIL: 未找到可用的 MPI(期望 /usr/local/openmpi 或发行版 openmpi)"
    exit 1
fi

# ---------- 主构建检查 ----------
ensure_main_build()
{
    local lib
    for lib in "${MAIN_LIBS[@]}"; do
        if [ ! -f "$lib" ]; then
            echo "[run.sh] 主库静态库缺失($lib), 自动触发主构建(XPU_TYPE=NPU, BUILD_PYTHON=OFF)..."
            cmake -S "$ROOT" -B "$ROOT/build" -DXPU_TYPE=NPU -DBUILD_PYTHON=OFF
            cmake --build "$ROOT/build" -j
            return 0
        fi
    done
}

# ---------- 编译测试 ----------
build_test()
{
    echo "[run.sh] 编译测试: $EX_SRC -> $EX_BIN"
    bisheng --npu-arch=dav-2002 -O2 -std=c++17 -w \
        -x asc "$EX_SRC" -x none -o "$EX_BIN" \
        -DOMPI_SKIP_MPICXX \
        -I"$MPI_INC" -I"$CANN_INC" \
        -I"$ROOT/src/smem/include/host" -I"$ROOT/src/smem/include/device" \
        -Wl,--start-group "${MAIN_LIBS[@]}" -Wl,--end-group \
        -L"$MPI_LIB" -lmpi -L"$CANN_LIB" -lascendcl -lruntime \
        -ldl -pthread -lm
    echo "[run.sh] 编译完成: $EX_BIN"
}

# ---------- 定位 launch json(取版本号最高) ----------
find_launch_json()
{
    ls -v "$KERNEL_DIR"/libmf_sdma_orch_v*.json 2>/dev/null | tail -n 1 || true
}

# ---------- 运行 ----------
run_test()
{
    local json
    json="$(find_launch_json)"
    if [ -z "$json" ]; then
        echo "FAIL: 未找到 launch json(先完成主构建): $KERNEL_DIR/libmf_sdma_orch_v*.json"
        exit 1
    fi
    if [ ! -x "$EX_BIN" ]; then
        echo "FAIL: 测试未编译(先跑 bash run.sh --build-only): $EX_BIN"
        exit 1
    fi
    export MF_SDMA_ORCH_JSON="$json"
    export LD_LIBRARY_PATH="$CANN_LIB:$MPI_LIB${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    local base="${TEST_DEVICE_BASE:-0}"
    echo "[run.sh] 运行: dev$base/dev$((base + 1)) 轮数=${STRESS_ROUNDS:-30000} json=$(basename "$json")"
    if [ "$(id -u)" -eq 0 ]; then
        exec "$MPI_RUN" --allow-run-as-root -np 2 bash -c \
            "DEVICE_ID=\$(( $base + \$OMPI_COMM_WORLD_RANK )) exec $EX_BIN"
    fi
    exec "$MPI_RUN" -np 2 bash -c \
        "DEVICE_ID=\$(( $base + \$OMPI_COMM_WORLD_RANK )) exec $EX_BIN"
}

# ---------- 主流程 ----------
if [ "$RUN_ONLY" -eq 0 ]; then
    ensure_main_build
    build_test
fi
if [ "$BUILD_ONLY" -eq 0 ]; then
    run_test
fi
