#!/bin/bash
cd "$(dirname "$0")"
if ! make >/dev/null 2>&1; then
    echo "[run] build FAILED:"
    make
    exit 1
fi
timeout 60 mpirun --allow-run-as-root -np 2 ./test
echo "[run] exit=$? (124=timeout/hang)"
