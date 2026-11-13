#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
make
ret=0
DEV_LIST=${DEV_LIST:-"1,3,1"} timeout --kill-after=10 60 mpirun --allow-run-as-root -np 3 ./test </dev/null || ret=$?
echo "[run] exit=$ret (124=timeout/hang)"
exit "$ret"
