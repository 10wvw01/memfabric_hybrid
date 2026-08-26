#!/bin/bash
cd "$(dirname "$0")"
make -f Makefile.reverse >/dev/null 2>&1
A=${A_DEV:-0} B=${B_DEV:-1}
echo "[run-reverse] A_DEV=$A B_DEV=$B  (timeout 60s)"
A_DEV=$A B_DEV=$B timeout 60 mpirun --allow-run-as-root -np 2 ./test_reverse
echo "[run-reverse] exit=$? (124=timeout/hang)"
