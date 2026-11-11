#!/bin/bash
set -uo pipefail
cd "$(dirname "$0")"
make || exit $?
timeout --kill-after=10 60 mpirun --allow-run-as-root --stdin none -np 2 ./test
status=$?
echo "[run] exit=$status (124=timeout/hang)"
exit "$status"
