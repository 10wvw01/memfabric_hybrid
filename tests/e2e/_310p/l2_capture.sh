#!/usr/bin/env bash
# l2_capture.sh — L2 serve profiler 采集（预检配方固化版）
# 用法: bash l2_capture.sh <mode:stock|fused> <eng:eager|graph> <label> [window]
# 产出: /tmp/opencode/l2_captures/<label>/  (含 rank0/rank1 ascend_pt 原始数据)
set -uo pipefail

MODE=$1; ENG=$2; LABEL=$3; WINDOW_S=${4:-10}
PORT=8000
OUT=/tmp/opencode/l2_captures/$LABEL
mkdir -p "$OUT"; rm -rf "$OUT"/*

# ---- serve 环境 ----
export ASCEND_RT_VISIBLE_DEVICES=0,1
export VLLM_WORKER_MULTIPROC_METHOD=spawn
source /usr/local/memfabric_hybrid/set_env.sh
unset VLLM_ASCEND_310P_ENABLE_MEMFABRIC_MM_AR VLLM_ASCEND_310P_MEMFABRIC_STORE_URL \
    VLLM_ASCEND_310P_MEMFABRIC_LOCAL_BYTES VLLM_ASCEND_310P_MEMFABRIC_MM_AR_BATCH_BASEM_COUNT \
    VLLM_ASCEND_310P_MEMFABRIC_MM_AR_WARMUP_FALLBACK MF_SDMA_ORCH_JSON
if [ "$MODE" = "fused" ]; then
    export VLLM_ASCEND_310P_ENABLE_MEMFABRIC_MM_AR=1
    export VLLM_ASCEND_310P_MEMFABRIC_STORE_URL=tcp://127.0.0.1:8581
    export VLLM_ASCEND_310P_MEMFABRIC_LOCAL_BYTES=$((96*1024*1024))
    export VLLM_ASCEND_310P_MEMFABRIC_MM_AR_BATCH_BASEM_COUNT=1
    export VLLM_ASCEND_310P_MEMFABRIC_MM_AR_WARMUP_FALLBACK=1
    export MF_SDMA_ORCH_JSON=$(ls -v /usr/local/memfabric_hybrid/*/aarch64-linux/hybm/aicpu_kernel/libmf_sdma_orch_v*.json | tail -1)
fi
EXTRA=(--enforce-eager)
[ "$ENG" = "graph" ] && EXTRA=(--compilation-config '{"cudagraph_mode":"FULL_DECODE_ONLY","cudagraph_capture_sizes":[10]}')

echo "[$LABEL] starting serve ($MODE/$ENG) $(date +%H:%M:%S)"
setsid vllm serve /home/models/Qwen/Qwen3.6-35B-A3B-w8a8 \
    --host 0.0.0.0 --port $PORT --tensor-parallel-size 2 \
    --quantization ascend --served-model-name qwen3.6-35b-a3b-w8a8 \
    --trust-remote-code --dtype float16 --max-num-seqs 16 \
    --max-num-batched-tokens 4096 --max-model-len 8192 \
    --gpu-memory-utilization 0.90 \
    --no-enable-prefix-caching --enable-chunked-prefill \
    --async-scheduling \
    --mamba-ssm-cache-dtype float16 --mamba-cache-mode align \
    --additional-config '{"enable_flashcomm1":false,"enable_flashcomm2_parallel_size":0,"enable_prefill_mc2":false,"enable_fused_mc2":0,"ascend_compilation_config":{"fuse_norm_quant":false,"enable_npugraph_ex":false}}' \
    --profiler-config "{\"profiler\":\"torch\",\"torch_profiler_dir\":\"$OUT\",\"torch_profiler_with_stack\":false}" \
    "${EXTRA[@]}" > "$OUT/serve.log" 2>&1 &
SPID=$!

for i in $(seq 1 90); do
    curl -sf -m 3 http://127.0.0.1:$PORT/health > /dev/null 2>&1 && break
    kill -0 $SPID 2>/dev/null || { echo "[$LABEL] serve died"; tail -20 "$OUT/serve.log"; exit 1; }
    sleep 10
done
curl -sf -m 3 http://127.0.0.1:$PORT/health > /dev/null || { echo "[$LABEL] health timeout"; exit 1; }
echo "[$LABEL] healthy $(date +%H:%M:%S)"
curl -s http://127.0.0.1:$PORT/v1/completions -H "Content-Type: application/json" \
    -d '{"model":"qwen3.6-35b-a3b-w8a8","prompt":"warmup","max_tokens":8,"ignore_eos":true}' > /dev/null

# ---- 采集1: decode 稳态 (并发10 模拟 L3 负载) ----
echo "[$LABEL] capture: decode window ${WINDOW_S}s"
curl -s -X POST http://127.0.0.1:$PORT/start_profile > /dev/null
for i in $(seq 1 10); do
    setsid curl -s -m 300 http://127.0.0.1:$PORT/v1/completions -H "Content-Type: application/json" \
        -d '{"model":"qwen3.6-35b-a3b-w8a8","prompt":"1 2 3 4 5 6 7 8 9 10 ","max_tokens":2048,"ignore_eos":true}' \
        > "$OUT/decode_$i.json" < /dev/null > /dev/null 2>&1 &
done
sleep $WINDOW_S
curl -s -m 300 -X POST http://127.0.0.1:$PORT/stop_profile > /dev/null
echo "[$LABEL] decode capture stopped $(date +%H:%M:%S)"
sleep 50   # 等导出 + 让剩余 decode 跑完

# ---- 采集2: prefill 波 (仅 fused eager 需要, 看大M direct 路径) ----
if [ "$MODE" = "fused" ] && [ "$ENG" = "eager" ]; then
    echo "[$LABEL] capture: prefill window"
    curl -s -X POST http://127.0.0.1:$PORT/start_profile > /dev/null
    python3 - <<PYEOF
import json, urllib.request
ids = [64] * 4095
payload = json.dumps({"model":"qwen3.6-35b-a3b-w8a8","prompt":ids,"max_tokens":8,"ignore_eos":True}).encode()
req = urllib.request.Request("http://127.0.0.1:$PORT/v1/completions", data=payload, headers={"Content-Type":"application/json"})
urllib.request.urlopen(req, timeout=120).read()
print("prefill req done")
PYEOF
    curl -s -m 300 -X POST http://127.0.0.1:$PORT/stop_profile > /dev/null
    echo "[$LABEL] prefill capture stopped $(date +%H:%M:%S)"
    sleep 50
fi

# ---- 停服 ----
kill -TERM -- -$SPID 2>/dev/null || kill -TERM $SPID 2>/dev/null
wait $SPID 2>/dev/null
sleep 15
pkill -9 -f "VLLM::" 2>/dev/null
echo "[$LABEL] DONE, artifacts in $OUT"
ls "$OUT" | head -5
