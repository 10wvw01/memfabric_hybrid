#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# U1 双Die多M试验：每场仅使用一种通信数据面，不修改正式内核。
set -Eeuo pipefail
[[ "$#" == 3 ]] || { echo "usage: u1_run.sh NEW_ABSOLUTE_DIR small|large smoke|screen|confirm" >&2; exit 2; }
ROOT="$(cd "$(dirname "$0")/../../../../.." && pwd)"
PERF="$ROOT/csrc/_310P/memfabric_mm_ar_v2/test/perf"
OUT="$1"; SCOPE="$2"; PHASE="$3"
case "$SCOPE" in small|large) ;; *) echo "invalid scope" >&2; exit 2;; esac
case "$PHASE" in smoke|screen|confirm) ;; *) echo "invalid phase" >&2; exit 2;; esac
[[ "$OUT" == /* && ! -e "$OUT" ]] || { echo "output must be UNUSED absolute path" >&2; exit 2; }
[[ "${U1_G0_APPROVED:-}" == YES && "${U1_EXCLUSIVE_APPROVED:-}" == YES &&
   "${U1_RESTORE_PLAN_VERIFIED:-}" == YES ]] || {
   echo "G0/exclusivity/restore approval missing" >&2; exit 2; }
if [[ "$PHASE" == confirm ]]; then
  [[ "${U1_CONFIRM_APPROVED:-}" == YES ]] || {
    echo "confirm only after independent screen report review" >&2; exit 2; }
fi
[[ "${ASCEND_RT_VISIBLE_DEVICES:-}" =~ ^[0-9]+,[0-9]+$ ]] || {
  echo "pin same-card physical die pair and verify mapping in G0" >&2; exit 2; }
[[ "${ASCEND_RT_VISIBLE_DEVICES%,*}" != "${ASCEND_RT_VISIBLE_DEVICES#*,}" ]] || {
  echo "duplicate visible Die ID" >&2; exit 2; }
[[ "${MF_SDMA_ORCH_JSON:-}" == /*/libmf_sdma_orch_v11.json &&
  -f "${MF_SDMA_ORCH_JSON:-}" ]] || { echo "KVER11 JSON missing" >&2; exit 2; }
