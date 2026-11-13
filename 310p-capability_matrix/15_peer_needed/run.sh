#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
make
A=${A_DEV:-0} B=${B_DEV:-2}
result=0
for M in DEFAULT ENABLE ENABLE_R0 ENABLE_BOTH PEER; do
  echo "[run] MODE=$M A_DEV=$A B_DEV=$B (timeout 60s)"
  ret=0
  A_DEV=$A B_DEV=$B MODE=$M timeout --kill-after=10 60 mpirun --allow-run-as-root -np 2 ./test </dev/null || ret=$?
  echo "[run] MODE=$M exit=$ret (124=timeout/hang)"
  if [ "$ret" -ne 0 ]; then result=1; fi
  echo
done
exit "$result"
