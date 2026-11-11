#!/bin/bash
cd "$(dirname "$0")"
make >/dev/null 2>&1
DEV=${DEV:-0} timeout 60 ./test
echo "[run] exit=$? (124=timeout/hang)"
