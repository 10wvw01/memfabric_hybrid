#!/bin/bash
# repro_207001 — minimal demo to reproduce AclrtMallocPhysical ret:207001
#
# Reproduces the failure seen in memcache_npu_test at:
#   hybm_vmm_based_segment.cpp MallocFromDevice
#   AclrtMallocPhysical failed, ret:207001
#
# Usage:
#   bash repro_207001_run.sh [device_id] [visible_devices]
#   bash repro_207001_run.sh 0 2,3
set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
CANN="${ASCEND_HOME_PATH:-/usr/local/Ascend/cann-9.1.0}"
DEVICE_ID="${1:-0}"
VISIBLE_DEVICES="${2:-2,3}"

export ASCEND_HOME_PATH="$CANN"
export LD_LIBRARY_PATH="/usr/local/Ascend/driver/lib64/driver:$CANN/aarch64-linux/lib64:$CANN/lib64:${LD_LIBRARY_PATH:-}"
export ASCEND_RT_VISIBLE_DEVICES="$VISIBLE_DEVICES"

echo "=== Environment ==="
echo "REPO=$REPO"
echo "CANN=$CANN"
echo "ASCEND_RT_VISIBLE_DEVICES=$ASCEND_RT_VISIBLE_DEVICES"
echo "deviceId(logical)=$DEVICE_ID"
echo ""

echo "=== NPU status ==="
npu-smi info 2>&1 | grep -E '310P3|Hugepages' | head -8
echo ""

echo "=== Build ==="
gcc -Wall -o "$SCRIPT_DIR/repro_207001" "$SCRIPT_DIR/repro_207001.c" \
    -I"$CANN/aarch64-linux/include" \
    -L"$CANN/aarch64-linux/lib64" -lascendcl
if [ $? -ne 0 ]; then
    echo "[FAIL] build failed"
    exit 1
fi
echo "[OK] build"
echo ""

echo "=== Run (deviceId=$DEVICE_ID) ==="
"$SCRIPT_DIR/repro_207001" "$DEVICE_ID" 2>&1 | tee "$SCRIPT_DIR/repro_207001.log"
echo ""

echo "=== Done. Log saved to $SCRIPT_DIR/repro_207001.log ==="
