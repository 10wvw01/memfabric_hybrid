#!/usr/bin/env bash
# run.sh — 310P 卡内双芯 HCCL AllReduce 验证一键编译与运行
#
# 用法:
#   bash run.sh                # 编译并运行两个变体(默认展开 / AI_CPU 展开)
#   bash run.sh --build-only   # 仅编译(产物 /tmp/mf_310p_hccl_allreduce)
#   bash run.sh --run-only     # 仅运行(要求已编译)
#
# 旋钮(环境变量):
#   TEST_DEVICE_BASE=0         起始设备号(须为同卡对 0/2/4/6; base 与 base+1 同卡)
#
# 依赖: CANN(ASCEND_HOME_PATH 或默认安装路径 /usr/local/Ascend)

set -euo pipefail

# ---------- 路径常量 ----------
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC="$SCRIPT_DIR/310p_hccl_allreduce.cc"
BIN="/tmp/mf_310p_hccl_allreduce"
BUILD_ONLY=0
RUN_ONLY=0
if [ "${1:-}" = "--build-only" ]; then
    BUILD_ONLY=1
fi
if [ "${1:-}" = "--run-only" ]; then
    RUN_ONLY=1
fi

BASE="${TEST_DEVICE_BASE:-0}"
if [ $((BASE % 2)) -ne 0 ] || [ "$BASE" -lt 0 ] || [ "$BASE" -gt 6 ]; then
    echo "TEST_DEVICE_BASE 须为 0/2/4/6(同卡设备对的起始号), 当前: $BASE" >&2
    exit 1
fi

# ---------- CANN 环境 ----------
if [ -z "${ASCEND_HOME_PATH:-}" ] && [ -f /usr/local/Ascend/cann/set_env.sh ]; then
    # shellcheck disable=SC1091
    source /usr/local/Ascend/cann/set_env.sh
fi
CANN_ROOT="${ASCEND_HOME_PATH:-/usr/local/Ascend/cann-9.1.0}"
INC="$CANN_ROOT/aarch64-linux/include"
LIB="$CANN_ROOT/aarch64-linux/lib64"
if [ ! -d "$INC" ]; then
    INC="$CANN_ROOT/include"
fi
if [ ! -d "$LIB" ]; then
    LIB="$CANN_ROOT/lib64"
fi
if [ ! -f "$INC/hccl/hccl.h" ]; then
    echo "找不到 hccl.h (INC=$INC), 请确认 CANN 安装" >&2
    exit 1
fi

# ---------- 编译 ----------
if [ "$RUN_ONLY" -eq 0 ]; then
    g++ -O2 -std=c++17 "$SRC" -o "$BIN" \
        -I"$INC" \
        -L"$LIB" -lascendcl -lhccl \
        -Wl,-rpath,"$LIB"
    echo "build OK: $BIN"
fi
if [ "$BUILD_ONLY" -eq 1 ]; then
    exit 0
fi

# ---------- 运行 ----------
unset ASCEND_RT_VISIBLE_DEVICES # 保持恒等映射, 直接用物理设备号

echo "===== 变体 1: 默认展开模式 (dev$BASE+dev$((BASE + 1)), 同卡) ====="
"$BIN" "$BASE"

echo
echo "===== 变体 2: HCCL_OP_EXPANSION_MODE=AI_CPU (点名 AICPU 展开) ====="
HCCL_OP_EXPANSION_MODE=AI_CPU "$BIN" "$BASE"
