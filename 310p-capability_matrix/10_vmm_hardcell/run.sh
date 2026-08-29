#!/bin/bash
cd "$(dirname "$0")"
make >/dev/null 2>&1
A=${A_DEV:-1} B=${B_DEV:-4}
echo "[run] A_DEV=$A B_DEV=$B (cross-card VMM import)"
A_DEV=$A B_DEV=$B timeout 60 mpirun --allow-run-as-root -np 2 ./test
echo "[run] exit=$? (124=timeout/hang)"
