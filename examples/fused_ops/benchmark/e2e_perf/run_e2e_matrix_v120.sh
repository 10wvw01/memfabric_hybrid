#!/bin/bash
# Three-arm E2E TTFT matrix (trunk v1.2.0 runtime variant): stock vs
# smem_mm_ar_310p3 (F10, local reduction) vs smem_mm_iar_310p3 (F11, hardware
# reduction).
#
# Diff vs run_e2e_matrix.sh (v28): serve arms call the *_v120.sh scripts, which
# point the memfabric host libs + orch runtime at the trunk build (auto-deploy
# inside smem_shm_init).
#
# DFLASH=1 (default): prefill splits into 1280x3+256 (deployment block type).
# DFLASH=0: prefill splits into 3840+256 (mamba-aligned) — the 3840-row main
# block engages the fusion; TPOT regresses ~2.5x, TTFT-comparison only.
# Engagement gate M >= 1280 (wrapper default; native-M launch runs partial
# chunks as their own wave — no pad tax).
#
#   S1  stock   (baseline; no fusion)
#   F10 smem     SMEM_OP=ar   (smem_mm_ar_310p3, local reduction)
#   F11 smem     SMEM_OP=iar  (smem_mm_iar_310p3, hardware reduction)
#
# Round 1: S1 -> F10 -> F11 (profiles on); Round 2 (order-reversed
# confirmation): F11 -> F10 -> S1 (profiles off). Same SEED_BASE everywhere
# => identical prompts per arm position => paired analysis. Fresh server per
# arm => cold prefix cache. Takeover verification via serve-log markers
# (6 = 2 workers x 3: session up / installed on 40 / verified bit-exact).
#
# Env overrides: ARMS, ROUNDS, LENS, REQUESTS, TS, DFLASH
set -u
# 宿主环境可能全局导出 ASCEND_RT_VISIBLE_DEVICES，serve 前必须清掉。
unset ASCEND_RT_VISIBLE_DEVICES
DIR=$(cd "$(dirname "$0")" && pwd)
PY=${PY:-python3}
DFLASH=${DFLASH:-1}

TS=${TS:-prd_$(date +%Y%m%d_%H%M%S)}
OUT=${OUT:-$DIR/results/e2e_dflash${DFLASH}_$TS}
REQUESTS=${REQUESTS:-10}
LENS=${LENS:-"4096"}
SEED_BASE=100
ROUNDS=${ROUNDS:-2}
mkdir -p "$OUT"

# --- device discipline: physical 4/5 only, wait for baseline ---
dev_mem() {
  npu-smi info 2>/dev/null | awk '/38144/{getline; print $8}' | sed -n "$1p" 2>/dev/null | tr -d ' '
}
devices_free() {
  pgrep -f "VLLM[:]" >/dev/null && return 1
  local m4 m5
  m4=$(dev_mem 1); m5=$(dev_mem 2)
  [ -n "$m4" ] && [ -n "$m5" ] && [ "$m4" -lt 3000 ] && [ "$m5" -lt 3000 ]
}
wait_baseline() {
  for i in $(seq 1 30); do
    devices_free && { echo "devices 4/5 back at baseline"; return 0; }
    sleep 10
  done
  echo "WARNING: devices 4/5 not at baseline after 300s"; return 1
}

start_arm() {  # $1=arm $2=round
  local CFG OP
  case $1 in
    S1)  CFG=stock; unset SMEM_OP ;;
    F10) CFG=smem;  export SMEM_OP=ar ;;
    F11) CFG=smem;  export SMEM_OP=iar ;;
    *) echo "unknown arm $1"; return 1 ;;
  esac
  export SMEM_PROFILE_DIR=$OUT/profile_$1_r$2
  mkdir -p "$SMEM_PROFILE_DIR"
  nohup bash $DIR/vllm_serve_${CFG}_v120.sh > $OUT/serve_$1_r$2.log 2>&1 &
  echo "$CFG (arm=$1 DFLASH=$DFLASH SMEM_OP=${SMEM_OP:-none}) pid=$!"
}

