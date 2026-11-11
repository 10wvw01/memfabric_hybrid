#!/bin/bash
set -uo pipefail
cd "$(dirname "$0")"
make || exit $?
A=${A_DEV:-0} B=${B_DEV:-2}
result=0
for M in DEFAULT ENABLE ENABLE_R0 ENABLE_BOTH PEER; do
  echo "[run] MODE=$M A_DEV=$A B_DEV=$B (timeout --kill-after=10 60s)"
  A_DEV=$A B_DEV=$B MODE=$M timeout --kill-after=10 60 mpirun --allow-run-as-root --stdin none -np 2 ./test
  status=$?
  echo "[run] MODE=$M exit=$status (124=timeout/hang)"
  if (( status != 0 )); then result=$status; fi
  echo
done
exit "$result"
