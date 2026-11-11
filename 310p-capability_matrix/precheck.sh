#!/usr/bin/env bash
# Print the current environment; capability outcomes come from the individual tests.
set -euo pipefail
printf '=== CANN ===\n'
readlink -f "${ASCEND_HOME:-${ASCEND_HOME_PATH:-/usr/local/Ascend/cann}}"
printf '=== Devices and processes ===\n'
npu-smi info
printf '=== Container processes in uninterruptible sleep ===\n'
ps -eo pid,ppid,stat,comm | awk 'NR == 1 || $3 ~ /^D/'
