#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
make
A=${A_DEV:-0} B=${B_DEV:-2}
echo "[run] A_DEV=$A B_DEV=$B  (timeout 60s)"
ret=0
A_DEV=$A B_DEV=$B timeout --kill-after=10 60 mpirun --allow-run-as-root -np 2 ./test </dev/null || ret=$?
echo "[run] exit=$ret (124=timeout/hang)"
exit "$ret"
