#!/bin/bash
cd "$(dirname "$0")"
make >/dev/null 2>&1
D=${DEV:-0}
echo "[run] DEV=$D (both ranks same device, timeout 60s)"
DEV=$D timeout 60 mpirun --allow-run-as-root -np 2 ./test
echo "[run] exit=$? (124=timeout/hang)"
