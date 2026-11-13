#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
make
echo "[run] 8 NPU IPC pool (timeout 120s)"
ret=0
timeout --kill-after=10 120 mpirun --allow-run-as-root -np 8 ./test </dev/null || ret=$?
echo "[run] exit=$ret (124=timeout/hang)"
exit "$ret"
