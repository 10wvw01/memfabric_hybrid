#!/bin/bash
# M2 functional matrix launcher: {ar, iar} x M{...} double-rank bit-exact.
# Device discipline: physical 4/5 only. One cell = 2 rank processes; a cell
# passes only if BOTH ranks exit 0 (golden comparison is per-rank).
#
# Env overrides: OPS, MS, DEVICES, PORT, OUTDIR
set -u
DIR=$(cd "$(dirname "$0")" && pwd)
PRD=$(cd "$DIR/../.." && pwd)
GA=/usr/local/Ascend/cann-9.1.0
MFOUT=/home/w00804146/310P_2/memfabric_hybrid/output
PKGS=$PRD
PY=${PYTHON:-python3}

OPS=${OPS:-"ar iar"}
MS=${MS:-"1280 3840 4096"}
DEVICES=${DEVICES:-4,5}
PORT=${PORT:-8719}
OUTDIR=${OUTDIR:-/tmp/opencode/prd_m2}
mkdir -p "$OUTDIR"

unset ASCEND_RT_VISIBLE_DEVICES
source $GA/set_env.sh

pkg_of() {  # $1 = op -> custom opp vendor path
    echo "$PKGS/smem_mm_$1_310p3/build_out/_CPack_Packages/Linux/External/custom_opp_ubuntu_aarch64.run/packages/vendors/smem_mm_$1_310p3"
}

run_cell() {  # $1 = op, $2 = M
    local OP=$1 M=$2 PKG LOG URL P0 P1 R0 R1
    PKG=$(pkg_of $OP)
    LOG=$OUTDIR/m2_${OP}_m${M}.log
    URL=tcp://127.0.0.1:$PORT
    : > "$LOG"
    echo "== M2 op=$OP M=$M (devices $DEVICES)" | tee -a "$OUTDIR/summary.txt"
    export ASCEND_CUSTOM_OPP_PATH=$PKG
    export MF_SDMA_ORCH_DEPLOY_HELPER=/home/w00804146/310P_2/memfabric_hybrid/tools/aicpu_kernel_deploy/orch_deploy_helper
    export MF_SDMA_ORCH_JSON=$MFOUT/hybm/aicpu_kernel/libmf_sdma_orch_v28.json
    export MEMFABRIC_HYBRID_HOME_PATH=$MFOUT
    export MEMFABRIC_HYBRID_EXTEND_LIB_PATH=$MFOUT/hybm/lib64
    export MIXFABRIC_SDK_OUT=$MFOUT
    export LD_LIBRARY_PATH=$MFOUT/smem/lib64:$MFOUT/hybm/lib64:$MFOUT/acc_links/lib64:$PKG/op_api/lib:${LD_LIBRARY_PATH:-}
    export ASCEND_GLOBAL_LOG_LEVEL=0
    RANK=0 $PY $DIR/m2_xops_test.py --devices $DEVICES --m $M --op $OP --store-url $URL >> "$LOG" 2>&1 &
    P0=$!
    RANK=1 $PY $DIR/m2_xops_test.py --devices $DEVICES --m $M --op $OP --store-url $URL >> "$LOG" 2>&1 &
    P1=$!
    wait $P0; R0=$?
    wait $P1; R1=$?
    if [ $R0 -eq 0 ] && [ $R1 -eq 0 ]; then
        echo "   PASS (both ranks exit 0)" | tee -a "$OUTDIR/summary.txt"
    else
        echo "   FAIL (rank0=$R0 rank1=$R1) — see $LOG" | tee -a "$OUTDIR/summary.txt"
        tail -5 "$LOG"
    fi
    PORT=$((PORT + 1))
    sleep 3
}

for OP in $OPS; do
    for M in $MS; do
        run_cell $OP $M
    done
done

echo ""
echo "M2 matrix done -> $OUTDIR/summary.txt"
