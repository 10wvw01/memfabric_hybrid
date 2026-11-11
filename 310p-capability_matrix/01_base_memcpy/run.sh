#!/bin/bash
set -uo pipefail
cd "$(dirname "$0")"
make || exit $?
DEV=${DEV:-0} timeout --kill-after=10 60 ./test
status=$?
echo "[run] exit=$status (124=timeout/hang)"
exit "$status"
