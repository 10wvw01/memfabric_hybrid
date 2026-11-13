#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
make
A=${A_DEV:-1} B=${B_DEV:-4}
echo "[run] A_DEV=$A B_DEV=$B (cross-card VMM import)"
ret=0
A_DEV=$A B_DEV=$B timeout --kill-after=10 60 mpirun --allow-run-as-root -np 2 ./test </dev/null || ret=$?
echo "[run] exit=$ret (124=timeout/hang)"
exit "$ret"
