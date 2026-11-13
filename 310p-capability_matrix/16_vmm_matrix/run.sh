#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
make
MB=${SLAB_MB:-16}
echo "[run] 8 NPU VMM pool full-mesh (no EnablePeerAccess, SLAB_MB=$MB, timeout 120s)"
ret=0
SLAB_MB=$MB timeout --kill-after=10 120 mpirun --allow-run-as-root -np 8 ./test </dev/null || ret=$?
echo "[run] exit=$ret (124=timeout/hang)"
exit "$ret"
