#!/bin/bash
# Op-level matrix runner: {stock, ar, iar} x {M...} x (reps in-process).
#   stock = MM + HCCL AllReduce (baseline)
#   ar    = smem_mm_ar_310p3  (local reduction)
#   iar   = smem_mm_iar_310p3 (hardware reduction)
# Device discipline: physical 4/5 only.
#
# Env overrides: MS, PATHS, REPS, ITERS, DEVICES, TS, OUTROOT, BASE_PORT,
#                CELL_TIMEOUT
set -u
GA=/usr/local/Ascend/cann-9.1.0
MFOUT=/home/w00804146/310P_2/memfabric_hybrid/output
DIR=$(cd "$(dirname "$0")" && pwd)
PRD=$(cd "$DIR/../.." && pwd)

TS=${TS:-$(date +%Y%m%d_%H%M%S)}
OUTROOT=${OUTROOT:-$DIR/results/$TS}
MS=${MS:-"1280 3840 4096"}
PATHS=${PATHS:-"stock ar iar"}
REPS=${REPS:-3}
ITERS=${ITERS:-30}
DEVICES=${DEVICES:-4,5}
BASE_PORT=${BASE_PORT:-8760}
CELL_TIMEOUT=${CELL_TIMEOUT:-1200}   # 单格看门狗秒数：到期 TERM、60s 后升级 KILL。

unset ASCEND_RT_VISIBLE_DEVICES
source $GA/set_env.sh
export MF_SDMA_ORCH_DEPLOY_HELPER=/home/w00804146/310P_2/memfabric_hybrid/tools/aicpu_kernel_deploy/orch_deploy_helper
export MF_SDMA_ORCH_JSON=$MFOUT/hybm/aicpu_kernel/libmf_sdma_orch_v28.json
export MEMFABRIC_HYBRID_HOME_PATH=$MFOUT
export MEMFABRIC_HYBRID_EXTEND_LIB_PATH=$MFOUT/hybm/lib64
export MIXFABRIC_SDK_OUT=$MFOUT
export LD_LIBRARY_PATH=$MFOUT/smem/lib64:$MFOUT/hybm/lib64:$MFOUT/acc_links/lib64:${LD_LIBRARY_PATH:-}
export ASCEND_GLOBAL_LOG_LEVEL=0

# 每个 path 切到自己的算子包（stock 用 ar 包占位——不调用融合核，路径无实际作用）
pkg_of() {
    case $1 in
        stock|ar)  echo "$PRD/smem_mm_ar_310p3/build_out/_CPack_Packages/Linux/External/custom_opp_ubuntu_aarch64.run/packages/vendors/smem_mm_ar_310p3" ;;
        iar)       echo "$PRD/smem_mm_iar_310p3/build_out/_CPack_Packages/Linux/External/custom_opp_ubuntu_aarch64.run/packages/vendors/smem_mm_iar_310p3" ;;
    esac
}

mkdir -p "$OUTROOT"
echo "op-bench matrix: PATHS='$PATHS' MS='$MS' REPS=$REPS ITERS=$ITERS -> $OUTROOT"

run_cell() {  # $1=path $2=M
    local PATH_=$1 M=$2 OUT PORT STORE R0 R1
    OUT=$OUTROOT/${PATH_}_m${M}
    PORT=$BASE_PORT
    STORE=$((PORT + 100))
    mkdir -p "$OUT"
    echo "== cell path=$PATH_ M=$M"
    export ASCEND_CUSTOM_OPP_PATH=$(pkg_of $PATH_)
    RANK=0 $PY $DIR/op_bench.py --path $PATH_ --m $M --iters $ITERS --reps $REPS \
        --devices $DEVICES --store-url tcp://127.0.0.1:$STORE --dist-port $PORT \
        --outdir $OUT > $OUT/rank0.log 2>&1 &
    local P0=$!
    RANK=1 $PY $DIR/op_bench.py --path $PATH_ --m $M --iters $ITERS --reps $REPS \
        --devices $DEVICES --store-url tcp://127.0.0.1:$STORE --dist-port $PORT \
        --outdir $OUT > $OUT/rank1.log 2>&1 &
    local P1=$!
    local rc=0
    # 单格看门狗：防协议/环境异常把 rank 拖成无限挂、进程树占设备。
    local waited=0
    while kill -0 $P0 2>/dev/null || kill -0 $P1 2>/dev/null; do
        if [ $waited -ge $CELL_TIMEOUT ]; then
            echo "   CELL TIMEOUT after ${CELL_TIMEOUT}s — killing"
            kill -TERM $P0 $P1 2>/dev/null; sleep 60
            kill -9 $P0 $P1 2>/dev/null
            pkill -9 -f "op_benc[h].*--path $PATH_ --m $M " 2>/dev/null
            rc=99
            break
        fi
        sleep 5
        waited=$((waited + 5))
    done
    wait $P0; R0=$?
    wait $P1; R1=$?
    if [ $rc -eq 0 ]; then
        if [ $R0 -eq 0 ] && [ $R1 -eq 0 ]; then
            echo "   DONE (both ranks exit 0)"
        else
            echo "   FAIL rank0=$R0 rank1=$R1 — tail rank logs:"
            tail -3 $OUT/rank0.log; tail -3 $OUT/rank1.log
        fi
    fi
    BASE_PORT=$((BASE_PORT + 2))
    sleep 5
}

PY=${PY:-python3}
for PATH_ in $PATHS; do
    for M in $MS; do
        run_cell $PATH_ $M
    done
done

echo ""
echo "op-bench matrix complete -> $OUTROOT"
$PY $DIR/make_report.py --root $OUTROOT --anchors $DIR/anchors_run11_run31.json || true
