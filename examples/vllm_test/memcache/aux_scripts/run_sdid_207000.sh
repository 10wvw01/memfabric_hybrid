#!/bin/bash
# repro_sdid_207000 — minimal repro for HYBM "get sdid failed: 207000"
#
# Reproduces: store.init() fails because RtGetDeviceInfo(INFO_TYPE_SDID)
# returns 107000 (ACL_ERROR_INVALID_DEVICE), preventing HYBM segment init.
#
# Usage:
#   bash run_sdid_207000.sh [device_id] [protocol] [hbm_size]
#
# Examples:
#   bash run_sdid_207000.sh 6 device_sdma 2GB
#   bash run_sdid_207000.sh 6 host_shm 0
#   bash run_sdid_207000.sh 0 device_sdma 2GB
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
CANN="${ASCEND_HOME_PATH:-/usr/local/Ascend/cann-9.1.0}"

DEVICE_ID="${1:-6}"
PROTOCOL="${2:-device_sdma}"
HBM_SIZE="${3:-2GB}"

export ASCEND_HOME_PATH="$CANN"
export LD_LIBRARY_PATH="${REPO}/output/memcache/lib64:${LD_LIBRARY_PATH:-}"

# Auto-detect Python site-packages lib paths
MEMCACHE_LIB=$(python3 -c "import memcache_hybrid, os; print(os.path.dirname(memcache_hybrid.__file__)+'/lib')" 2>/dev/null || echo "")
MEMFABRIC_LIB=$(python3 -c "import memfabric_hybrid, os; print(os.path.dirname(memfabric_hybrid.__file__)+'/lib')" 2>/dev/null || echo "")

if [[ -n "$MEMCACHE_LIB" && -d "$MEMCACHE_LIB" ]]; then
    export LD_LIBRARY_PATH="${MEMCACHE_LIB}:${LD_LIBRARY_PATH}"
fi
if [[ -n "$MEMFABRIC_LIB" && -d "$MEMFABRIC_LIB" ]]; then
    export LD_LIBRARY_PATH="${MEMFABRIC_LIB}:${LD_LIBRARY_PATH}"
fi

export LD_LIBRARY_PATH="/usr/local/Ascend/driver/lib64:${CANN}/lib64:${LD_LIBRARY_PATH}"

echo "=== Environment ==="
echo "CANN=$CANN"
echo "REPO=$REPO"
echo "DEVICE_ID=$DEVICE_ID"
echo "PROTOCOL=$PROTOCOL"
echo "HBM_SIZE=$HBM_SIZE"
echo ""

echo "=== NPU status ==="
npu-smi info 2>&1 | grep -E '310P3|Hugepages' | head -8
echo ""

echo "=== Run repro ==="
python3 "$SCRIPT_DIR/repro_sdid_207000.py" "$DEVICE_ID" "$PROTOCOL" "$HBM_SIZE" 2>&1 | tee "$SCRIPT_DIR/repro_sdid_207000.log"
echo ""

echo "=== Done. Log saved to $SCRIPT_DIR/repro_sdid_207000.log ==="
