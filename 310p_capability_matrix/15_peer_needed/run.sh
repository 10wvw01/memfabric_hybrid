#!/bin/bash
cd "$(dirname "$0")"
make >/dev/null 2>&1
A=${A_DEV:-0} B=${B_DEV:-2}
for M in DEFAULT ENABLE PEER; do
  echo "[run] MODE=$M A_DEV=$A B_DEV=$B (timeout 60s)"
  A_DEV=$A B_DEV=$B MODE=$M timeout 60 mpirun --allow-run-as-root -np 2 ./test
  echo "[run] MODE=$M exit=$? (124=timeout/hang)"
  echo
done
