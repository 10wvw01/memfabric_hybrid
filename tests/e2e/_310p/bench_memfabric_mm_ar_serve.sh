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
DFLASH_ENABLED=${MEMFABRIC_MM_AR_E2E_DFLASH:-0}
DFLASH_MODEL=${MEMFABRIC_MM_AR_E2E_DFLASH_MODEL:-/home/models/Qwen/Qwen3.6-35B-A3B-DFlash}
DFLASH_TOKENS=${MEMFABRIC_MM_AR_E2E_DFLASH_TOKENS:-7}
ORCH_JSON=$(ls -v /usr/local/memfabric_hybrid/*/aarch64-linux/hybm/aicpu_kernel/libmf_sdma_orch_v*.json 2>/dev/null | tail -1)
DEVICES=${MEMFABRIC_MM_AR_E2E_DEVICES:-0,1}
V2_SERVE=${MEMFABRIC_MM_AR_E2E_V2:-0}

# Refuse to start on a device that already runs someone else's workload.
# Only rows whose 3rd field looks like a PCI bus id are device rows (this
# filters out the header and the bottom "Process" table, whose rows carry
# PIDs there). Thresholds: >10% AICore or >4GB memory = busy.
preflight_device_idle() {
    local chip dev bus aicore mem rest busy=0
    while read -r chip dev bus aicore mem rest; do
        [[ "$bus" =~ ^0000:[0-9a-f]{2}:[0-9a-f]{2}\.[0-9]$ ]] || continue
        case ",$DEVICES," in *",$dev,"*) ;; *) continue ;; esac
        mem=${mem%%/*}
        if [ "${aicore:-0}" -gt 10 ] || [ "${mem:-0}" -gt 4096 ]; then
            echo "[$(date +%H:%M:%S)] device $dev is BUSY (aicore=${aicore}% mem=${mem}MB) - refusing to start"
            busy=1
        fi
    done < <(npu-smi info 2>/dev/null | tr -d '|' | tr -s ' ')
    return "$busy"
}

mkdir -p "$OUT_DIR"
cd "$(dirname "$0")/../../.."   # repo root

# Wait until every target device's memory returns to its idle baseline
# (<2.5GB).  The NPU driver releases the previous serve's memory
# asynchronously, so a fixed sleep can race the next serve's startup
# free-memory check (observed: 37.61GB free vs 38.92GB required, off by
# only 1.3GB of leftover).
wait_devices_drained() {
    local deadline=$((SECONDS + 300)) chip dev bus aicore mem rest
    while [ $SECONDS -lt $deadline ]; do
        local dirty=0
        while read -r chip dev bus aicore mem rest; do
            [[ "$bus" =~ ^0000:[0-9a-f]{2}:[0-9a-f]{2}\.[0-9]$ ]] || continue
            case ",$DEVICES," in *",$dev,"*) ;; *) continue ;; esac
            mem=${mem%%/*}
            [ "${mem:-0}" -gt 2500 ] && dirty=1
        done < <(npu-smi info 2>/dev/null | tr -d '|' | tr -s ' ')
        [ "$dirty" = 0 ] && return 0
        sleep 5
    done
    echo "[$(date +%H:%M:%S)] warning: devices still not drained after 300s"
}

combo_idx=$(( ${MEMFABRIC_MM_AR_E2E_CYCLE_START:-1} - 1 ))
for combo in "${COMBOS[@]}"; do
    combo_idx=$((combo_idx + 1))
    read -r MODE GRAPH <<< "$combo"
    TAG="${MODE}_${GRAPH}"
    if [ "$MODE" = "fused" ] && [ "$BATCH_BASEM_COUNT" != "1" ]; then
        TAG="fusedq${BATCH_BASEM_COUNT}_${GRAPH}"
    fi
    # Optional per-combo index suffix so repeated serves of the same combo
    # (serve-level A/B replication) do not overwrite each other's artifacts.
    if [ "${MEMFABRIC_MM_AR_E2E_CYCLE_TAG:-0}" = "1" ]; then
        TAG="${TAG}_c${combo_idx}"
    fi
    LOG="$OUT_DIR/${TAG}.log"
    BENCH_LOG="$OUT_DIR/${TAG}_bench.log"

    export ASCEND_RT_VISIBLE_DEVICES=$DEVICES
    export VLLM_WORKER_MULTIPROC_METHOD=spawn
    unset VLLM_ASCEND_310P_ENABLE_MEMFABRIC_MM_AR \
        VLLM_ASCEND_310P_MEMFABRIC_STORE_URL \
        VLLM_ASCEND_310P_MEMFABRIC_LOCAL_BYTES \
        VLLM_ASCEND_310P_MEMFABRIC_MM_AR_BATCH_BASEM_COUNT \
        VLLM_ASCEND_310P_MEMFABRIC_MM_AR_WARMUP_FALLBACK \
        VLLM_ASCEND_310P_MEMFABRIC_MM_AR_V2_SERVE \
        VLLM_ASCEND_310P_MEMFABRIC_V2_CHUNK_ROWS \
        VLLM_ASCEND_310P_MEMFABRIC_V2_OUTPUT_SLOTS MF_SDMA_ORCH_JSON

    if [ -f /usr/local/memfabric_hybrid/set_env.sh ]; then
        # shellcheck disable=SC1091
        source /usr/local/memfabric_hybrid/set_env.sh
    fi

    EXTRA=(--enforce-eager)
    if [ "$GRAPH" = "graph" ]; then
        # compile_ranges_endpoints split at MIN_FUSED_M-1: isolates stock/fused
        # compile-cache artifacts (cache key ignores the plugin module - arms
        # with identical configs silently share artifacts, found 2026-10-19)
        # and prepares the graph-mode V2 fix (report 4.6; not yet effective).
        EXTRA=(--compilation-config '{"cudagraph_mode":"FULL_DECODE_ONLY","cudagraph_capture_sizes":[10],"compile_ranges_endpoints":[2047,4096]}')
    fi

    SPEC_ARGS=()
    if [ "$DFLASH_ENABLED" = "1" ]; then
        SPEC_ARGS=(--speculative-config "{\"method\":\"dflash\",\"model\":\"$DFLASH_MODEL\",\"num_speculative_tokens\":$DFLASH_TOKENS}")
    fi

    if [ "$MODE" = "fused" ]; then
        export VLLM_ASCEND_310P_ENABLE_MEMFABRIC_MM_AR=1
        export VLLM_ASCEND_310P_MEMFABRIC_STORE_URL="tcp://127.0.0.1:${STORE_PORT}"
        export VLLM_ASCEND_310P_MEMFABRIC_LOCAL_BYTES=$((96 * 1024 * 1024))
        export VLLM_ASCEND_310P_MEMFABRIC_MM_AR_BATCH_BASEM_COUNT="$BATCH_BASEM_COUNT"
        export VLLM_ASCEND_310P_MEMFABRIC_MM_AR_WARMUP_FALLBACK=1
        if [ "$V2_SERVE" = "1" ]; then
            export VLLM_ASCEND_310P_MEMFABRIC_MM_AR_V2_SERVE=1
            export VLLM_ASCEND_310P_MEMFABRIC_V2_CHUNK_ROWS="${MEMFABRIC_MM_AR_E2E_V2_CHUNK_ROWS:-1024}"
            export VLLM_ASCEND_310P_MEMFABRIC_V2_OUTPUT_SLOTS="${MEMFABRIC_MM_AR_E2E_V2_OUTPUT_SLOTS:-4}"
        fi
        [ -n "$ORCH_JSON" ] && [ -f "$ORCH_JSON" ] && export MF_SDMA_ORCH_JSON="$ORCH_JSON"
    fi

    preflight_device_idle || { echo "[$TAG] target devices busy, aborting"; exit 1; }

    echo "[$(date +%H:%M:%S)] starting $TAG server (log: $LOG)"
    # setsid: non-interactive bash does not put background jobs into their own
    # process group, so an explicit session is required for the later
    # group-wide TERM/KILL to reach EngineCore/Worker children.
    setsid vllm serve "$MODEL" \
        --host 0.0.0.0 --port "$PORT" --tensor-parallel-size 2 \
        --quantization ascend --served-model-name qwen3.6-35b-a3b-w8a8 \
        --trust-remote-code --dtype float16 --max-num-seqs 16 \
        --max-num-batched-tokens 4096 --max-model-len 8192 \
        --gpu-memory-utilization 0.90 \
        --no-enable-prefix-caching --enable-chunked-prefill \
        --async-scheduling \
        --mamba-ssm-cache-dtype float16 --mamba-cache-mode align \
        --additional-config '{"enable_flashcomm1":false,"enable_flashcomm2_parallel_size":0,"enable_prefill_mc2":false,"enable_fused_mc2":0,"ascend_compilation_config":{"fuse_norm_quant":false,"enable_npugraph_ex":false}}' \
        "${SPEC_ARGS[@]}" \
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
        HF_HUB_OFFLINE=1 vllm bench serve --backend vllm \
            --endpoint /v1/completions \
            --base-url "http://127.0.0.1:${PORT}" \
            --model qwen3.6-35b-a3b-w8a8 \
            --tokenizer "$MODEL" \
            --dataset-name random --random-input-len "$INPUT_LEN" \
            --random-output-len "$OUTPUT_LEN" \
            --num-prompts "$NUM_PROMPTS" --max-concurrency "$CONCURRENCY" \
            --ignore-eos \
            --num-warmups 5 \
            --percentile-metrics ttft,tpot,itl \
            --metric-percentiles "50,90,99,99.9" \
            --save-result --save-detailed --result-dir "$OUT_DIR" \
            --result-filename "${TAG}_rep${rep}_run.json" \
            > >(tee -a "$BENCH_LOG") 2>&1 || {
                echo "[$TAG] bench rep $rep failed" | tee -a "$BENCH_LOG"
                if [ "$V2_SERVE" = "1" ]; then
                    kill -TERM -- -"$SERVE_PID" 2>/dev/null || true
                    exit 1
                fi
            }
    done

    echo "[$(date +%H:%M:%S)] stopping $TAG server"
    kill -TERM -- -"$SERVE_PID" 2>/dev/null || kill -TERM "$SERVE_PID" 2>/dev/null || true
    wait "$SERVE_PID" 2>/dev/null || true
    wait_devices_drained
done

echo "all combos done; artifacts in $OUT_DIR"