for v in V2_HOST_LIB U1_DEVICE_SO_PATH U1_CORE_SO_LIB64 U1_CORE_SO_PY; do
  p="${!v:-}"
  [[ "$p" == /* && -f "$p" ]] || { echo "missing true SO path: $v" >&2; exit 2; }
done
[[ ! -v MF_KFC_BYPASS && ! -v MF310P_V2_NOWARMUP &&
   ! -v MF310P_V2_AR_ONLY_NOWARMUP ]] || { echo "KFC/first-write bypass forbidden" >&2; exit 2; }
for tool in python3 torchrun timeout sha256sum nm npu-smi readlink; do
  command -v "$tool" >/dev/null || { echo "missing required tool: $tool" >&2; exit 2; }
done
[[ -z "$(git -C "$ROOT" status --porcelain --untracked-files=no)" ]] || {
  echo "tracked code dirty" >&2; exit 2; }
sha() { sha256sum "$1" | awk '{print $1}'; }
export U1_DEVICE_SHA256="$(sha "$U1_DEVICE_SO_PATH")"
export U1_HOST_SHA256="$(sha "$U1_CORE_SO_LIB64")"
export U1_KVER_SHA256="$(sha "$MF_SDMA_ORCH_JSON")"
[[ "$U1_DEVICE_SHA256" == 2a19390c83c3fe6b0bdb8946d88990cca66b331f765ca445dbda2e787c2d2a19 ]] || {
  echo "D4 Device SO SHA mismatch" >&2; exit 2; }
[[ "$U1_HOST_SHA256" == 1dc46d7dcbe583a5dbb78b4c65a0b5e22f9f352976c53ed31b761fa744f91a30 ]] || {
  echo "MemFabric patched SO SHA mismatch" >&2; exit 2; }
[[ "$(sha "$U1_CORE_SO_PY")" == "$U1_HOST_SHA256" ]] || {
  echo "MemFabric dual-path SO mismatch" >&2; exit 2; }
[[ "$U1_KVER_SHA256" == 25e16b0af2f16989231ff0443698261257bd8142834783a0c1e4ad57cd9ebc78 ]] || {
  echo "KVER11 SHA mismatch" >&2; exit 2; }
plugin_path="$(python3 -c 'import vllm_ascend.vllm_ascend_C as x;print(x.__file__)' 2>&1 | tail -n 1)"
[[ -f "$plugin_path" && "$(readlink -f "$plugin_path")" == "$(readlink -f "$V2_HOST_LIB")" ]] || {
  echo "Python extension loaded from another Host SO path" >&2; exit 2; }
export U1_PLUGIN_SHA256="$(sha "$plugin_path")"
[[ "$U1_PLUGIN_SHA256" == 8eb65de98c3d8664ff10b421c1d7ad31318dc720c3f5679cd4d34e187203bea1 ]] || {
  echo "Gate-C diagnostic ON plugin SHA mismatch" >&2; exit 2; }
nm -D "$V2_HOST_LIB" | grep -F "mf310p_v2_trial_run_nz_direct_small" >/dev/null || {
  echo "T1 diagnostic ABI missing" >&2; exit 2; }
nm -D "$V2_HOST_LIB" | grep -F "mf310p_v2_trial_run" >/dev/null || {
  echo "D4 formal ABI missing" >&2; exit 2; }

mkdir -p "$OUT/sessions"
npu-smi info > "$OUT/g0_npu_before.txt" 2>&1
PYTHONPATH="$PERF" python3 - "$SCOPE" "$PHASE" "$OUT/manifest.json" "$ROOT" <<'PY'
import json, os, subprocess, sys
from u1_spec import plan
scope, phase, dest, root = sys.argv[1:]
data = {
    "experiment": "U1_FULLCHAIN", "scope": scope, "phase": phase,
    "repo_head": subprocess.check_output(["git", "-C", root, "rev-parse", "HEAD"], text=True).strip(),
    "device_so_sha256": os.environ["U1_DEVICE_SHA256"],
    "plugin_sha256": os.environ["U1_PLUGIN_SHA256"],
    "host_so_sha256": os.environ["U1_HOST_SHA256"],
    "kver11_sha256": os.environ["U1_KVER_SHA256"],
    "physical_devices": os.environ["ASCEND_RT_VISIBLE_DEVICES"],
    "session_ids": [x["session"] for x in plan(scope, phase)],
    "caveat": "Actual KVER11 AICPU registration/real loaded device SO and same-card die must be proven in G0",
}
with open(dest, "x") as fp:
    json.dump(data, fp, ensure_ascii=False, indent=2)
    fp.write("\n")
PY
n=2
[[ "$PHASE" == confirm ]] && n=8
for ((stage=1; stage<=n; stage++)); do
  label="$(PYTHONPATH="$PERF" python3 -c \
    "from u1_spec import session_id;print(session_id('$SCOPE', $stage))")"
  dir="$OUT/sessions/$label"
  mkdir "$dir"
  npu-smi info > "$dir/pre_npu_smi.txt" 2>&1
  echo "U1 running: $label" >&2
  if ! timeout --signal=TERM --kill-after=30s 15m torchrun --standalone --nproc-per-node=2 \
      "$PERF/u1_arm.py" --scope "$SCOPE" --phase "$PHASE" --stage "$stage" \
      --out "$dir" > "$dir/session.log" 2>&1; then
    echo "U1 FAIL $label: preserve raw logs; quarantine die/KFC; DO NOT continue" >&2
    exit 1
  fi
  [[ -s "$dir/${label}_rank0.json" && -s "$dir/${label}_rank1.json" ]] || {
    echo "missing 2-Rank JSON $label" >&2; exit 1; }
  npu-smi info > "$dir/post_npu_smi.txt" 2>&1
done
if [[ "$PHASE" == smoke ]]; then
  echo "U1 smoke PASS; this is correctness only, not a performance screen." >&2
else
  python3 "$PERF/u1_analyze.py" --root "$OUT" --scope "$SCOPE" \
    --phase "$PHASE" --out "$OUT/u1_analysis.json"
fi
npu-smi info > "$OUT/npu_after.txt" 2>&1
echo "U1 $SCOPE/$PHASE complete. Archive all raw logs and G0 restore record."
