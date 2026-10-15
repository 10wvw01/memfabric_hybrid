#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Huawei Technologies Co., Ltd. All Rights Reserved.
#
# 310P3 MemFabric mm_ar end-to-end serve benchmark launcher.
#
# Runs the stock/fused x eager/graph 4-config throughput matrix for the
# Qwen3.6-35B-A3B-w8a8 stack: input 4096 / output 2048 tokens, 50 random
# prompts, max concurrency 10 (matches the FULL_DECODE_ONLY graph capture
# size). Each combination starts its own `vllm serve` on the TP=2 die pair,
# waits for health, runs `vllm bench serve`, then stops the server.
#
# Usage:
#   bash tests/e2e/_310p/bench_memfabric_mm_ar_serve.sh [out_dir] [combos...]
#   combos default to "stock eager" "stock graph" "fused eager" "fused graph"
#
# Required environment (see tests/e2e/_310p/README.md):
#   - feature-on vllm_ascend_C build (VLLM_ASCEND_310P_ENABLE_MEMFABRIC_MM_AR=1)
#   - wgm-dev-310p MemFabric run package + deployed aicpu kernel

set -euo pipefail

OUT_DIR=${1:-/tmp/mm_ar_serve_bench}
shift || true
COMBOS=("$@")
if [ ${#COMBOS[@]} -eq 0 ]; then
    COMBOS=("stock eager" "stock graph" "fused eager" "fused graph")
fi

MODEL=${MEMFABRIC_MM_AR_E2E_MODEL:-/home/models/Qwen/Qwen3.6-35B-A3B-w8a8}
PORT=${MEMFABRIC_MM_AR_E2E_PORT:-8000}
STORE_PORT=${MEMFABRIC_MM_AR_E2E_STORE_PORT:-8581}
BATCH_BASEM_COUNT=${MEMFABRIC_MM_AR_E2E_BATCH_BASEM_COUNT:-1}
NUM_PROMPTS=${MEMFABRIC_MM_AR_E2E_NUM_PROMPTS:-50}
CONCURRENCY=${MEMFABRIC_MM_AR_E2E_CONCURRENCY:-10}
INPUT_LEN=${MEMFABRIC_MM_AR_E2E_INPUT_LEN:-4096}
OUTPUT_LEN=${MEMFABRIC_MM_AR_E2E_OUTPUT_LEN:-2048}
REPEAT=${MEMFABRIC_MM_AR_E2E_REPEAT:-1}

mkdir -p "$OUT_DIR"
cd "$(dirname "$0")/../../.."   # repo root

for combo in "${COMBOS[@]}"; do
    read -r MODE GRAPH <<< "$combo"
    TAG="${MODE}_${GRAPH}"
    LOG="$OUT_DIR/${TAG}.log"
    BENCH_LOG="$OUT_DIR/${TAG}_bench.log"

    export ASCEND_RT_VISIBLE_DEVICES=${MEMFABRIC_MM_AR_E2E_DEVICES:-0,1}
    export VLLM_WORKER_MULTIPROC_METHOD=spawn
    unset VLLM_ASCEND_310P_ENABLE_MEMFABRIC_MM_AR \
        VLLM_ASCEND_310P_MEMFABRIC_STORE_URL \
        VLLM_ASCEND_310P_MEMFABRIC_LOCAL_BYTES \
        VLLM_ASCEND_310P_MEMFABRIC_MM_AR_BATCH_BASEM_COUNT \
        VLLM_ASCEND_310P_MEMFABRIC_MM_AR_WARMUP_FALLBACK MF_SDMA_ORCH_JSON

    if [ -f /usr/local/memfabric_hybrid/set_env.sh ]; then
        # shellcheck disable=SC1091
        source /usr/local/memfabric_hybrid/set_env.sh
    fi

    EXTRA=(--enforce-eager)
    if [ "$GRAPH" = "graph" ]; then
        EXTRA=(--compilation-config '{"cudagraph_mode":"FULL_DECODE_ONLY","cudagraph_capture_sizes":[10]}')
    fi

    if [ "$MODE" = "fused" ]; then
        export VLLM_ASCEND_310P_ENABLE_MEMFABRIC_MM_AR=1
        export VLLM_ASCEND_310P_MEMFABRIC_STORE_URL="tcp://127.0.0.1:${STORE_PORT}"
        export VLLM_ASCEND_310P_MEMFABRIC_LOCAL_BYTES=$((96 * 1024 * 1024))
        export VLLM_ASCEND_310P_MEMFABRIC_MM_AR_BATCH_BASEM_COUNT="$BATCH_BASEM_COUNT"
        export VLLM_ASCEND_310P_MEMFABRIC_MM_AR_WARMUP_FALLBACK=1
        ORCH_JSON=/usr/local/memfabric_hybrid/1.2.0/aarch64-linux/hybm/aicpu_kernel/libmf_sdma_orch_v10.json
        [ -f "$ORCH_JSON" ] && export MF_SDMA_ORCH_JSON="$ORCH_JSON"
    fi

    echo "[$(date +%H:%M:%S)] starting $TAG server (log: $LOG)"
    # setsid: non-interactive bash does not put background jobs into their own
    # process group, so an explicit session is required for the later
    # group-wide TERM/KILL to reach EngineCore/Worker children.
    setsid vllm serve "$MODEL" \
        --host 127.0.0.1 --port "$PORT" --tensor-parallel-size 2 \
        --quantization ascend --served-model-name qwen3.6-35b-a3b-w8a8 \
        --trust-remote-code --dtype float16 --max-num-seqs 16 \
        --max-num-batched-tokens 4096 --max-model-len 8192 \
        --gpu-memory-utilization 0.90 \
        --no-enable-prefix-caching --enable-chunked-prefill \
        --mamba-ssm-cache-dtype float16 --mamba-cache-mode align \
        --additional-config '{"enable_flashcomm1":false,"enable_flashcomm2_parallel_size":0,"enable_prefill_mc2":false,"enable_fused_mc2":0,"ascend_compilation_config":{"fuse_norm_quant":true,"enable_npugraph_ex":false}}' \
        "${EXTRA[@]}" > "$LOG" 2>&1 &
    SERVE_PID=$!

    # Wait for health (model load can take tens of minutes on first load).
    DEADLINE=$((SECONDS + 2400))
    until curl -sf "http://127.0.0.1:${PORT}/health" > /dev/null 2>&1; do
        if ! kill -0 "$SERVE_PID" 2>/dev/null; then
            echo "[$TAG] server exited early; tail of $LOG:"; tail -40 "$LOG"; exit 1
        fi
        if [ "$SECONDS" -ge "$DEADLINE" ]; then
            echo "[$TAG] health timeout"; kill -TERM -- -"$SERVE_PID" 2>/dev/null || kill "$SERVE_PID"; exit 1
        fi
        sleep 5
    done
    echo "[$(date +%H:%M:%S)] $TAG healthy; enabled layers: $(grep -c 'Enable 310P3 TP=2 MemFabric unquantized mm_ar' "$LOG" || true)"

    for rep in $(seq 1 "$REPEAT"); do
        # Pin the tokenizer to the local model path (no network access) and
        # keep the benchmark fully offline.
        HF_HUB_OFFLINE=1 vllm bench serve --model qwen3.6-35b-a3b-w8a8 \
            --tokenizer "$MODEL" \
            --base-url "http://127.0.0.1:${PORT}" \
            --dataset-name random --random-input-len "$INPUT_LEN" \
            --random-output-len "$OUTPUT_LEN" \
            --num-prompts "$NUM_PROMPTS" --max-concurrency "$CONCURRENCY" \
            --save-result --result-dir "$OUT_DIR" \
            --result-filename "${TAG}_rep${rep}.json" \
            > >(tee -a "$BENCH_LOG") 2>&1 || echo "[$TAG] bench rep $rep failed"
    done

    echo "[$(date +%H:%M:%S)] stopping $TAG server"
    kill -TERM -- -"$SERVE_PID" 2>/dev/null || kill -TERM "$SERVE_PID" 2>/dev/null || true
    wait "$SERVE_PID" 2>/dev/null || true
    sleep 20
done

echo "all combos done; artifacts in $OUT_DIR"
