#!/usr/bin/env bash
# run.sh — 08 示例一键编译与运行(封装 bisheng 长命令/环境自检/MPI 探测/双 rank 拉起)
#
# 用法:
#   bash run.sh                # 编译示例并 mpirun -np 2 运行(默认)
#   bash run.sh --build-only   # 仅编译(产物 /tmp/ex08, 不运行)
#   bash run.sh --run-only     # 仅运行(要求 /tmp/ex08 已编译)
#
# 依赖(任一缺失脚本会报错并给出定位提示):
#   - CANN >= 9.1: 环境变量 ASCEND_HOME_PATH 指向安装根(未设时按常见路径兜底探测)
#   - bisheng 编译器: 在 PATH 中(支持 --npu-arch=dav-2002, CANN 工具链自带)
#   - MPI: 自动探测 /usr/local/openmpi(自装) 或发行版 openmpi 两套之一
#          (编译与启动强制同源, 混用会段错误——见 README 必要条件)

set -euo pipefail

# ---------- 全局路径常量 ----------
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"        # 脚本所在目录(即示例目录)
ROOT="$(cd "$SCRIPT_DIR/../../../.." && pwd)"      # 仓库根(示例目录上溯四级)
KERNEL_DIR="$ROOT/build/src/hybm/ops/hybm_kernel"  # 编排 kernel 三件套构建目录
EX_SRC="$SCRIPT_DIR/08_310p_aicore_aicpu_sdma.asc" # 示例源文件(host+AICore 单源)
EX_BIN="/tmp/ex08"                                 # 示例可执行产物路径
BUILD_ONLY=0                                       # 仅编译模式标志(--build-only)
RUN_ONLY=0                                         # 仅运行模式标志(--run-only)

# 主库四个静态库(主构建产物; 链接顺序由 --start-group/--end-group 包容环依赖)
MAIN_LIBS=(
    "$ROOT/build/src/smem/csrc/libmf_smem.a"
    "$ROOT/build/src/hybm/csrc/libmf_hybm_core.a"
    "$ROOT/build/src/acc_links/csrc/libacc_tcp_net.a"
    "$ROOT/build/src/acc_offload/csrc/libmf_acc_offload.a"
)

# ---------- 参数解析 ----------
for arg in "$@"; do
    case "$arg" in
        --build-only) BUILD_ONLY=1 ;; # 仅编译
        --run-only) RUN_ONLY=1 ;;     # 仅运行
        *) echo "FAIL: 未知参数: $arg (支持 --build-only / --run-only)"; exit 1 ;;
    esac
done
if [ "$BUILD_ONLY" -eq 1 ] && [ "$RUN_ONLY" -eq 1 ]; then
    echo "FAIL: --build-only 与 --run-only 互斥"
    exit 1
fi

# ---------- 环境自检: CANN(未设时常见路径兜底) ----------
if [ -z "${ASCEND_HOME_PATH:-}" ]; then
    for cand in /usr/local/Ascend/ascend-toolkit/latest /usr/local/Ascend/cann*; do
        if [ -d "$cand" ]; then
            ASCEND_HOME_PATH="$cand" # 命中常见安装路径, 自动注入
            break
        fi
    done
fi
if [ -z "${ASCEND_HOME_PATH:-}" ]; then
    echo "FAIL: 未设置 ASCEND_HOME_PATH(CANN 安装根, 例: /usr/local/Ascend/cann-9.1.0)"
    exit 1
fi
CANN_INC="$ASCEND_HOME_PATH/include"              # CANN 头文件目录
CANN_LIB="$ASCEND_HOME_PATH/aarch64-linux/lib64"  # CANN 库目录(ascendcl/runtime)

# ---------- 环境自检: bisheng ----------
if ! command -v bisheng >/dev/null 2>&1; then
    echo "FAIL: bisheng 不在 PATH(AICore host 统一编译器, 通常在 CANN 工具链 bin 下)"
    exit 1
fi

# ---------- MPI 探测(自装优先, 发行版兜底; 编译与启动同源) ----------
if [ -x /usr/local/openmpi/bin/mpirun ]; then
    MPI_INC="/usr/local/openmpi/include"     # 自装 OpenMPI 头目录
    MPI_LIB="/usr/local/openmpi/lib"         # 自装 OpenMPI 库目录
    MPI_RUN="/usr/local/openmpi/bin/mpirun"  # 自装启动器
elif [ -x /usr/bin/mpirun ] && [ -d /usr/lib/aarch64-linux-gnu/openmpi ]; then
    MPI_INC="/usr/lib/aarch64-linux-gnu/openmpi/include" # 发行版头目录
    MPI_LIB="/usr/lib/aarch64-linux-gnu/openmpi/lib"     # 发行版库目录
    MPI_RUN="/usr/bin/mpirun"                            # 发行版启动器
else
    echo "FAIL: 未找到可用的 MPI(期望 /usr/local/openmpi 或发行版 openmpi)"
    exit 1
fi

# ---------- 主构建检查: 任一静态库缺失则自动触发主构建 ----------
ensure_main_build()
{
    local lib    # 遍历变量
    for lib in "${MAIN_LIBS[@]}"; do
        if [ ! -f "$lib" ]; then
            echo "[run.sh] 主库静态库缺失($lib), 自动触发主构建(XPU_TYPE=NPU, BUILD_PYTHON=OFF)..."
            cmake -S "$ROOT" -B "$ROOT/build" -DXPU_TYPE=NPU -DBUILD_PYTHON=OFF
            cmake --build "$ROOT/build" -j
            return 0
        fi
    done
}

# ---------- 编译示例(bisheng host+AICore 统一编译; 长命令的唯一归宿) ----------
build_example()
{
    echo "[run.sh] 编译示例: $EX_SRC -> $EX_BIN"
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

# ---------- 定位 launch json(取版本号最高的一件, 免随 KVER 手工同步) ----------
find_launch_json()
{
    ls -v "$KERNEL_DIR"/libmf_sdma_orch_v*.json 2>/dev/null | tail -n 1 || true
}

# ---------- 运行(设备侧零手工部署: 建池时库内自动迁移+校验) ----------
run_example()
{
    local json # launch json 绝对路径(为空表示主构建未产出)
    json="$(find_launch_json)"
    if [ -z "$json" ]; then
        echo "FAIL: 未找到 launch json(先完成主构建): $KERNEL_DIR/libmf_sdma_orch_v*.json"
        exit 1
    fi
    if [ ! -x "$EX_BIN" ]; then
        echo "FAIL: 示例未编译(先跑 bash run.sh 或 bash run.sh --build-only): $EX_BIN"
        exit 1
    fi
    export MF_SDMA_ORCH_JSON="$json" # 显式指定 launch json(免依赖 CWD 与安装前缀发现)
    export LD_LIBRARY_PATH="$CANN_LIB:$MPI_LIB${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" # 运行期库搜索路径
    echo "[run.sh] 运行: $MPI_RUN -np 2 $EX_BIN"
    echo "[run.sh] launch json: $json"
    if [ "$(id -u)" -eq 0 ]; then # root 用户需显式放行
        exec "$MPI_RUN" --allow-run-as-root -np 2 "$EX_BIN"
    fi
    exec "$MPI_RUN" -np 2 "$EX_BIN"
}

# ---------- 主流程 ----------
if [ "$RUN_ONLY" -eq 0 ]; then # 非仅运行: 先确保主构建, 再编译示例
    ensure_main_build
    build_example
fi
if [ "$BUILD_ONLY" -eq 0 ]; then # 非仅编译: 运行
    run_example
fi
