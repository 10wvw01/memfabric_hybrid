#!/bin/bash
# run_e0a.sh — E0a 一键验收：部署 MemFabric 协同编排 kernel 到目标设备并跑验收探针
# 用法: bash run_e0a.sh [dev]        (默认 dev0)
#       bash run_e0a.sh clean [dev]  (清理设备侧文件 + 杀 deployer)
#
# 部署纪律(全部为 xcclpp 实测沉淀, 见 310p-aicore-aicpu-开发计划.md 附录 A.4):
#   1. 同一时刻只跑一个 deployer(并发互斥: 后起的假成功不落盘)
#   2. deployer json 喂 CUSTAICPUKernel(mode=1 才落真 ELF), launch json 喂 AICPUKernel(cpuMode=0)
#   3. 复制时改版本名(PID 后缀多版本 glob 拼接 = 损坏 ELF)
#   4. 部署粒度 = 每卡一次(同卡双 die 共享设备 OS 文件系统, X9 实测)
#   5. KFC sync= 命令退出码(0→sync=0, 非零→507018); 重试只对真偶发失败有意义
set -uo pipefail
cd "$(dirname "$0")"

CANN_HOME=${CANN_HOME:-/usr/local/Ascend/cann-9.1.0}
DEV=${2:-0}
KVER=${KVER:-1}
DEP_SO="aicpu_kernel/libmf_overlap_aicpu.so"     # CUST 迁移源 so
DEP_JSON="aicpu_kernel/libmf_overlap_aicpu.json" # deployer 喂的 CUST json
DEV_SO="/usr/lib64/aicpu_kernels/0/aicpu_kernels_device/libmf_overlap_v${KVER}.so" # CP1 搜索路径目标
ALIVE_SECS=${ALIVE_SECS:-900}
DEP_PID=""

ok()   { echo -e "  \e[32m[OK]\e[0m  $*"; }
warn() { echo -e "  \e[33m[WARN]\e[0m $*"; }
fail() { echo -e "  \e[31m[FAIL]\e[0m $*"; exit 1; }

# KFC 封装: 90s 超时 + 3 次重试(间隔 10s), 判据 = 输出含 sync=0
kfc() {  # kfc <dev> <cmd...>  → 0=sync=0
  local dev=$1; shift; local cmd="$*"; local out rc i
  for i in 1 2 3; do
    out=$(timeout 90 ./mini_kfc "$dev" "$cmd" 2>&1); rc=$?
    if [[ $rc -eq 0 && "$out" == *"sync=0"* ]]; then return 0; fi
    warn "KFC 第${i}次未过 (rc=$rc): ${cmd:0:60}..."
    [[ $i -lt 3 ]] && sleep 10
  done
  return 1
}

kill_deployers() { pkill -f './deployer ' 2>/dev/null && warn "已清理旧 deployer" || true; sleep 2; }
on_exit() { [[ -n "$DEP_PID" ]] && kill "$DEP_PID" 2>/dev/null; }
trap on_exit EXIT

# 验证门: 目标文件存在且含入口符号(两道都过才有效)
do_verify() {
  kfc "$1" "test -f $DEV_SO" || return 1
  kfc "$1" "grep -q HybmOverlapRun $DEV_SO" || return 1
  return 0
}

do_deploy() {
  local dev=$1
  if do_verify "$dev"; then
    ok "dev$dev kernel 已在且有效, 跳过部署 (重部署: 先 clean)"
    return 0
  fi
  log_msg() { echo "  $*"; }
  log_msg "部署 kernel 到 dev$dev ..."
  kill_deployers
  kfc "$dev" "rm -f /home/CustAiCpuUser/lib/libmf_overlap*" || fail "dev$dev 清旧文件失败"
  ./deployer "$dev" "$DEP_JSON" "$ALIVE_SECS" &
  DEP_PID=$!
  log_msg "deployer 已启动 (pid=$DEP_PID, 保活 ${ALIVE_SECS}s), 等待落盘 ..."
  sleep 20
  # 复制时改版本名: glob 只匹配本 deployer 落的单版本文件, 避免 PID 后缀多版本拼接
  kfc "$dev" "cat /home/CustAiCpuUser/lib/libmf_overlap_aicpu.so.* > $DEV_SO" \
    || fail "dev$dev 复制失败 (deployer 假成功? 确认无其他 deployer)"
  kfc "$dev" "chmod 644 $DEV_SO" || fail "dev$dev chmod 失败"
  do_verify "$dev" || fail "dev$dev 验证门失败"
  ok "dev$dev 部署完成"
}

do_clean() {
  local dev=$1
  kill_deployers
  kfc "$dev" "rm -f /usr/lib64/aicpu_kernels/0/aicpu_kernels_device/libmf_overlap*.so /home/CustAiCpuUser/lib/libmf_overlap*" \
    && ok "dev$dev overlap 文件已清 (xcclpp 的 so 不受影响)" || warn "dev$dev 清理未完全"
}

# ---------- 主流程 ----------
source "$CANN_HOME/set_env.sh" >/dev/null 2>&1
make host_tools kernel probe KVER="$KVER" >/dev/null || fail "编译失败"

if [[ "${1:-run}" == "clean" ]]; then
  do_clean "${2:-0}"
  exit 0
fi

DEV=${1:-0}
do_deploy "$DEV"
echo "=== 运行 E0a 验收探针 (dev$DEV) ==="
./probe_e0a "$DEV" "aicpu_kernel/libmf_overlap_v${KVER}.json" || fail "E0a 探针未通过"
ok "E0a 验收通过: MemFabric 构建的协同编排 kernel 已在 CP1 内执行并完成 SQE 直投闭环"
