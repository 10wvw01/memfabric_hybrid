#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# 一种通信后端/一进程；两轮 ABBA，Stock 使用真正原生FP16 HCCL all_reduce。
set -Eeuo pipefail
[[ "$#" == 2 ]] || { echo "usage: f1c_run_abba.sh ABSENT_ABSOLUTE_OUT_DIR M" >&2; exit 2; }
ROOT="$(cd "$(dirname "$0")/../../../../.." && pwd)"
DRIVER="$ROOT/csrc/_310P/memfabric_mm_ar_v2/test/perf/f1c_arm.py"
ANALYZER="$ROOT/csrc/_310P/memfabric_mm_ar_v2/test/perf/f1c_analyze.py"
OUT="$1"
M="$2"
case "$M" in 1|8|10|16|32) ;; *) echo "Unsupported M=$M" >&2; exit 2;; esac
[[ "$OUT" == /* && ! -e "$OUT" ]] || { echo "output must be UNUSED absolute path" >&2; exit 2; }
[[ "$F1C_G0_APPROVED" == YES && "$F1C_EXCLUSIVE_APPROVED" == YES && "$F1C_RESTORE_PLAN_VERIFIED" == YES ]] || {
  echo "G0, exclusivity and backup/restore approval required" >&2; exit 2; }
[[ "$ASCEND_RT_VISIBLE_DEVICES" =~ ^[0-9]+,[0-9]+$ ]] || { echo "TP2 physical die pair required" >&2; exit 2; }
first="$(echo "$ASCEND_RT_VISIBLE_DEVICES" | cut -d, -f1)"
second="$(echo "$ASCEND_RT_VISIBLE_DEVICES" | cut -d, -f2)"
[[ "$first" != "$second" ]] || { echo "same die twice" >&2; exit 2; }
[[ "$MF_SDMA_ORCH_JSON" == /*/libmf_sdma_orch_v11.json && -f "$MF_SDMA_ORCH_JSON" ]] || {
  echo "KVER11 must be pinned" >&2; exit 2; }
