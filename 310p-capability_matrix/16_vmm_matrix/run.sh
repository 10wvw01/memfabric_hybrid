#!/bin/bash
set -uo pipefail
cd "$(dirname "$0")"
case "${MATRIX_N:-8}" in 4|8) ;; *) echo "MATRIX_N must be 4 or 8" >&2; exit 2 ;; esac
make -B MATRIX_N="${MATRIX_N:-8}" || exit $?
MB=${SLAB_MB:-16}
echo "[run] ${MATRIX_N:-8} NPU VMM pool full-mesh (no EnablePeerAccess, SLAB_MB=$MB, timeout --kill-after=10 120s)"
SLAB_MB=$MB timeout --kill-after=10 120 mpirun --allow-run-as-root --stdin none -np "${MATRIX_N:-8}" ./test
status=$?
echo "[run] exit=$status (124=timeout/hang)"
exit "$status"
