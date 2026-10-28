#!/usr/bin/env bash
# Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
# MemFabric_Hybrid is licensed under Mulan PSL v2.
# run_gate_e.sh — Gate E 507014 复现会话运行器（w00930648 容器适配版）
#
# 与原版(g1d_run.sh + gate_e_eager.sh)的差异（全部显式列出，保持可审计）:
#   1. 设备 dev2,3（原版锁 0,1）——依赖 cherry-pick 5802f550 SetAccess 物理号修复
#   2. 模型 /home/data/Qwen/...（本容器可见路径）
#   3. store/serve 端口 18581/8010（避开同事容器 8581/8000）
#   4. 采样不用 npu-smi（卡4死锁会挂全局命令）——改用日志/进程采样
#   5. V2 设备库/插件二进制 = D1R4 原版（SHA 已核验）; Host SO = diag 分支重建+物理号修复
#   6. VARIANT=eager(原故障配置) | graph(同事新参考: FULL_DECODE_ONLY)
#
# 用法: bash run_gate_e.sh <会话编号> [VARIANT=eager] [DURATION_S=620]
set -uo pipefail

SESSION="${1:?需要会话编号}"
VARIANT="${2:-eager}"
DURATION_S="${3:-620}"

V2_TREE=/tmp/opencode/vllm-ascend-diag
PYINJ=/tmp/opencode/pyinj
VENV=/tmp/opencode/venv-repro
MF_PREFIX=/usr/local/memfabric_hybrid/latest
ORCH_JSON=$MF_PREFIX/aarch64-linux/hybm/aicpu_kernel/libmf_sdma_orch_v12.json
MODEL=/home/data/Qwen/Qwen3.6-35B-A3B-w8a8
STORE_URL=tcp://127.0.0.1:18581
PORT=8010
DEVICES=6,7

OUT_ROOT=/tmp/opencode/repro/sessions
OUT_DIR="$OUT_ROOT/${SESSION}_${VARIANT}"
mkdir -p "$OUT_DIR"
LOG="$OUT_DIR/serve.log"

echo "[$(date +%T)] session=$SESSION variant=$VARIANT dur=${DURATION_S}s out=$OUT_DIR"

# ---------- 环境组装 ----------
source /usr/local/Ascend/cann-9.1.0/bin/setenv.bash
source /usr/local/memfabric_hybrid/set_env.sh
export PATH="$VENV/bin:$PATH"
export ASCEND_RT_VISIBLE_DEVICES=$DEVICES
export VLLM_WORKER_MULTIPROC_METHOD=spawn
export MF_SDMA_ORCH_JSON="$ORCH_JSON"
export MF_SDMA_ORCH_DIAG=1
export MF_SDMA_ORCH_G1C_TRACE=1
export MF_SDMA_ORCH_G1D_TRACE=1
export MF310P_V2_G1C_TRACE=1
export MF310P_V2_G1D_TRACE=1
export PYTHONPATH="$PYINJ:$V2_TREE:${PYTHONPATH:-}"
export VLLM_ASCEND_310P_ENABLE_MEMFABRIC_MM_AR=1
export VLLM_ASCEND_310P_MEMFABRIC_STORE_URL=$STORE_URL
export VLLM_ASCEND_310P_MEMFABRIC_LOCAL_BYTES=$((96 * 1024 * 1024))
export VLLM_ASCEND_310P_MEMFABRIC_MM_AR_BATCH_BASEM_COUNT=1
export VLLM_ASCEND_310P_MEMFABRIC_MM_AR_WARMUP_FALLBACK=1
export VLLM_ASCEND_310P_MEMFABRIC_MM_AR_V2_SERVE=1
export VLLM_ASCEND_310P_MEMFABRIC_V2_CHUNK_ROWS=1024
export VLLM_ASCEND_310P_MEMFABRIC_V2_OUTPUT_SLOTS=4

# ---------- provenance ----------
{
  date -u '+utc=%Y-%m-%dT%H:%M:%SZ'
  echo "session=$SESSION variant=$VARIANT devices=$DEVICES"
  echo "v2_tree=$(git -C $V2_TREE rev-parse HEAD)"
  echo "mf_tree=$(git -C /tmp/opencode/mf_diag rev-parse HEAD) + 5c5562bf(physical-fix)"
  sha256sum "$ORCH_JSON" \
      "$MF_PREFIX/aarch64-linux/lib64/libmf_hybm_core.so" \
      "$VENV/lib/python3.12/site-packages/memfabric_hybrid/lib/libmf_hybm_core.so" \
      "$V2_TREE/vllm_ascend/libmf310p_v2_trial_device_g1d.so" \
      "$V2_TREE/vllm_ascend/vllm_ascend_C.cpython-312-aarch64-linux-gnu.so"
  echo "expect: v12json=90172726... v2dev=d9a6de66... plugin=bcbb4f67..."
  echo "model=$MODEL"
} > "$OUT_DIR/provenance.txt" 2>&1

# ---------- 设备侧 v12 预部署(独立进程 KFC, 规避 torch_npu 错误闩锁) ----------
echo "[$(date +%T)] pre-deploy v12 kernel to card(dev6)"
ASCEND_RT_VISIBLE_DEVICES=6 "$MF_PREFIX/aarch64-linux/lib64/orch_deploy_helper" 0 "$ORCH_JSON" \
    > "$OUT_DIR/deploy.log" 2>&1
DEPLOY_RC=$?
echo "[$(date +%T)] deploy rc=$DEPLOY_RC (log: deploy.log)"
if [ $DEPLOY_RC -ne 0 ] && [ $DEPLOY_RC -ne 3 ]; then
  echo "FATAL: deploy failed rc=$DEPLOY_RC"; tail -20 "$OUT_DIR/deploy.log"; exit 1
