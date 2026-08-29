#!/bin/bash
cd "$(dirname "$0")"
make >/dev/null 2>&1
MB=${SLAB_MB:-16}
echo "[run] 8 NPU VMM pool full-mesh (no EnablePeerAccess, SLAB_MB=$MB, timeout 120s)"
SLAB_MB=$MB timeout 120 mpirun --allow-run-as-root -np 8 ./test
echo "[run] exit=$? (124=timeout/hang)"
