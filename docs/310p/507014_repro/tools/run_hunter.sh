#!/usr/bin/env bash
# Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
# MemFabric_Hybrid is licensed under Mulan PSL v2.
# run_hunter.sh — 构建 + 拉起邮箱丢失猎手(dev2/dev3, 18s epoch + 监督重拉的 wzy-dev-fused 构建)
# 用法: bash run_hunter.sh [--smoke | --soak]
#   --smoke: 冒烟(约 60s, 3 launch)
#   --soak:  长稳(默认 1800s, 可用 HUNTER_SOAK_S 覆盖)
set -euo pipefail

MF=/tmp/opencode/mf_diag
SRC=/tmp/opencode/repro/hunter.asc
BIN=/tmp/opencode/repro/hunter
CANN_INC="$ASCEND_HOME_PATH/include"
CANN_LIB="$ASCEND_HOME_PATH/aarch64-linux/lib64"
MPI_INC=/usr/local/openmpi/include
MPI_LIB=/usr/local/openmpi/lib
MPI_RUN=/usr/local/openmpi/bin/mpirun

if [ ! -f "$BIN" ] || [ "$SRC" -nt "$BIN" ]; then
    echo "[run_hunter] build hunter"
    bisheng --npu-arch=dav-2002 -O2 -std=c++17 -w \
        -x asc "$SRC" -x none -o "$BIN" \
        -DOMPI_SKIP_MPICXX \
        -I"$MPI_INC" -I"$CANN_INC" \
        -I"$MF/src/smem/include/host" -I"$MF/src/smem/include/device" \
        -Wl,--start-group \
        "$MF/build/src/smem/csrc/libmf_smem.a" \
        "$MF/build/src/hybm/csrc/libmf_hybm_core.a" \
        "$MF/build/src/acc_links/csrc/libacc_tcp_net.a" \
        "$MF/build/src/acc_offload/csrc/libmf_acc_offload.a" \
        -Wl,--end-group \
        -L"$MPI_LIB" -lmpi -L"$CANN_LIB" -lascendcl -lruntime -ldl -pthread -lm
fi

export ASCEND_RT_VISIBLE_DEVICES=2,3
export MF_SDMA_ORCH_JSON="$MF/build/src/hybm/ops/hybm_kernel/libmf_sdma_orch_v12.json"
export LD_LIBRARY_PATH="$CANN_LIB:$MPI_LIB${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export HUNTER_STORE_URL="${HUNTER_STORE_URL:-tcp://127.0.0.1:8583}"

MODE="${1:--soak}"
if [ "$MODE" = "--smoke" ]; then
    export HUNTER_LAUNCHES=3 HUNTER_ROUNDS=16 HUNTER_SOAK_S=120
else
    export HUNTER_LAUNCHES=0 HUNTER_ROUNDS="${HUNTER_ROUNDS:-16}" HUNTER_SOAK_S="${HUNTER_SOAK_S:-1800}"
fi

echo "[run_hunter] mode=$MODE devices=$ASCEND_RT_VISIBLE_DEVICES json=$MF_SDMA_ORCH_JSON"
exec "$MPI_RUN" --allow-run-as-root --mca plm isolated -np 2 "$BIN"
