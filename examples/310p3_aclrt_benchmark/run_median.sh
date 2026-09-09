#!/bin/bash
# 310P3 Bare ACL Benchmark — run 5 times, extract key data, compute median
#
# Usage: ./run_median.sh [devA] [devB]
#   devA: rank0 device ID (default: 0)
#   devB: rank1 device ID (default: 1)

set -e

CURRENT_DIR=$(cd $(dirname ${BASH_SOURCE:-$0}) && pwd)

DEV_A="${1:-0}"
DEV_B="${2:-1}"

BIN="${CURRENT_DIR}/310p3_aclrt_benchmark"
if [ ! -f "${BIN}" ]; then
    echo "[run] binary not found, building..."
    bash ${CURRENT_DIR}/build.sh
fi

CANN_PATH="${ASCEND_HOME_PATH:-/usr/local/Ascend/ascend-toolkit/latest}"
export ASCEND_HOME_PATH="${CANN_PATH}"
export LD_LIBRARY_PATH="${CANN_PATH}/aarch64-linux/lib64:${LD_LIBRARY_PATH:-}"

RUNS=5
TMPDIR=$(mktemp -d)
trap "rm -rf ${TMPDIR}" EXIT

echo "=== Running ${RUNS} iterations: dev${DEV_A} + dev${DEV_B} ==="
for i in $(seq 1 ${RUNS}); do
    echo "--- Run ${i}/${RUNS} ---"
    mpirun --allow-run-as-root -np 2 \
        -x ASCEND_HOME_PATH \
        -x LD_LIBRARY_PATH \
        ${BIN} ${DEV_A} ${DEV_B} > "${TMPDIR}/run_${i}.txt" 2>&1 || true
done

python3 - "${TMPDIR}" "${DEV_A}" "${DEV_B}" "${RUNS}" << 'PYEOF'
import os, sys, re

tmpdir, dev_a, dev_b, runs = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
dev_a, dev_b = int(dev_a), int(dev_b)

bs_list = ["1MB", "4MB", "16MB", "64MB", "128MB"]

def parse_file(path):
    """Extract single-rank and concurrent H2D/D2H bandwidth values."""
    single = {}   # bs -> (h2d_single, d2h_single)
    conc = {}     # bs -> (devA_h2d, devB_h2d, agg_h2d, devA_d2h, devB_d2h, agg_d2h)
    in_conc = False
    in_h2d_d2h = False
    with open(path) as f:
        for line in f:
            line = line.rstrip()
            if "=== Concurrent" in line:
                in_conc = True
                in_h2d_d2h = False
                continue
            if "H2D single-sync" in line:
                in_h2d_d2h = True
                continue
            if "D2D single-sync" in line:
                in_h2d_d2h = False
                continue
            if not re.match(r'^(1|4|16|64|128)MB\s', line):
                continue
            parts = line.split()
            bs = parts[0]
            if bs not in bs_list:
                continue
            nums = [p for p in parts[1:] if re.match(r'^\d+\.\d+$', p)]
            if in_conc:
                if len(nums) >= 6:
                    conc[bs] = (float(nums[0]), float(nums[1]), float(nums[2]),
                               float(nums[3]), float(nums[4]), float(nums[5]))
            elif in_h2d_d2h:
                if len(nums) >= 4:
                    single[bs] = (float(nums[0]), float(nums[2]))
    return single, conc

all_single = []
all_conc = []
for i in range(1, runs + 1):
    s, c = parse_file(os.path.join(tmpdir, f"run_{i}.txt"))
    all_single.append(s)
    all_conc.append(c)

def median(vals):
    vals = sorted(vals)
    n = len(vals)
    if n == 0:
        return 0.0
    if n % 2 == 1:
        return vals[n // 2]
    return (vals[n // 2 - 1] + vals[n // 2]) / 2.0

print()
print(f"topology: dev{dev_a} + dev{dev_b}  ({'same-card cross-die' if dev_a // 2 == dev_b // 2 else 'cross-card'})")
print()

# Single-rank table
print("=== Single-rank H2D/D2H (single-sync, median of 5 runs) ===")
print(f"{'BlockSize':<12} {'H2D-single (GB/s)':<20} {'D2H-single (GB/s)':<20}")
print("-" * 52)
single_med = {}
for bs in bs_list:
    h2d_vals = [all_single[i].get(bs, (0, 0))[0] for i in range(runs)]
    d2h_vals = [all_single[i].get(bs, (0, 0))[1] for i in range(runs)]
    h2d_med = median(h2d_vals)
    d2h_med = median(d2h_vals)
    single_med[bs] = (h2d_med, d2h_med)
    print(f"{bs:<12} {h2d_med:<20.1f} {d2h_med:<20.1f}")

print()

# Concurrent table
print("=== Concurrent H2D/D2H (both ranks, single-sync, median of 5 runs) ===")
print(f"{'BlockSize':<12} {'devA-H2D':<10} {'devB-H2D':<10} {'agg-H2D':<10} {'H2D-ratio':<10} {'devA-D2H':<10} {'devB-D2H':<10} {'agg-D2H':<10} {'D2H-ratio':<10}")
print("-" * 92)
for bs in bs_list:
    devA_h2d_vals = [all_conc[i].get(bs, (0,)*6)[0] for i in range(runs)]
    devB_h2d_vals = [all_conc[i].get(bs, (0,)*6)[1] for i in range(runs)]
    agg_h2d_vals  = [all_conc[i].get(bs, (0,)*6)[2] for i in range(runs)]
    devA_d2h_vals = [all_conc[i].get(bs, (0,)*6)[3] for i in range(runs)]
    devB_d2h_vals = [all_conc[i].get(bs, (0,)*6)[4] for i in range(runs)]
    agg_d2h_vals  = [all_conc[i].get(bs, (0,)*6)[5] for i in range(runs)]
    devA_h2d_m = median(devA_h2d_vals)
    devB_h2d_m = median(devB_h2d_vals)
    agg_h2d_m  = median(agg_h2d_vals)
    devA_d2h_m = median(devA_d2h_vals)
    devB_d2h_m = median(devB_d2h_vals)
    agg_d2h_m  = median(agg_d2h_vals)
    h2d_ratio = devB_h2d_m / single_med[bs][0] if single_med[bs][0] > 0 else 0
    d2h_ratio = devB_d2h_m / single_med[bs][1] if single_med[bs][1] > 0 else 0
    print(f"{bs:<12} {devA_h2d_m:<10.1f} {devB_h2d_m:<10.1f} {agg_h2d_m:<10.1f} {h2d_ratio:<10.2f} {devA_d2h_m:<10.1f} {devB_d2h_m:<10.1f} {agg_d2h_m:<10.1f} {d2h_ratio:<10.2f}")

print()
print(f"H2D-ratio = dev{dev_b}-H2D(concurrent median) / dev{dev_b}-H2D(single median); ~1.0x: no contention, <1.0x: contention")
PYEOF
