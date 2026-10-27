#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# U1一次G0/一次部署，顺序执行小M与大M全链路快速扫描；
# D1诊断需单独明确授权；完整ABBA不能自动触发。
set -Eeuo pipefail
[[ "$#" == 1 ]] || { echo "usage: bash u1_campaign.sh NEW_ABSOLUTE_ROOT" >&2; exit 2; }
ROOT="$(cd "$(dirname "$0")/../../../../.." && pwd)"
PERF="$ROOT/csrc/_310P/memfabric_mm_ar_v2/test/perf"
OUT="$1"
[[ "$OUT" == /* && ! -e "$OUT" ]] || { echo "need unused absolute output root" >&2; exit 2; }
[[ "${U1_G0_APPROVED:-}" == YES && "${U1_EXCLUSIVE_APPROVED:-}" == YES &&
   "${U1_RESTORE_PLAN_VERIFIED:-}" == YES ]] || {
  echo "Complete G0, exclusive TP2 + backup/restore checklist before U1." >&2; exit 2; }
mkdir "$OUT"
echo "U1/G0 M10 Stock→T1 smoke; fail before full-M work if new driver not viable" >&2
bash "$PERF/u1_run.sh" "$OUT/small_smoke" small smoke
echo "U1/G0 M2048 Stock→D4 smoke; no test stats merged with main experiments" >&2
bash "$PERF/u1_run.sh" "$OUT/large_smoke" large smoke
echo "U1/D0 SMALL multi-M screen first (Stock vs T1)" >&2
bash "$PERF/u1_run.sh" "$OUT/small_screen" small screen
echo "U1/D0 LARGE multi-M screen next (Stock vs D4)" >&2
bash "$PERF/u1_run.sh" "$OUT/large_screen" large screen
echo "U1/D0 complete; both operator maps at same deployment. Confirm is NOT automatic." >&2

if [[ "${U1_DIAG_APPROVED:-}" != YES ]]; then
  echo "D1 diagnostics NOT executed (set U1_DIAG_APPROVED=YES after separate G0 evidence review)." >&2
  exit 0
fi
# D1只借用同一次受控部署，仍需分别独占设备；不重复安装二进制。
for v in U1_DEVICE_SO_PATH U1_CORE_SO_LIB64 MF_SDMA_ORCH_JSON V2_HOST_LIB; do
  [[ -f "${!v:-}" ]] || { echo "D1 missing $v" >&2; exit 2; }
done
sha() { sha256sum "$1" | awk '{print $1}'; }
export U1_DEVICE_SHA256="$(sha "$U1_DEVICE_SO_PATH")"
export U1_HOST_SHA256="$(sha "$U1_CORE_SO_LIB64")"
export U1_KVER_SHA256="$(sha "$MF_SDMA_ORCH_JSON")"
export U1_PLUGIN_SHA256="$(sha "$V2_HOST_LIB")"
mkdir "$OUT/diag"
npu-smi info > "$OUT/diag/pre_npu_smi.txt" 2>&1
# D1-A不创建分布式组、不建MemFabric池，只检测单Die MM与 MM+CleanLines。
timeout --signal=TERM --kill-after=30s 10m python3 "$PERF/u1_diag_mm.py" \
  --device-so "$U1_DEVICE_SO_PATH" --ms 10 16 64 256 --mm-cores 4 8 \
  --out "$OUT/diag/small_mm.json" > "$OUT/diag/small_mm.log" 2>&1 || {
  echo "U1 D1 small MM failed; preserve and STOP" >&2; exit 1; }
# D1-B只做成熟的形式核与串行对照。单Chunk GateC另需证明正确性再单独执行。
for case in formal loop4; do
  dir="$OUT/diag/$case"
  mkdir "$dir"
  timeout --signal=TERM --kill-after=30s 15m torchrun --standalone --nproc-per-node=2 \
    "$PERF/u1_diag_large.py" --case "$case" --out "$dir" --warmup 3 --iters 8 \
      > "$dir/session.log" 2>&1 || {
    echo "U1 D1 large case $case failed; preserve and STOP" >&2; exit 1; }
done
npu-smi info > "$OUT/diag/post_npu_smi.txt" 2>&1
echo "U1 D1 safe diagnostics done. Short Profiler only after new approval; not mixed with timing."
