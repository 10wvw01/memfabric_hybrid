#!/bin/bash
# run_e0b.sh — E0b 一键执行：确保 dev0 kernel 就绪 + 双 rank 池上跑池 GVA 探针
# 用法: bash run_e0b.sh [devA devB]   (默认 0 1, 须为同卡 die 对——本实验数据面为跨 die)
#
# 判读(run_e0b 输出):
#   轮A(纯本地src→peer池GVA) + 轮B(own池GVA→peer池GVA) 双 PASS = E0b 通过,数据面架构成立
#   仅轮A过 = 本端 AclVmm 窗口 VA 不被 SDMA 翻译(触发计划 Plan B 复核)
#   轮A败   = 导入 VA 不可译 → Plan B(IPC Case A 段层导入模式)
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

# KFC 封装: 90s 超时 + 3 次重试(间隔 10s), 判据 = 输出含 sync=0
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

# 部署保障(E0a 的 deployer 已随 trap 退出 → 清理链会延迟删除 /0/ 副本, 跑前必验)
ensure_deploy() {
  local dev=$1
  if do_verify "$dev"; then
    ok "dev$dev kernel 有效, 跳过部署"
    return 0
  fi
  warn "dev$dev kernel 缺失(清理链已删?), 重新部署 ..."
  pkill -f './deployer ' 2>/dev/null && sleep 2
  kfc "$dev" "rm -f /home/CustAiCpuUser/lib/libmf_overlap*" || fail "dev$dev 清旧文件失败"
  ./deployer "$dev" "$DEP_JSON" "$ALIVE_SECS" >>"$LOG_DIR/deployer_e0b.log" 2>&1 &
  local pid=$!
  sleep 20
  kfc "$dev" "cat /home/CustAiCpuUser/lib/libmf_overlap_aicpu.so.* > $DEV_SO" || { kill $pid 2>/dev/null; fail "dev$dev 复制失败"; }
  kfc "$dev" "chmod 644 $DEV_SO" || { kill $pid 2>/dev/null; fail "dev$dev chmod 失败"; }
  do_verify "$dev" || { kill $pid 2>/dev/null; fail "dev$dev 验证门失败"; }
  ok "dev$dev 部署完成 (deployer pid=$pid 保活中, 实验结束后自动回收)"
}

# ---------- 主流程 ----------
source "$CANN_HOME/set_env.sh" >/dev/null 2>&1
make host_tools kernel probe probe_e0b KVER="$KVER" >/dev/null || fail "编译失败"

ensure_deploy "$DEVA"

rm -f /tmp/e0b_pool -r 2>/dev/null
echo "=== 运行 E0b 双 rank (r0=dev$DEVA r1=dev$DEVB) ==="
export ASCEND_HOME_PATH="$CANN_HOME"
export LD_LIBRARY_PATH="$CANN_HOME/aarch64-linux/lib64:/usr/local/Ascend/driver/lib64/driver:/usr/local/Ascend/driver/lib64/common"
./probe_e0b 0 "aicpu_kernel/libmf_overlap_v${KVER}.json" > "$LOG_DIR/e0b_r0.log" 2>&1 &
P0=$!
./probe_e0b 1 "aicpu_kernel/libmf_overlap_v${KVER}.json" > "$LOG_DIR/e0b_r1.log" 2>&1 &
P1=$!
wait $P0; R0=$?
wait $P1; R1=$?
tail -n +1 "$LOG_DIR/e0b_r0.log" | grep -E "OK|INFO|轮|PASS|FAIL"
echo "--- r1 ---"
tail -n +1 "$LOG_DIR/e0b_r1.log" | grep -E "OK|INFO|轮|PASS|FAIL"

if [[ $R0 -eq 0 && $R1 -eq 0 ]]; then
  ok "E0b 通过: AclVmm 池 GVA 可作 SQE src/dst —— 数据面架构成立"
  exit 0
fi
fail "E0b 未通过 (r0=$R0 r1=$R1) —— 按轮A/轮B 结果判读 Plan B"