fi

# ---------- serve 启动 ----------
EXTRA_ARGS=""
BENCH_OUT=256
BENCH_N=25
if [ "$VARIANT" = "graph" ]; then
  EXTRA_ARGS='--compilation-config {"cudagraph_mode":"FULL_DECODE_ONLY","cudagraph_capture_sizes":[10]}'
  BENCH_OUT=2048
  BENCH_N=50
else
  EXTRA_ARGS="--enforce-eager"
fi

echo "[$(date +%T)] starting serve ($VARIANT)"
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
    $EXTRA_ARGS \
    > "$LOG" 2>&1 &
SERVE_PID=$!

cleanup() {
  kill -TERM -- -"$SERVE_PID" 2>/dev/null || kill -TERM "$SERVE_PID" 2>/dev/null || true
}
trap cleanup EXIT

# ---------- 健康等待 ----------
DEADLINE=$((SECONDS + 2400))
until curl -sf "http://127.0.0.1:${PORT}/health" > /dev/null 2>&1; do
  if ! kill -0 "$SERVE_PID" 2>/dev/null; then
    echo "FATAL: serve exited early"; tail -40 "$LOG"; exit 1
  fi
  if grep -q "507014" "$LOG" 2>/dev/null; then
    echo "FAULT-AT-STARTUP: 507014 during boot"; break
  fi
  [ "$SECONDS" -ge "$DEADLINE" ] && { echo "FATAL: health timeout"; exit 1; }
  sleep 5
done
LAYERS=$(grep -c 'Enable 310P3 TP=2 MemFabric unquantized mm_ar' "$LOG" || true)
G1B_OK=$(grep -c 'sitecustomize: hybm_set_log_level(1) rc=0' "$LOG" || true)
MFDIAG=$(grep -c '\[MF_DIAG\]' "$LOG" || true)
echo "[$(date +%T)] healthy: layers=$LAYERS g1b_inject=$G1B_OK mf_diag_lines=$MFDIAG"
if [ "$LAYERS" -lt 80 ]; then echo "WARN: V2 路由层数 $LAYERS < 80, 检查 serve.log"; fi
if [ "$G1B_OK" -lt 1 ]; then echo "WARN: G1B 日志注入未生效([MF_DIAG] 将缺失)"; fi

# ---------- 507014 watchdog + 滚动负载 ----------
FAULT=0
watch_507014() {
  while kill -0 "$SERVE_PID" 2>/dev/null; do
    if grep -q "507014" "$LOG" 2>/dev/null; then
      echo "[$(date +%T)] *** 507014 DETECTED — 停止纪律: 保全现场, 不重启 ***"
      grep -n "507014" "$LOG" | head -5
      FAULT=1
      return
    fi
    sleep 2
  done
}
watch_507014 &
WATCH_PID=$!

REP=0
END=$((SECONDS + DURATION_S))
while [ $SECONDS -lt $END ] && [ $FAULT -eq 0 ]; do
  REP=$((REP+1))
  HF_HUB_OFFLINE=1 vllm bench serve --backend vllm \
      --endpoint /v1/completions --base-url "http://127.0.0.1:${PORT}" \
      --model qwen3.6-35b-a3b-w8a8 --tokenizer "$MODEL" \
      --dataset-name random --random-input-len 4096 --random-output-len $BENCH_OUT \
      --num-prompts $BENCH_N --max-concurrency 10 --ignore-eos \
      --percentile-metrics ttft,tpot,itl --metric-percentiles "50,90,99,99.9" \
      --save-result --result-dir "$OUT_DIR" --result-filename "stability_rep${REP}.json" \
      >> "$OUT_DIR/bench.log" 2>&1 || { echo "bench rep $REP failed"; break; }
done
# watchdog 继续盯到 serve 完全退出(停机期故障也算数——smoke4 教训)
while kill -0 "$SERVE_PID" 2>/dev/null; do
  if grep -q "507014" "$LOG" 2>/dev/null; then
    echo "[$(date +%T)] *** 507014 DETECTED(含停机期) — 保全现场, 不重启 ***"
    FAULT=1
    break
  fi
  sleep 2
done
kill $WATCH_PID 2>/dev/null || true

# ---------- 收尾 ----------
sleep 5
ERRS=$(grep -c "507014" "$LOG" || true)
if [ $FAULT -eq 1 ] || [ "$ERRS" -gt 0 ]; then
  echo "[$(date +%T)] FAULT session — 保全后终止"
  grep -E "\[MF_DIAG\]|\[MF_G1C\]|\[MF_G1D\]|\[V2_G1C\]|\[V2_G1D\]|507014" "$LOG" \
      > "$OUT_DIR/forensic_excerpt.log" || true
  cp "$LOG" "$OUT_DIR/serve.log.fault" 2>/dev/null || true
  kill -TERM -- -"$SERVE_PID" 2>/dev/null || true
  echo "=== SESSION $SESSION RESULT: FAULT(507014) — 现场: $OUT_DIR ==="
  exit 2
fi
kill -TERM -- -"$SERVE_PID" 2>/dev/null || true
wait "$SERVE_PID" 2>/dev/null || true
ERRS=$(grep -c "507014" "$LOG" || true)
echo "=== SESSION $SESSION RESULT: $([ $FAULT -eq 1 ] || [ "$ERRS" -gt 0 ] && echo FAULT507014 || echo CLEAN) (reps=$REP, 507014_count=$ERRS) ==="
exit 0
