#!/bin/bash
# run_e0c.sh — E0c 一键执行：池上协同最小复现（AICore×AICPU 八步语义全链）
# 用法: bash run_e0c.sh [devA devB]   (默认 0 1, 须为同卡 die 对)
#
# 验收: r0 终态数据 == pattern+INC(经 r1 AICore 计算回程) + 结果标志到达 +
#       双侧 AICore 状态无超时 + 双侧 AICPU 编排 mainRet=0/head 前进
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

# 部署保障(同卡双 die 共享文件系统 → 部署 devA 即覆盖 devB, X9 实测)
ensure_deploy() {
  local dev=$1
  if do_verify "$dev"; then
    ok "dev$dev kernel 有效, 跳过部署(同卡双 die 共享)"
    return 0
  fi
  warn "dev$dev kernel 缺失, 重新部署 ..."
  pkill -f './deployer ' 2>/dev/null && sleep 2
  kfc "$dev" "rm -f /home/CustAiCpuUser/lib/libmf_overlap*" || fail "dev$dev 清旧文件失败"
  ./deployer "$dev" "$DEP_JSON" "$ALIVE_SECS" >>"$LOG_DIR/deployer_e0c.log" 2>&1 &
  local pid=$!
  sleep 20
  kfc "$dev" "cat /home/CustAiCpuUser/lib/libmf_overlap_aicpu.so.* > $DEV_SO" || { kill $pid 2>/dev/null; fail "dev$dev 复制失败"; }
  kfc "$dev" "chmod 644 $DEV_SO" || { kill $pid 2>/dev/null; fail "dev$dev chmod 失败"; }
  do_verify "$dev" || { kill $pid 2>/dev/null; fail "dev$dev 验证门失败"; }
  ok "dev$dev 部署完成 (deployer pid=$pid 保活中)"
}

# ---------- 主流程 ----------
source "$CANN_HOME/set_env.sh" >/dev/null 2>&1
make host_tools kernel probe probe_e0b probe_e0c KVER="$KVER" >/dev/null || fail "编译失败"

ensure_deploy "$DEVA"

rm -rf /tmp/e0c_pool 2>/dev/null
echo "=== 运行 E0c 双 rank 协同 (r0=dev$DEVA r1=dev$DEVB) ==="
export ASCEND_HOME_PATH="$CANN_HOME"
export LD_LIBRARY_PATH="$CANN_HOME/aarch64-linux/lib64:/usr/local/Ascend/driver/lib64/driver:/usr/local/Ascend/driver/lib64/common"
./probe_e0c 0 "aicpu_kernel/libmf_overlap_v${KVER}.json" > "$LOG_DIR/e0c_r0.log" 2>&1 &
P0=$!
./probe_e0c 1 "aicpu_kernel/libmf_overlap_v${KVER}.json" > "$LOG_DIR/e0c_r1.log" 2>&1 &
P1=$!
wait $P0; R0=$?
wait $P1; R1=$?
grep -E "OK|INFO|launch|同步|status|mismatch|标志|PASS|FAIL" "$LOG_DIR/e0c_r0.log"
echo "--- r1 ---"
grep -E "OK|INFO|launch|同步|status|PASS|FAIL" "$LOG_DIR/e0c_r1.log"

if [[ $R0 -eq 0 && $R1 -eq 0 ]]; then
  ok "E0c 通过: 池上 AICore×AICPU 八步语义全链成立"
  exit 0
fi
fail "E0c 未通过 (r0=$R0 r1=$R1) —— 按 AICore status 面包屑/AICPU mainRet 定位"
