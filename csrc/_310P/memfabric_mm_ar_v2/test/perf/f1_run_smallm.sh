#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# F1小M现有T1低开销路线基准筛选，必须保留Stock HCCL参考。
set -Eeuo pipefail
ROOT="$(cd "$(dirname "$0")/../../../../.." && pwd)"
DRIVER="$ROOT/csrc/_310P/memfabric_mm_ar_v2/test/t1_nz_direct_trial.py"
ANALYZER="$ROOT/csrc/_310P/memfabric_mm_ar_v2/test/perf/f1_smallm_analyze.py"
OUT="${1:?usage: f1_run_smallm.sh ABS_NEW_OUTPUT_DIR}"
[[ "$OUT" == /* && ! -e "$OUT" ]] || { echo "expected unused absolute output directory" >&2; exit 2; }
[[ "${F1_EXCLUSIVE_APPROVED:-}" == "YES" ]] || { echo "check exclusive TP2 devices and Host/Device SO SHA first" >&2; exit 2; }
[[ "${ASCEND_RT_VISIBLE_DEVICES:-}" =~ ^[0-9]+,[0-9]+$ ]] || { echo "physical TP2 die pair required" >&2; exit 2; }
[[ "${MF_SDMA_ORCH_JSON:-}" == /*/libmf_sdma_orch_v11.json && -f "$MF_SDMA_ORCH_JSON" ]] || { echo "pin MemFabric KVER11" >&2; exit 2; }
[[ ! -v MF310P_V2_NOWARMUP && ! -v MF_KFC_BYPASS ]] || { echo "bypass not permitted" >&2; exit 2; }
[[ "${V2_HOST_LIB:-}" == /* && -f "${V2_HOST_LIB:-}" ]] || {
  echo "BLOCKED: pin actual exported T1 Host ABI using absolute V2_HOST_LIB path" >&2
  exit 2
}
[[ "${F1_DEVICE_SO_PATH:-}" == /* && -f "${F1_DEVICE_SO_PATH:-}" ]] || {
  echo "BLOCKED: pin installed device binary through F1_DEVICE_SO_PATH" >&2
  exit 2
}
command -v nm >/dev/null || { echo "Missing nm for T1 Host ABI preflight" >&2; exit 2; }
nm -D "$V2_HOST_LIB" | grep -F "mf310p_v2_trial_run_nz_direct_small" >/dev/null || {
  echo "BLOCKED: T1 nz_direct_small Host ABI not exported; rebuild/deploy matched artifacts" >&2
  exit 2
}
command -v timeout >/dev/null || { echo "Missing GNU timeout" >&2; exit 2; }
# vllm 平台插件注册日志走 stdout（环境实测），须取末行才是真实扩展路径。
ext_path="$(python3 -c 'import vllm_ascend.vllm_ascend_C as ext; print(ext.__file__)' 2>/dev/null | tail -n 1)" || {
  echo "BLOCKED: current Python plugin could not be imported" >&2; exit 2;
}
[[ -f "$ext_path" ]] || { echo "BLOCKED: Python plugin SO missing" >&2; exit 2; }
[[ "$(git -C "$ROOT" status --porcelain --untracked-files=no)" == "" ]] || {
  echo "Tracked source tree dirty; stop and record diff before benchmarking" >&2
  exit 2
}
mkdir "$OUT"
cat > "$OUT/manifest.txt" <<EOF
experiment=F1_low_M_M10_cost_screening
repo_head=$(git -C "$ROOT" rev-parse HEAD)
devices=$ASCEND_RT_VISIBLE_DEVICES
orch_sha256=$(sha256sum "$MF_SDMA_ORCH_JSON" | awk '{print $1}')
num_m=6
mm_cores=4,8
add_cores=1,4,8
host_timing=Stock_MM_HCCL_prepool_vs_T1_MM_MemFabric_postpool
warning=do_not_make_final_GO_from_prepool_vs_postpool_host_sync
host_lib_path=$V2_HOST_LIB
host_lib_sha256=$(sha256sum "$V2_HOST_LIB" | awk '{print $1}')
python_plugin_path=$ext_path
python_plugin_sha256=$(sha256sum "$ext_path" | awk '{print $1}')
device_so_path=$F1_DEVICE_SO_PATH
device_so_sha256=$(sha256sum "$F1_DEVICE_SO_PATH" | awk '{print $1}')
session_timeout=30m_plus_30s_kill
EOF
# 一次执行36种形状分核，含M10以及邻近M8/M16，所有shape均逐次位精确校验。
timeout --signal=TERM --kill-after=30s 30m torchrun --standalone --nproc-per-node=2 "$DRIVER" \
  --mode full --rows 1 8 10 16 32 64 \
  --mm-cores 4 8 --add-cores 1 4 8 --repeat 30 \
  --chunk-rows 256 > "$OUT/f1_t1_stock.log" 2>&1 || {
    echo "F1 failed; preserve logs, quarantine device and stop (no silent repair)." >&2
    exit 1
  }
python3 "$ANALYZER" --log "$OUT/f1_t1_stock.log" --out "$OUT/f1_screening.json"
echo "F1 initial screen only: must still separate MM, protocol/IB, wait, add by valid experiments."
