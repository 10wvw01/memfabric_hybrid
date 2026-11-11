#!/bin/bash
set -uo pipefail
cd "$(dirname "$0")"
make || exit $?
D=${DEV:-0}
echo "[run] DEV=$D (both ranks same device, timeout --kill-after=10 60s)"
DEV=$D timeout --kill-after=10 60 mpirun --allow-run-as-root --stdin none -np 2 ./test
status=$?
echo "[run] exit=$status (124=timeout/hang)"
exit "$status"
