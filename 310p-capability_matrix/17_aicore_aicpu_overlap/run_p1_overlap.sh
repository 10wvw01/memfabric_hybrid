#!/bin/bash
# run_p1_overlap.sh — P1 验收一键执行(八步协同全链产品化)
# 用法: bash run_p1_overlap.sh [devA devB]   (默认 0 1, 同卡 die 对)
#
# 纪律(A.4): deployer 保活到期退出会触发清理链删设备侧文件 —— 每次正式验证前
# 必跑验证门, 缺失则重新部署(本脚本内置 ensure_deploy)。
set -uo pipefail
cd "$(dirname "$0")"

CANN_HOME=${CANN_HOME:-/usr/local/Ascend/cann-9.1.0}
DEVA=${1:-0}
DEVB=${2:-1}
KVER=${KVER:-1}
DEP_JSON="aicpu_kernel/libmf_overlap_aicpu.json"
DEV_SO="/usr/lib64/aicpu_kernels/0/aicpu_kernels_device/libmf_overlap_v${KVER}.so"
ALIVE_SECS=${ALIVE_SECS:-3600}
LOG_DIR=logs
mkdir -p "$LOG_DIR"

ok()   { echo -e "  \e[32m[OK]\e[0m  $*"; }
warn() { echo -e "  \e[33m[WARN]\e[0m $*"; }
fail() { echo -e "  \e[31m[FAIL]\e[0m $*"; exit 1; }

kfc() {
  local dev=$1; shift; local cmd="$*"; local out rc i
  for i in 1 2 3; do
    out=$(timeout 90 ./mini_kfc "$dev" "$cmd" 2>&1); rc=$?
    if [[ $rc -eq 0 && "$out" == *"sync=0"* ]]; then return 0; fi
    warn "KFC 第${i}次未过 (rc=$rc): ${cmd:0:60}..."
    [[ $i -lt 3 ]] && sleep 10
  done
  return 1
}

do_verify() {
  kfc "$1" "test -f $DEV_SO" || return 1
  kfc "$1" "grep -q HybmOverlapRun $DEV_SO" || return 1
  return 0
}

ensure_deploy() {
  local dev=$1
  if do_verify "$dev"; then
    ok "dev$dev kernel 有效, 跳过部署(同卡双 die 共享, X9)"
    return 0
  fi
  warn "dev$dev kernel 缺失(清理链已删?), 重新部署 ..."
  pkill -f './deployer ' 2>/dev/null && sleep 2
  kfc "$dev" "rm -f /home/CustAiCpuUser/lib/libmf_overlap*" || fail "dev$dev 清旧文件失败"
  ./deployer "$dev" "$DEP_JSON" "$ALIVE_SECS" >>"$LOG_DIR/deployer_p1.log" 2>&1 &
  local pid=$!
  sleep 20
  kfc "$dev" "cat /home/CustAiCpuUser/lib/libmf_overlap_aicpu.so.* > $DEV_SO" || { kill $pid 2>/dev/null; fail "dev$dev 复制失败"; }
  kfc "$dev" "chmod 644 $DEV_SO" || { kill $pid 2>/dev/null; fail "dev$dev chmod 失败"; }
  do_verify "$dev" || { kill $pid 2>/dev/null; fail "dev$dev 验证门失败"; }
  ok "dev$dev 部署完成 (deployer pid=$pid 保活 ${ALIVE_SECS}s)"
}

# ---------- 主流程 ----------
source "$CANN_HOME/set_env.sh" >/dev/null 2>&1
make host_tools kernel probe probe_e0b probe_e0c probe_p1_shm probe_p1_overlap KVER="$KVER" >/dev/null \
  || fail "编译失败"

ensure_deploy "$DEVA"

rm -rf /tmp/p1_overlap /tmp/p1_shm 2>/dev/null
export MF_310P_SHM_OVERLAP=1
export MF_310P_OVERLAP_JSON="aicpu_kernel/libmf_overlap_v${KVER}.json"
export ASCEND_HOME_PATH="$CANN_HOME"
export LD_LIBRARY_PATH="$CANN_HOME/aarch64-linux/lib64:/usr/local/Ascend/driver/lib64/driver:/usr/local/Ascend/driver/lib64/common"

echo "=== P1 验收: 八步协同全链产品化 (r0=dev$DEVA r1=dev$DEVB) ==="
timeout 300 ./probe_p1_overlap 0 > "$LOG_DIR/p1ov_r0.log" 2>&1 &
P0=$!
timeout 300 ./probe_p1_overlap 1 > "$LOG_DIR/p1ov_r1.log" 2>&1 &
P1=$!
wait $P0; R0=$?
wait $P1; R1=$?
grep -E "OK|FAIL|INFO|===" "$LOG_DIR/p1ov_r0.log"
echo "--- r1 ---"
grep -E "OK|FAIL|INFO|===" "$LOG_DIR/p1ov_r1.log"

if [[ $R0 -eq 0 && $R1 -eq 0 ]]; then
  ok "P1 验收通过: E0c 场景产品化重跑成功(全部走产品 API)"
  exit 0
fi
fail "P1 验收未通过 (r0=$R0 r1=$R1)"
