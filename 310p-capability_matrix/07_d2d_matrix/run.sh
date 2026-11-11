#!/bin/bash
cd "$(dirname "$0")"
make >/dev/null 2>&1
echo "[run] 8 NPU IPC pool (timeout 120s)"
timeout 120 mpirun --allow-run-as-root -np 8 ./test
echo "[run] exit=$? (124=timeout/hang)"
