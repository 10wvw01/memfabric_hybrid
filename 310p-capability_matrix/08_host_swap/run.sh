#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
if ! make; then
    echo "[run] build FAILED:"
    make
    exit 1
fi
ret=0
timeout --kill-after=10 60 mpirun --allow-run-as-root -np 2 ./test </dev/null || ret=$?
echo "[run] exit=$ret (124=timeout/hang)"
exit "$ret"
