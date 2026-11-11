#!/bin/bash
set -uo pipefail
cd "$(dirname "$0")"
if ! make >/dev/null 2>&1; then
    echo "[run] build FAILED:"
    make
    exit 1
fi
timeout --kill-after=10 60 mpirun --allow-run-as-root --stdin none -np 2 ./test
status=$?
echo "[run] exit=$status (124=timeout/hang)"
exit "$status"