wait_health() {
  for i in $(seq 1 240); do
    curl -s -m 2 http://127.0.0.1:8000/health >/dev/null 2>&1 && return 0
    sleep 5
  done
  return 1
}

check_markers() {  # $1=arm $2=round — takeover verification
  local LOG=$OUT/serve_$1_r$2.log N
  case $1 in
    S1)
      N=$(grep -cE "session up|installed on|bit-exact|M-hist|FALLBACK to native" "$LOG" || true)
      echo "[$1 r$2] stock marker check (expect 0): $N"
      [ "$N" -eq 0 ] || echo "WARNING: [$1 r$2] stock run shows fusion markers ($N)!"
      ;;
    F10|F11)
      N=$(grep -cE "session up|installed on 40|verified bit-exact" "$LOG" || true)
      echo "[$1 r$2] fusion marker check (expect 6 = 2 workers x 3): $N"
      if [ "$N" -lt 6 ]; then
        echo "WARNING: [$1 r$2] markers incomplete ($N) — takeover may be partial!"
        grep -oE "FALLBACK to native on [^:]+: .*" "$LOG" | sort -u | head -6
      fi
      echo "[$1 r$2] first-verify lines:"
      grep "first fused call verified" "$LOG" | head -2
      echo "[$1 r$2] fallback reasons (distinct):"
      grep -oE "FALLBACK to native on [^:]+: .*" "$LOG" | sort -u | head -6
      ;;
  esac
}

stop_server() {
  pkill -9 -f "vllm serv[e] /home/models" 2>/dev/null
  pkill -9 -f "VLLM[:]" 2>/dev/null
  sleep 12
  for i in $(seq 1 15); do pgrep -f "VLLM[:]" >/dev/null || break; sleep 5; done
  pgrep -f "VLLM[:]" >/dev/null && echo "WARNING: vllm processes still alive!" \
                                || echo "server stopped cleanly"
  wait_baseline
}

run_arm() {  # $1=arm $2=round $3=profile?
  local ARM=$1 R=$2 DO_PROF=${3:-0}
  echo ""
  echo "######## ARM $ARM / ROUND $R ########"
  if ! devices_free; then
    echo "devices 4/5 NOT free — skipping $ARM r$R (rerun later with ARMS=$ARM ROUNDS=$R)"
    return 1
  fi
  start_arm $ARM $R
  if ! wait_health; then
    echo "server $ARM r$R did NOT become healthy — skipping"
    tail -8 $OUT/serve_${ARM}_r${R}.log | cut -c1-200
    stop_server
    return 1
  fi
  echo "server healthy"
  check_markers $ARM $R
  for LEN in $LENS; do
    $PY $DIR/ttft_bench.py --input-len $LEN --requests $REQUESTS \
      --seed-base $SEED_BASE --out $OUT/ttft_${ARM}_r${R}_m${LEN}.json
    if [ "$DO_PROF" = "1" ]; then
      $PY $DIR/profile_e2e.py --input-len $LEN \
        --out $OUT/profile_${ARM}_r${R}_m${LEN}.json
      sleep 5   # let the profile trace flush to disk
    fi
  done
  # Post-bench takeover evidence: after real requests, the only fallback
  # reasons left must be small/off-contract M (e.g. 256 tail blocks) —
  # no "M=1280 < MIN_ROWS" may appear.
  echo "[$ARM r$R] fallback reasons after bench (distinct):"
  grep -oE "FALLBACK to native on [^:]+: .*" $OUT/serve_${ARM}_r${R}.log | sort -u | head -8
  stop_server
}

# round 1: S1 -> F10 -> F11 (profiles on); round 2: reversed (profiles off)
for R in $(seq 1 $ROUNDS); do
  if [ $((R % 2)) -eq 1 ]; then ORDER="S1 F10 F11"; else ORDER="F11 F10 S1"; fi
  DO_PROF=$(( R % 2 == 1 ? 1 : 0 ))
  for ARM in ${ARMS:-$ORDER}; do
    run_arm $ARM $R $DO_PROF
  done
done

echo ""
echo "Three-arm E2E matrix complete -> $OUT"
ls $OUT/*.json 2>/dev/null | head -30