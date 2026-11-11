#!/bin/bash
set -uo pipefail
cd "$(dirname "$0")"
make || exit $?
A=${A_DEV:-0} B=${B_DEV:-2}
echo "[run] A_DEV=$A B_DEV=$B  (timeout --kill-after=10 60s)"
A_DEV=$A B_DEV=$B timeout --kill-after=10 60 mpirun --allow-run-as-root --stdin none -np 2 ./test
status=$?
echo "[run] exit=$status (124=timeout/hang)"
exit "$status"
