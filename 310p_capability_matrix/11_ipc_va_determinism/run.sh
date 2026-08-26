#!/bin/bash
cd "$(dirname "$0")"
make >/dev/null 2>&1
DEV_LIST=${DEV_LIST:-"1,3,1"} timeout 60 mpirun --allow-run-as-root -np 3 ./test
echo "[run] exit=$? (124=timeout/hang)"
