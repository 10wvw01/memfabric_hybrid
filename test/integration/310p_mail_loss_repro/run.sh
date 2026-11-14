#!/usr/bin/env bash
# 编译并运行310P3双 rank 接收通知丢失与等待超时复现测试。
#
# 用法：
#   EX09_DEVICE_BASE=0 bash run.sh  # 编译后使用设备0/1运行
#   bash run.sh --build-only       # 仅编译，输出 /tmp/mf_mail_loss_repro
#   EX09_DEVICE_BASE=0 bash run.sh --run-only  # 使用已有测试程序运行
#
# 参数说明见本目录 README.md。编译和运行使用同一套 MPI。
# 未设置 EX09_DEVICE_BASE 时，默认使用设备4/5。

set -euo pipefail

# 构建路径。
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"     # 仓库根(test/integration/<本目录> 上溯三级)
KERNEL_DIR="$ROOT/build/src/hybm/ops/hybm_kernel"
EX_SRC="$SCRIPT_DIR/mail_loss_repro.asc"
EX_BIN="/tmp/mf_mail_loss_repro"
BUILD_ONLY=0
RUN_ONLY=0

MAIN_LIBS=(
    "$ROOT/build/src/smem/csrc/libmf_smem.a"
    "$ROOT/build/src/hybm/csrc/libmf_hybm_core.a"
    "$ROOT/build/src/acc_links/csrc/libacc_tcp_net.a"
    "$ROOT/build/src/acc_offload/csrc/libmf_acc_offload.a"
)

# 运行模式。
for arg in "$@"; do
    case "$arg" in
        --build-only) BUILD_ONLY=1 ;;
        --run-only) RUN_ONLY=1 ;;
        *) echo "ERROR: 未知参数: $arg (支持 --build-only / --run-only)"; exit 1 ;;
    esac
done
if [ "$BUILD_ONLY" -eq 1 ] && [ "$RUN_ONLY" -eq 1 ]; then
    echo "ERROR: --build-only 与 --run-only 互斥"
    exit 1
fi

# 未指定 CANN 路径时查找已安装的目录。
if [ -z "${ASCEND_HOME_PATH:-}" ]; then
    for cand in /usr/local/Ascend/ascend-toolkit/latest /usr/local/Ascend/cann*; do
        if [ -d "$cand" ]; then
            ASCEND_HOME_PATH="$cand"
            break
        fi
    done
fi
if [ -z "${ASCEND_HOME_PATH:-}" ]; then
    echo "ERROR: 未设置 ASCEND_HOME_PATH(例: /usr/local/Ascend/cann-9.1.0)"
    exit 1
fi
CANN_INC="$ASCEND_HOME_PATH/include"
CANN_LIB="$ASCEND_HOME_PATH/aarch64-linux/lib64"

# 检查编译器。
if ! command -v bisheng >/dev/null 2>&1; then
    echo "ERROR: bisheng 不在 PATH(CANN 工具链 bin 下)"
    exit 1
fi

# 保持头文件、库和启动程序来自同一套 MPI。
if [ -x /usr/local/openmpi/bin/mpirun ]; then
    MPI_INC="/usr/local/openmpi/include"
    MPI_LIB="/usr/local/openmpi/lib"
    MPI_RUN="/usr/local/openmpi/bin/mpirun"
elif [ -x /usr/bin/mpirun ] && [ -d /usr/lib/aarch64-linux-gnu/openmpi ]; then
    MPI_INC="/usr/lib/aarch64-linux-gnu/openmpi/include"
    MPI_LIB="/usr/lib/aarch64-linux-gnu/openmpi/lib"
    MPI_RUN="/usr/bin/mpirun"
else
    echo "ERROR: 未找到可用的 MPI(期望 /usr/local/openmpi 或发行版 openmpi)"
    exit 1
fi

# 静态库缺失时先构建主库。
ensure_main_build()
{
    local lib
    for lib in "${MAIN_LIBS[@]}"; do
        if [ ! -f "$lib" ]; then
            echo "[run.sh] 静态库不存在: $lib；开始构建(XPU_TYPE=NPU, BUILD_PYTHON=OFF)"
            cmake -S "$ROOT" -B "$ROOT/build" -DXPU_TYPE=NPU -DBUILD_PYTHON=OFF
            cmake --build "$ROOT/build" -j
            return 0
        fi
    done
}

# 编译测试程序。
build_example()
{
    echo "[run.sh] 编译测试程序: $EX_SRC -> $EX_BIN"
    bisheng --npu-arch=dav-2002 -O2 -std=c++17 -w \
        -x asc "$EX_SRC" -x none -o "$EX_BIN" \
        -DOMPI_SKIP_MPICXX \
        -I"$MPI_INC" -I"$CANN_INC" \
        -I"$ROOT/src/smem/include/host" -I"$ROOT/src/smem/include/device" \
        -I"$ROOT/src/hybm/ops/hybm_kernel" \
        -Wl,--start-group "${MAIN_LIBS[@]}" -Wl,--end-group \
        -L"$MPI_LIB" -lmpi -L"$CANN_LIB" -lascendcl -lruntime \
        -ldl -pthread -lm
    echo "[run.sh] 编译完成: $EX_BIN"
}

# 选择版本号最大的 AICPU kernel 启动配置。
find_launch_json()
{
    ls -v "$KERNEL_DIR"/libmf_sdma_orch_v*.json 2>/dev/null | tail -n 1 || true
}

# 两个 rank 分别使用 base 和 base+1 指定的设备。
run_example()
{
    local json
    json="$(find_launch_json)"
    if [ -z "$json" ]; then
        echo "ERROR: 未找到 launch json(先完成主构建): $KERNEL_DIR/libmf_sdma_orch_v*.json"
        exit 1
    fi
    if [ ! -x "$EX_BIN" ]; then
        echo "ERROR: 测试程序不存在或不可执行，请先运行 bash run.sh --build-only: $EX_BIN"
        exit 1
    fi
    export MF_SDMA_ORCH_JSON="$json"
    export MF_SDMA_ORCH_EPOCH_BUDGET_S="${EX09_EPOCH_BUDGET_S:-1}"
    export MF_LOG_LEVEL="${MF_LOG_LEVEL:-INFO}"
    export LD_LIBRARY_PATH="$CANN_LIB:$MPI_LIB${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    local base="${EX09_DEVICE_BASE:-4}"
    echo "[run.sh] 运行: device=$base/$((base + 1)) AICPU单次运行时间=${MF_SDMA_ORCH_EPOCH_BUDGET_S}s json=$(basename "$json")"
    if [ "$(id -u)" -eq 0 ]; then
        exec "$MPI_RUN" --allow-run-as-root -np 2 bash -c \
            "DEVICE_ID=\$(( $base + \$OMPI_COMM_WORLD_RANK )) exec $EX_BIN"
    fi
    exec "$MPI_RUN" -np 2 bash -c \
        "DEVICE_ID=\$(( $base + \$OMPI_COMM_WORLD_RANK )) exec $EX_BIN"
}

# 根据运行模式选择编译或执行。
if [ "$RUN_ONLY" -eq 0 ]; then
    ensure_main_build
    build_example
fi
if [ "$BUILD_ONLY" -eq 0 ]; then
    run_example
fi
