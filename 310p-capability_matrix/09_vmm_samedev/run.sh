#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
make
D=${DEV:-0}
echo "[run] DEV=$D (both ranks same device, timeout 60s)"
ret=0
DEV=$D timeout --kill-after=10 60 mpirun --allow-run-as-root -np 2 ./test </dev/null || ret=$?
echo "[run] exit=$ret (124=timeout/hang)"
exit "$ret"
