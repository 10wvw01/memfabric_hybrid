#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# T0/T1/T2/V1 回归脚本（已纳入 G3 修复），使用设备 0、1。
set -uo pipefail
OUT=/tmp/opencode/g3/regression
mkdir -p "$OUT"
# 从脚本位置解析仓库根目录，不依赖旧实机绝对路径。
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../../.." && pwd)"
cd "$REPO_ROOT"
source /usr/local/Ascend/cann-9.1.0/set_env.sh
source /usr/local/memfabric_hybrid/set_env.sh
export ASCEND_RT_VISIBLE_DEVICES=0,1
export MF_SDMA_ORCH_JSON=/usr/local/memfabric_hybrid/latest/aarch64-linux/hybm/aicpu_kernel/libmf_sdma_orch_v11.json
export VLLM_ASCEND_310P_MEMFABRIC_STORE_URL=tcp://127.0.0.1:8581
# 不启用 MF_KFC_BYPASS；当前健康部署走独立 helper。
# T0 独立驱动未由实机代理入库；只有明确指定
# 现存诊断驱动目录时才执行，禁止加载旧路径的共享库。
DIAG_DIR="${MF310P_V2_DIAG_DRIVER_DIR:-}"
if [ -n "$DIAG_DIR" ] && [ ! -f "$DIAG_DIR/v2_trial.py" ]; then
    echo "invalid MF310P_V2_DIAG_DRIVER_DIR: missing v2_trial.py" >&2
    exit 2
fi

run() {
    name=$1; shift
    echo "=== $name start $(date +%H:%M:%S) ==="
    (cd "$DIAG_DIR" && timeout 1200 torchrun --standalone --nproc-per-node=2 "$@" ) > "$OUT/$name.log" 2>&1
    rc=$?
    tail -2 "$OUT/$name.log" | head -1
    echo "=== $name rc=$rc $(date +%H:%M:%S) ===" | tee -a "$OUT/status.txt"
    sleep 45
}

if [ -n "$DIAG_DIR" ]; then
# T0 G0：官方 IB 探针。
run t0_g0 v2_trial.py --mode g0 --chunks 1024 --reps 1000
# T0 G1：串行多 Chunk 位精确性，减少重复次数。
run t0_g1 v2_trial.py --mode g1 --chunks 256 512 1024 --rows 256 512 1024 --reps 100
# T0 G2：在 M=2048/4096/8192 比较串行与交叠。
run t0_g2 v2_trial.py --mode g2 --chunks 1024 --rows 2048 4096 8192 --reps 20
# T0 Stock 性能基线。
run t0_stock v2_trial.py --mode stock --chunks 1024 --rows 2048 4096 8192 --reps 20
else
  echo "T0 standalone driver not in repository; SKIP G0/G1/G2/Stock (set MF310P_V2_DIAG_DRIVER_DIR)" | tee -a "$OUT/status.txt"
fi
# T1：小 M 原始 NZ 直接 ND 输出测试。
(timeout 1200 torchrun --standalone --nproc-per-node=2 \
  csrc/_310P/memfabric_mm_ar_v2/test/t1_nz_direct_trial.py --mode full --rows 1 2 4 8 16 32 64 128 255 256 --repeat 20 ) > "$OUT/t1.log" 2>&1
echo "t1 rc=$?" | tee -a "$OUT/status.txt"
sleep 45
# T2：AR-only 压力测试，含 AICore 暂存与协议状态校验。
timeout 1200 torchrun --standalone --nproc-per-node=2 \
  csrc/_310P/memfabric_mm_ar_v2/test/t2_ar_stress.py --chunk-rows 1024 --repeat 100 \
  > "$OUT/t2.log" 2>&1
echo "t2 rc=$?" | tee -a "$OUT/status.txt"
sleep 45
# V1 层级基准回归，覆盖所有指定 M 的位精确结果。
timeout 1800 torchrun --standalone --nproc-per-node=2 \
  benchmarks/scripts/bench_310p_memfabric_mm_ar_layer.py \
  --rows 255 256 257 512 1024 2048 4096 8192 --repeat 5 > "$OUT/v1_bench.log" 2>&1
echo "v1 rc=$?" | tee -a "$OUT/status.txt"
echo "REGRESSION DONE"
