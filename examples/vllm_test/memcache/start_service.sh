#!/bin/bash
# start_service.sh — Start MetaService + vLLM with MemCache
#
# Merges: start_bg.sh, start_memcache_vllm.sh
#
# Usage:
#   bash start_service.sh [options]
#
# Options:
#   --device-ids <a,b>          NPU visible devices (default: 6,7)
#   --tp <N>                    tensor parallel size (default: 2)
#   --port <PORT>               vLLM server port (default: 4077)
#   --gpu-mem-util <float>      GPU memory utilization (default: 0.80)
#   --model <PATH>              model path (default: /home/models/Qwen3.6-35B-A3B-w8a8)
#   --served-model-name <NAME>  served model name (default: Qwen3.6-35B-A3B)
#   --memcache-dir <DIR>        memcache repo dir (default: /home/l30023043/memcache)
#   --config <PATH>             MMC_LOCAL_CONFIG_PATH (default: cases/config/mmc-local-integration.conf)
#   --detach                    run services detached with setsid (survives shell exit)
#   --meta-only                 start only MetaService (skip vLLM)
#   --help                      show help
#
# Examples:
#   # Foreground (default): MetaService + vLLM, waits for health check
#   bash start_service.sh --device-ids 6,7
#
#   # Detached: services survive shell exit
#   bash start_service.sh --device-ids 6,7 --detach
#
#   # 4-rank TP=4
#   bash start_service.sh --device-ids 0,1,2,3 --tp 4

set -euo pipefail

readonly PKG_DIR="$(cd "$(dirname "$0")" && pwd)"

# ============================================================================
# Defaults
# ============================================================================
DEVICE_IDS="6,7"
TP_SIZE="2"
VLLM_PORT="4077"
GPU_MEM_UTIL="0.80"
MODEL_PATH="/home/models/Qwen3.6-35B-A3B-w8a8"
SERVED_MODEL_NAME="Qwen3.6-35B-A3B"
MEMCACHE_DIR="/home/l30023043/memcache"
LOCAL_CONFIG="$PKG_DIR/cases/config/mmc-local-integration.conf"
DETACH=0
META_ONLY=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --device-ids)       DEVICE_IDS="$2"; shift 2 ;;
        --tp)               TP_SIZE="$2"; shift 2 ;;
        --port)             VLLM_PORT="$2"; shift 2 ;;
        --gpu-mem-util)     GPU_MEM_UTIL="$2"; shift 2 ;;
        --model)            MODEL_PATH="$2"; shift 2 ;;
        --served-model-name) SERVED_MODEL_NAME="$2"; shift 2 ;;
        --memcache-dir)     MEMCACHE_DIR="$2"; shift 2 ;;
        --config)           LOCAL_CONFIG="$2"; shift 2 ;;
        --detach)           DETACH=1; shift ;;
        --meta-only)        META_ONLY=1; shift ;;
        --help|-h)          sed -n '2,30p' "$0"; exit 0 ;;
        *) echo "[ERROR] 未知参数: $1"; exit 1 ;;
    esac
done

# ============================================================================
# Environment setup
# ============================================================================
MMC_LIB=$(python3 -c "import memcache_hybrid, os; print(os.path.dirname(memcache_hybrid.__file__)+'/lib')" 2>/dev/null || echo "")
MF_LIB=$(python3 -c "import memfabric_hybrid, os; print(os.path.dirname(memfabric_hybrid.__file__)+'/lib')" 2>/dev/null || echo "")
export ASCEND_HOME_PATH="${ASCEND_HOME_PATH:-/usr/local/Ascend/cann-9.1.0}"
export LD_LIBRARY_PATH="/usr/local/Ascend/driver/lib64:${ASCEND_HOME_PATH}/lib64:${MEMCACHE_DIR}/output/memcache/lib64:${MMC_LIB}:${MF_LIB}:${LD_LIBRARY_PATH:-}"
export MMC_MEMCACHE_DIR="$MEMCACHE_DIR"
export MMC_META_CONFIG_PATH="$MEMCACHE_DIR/config/mmc-meta.conf"
export MMC_LOCAL_CONFIG_PATH="$LOCAL_CONFIG"
export ASCEND_KERNEL_META_DIR="/tmp/ascend_kernel_meta"
export PYTORCH_NPU_ALLOC_CONF=expandable_segments:True
export ASCEND_RT_VISIBLE_DEVICES="$DEVICE_IDS"

