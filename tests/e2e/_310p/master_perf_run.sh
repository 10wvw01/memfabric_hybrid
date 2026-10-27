#!/usr/bin/env bash
# master_perf_run.sh — L1/L2/L3 全量重跑总控
# Phase 1: L1 算子级 (v11, q1/2/4 × M矩阵)          ~1.5h
# Phase 2: L3 e2e   (4组合 × 3轮, detailed)          ~4h
# Phase 3: L2 profiler 采集 (fused/stock × eager/graph) ~0.5h
# Phase 4: L2 离线解析 (analyse -> task_time.csv)
set -uo pipefail
BR=/home/USER/310P/bench_results
VA=/home/USER/310P/vllm-ascend
echo "=== MASTER START $(date +%H:%M:%S) ==="

# ---------- Phase 1: L1 ----------
echo "=== PHASE 1: L1 op-level v11 $(date +%H:%M:%S) ==="
source /usr/local/memfabric_hybrid/set_env.sh
export ASCEND_RT_VISIBLE_DEVICES=0,1
export VLLM_ASCEND_310P_ENABLE_MEMFABRIC_MM_AR=1
export VLLM_ASCEND_310P_MEMFABRIC_STORE_URL=tcp://127.0.0.1:8581
export VLLM_ASCEND_310P_MEMFABRIC_LOCAL_BYTES=$((96*1024*1024))
export MF_SDMA_ORCH_JSON=$(ls -v /usr/local/memfabric_hybrid/*/aarch64-linux/hybm/aicpu_kernel/libmf_sdma_orch_v*.json | tail -1)
mkdir -p $BR/op_level_v11
cd $VA
for q in 1 2 4; do
    torchrun --standalone --nproc-per-node=2 \
        tests/e2e/_310p/bench_memfabric_mm_ar_op.py \
        --rows 1 16 32 64 256 512 1024 2048 4096 \
        --iters 60 --batch-basem-count $q \
        --csv $BR/op_level_v11/mm_ar_op_v11_q${q}.csv >> /tmp/l1_op.log 2>&1 \
        && echo "L1 q$q OK" || echo "L1 q$q FAILED"
done
echo "=== PHASE 1 DONE $(date +%H:%M:%S) ==="

# ---------- Phase 2: L3 ----------
echo "=== PHASE 2: L3 e2e 4x3 $(date +%H:%M:%S) ==="
unset VLLM_ASCEND_310P_ENABLE_MEMFABRIC_MM_AR VLLM_ASCEND_310P_MEMFABRIC_STORE_URL \
    VLLM_ASCEND_310P_MEMFABRIC_LOCAL_BYTES VLLM_ASCEND_310P_MEMFABRIC_MM_AR_BATCH_BASEM_COUNT \
    VLLM_ASCEND_310P_MEMFABRIC_MM_AR_WARMUP_FALLBACK MF_SDMA_ORCH_JSON
export MEMFABRIC_MM_AR_E2E_REPEAT=3
bash $VA/tests/e2e/_310p/bench_memfabric_mm_ar_serve.sh $BR/v11_l3 \
    "stock graph" "fused graph" "stock eager" "fused eager" >> /tmp/l3_bench.log 2>&1 \
    && echo "L3 OK" || echo "L3 FAILED (see /tmp/l3_bench.log)"
echo "=== PHASE 2 DONE $(date +%H:%M:%S) ==="

# ---------- Phase 3: L2 captures ----------
echo "=== PHASE 3: L2 captures $(date +%H:%M:%S) ==="
for spec in "fused eager fused_eager" "stock eager stock_eager" "fused graph fused_graph"; do
    read -r M E L <<< "$spec"
    bash $VA/tests/e2e/_310p/l2_capture.sh $M $E $L 10 >> /tmp/l2_capture.log 2>&1 \
        && echo "L2 $L OK" || echo "L2 $L FAILED"
    sleep 10
done
echo "=== PHASE 3 DONE $(date +%H:%M:%S) ==="

# ---------- Phase 4: L2 离线解析 ----------
echo "=== PHASE 4: L2 offline analyse $(date +%H:%M:%S) ==="
for L in fused_eager stock_eager fused_graph; do
    for RD in /tmp/opencode/l2_captures/$L/dp0_pp0_tp0*_ascend_pt; do
        [ -d "$RD" ] || continue
        ASCEND_RT_VISIBLE_DEVICES=0 python3 -c "
from torch_npu.profiler.profiler import analyse
analyse('$RD')" >> /tmp/l2_analyse.log 2>&1 \
            && echo "analyse $RD OK" || echo "analyse $RD FAILED"
    done
done
echo "=== PHASE 4 DONE $(date +%H:%M:%S) ==="
echo "=== MASTER ALL_DONE $(date +%H:%M:%S) ==="
