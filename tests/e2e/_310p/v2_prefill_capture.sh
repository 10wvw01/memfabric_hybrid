#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Huawei Technologies Co., Ltd. All Rights Reserved.
#
# v2_prefill_capture.sh — Stock/V2 prefill short-window profiler capture.
#
# Starts one `vllm serve` (confirmed Qwen3.6-35B-A3B-w8a8 eager parameters,
# identical to bench_memfabric_mm_ar_serve.sh) for the requested arm, warms
# it up with 4096-token/1-token requests (this also finishes V2 pool
# creation + first-call bit-exact verification OUTSIDE the capture window),
# then captures a short profiler window over the same request shape with
# the vLLM torch_npu profiler started via /start_profile. Both TP ranks
# export ascend_pt traces into the output directory.
#
# Usage:
#   bash tests/e2e/_310p/v2_prefill_capture.sh <arm:stock|v2> <label> \
#        [window_reqs] [warmup_reqs] [input_tokens]
# Artifacts: /tmp/opencode/v2_prefill_captures/<label>/
#   serve.log, requests.json, <rank>_ascend_pt/ (raw msprof export)

set -uo pipefail

ARM=${1:?arm must be stock or v2}
LABEL=${2:?label required}
WINDOW_REQS=${3:-5}
WARMUP_REQS=${4:-6}
INPUT_TOKENS=${5:-4096}