# ============================================================================
# Kill old processes
# ============================================================================
echo "[INFO] Stopping stale processes..."
pkill -9 -f mmc_meta_service 2>/dev/null || true
pkill -9 -f "vllm serve" 2>/dev/null || true
pkill -9 -f "VLLM::" 2>/dev/null || true
sleep 3

# ============================================================================
# Start MetaService
# ============================================================================
echo "[INFO] Starting MetaService..."
META_BIN="$MEMCACHE_DIR/output/memcache/bin/mmc_meta_service"
if [[ "$DETACH" -eq 1 ]]; then
    setsid "$META_BIN" > /tmp/mmc_meta.log 2>&1 &
else
    "$META_BIN" > /tmp/mmc_meta.log 2>&1 &
fi
META_PID=$!
sleep 3
if kill -0 "$META_PID" 2>/dev/null; then
    echo "[INFO] MetaService started (PID: $META_PID)"
else
    echo "[ERROR] MetaService failed to start"
    tail -20 /tmp/mmc_meta.log
    exit 1
fi

if [[ "$META_ONLY" -eq 1 ]]; then
    echo "[INFO] --meta-only: skipping vLLM"
    exit 0
fi

# ============================================================================
# Start vLLM with MemCache
# ============================================================================
echo "[INFO] Starting vLLM (TP=$TP_SIZE, port=$VLLM_PORT)..."
VLLM_CMD="vllm serve $MODEL_PATH \
  --host 127.0.0.1 --port $VLLM_PORT \
  --tensor-parallel-size $TP_SIZE --quantization ascend \
  --served-model-name $SERVED_MODEL_NAME --trust-remote-code \
  --dtype float16 --max-num-seqs 10 --max-num-batched-tokens 4096 \
  --max-model-len 8192 --gpu-memory-utilization $GPU_MEM_UTIL \
  --enforce-eager --enable-chunked-prefill \
  --mamba-ssm-cache-dtype float16 --mamba-cache-mode align \
  --additional-config '{\"enable_flashcomm1\":false,\"enable_flashcomm2_parallel_size\":0,\"enable_prefill_mc2\":false,\"enable_fused_mc2\":0,\"ascend_compilation_config\":{\"fuse_norm_quant\":false,\"enable_npugraph_ex\":false}}' \
  --no-enable-prefix-caching \
  --prefix-match-unit 128 \
  --kv-transfer-config '{\"kv_connector\":\"AscendStoreConnector\",\"kv_role\":\"kv_both\",\"kv_connector_extra_config\":{\"backend\":\"memcache\"}}'"

if [[ "$DETACH" -eq 1 ]]; then
    setsid bash -c "$VLLM_CMD" > /tmp/vllm_memcache.log 2>&1 &
    VLLM_PID=$!
    echo "[INFO] vLLM started detached (PID: $VLLM_PID)"
    # Detached mode: simple health check, no death detection
    for i in $(seq 1 60); do
        HTTP_CODE=$(curl -s -o /dev/null -w "%{http_code}" "http://127.0.0.1:$VLLM_PORT/health" 2>/dev/null || echo "000")
        if [[ "$HTTP_CODE" == "200" ]]; then
            echo "[INFO] vLLM ready at attempt $i"
            exit 0
        fi
        sleep 10
    done
    echo "[ERROR] vLLM not ready in 600s"
    exit 1
else
    bash -c "$VLLM_CMD" > /tmp/vllm_memcache.log 2>&1 &
    VLLM_PID=$!
    echo "[INFO] vLLM started (PID: $VLLM_PID)"
    # Foreground mode: with vLLM death detection
    for i in $(seq 1 60); do
        HTTP_CODE=$(curl -s -o /dev/null -w "%{http_code}" "http://127.0.0.1:$VLLM_PORT/health" 2>/dev/null || echo "000")
        if [[ "$HTTP_CODE" == "200" ]]; then
            echo "[INFO] vLLM ready at attempt $i"
            exit 0
        fi
        if ! ps -p "$VLLM_PID" > /dev/null 2>&1; then
            echo "[ERROR] vLLM died at attempt $i"
            tail -30 /tmp/vllm_memcache.log
            exit 1
        fi
        sleep 10
    done
    echo "[ERROR] vLLM not ready in 600s"
    kill "$VLLM_PID" 2>/dev/null || true
    exit 1
fi
