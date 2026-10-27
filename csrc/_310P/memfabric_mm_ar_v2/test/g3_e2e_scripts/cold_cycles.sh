#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# TP2 混合路由测试连续新建 torchrun 进程 20 轮，验证冷启动健壮性。
# 逐轮记录通过/失败，区分 orchestrator Kernel 初次部署与
# 稳定使用阶段的建池初始化表现。
set -uo pipefail
OUT_DIR=${1:-/tmp/opencode/g3/cold_cycles}
DEVS=${DEVS:-2,3}
STORE_PORT=${STORE_PORT:-8582}
CYCLES=${CYCLES:-20}
mkdir -p "$OUT_DIR"
cd /home/USER/310P/vllm-ascend
source /usr/local/Ascend/cann-9.1.0/set_env.sh
source /usr/local/memfabric_hybrid/set_env.sh
export ASCEND_RT_VISIBLE_DEVICES=$DEVS
export VLLM_ASCEND_310P_MEMFABRIC_STORE_URL="tcp://127.0.0.1:${STORE_PORT}"
pass=0; fail=0
for i in $(seq 1 "$CYCLES"); do
    timeout 600 torchrun --standalone --nproc-per-node=2 \
        csrc/_310P/memfabric_mm_ar_v2/test/e2e/tp2_v2_mixed_route_correctness.py \
        --rows 2048 2049 3073 4095 4097 --chunk-rows 1024 --reps 1 \
        > "$OUT_DIR/cycle_$i.log" 2>&1
    rc=$?
    # 每设备固定地址的 meta 区为独占资源，进程退出后
    # 由运行时异步释放；2026-10-18 发现紧邻启动会
    # 连续触发 MetaSetDeviceAccess 507018/507899 错误。
    # 两轮之间必须等待资源排空再创建新池。
    sleep 75
    if [ $rc -eq 0 ] && grep -q "TP2 V2 mixed-route PASS" "$OUT_DIR/cycle_$i.log"; then
        pass=$((pass+1)); echo "cycle $i: PASS" | tee -a "$OUT_DIR/summary.txt"
    else
        fail=$((fail+1)); reason=$(grep -m1 -E "AssertionError|RuntimeError|Error" "$OUT_DIR/cycle_$i.log" | head -c 200)
        echo "cycle $i: FAIL rc=$rc $reason" | tee -a "$OUT_DIR/summary.txt"
    fi
done
echo "TOTAL pass=$pass fail=$fail" | tee -a "$OUT_DIR/summary.txt"