for f in "$V2_HOST_LIB" "$F1C_DEVICE_SO_PATH" "$F1C_CORE_SO_LIB64" "$F1C_CORE_SO_PY"; do
  [[ "$f" == /* && -f "$f" ]] || { echo "missing absolute SO: $f" >&2; exit 2; }
done
[[ ! -v MF_KFC_BYPASS && ! -v MF310P_V2_NOWARMUP ]] || { echo "safety bypass forbidden" >&2; exit 2; }
for tool in python3 torchrun timeout nm sha256sum npu-smi; do
  command -v "$tool" >/dev/null || { echo "missing tool: $tool" >&2; exit 2; }
done
PORT_PROBE="$ROOT/csrc/_310P/memfabric_mm_ar_v2/test/perf/f1c_port_probe.py"
[[ -f "$PORT_PROBE" ]] || { echo "missing port probe: $PORT_PROBE" >&2; exit 2; }
[[ -z "$(git -C "$ROOT" status --porcelain --untracked-files=no)" ]] || {
  echo "tracked working tree dirty" >&2; exit 2; }
sha256() { sha256sum "$1" | awk '{print $1}'; }
export F1C_DEVICE_SHA256="$(sha256 "$F1C_DEVICE_SO_PATH")"
export F1C_HOST_SHA256="$(sha256 "$F1C_CORE_SO_LIB64")"
export F1C_KVER_SHA256="$(sha256 "$MF_SDMA_ORCH_JSON")"
plugin_path="$(python3 -c 'import vllm_ascend.vllm_ascend_C as ext;print(ext.__file__)' 2>&1 | tail -n 1)"
[[ -f "$plugin_path" && "$(readlink -f "$plugin_path")" == "$(readlink -f "$V2_HOST_LIB")" ]] || {
  echo "Python loaded plugin does not match V2_HOST_LIB" >&2; exit 2; }
export F1C_PLUGIN_SHA256="$(sha256 "$plugin_path")"
[[ "$F1C_DEVICE_SHA256" == 2a19390c83c3fe6b0bdb8946d88990cca66b331f765ca445dbda2e787c2d2a19 ]] || {
 echo "blocked: wrong device SO (must be D4 Tile2048 diag ON)" >&2; exit 2; }
[[ "$F1C_PLUGIN_SHA256" == 8eb65de98c3d8664ff10b421c1d7ad31318dc720c3f5679cd4d34e187203bea1 ]] || {
 echo "blocked: wrong Gate-C diagnostic Host extension" >&2; exit 2; }
[[ "$F1C_HOST_SHA256" == 1dc46d7dcbe583a5dbb78b4c65a0b5e22f9f352976c53ed31b761fa744f91a30 ]] || {
 echo "blocked: wrong physical-device patched memfabric host SO" >&2; exit 2; }
[[ "$(sha256 "$F1C_CORE_SO_PY")" == "$F1C_HOST_SHA256" ]] || {
 echo "installed Host SO two paths disagree" >&2; exit 2; }
[[ "$F1C_KVER_SHA256" == 25e16b0af2f16989231ff0443698261257bd8142834783a0c1e4ad57cd9ebc78 ]] || {
 echo "blocked: wrong KVER11" >&2; exit 2; }
nm -D "$V2_HOST_LIB" | grep -F "mf310p_v2_trial_run_nz_direct_small" >/dev/null || {
 echo "diagnostic T1 ABI missing" >&2; exit 2; }
mkdir -p "$OUT/sessions"
cat > "$OUT/manifest.txt" <<EOF
experiment=F1C_SEPARATE_PROCESS_CROSS_ABBA
repo_head=$(git -C "$ROOT" rev-parse HEAD)
M=$M
K=2048
N=2048
TP=2
devices=$ASCEND_RT_VISIBLE_DEVICES
stock_primary=FP16_F_linear_plus_FP16_HCCL_no_float32_conversion
fused=NZ_T1_MM8_ADD4
backend_isolation=HCCL_only_Stock__MemFabric_pool_only_fused
warmup=5
samples=50
order=A_B_B_A__A_B_B_A
device_so_path=$F1C_DEVICE_SO_PATH
device_so_sha256=$F1C_DEVICE_SHA256
plugin_path=$plugin_path
plugin_sha256=$F1C_PLUGIN_SHA256
memfabric_host_so_path=$F1C_CORE_SO_LIB64
memfabric_host_sha256=$F1C_HOST_SHA256
memfabric_host_so_second_path=$F1C_CORE_SO_PY
kver11_json_path=$MF_SDMA_ORCH_JSON
kver11_sha256=$F1C_KVER_SHA256
EOF
i=0
for arm in stock_fp16 memfabric_t1 memfabric_t1 stock_fp16 stock_fp16 memfabric_t1 memfabric_t1 stock_fp16; do
  i=$((i+1))
  printf -v idx "%02d" "$i"
  label="$idx""_m""$M""_""$arm"
  dir="$OUT/sessions/$label"
  mkdir "$dir"
  npu-smi info > "$dir/pre_npu_smi.txt" 2>&1 || { echo "NPU precheck failure: $label" >&2; exit 1; }
  python3 "$PORT_PROBE" > "$dir/pre_port_listeners.txt" 2>&1 || {
    echo "port probe failure or required port busy: $label" >&2; exit 1; }
  echo "F1-C $label (fresh TP2 workers)" >&2
  if ! timeout --signal=TERM --kill-after=30s 15m torchrun --standalone --nproc-per-node=2 \
      "$DRIVER" --arm "$arm" --session "$label" --m "$M" \
      --mm-cores 8 --add-cores 4 --warmup 5 --iters 50 \
      --output "$dir" > "$dir/session.log" 2>&1; then
    echo "F1-C FAIL at $label: preserve evidence, quarantine card; DO NOT auto-continue" >&2; exit 1;
  fi
  [[ -s "$dir/$label""_rank0.json" && -s "$dir/$label""_rank1.json" ]] || {
    echo "incomplete dual-rank JSON" >&2; exit 1; }
  npu-smi info > "$dir/post_npu_smi.txt" 2>&1 || {
    echo "NPU postcheck failure; do not continue" >&2; exit 1; }
done
python3 "$ANALYZER" --sessions "$OUT/sessions" --m "$M" --out "$OUT/f1c_provisional.json"
echo "F1-C provisional only: HCCL event stream audit and separate route evidence required."
