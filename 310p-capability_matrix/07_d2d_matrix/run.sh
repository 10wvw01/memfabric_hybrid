#!/bin/bash
set -uo pipefail
cd "$(dirname "$0")"
case "${MATRIX_N:-8}" in 4|8) ;; *) echo "MATRIX_N must be 4 or 8" >&2; exit 2 ;; esac
make -B MATRIX_N="${MATRIX_N:-8}" || exit $?
echo "[run] ${MATRIX_N:-8} NPU IPC pool (timeout --kill-after=10 120s)"
timeout --kill-after=10 120 mpirun --allow-run-as-root --stdin none -np "${MATRIX_N:-8}" ./test
status=$?
echo "[run] exit=$status (124=timeout/hang)"
exit "$status"
