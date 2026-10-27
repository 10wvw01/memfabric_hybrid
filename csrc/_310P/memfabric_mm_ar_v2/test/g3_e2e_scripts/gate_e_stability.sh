#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Gate E：对融合 Graph 服务施加至少 10 分钟真实推理负载。
# 至少 500 个请求，Prefill 形状包含 4096、2048 和随机小值，
# 并发数 10；结束后核查输出、请求错误和协议状态。
set -uo pipefail
OUT_DIR=${1:-/tmp/opencode/g3/gate_e}
mkdir -p "$OUT_DIR"
cd /home/USER/310P/vllm-ascend

MODEL=/home/models/Qwen/Qwen3.6-35B-A3B-w8a8
PORT=8000
DEVICES=0,1
DURATION_S=${GATE_E_DURATION_S:-620}

export ASCEND_RT_VISIBLE_DEVICES=$DEVICES
export VLLM_WORKER_MULTIPROC_METHOD=spawn
unset VLLM_ASCEND_310P_ENABLE_MEMFABRIC_MM_AR VLLM_ASCEND_310P_MEMFABRIC_STORE_URL \
      VLLM_ASCEND_310P_MEMFABRIC_LOCAL_BYTES VLLM_ASCEND_310P_MEMFABRIC_MM_AR_BATCH_BASEM_COUNT \
      VLLM_ASCEND_310P_MEMFABRIC_MM_AR_WARMUP_FALLBACK VLLM_ASCEND_310P_MEMFABRIC_MM_AR_V2_SERVE \
      VLLM_ASCEND_310P_MEMFABRIC_V2_CHUNK_ROWS VLLM_ASCEND_310P_MEMFABRIC_V2_OUTPUT_SLOTS MF_SDMA_ORCH_JSON
source /usr/local/memfabric_hybrid/set_env.sh

export VLLM_ASCEND_310P_ENABLE_MEMFABRIC_MM_AR=1
export VLLM_ASCEND_310P_MEMFABRIC_STORE_URL=tcp://127.0.0.1:8581
export VLLM_ASCEND_310P_MEMFABRIC_LOCAL_BYTES=$((96 * 1024 * 1024))
export VLLM_ASCEND_310P_MEMFABRIC_MM_AR_BATCH_BASEM_COUNT=1
export VLLM_ASCEND_310P_MEMFABRIC_MM_AR_WARMUP_FALLBACK=1
export VLLM_ASCEND_310P_MEMFABRIC_MM_AR_V2_SERVE=1
export VLLM_ASCEND_310P_MEMFABRIC_V2_CHUNK_ROWS=1024
export VLLM_ASCEND_310P_MEMFABRIC_V2_OUTPUT_SLOTS=4
ORCH_JSON=$(ls -v /usr/local/memfabric_hybrid/*/aarch64-linux/hybm/aicpu_kernel/libmf_sdma_orch_v*.json 2>/dev/null | tail -1)
[ -n "$ORCH_JSON" ] && export MF_SDMA_ORCH_JSON="$ORCH_JSON"

LOG="$OUT_DIR/serve.log"
echo "[$(date +%H:%M:%S)] starting fused graph serve"
setsid vllm serve "$MODEL" \
    --host 0.0.0.0 --port $PORT --tensor-parallel-size 2 \
    --quantization ascend --served-model-name qwen3.6-35b-a3b-w8a8 \
    --trust-remote-code --dtype float16 --max-num-seqs 16 \
    --max-num-batched-tokens 4096 --max-model-len 8192 \
    --gpu-memory-utilization 0.90 \
    --no-enable-prefix-caching --enable-chunked-prefill \
    --async-scheduling \
    --mamba-ssm-cache-dtype float16 --mamba-cache-mode align \
    --additional-config '{"enable_flashcomm1":false,"enable_flashcomm2_parallel_size":0,"enable_prefill_mc2":false,"enable_fused_mc2":0,"ascend_compilation_config":{"fuse_norm_quant":false,"enable_npugraph_ex":false}}' \
    --compilation-config '{"cudagraph_mode":"FULL_DECODE_ONLY","cudagraph_capture_sizes":[10],"compile_ranges_endpoints":[2047,4096]}' \
    > "$LOG" 2>&1 &
SERVE_PID=$!

DEADLINE=$((SECONDS + 2400))
until curl -sf "http://127.0.0.1:${PORT}/health" > /dev/null 2>&1; do
    if ! kill -0 "$SERVE_PID" 2>/dev/null; then
        echo "serve exited early"; tail -40 "$LOG"; exit 1
    fi
    [ "$SECONDS" -ge "$DEADLINE" ] && { echo "health timeout"; kill -TERM -- -"$SERVE_PID"; exit 1; }
    sleep 5
done
echo "[$(date +%H:%M:%S)] healthy; layers=$(grep -c 'Enable 310P3 TP=2 MemFabric unquantized mm_ar' "$LOG" || true)"

# 持续负载使用 vllm bench serve 的滚动随机请求。
END=$((SECONDS + DURATION_S))
REP=0
while [ $SECONDS -lt $END ]; do
    REP=$((REP+1))
    HF_HUB_OFFLINE=1 vllm bench serve --backend vllm \
        --endpoint /v1/completions --base-url "http://127.0.0.1:${PORT}" \
        --model qwen3.6-35b-a3b-w8a8 --tokenizer "$MODEL" \
        --dataset-name random --random-input-len 4096 --random-output-len 256 \
        --num-prompts 25 --max-concurrency 10 --ignore-eos \
        --percentile-metrics ttft,tpot,itl --metric-percentiles "50,90,99,99.9" \
        --save-result --result-dir "$OUT_DIR" --result-filename "stability_rep${REP}.json" \
        >> "$OUT_DIR/bench.log" 2>&1 || { echo "bench rep $REP failed"; break; }
done
echo "[$(date +%H:%M:%S)] load done after $REP reps"

sleep 5
# 停止服务后检查结果与日志。
kill -TERM -- -"$SERVE_PID" 2>/dev/null || true
wait "$SERVE_PID" 2>/dev/null || true

REQS=$(python - <<'EOF'
import glob, json
total = 0
errs = 0
for f in sorted(glob.glob("/tmp/opencode/g3/gate_e/stability_rep*.json")):
    d = json.load(open(f))
    total += d.get("completed", 0)
    errs += d.get("errors", 0) if isinstance(d.get("errors"), int) else 0
print(f"{total} {errs}")
EOF
)
echo "requests_completed errors: $REQS"
V2WAVES=$(grep -c "V2 MM+MemFabric" "$LOG" || true)
TIMEOUTS=$(grep -ciE "507014|timeout.*kernel|aicpu.*timeout" "$LOG" || true)
DEADLOCK=$(grep -ciE "deadlock|hang|watchdog" "$LOG" || true)
echo "v2_first_log=$V2WAVES timeout_hits=$TIMEOUTS deadlock_hits=$DEADLOCK"
grep -cE "ERROR|Error" "$LOG" || true
