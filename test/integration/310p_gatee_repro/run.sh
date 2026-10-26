#!/usr/bin/env bash
# run.sh — Gate E 507014 换班竞态复现器一键编译与运行(封装 bisheng 长命令/环境自检/MPI 探测/双 rank 拉起)
#
# 用法:
#   bash run.sh                # 编译复现器并 mpirun -np 2 运行(默认 dev4/dev5, 1s 换班)
#   bash run.sh --build-only   # 仅编译(产物 /tmp/mf_gatee_repro, 不运行)
#   bash run.sh --run-only     # 仅运行(要求 /tmp/mf_gatee_repro 已编译)
#
# 关键旋钮(环境变量, 长跑建议 nohup/tmux):
#   EX09_DEVICE_BASE=4        起始设备号(DEVICE_ID = base + rank; dev4/dev5 为空闲同卡 die 对)
#   EX09_EPOCH_BUDGET_S=1     编排换班周期(秒)——1=攻击模式(边界频率×15), 15=生产对照
#   EX09_TOTAL_GENS=10000000  总代数
#   EX09_BATCH_GENS=1         每次 launch 代数(1=V2 同构逐代)
#   EX09_WAIT_BUDGET=10000000 单次 launch 轮询总量上限(防 28s 同步超时)
#   EX09_CHUNK_BYTES=131072   单 chunk 字节(4194304=V2 保真模式)
#
# 依赖与 08 示例一致: CANN(ASCEND_HOME_PATH)/bisheng/MPI(两套自动识别同源使用)

set -euo pipefail

# ---------- 全局路径常量 ----------
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"     # 仓库根(test/integration/<本目录> 上溯三级)
KERNEL_DIR="$ROOT/build/src/hybm/ops/hybm_kernel"
EX_SRC="$SCRIPT_DIR/gatee_repro.asc"
EX_BIN="/tmp/mf_gatee_repro"
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

# ---------- 编译复现器 ----------
build_example()
{
    echo "[run.sh] 编译复现器: $EX_SRC -> $EX_BIN"
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

# ---------- 运行(设备侧零手工部署; DEVICE_ID 按 rank 偏移到目标 die 对) ----------
run_example()
{
    local json
    json="$(find_launch_json)"
    if [ -z "$json" ]; then
        echo "FAIL: 未找到 launch json(先完成主构建): $KERNEL_DIR/libmf_sdma_orch_v*.json"
        exit 1
    fi
    if [ ! -x "$EX_BIN" ]; then
        echo "FAIL: 复现器未编译(先跑 bash run.sh --build-only): $EX_BIN"
        exit 1
    fi
    export MF_SDMA_ORCH_JSON="$json"
    export MF_SDMA_ORCH_EPOCH_BUDGET_S="${EX09_EPOCH_BUDGET_S:-1}"
    export MF_LOG_LEVEL="${MF_LOG_LEVEL:-INFO}"
    export LD_LIBRARY_PATH="$CANN_LIB:$MPI_LIB${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    local base="${EX09_DEVICE_BASE:-4}"
    echo "[run.sh] 运行: dev$base/dev$((base + 1)) 换班=${MF_SDMA_ORCH_EPOCH_BUDGET_S}s json=$(basename "$json")"
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
    build_example
fi
if [ "$BUILD_ONLY" -eq 0 ]; then
    run_example
fi
