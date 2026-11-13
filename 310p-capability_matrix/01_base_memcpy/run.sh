#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
make
ret=0
DEV=${DEV:-0} timeout --kill-after=10 60 ./test </dev/null || ret=$?
echo "[run] exit=$ret (124=timeout/hang)"
exit "$ret"
