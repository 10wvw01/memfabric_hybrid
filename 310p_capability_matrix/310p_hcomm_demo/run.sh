#!/bin/bash
cd "$(dirname "$0")"
make >/dev/null 2>&1
echo "[run] hcomm-style 4 NPU IPC test (2 Duo cards, 1 socket, timeout 120s)"
timeout 120 mpirun --allow-run-as-root -np 4 ./test
echo "[run] exit=$? (124=timeout/hang)"
