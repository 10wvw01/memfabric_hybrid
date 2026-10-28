#!/bin/bash
# clean_stale_env.sh — Clean stale processes and IPC resources before MemCache tests.
#
# Stale Python/mmc_meta processes from previous test runs can hold config store
# connections (rankId:N reconnect:1), filling aliveRankSet_ and causing
# "AutoRanking failed, dataSize: 0" on the next run.
#
# Usage: bash debug/test/clean_stale_env.sh

set -u

echo "=== 1. Kill stale MetaService ==="
ps aux | grep -E 'mmc_meta_service|MetaService\.main' | grep -v grep | awk '{print $2}' | while read -r pid; do
    echo "  killing mmc_meta_service PID $pid"
    kill -9 "$pid" 2>/dev/null || true
done

echo "=== 2. Kill stale Python test processes (store.init / pytest / repro) ==="
ps aux | grep -E 'python.*store\.init|python.*pytest.*workload|python.*repro_store' | grep -v grep | awk '{print $2}' | while read -r pid; do
    echo "  killing stale python PID $pid"
    kill -9 "$pid" 2>/dev/null || true
done

echo "=== 3. Kill stale VLLM processes ==="
pkill -9 -f 'VLLM::' 2>/dev/null && echo "  killed VLLM processes" || echo "  no VLLM processes"

echo "=== 4. Clean shared memory and IPC ==="
rm -f /dev/shm/mf_dram_* /dev/shm/memfabric_hybrid_* /dev/shm/psm_* /dev/shm/sem.mp-* 2>/dev/null
echo "  cleaned /dev/shm"

# Clean System V IPC (if any)
ipcs -m 2>/dev/null | awk 'NR>3 && $2 ~ /^[0-9]+$/ {print $2}' | xargs -r ipcrm -m 2>/dev/null
ipcs -s 2>/dev/null | awk 'NR>3 && $2 ~ /^[0-9]+$/ {print $2}' | xargs -r ipcrm -s 2>/dev/null

echo "=== 5. Verify clean ==="
sleep 1
REMAINING=$(ps aux | grep -E 'mmc_meta_service|MetaService\.main' | grep -v grep | wc -l)
echo "  remaining mmc_meta processes: $REMAINING"
SHM_FILES=$(ls /dev/shm/mf_dram_* /dev/shm/memfabric_hybrid_* 2>/dev/null | wc -l)
echo "  remaining shm files: $SHM_FILES"

if [[ "$REMAINING" -eq 0 && "$SHM_FILES" -eq 0 ]]; then
    echo "=== CLEAN ==="
else
    echo "=== WARNING: some resources remain ==="
fi