MODEL=${MEMFABRIC_MM_AR_E2E_MODEL:-/home/models/Qwen/Qwen3.6-35B-A3B-w8a8}
PORT=${MEMFABRIC_MM_AR_E2E_PORT:-8000}
STORE_PORT=${MEMFABRIC_MM_AR_E2E_STORE_PORT:-8581}
DEVICES=${MEMFABRIC_MM_AR_E2E_DEVICES:-0,1}
ORCH_JSON=$(ls -v /usr/local/memfabric_hybrid/*/aarch64-linux/hybm/aicpu_kernel/libmf_sdma_orch_v11.json 2>/dev/null | tail -1)
OUT=/tmp/opencode/v2_prefill_captures/$LABEL
mkdir -p "$OUT"

preflight_device_idle() {
    local chip dev bus aicore mem rest busy=0
    while read -r chip dev bus aicore mem rest; do
        [[ "$bus" =~ ^0000:[0-9a-f]{2}:[0-9a-f]{2}\.[0-9]$ ]] || continue
        case ",$DEVICES," in *",$dev,"*) ;; *) continue ;; esac
        mem=${mem%%/*}
        if [ "${aicore:-0}" -gt 10 ] || [ "${mem:-0}" -gt 4096 ]; then
            echo "[$(date +%H:%M:%S)] device $dev BUSY (aicore=${aicore}% mem=${mem}MB) - refusing to start"
            busy=1
        fi
    done < <(npu-smi info 2>/dev/null | tr -d '|' | tr -s ' ')
    return "$busy"
}

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

preflight_device_idle || { echo "[$LABEL] devices busy, aborting"; exit 1; }

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

if [ "$ARM" = "v2" ]; then
    export VLLM_ASCEND_310P_ENABLE_MEMFABRIC_MM_AR=1
    export VLLM_ASCEND_310P_MEMFABRIC_STORE_URL="tcp://127.0.0.1:${STORE_PORT}"
    export VLLM_ASCEND_310P_MEMFABRIC_LOCAL_BYTES=$((96 * 1024 * 1024))
    export VLLM_ASCEND_310P_MEMFABRIC_MM_AR_WARMUP_FALLBACK=1
    export VLLM_ASCEND_310P_MEMFABRIC_MM_AR_V2_SERVE=1
    export VLLM_ASCEND_310P_MEMFABRIC_V2_CHUNK_ROWS=1024
    export VLLM_ASCEND_310P_MEMFABRIC_V2_OUTPUT_SLOTS=4
    [ -n "$ORCH_JSON" ] && [ -f "$ORCH_JSON" ] && export MF_SDMA_ORCH_JSON="$ORCH_JSON"
fi

LOG="$OUT/serve.log"
echo "[$LABEL] starting $ARM serve (eager) $(date +%H:%M:%S)"
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
    --enforce-eager \
    --profiler-config "{\"profiler\":\"torch\",\"torch_profiler_dir\":\"$OUT\",\"torch_profiler_with_stack\":false}" \
    > "$LOG" 2>&1 &
SPID=$!

DEADLINE=$((SECONDS + 2400))
until curl -sf -m 3 "http://127.0.0.1:${PORT}/health" > /dev/null 2>&1; do
    if ! kill -0 "$SPID" 2>/dev/null; then
        echo "[$LABEL] serve died; tail of $LOG:"; tail -40 "$LOG"; exit 1
    fi
    [ "$SECONDS" -ge "$DEADLINE" ] && { echo "[$LABEL] health timeout"; kill -TERM -- -"$SPID" 2>/dev/null; exit 1; }
    sleep 5
done
echo "[$LABEL] healthy $(date +%H:%M:%S)"

python3 - "$PORT" "$WARMUP_REQS" "$WINDOW_REQS" "$INPUT_TOKENS" "$OUT/requests.json" <<'PYEOF'
import json, sys, time, urllib.request

port, warmup, window, n_tokens, out_json = (
    int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4]), sys.argv[5])
URL = f"http://127.0.0.1:{port}"
ids = [64] * n_tokens

def send(max_tokens=1):
    payload = json.dumps({"model": "qwen3.6-35b-a3b-w8a8", "prompt": ids,
                          "max_tokens": max_tokens, "ignore_eos": True}).encode()
    req = urllib.request.Request(URL + "/v1/completions", data=payload,
                                 headers={"Content-Type": "application/json"})
    t0 = time.perf_counter()
    body = urllib.request.urlopen(req, timeout=300).read()
    dt = time.perf_counter() - t0
    resp = json.loads(body)
    toks = resp["usage"]["completion_tokens"]
    return {"latency_s": round(dt, 4), "completion_tokens": toks}

def profile(cmd):
    urllib.request.urlopen(urllib.request.Request(
        URL + f"/{cmd}", data=b"", method="POST",
        headers={"Content-Type": "application/json"}), timeout=300).read()

warm = [send() for _ in range(warmup)]
profile("start_profile")
t_win = time.time()
win = [send() for _ in range(window)]
win_wall = time.time() - t_win
profile("stop_profile")

result = {"arm_requests": {"warmup": warm, "window": win,
                           "window_wall_s": round(win_wall, 3)}}
with open(out_json, "w") as f:
    json.dump(result, f, indent=2)
print(f"warmup latencies: {[r['latency_s'] for r in warm]}")
print(f"window latencies: {[r['latency_s'] for r in win]}")
print(f"window wall: {win_wall:.2f}s")
PYEOF
[ $? -eq 0 ] || { echo "[$LABEL] request phase failed"; kill -TERM -- -"$SPID" 2>/dev/null; exit 1; }

# Wait for both ranks' trace export (raw msprof dirs) after stop_profile.
for i in $(seq 1 60); do
    n=$(find "$OUT" -maxdepth 1 -name "*_ascend_pt" -type d | wc -l)
    [ "$n" -ge 2 ] && break
    sleep 5
done
echo "[$LABEL] trace dirs: $(find "$OUT" -maxdepth 1 -name '*_ascend_pt' -type d | wc -l)"

echo "[$LABEL] stopping serve"
kill -TERM -- -"$SPID" 2>/dev/null || kill -TERM "$SPID" 2>/dev/null || true
wait "$SPID" 2>/dev/null || true
wait_devices_drained
echo "[$LABEL] DONE $(date +%H:%M:%S)"
