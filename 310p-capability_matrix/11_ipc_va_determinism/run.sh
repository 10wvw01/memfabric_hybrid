#!/bin/bash
set -uo pipefail
cd "$(dirname "$0")"
make || exit $?
DEV_LIST=${DEV_LIST:-"1,3,1"} timeout --kill-after=10 60 mpirun --allow-run-as-root --stdin none -np 3 ./test
status=$?
echo "[run] exit=$status (124=timeout/hang)"
exit "$status"
