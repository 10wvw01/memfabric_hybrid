#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

mode="${1:-all}"
case "$mode" in
    all|probe|pools|acl-pools) ;;
    *) echo "Usage: $0 [all|probe|pools|acl-pools]" >&2; exit 2 ;;
esac

export ASCEND_HOME="${ASCEND_HOME:-${ASCEND_HOME_PATH:-/usr/local/Ascend/cann}}"
export A_DEV="${A_DEV:-0}" B_DEV="${B_DEV:-1}"
if [[ -d /usr/local/Ascend/driver/lib64/driver ]]; then
    export LD_LIBRARY_PATH="/usr/local/Ascend/driver/lib64/driver:${LD_LIBRARY_PATH:-}"
fi
make ASCEND_HOME="$ASCEND_HOME"
echo "[run] mode=$mode A_DEV=$A_DEV B_DEV=$B_DEV ASCEND_HOME=$ASCEND_HOME"
if [[ "$mode" == all || "$mode" == probe ]]; then
    timeout --kill-after=10 "${TEST_TIMEOUT:-90}" mpirun --allow-run-as-root -np 1 ./test probe </dev/null
fi
if [[ "$mode" == all || "$mode" == pools ]]; then
    timeout --kill-after=10 "${TEST_TIMEOUT:-90}" mpirun --allow-run-as-root -np 2 ./test pools </dev/null
fi
if [[ "$mode" == acl-pools ]]; then
    timeout --kill-after=10 "${TEST_TIMEOUT:-90}" mpirun --allow-run-as-root -np 2 ./test acl-pools </dev/null
fi
