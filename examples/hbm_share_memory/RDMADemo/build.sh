#!/bin/bash
CURRENT_DIR=$(
    cd $(dirname ${BASH_SOURCE:-$0})
    pwd
)

SHORT=r:,v:,i:,b:,p:,
LONG=run-mode:,soc-version:,install-path:,build-type:,install-prefix:,
OPTS=$(getopt -a --options $SHORT --longoptions $LONG -- "$@")
eval set -- "$OPTS"
SOC_VERSION="Ascend910B3"

while :; do
    case "$1" in
    -v | --soc-version)
        SOC_VERSION="$2"
        shift 2
        ;;
    --)
        shift
        break
        ;;
    *)
        echo "[ERROR] Unexpected option: $1"
        break
        ;;
    esac
done

if [[ "$SOC_VERSION" == *310P* ]]; then
    echo "[ERROR] RDMADemo uses legacy SHM kernels. For Ascend310P use examples/memory_pool/01_basic/08_310p_aicore_aicpu_sdma or 09_310p_aicore_aicpu_sdma_4rank."
    exit 1
fi

VERSION_LIST="Ascend910A Ascend910B Ascend310B1 Ascend310B2 Ascend310B3 Ascend310B4 Ascend910B1 Ascend910B2 Ascend910B3 Ascend910B4"
if [[ " $VERSION_LIST " != *" $SOC_VERSION "* ]]; then
    echo "ERROR: SOC_VERSION should be in [$VERSION_LIST]"
    exit 1
fi

set -e
rm -rf build out
mkdir -p build
cmake -B build -DSOC_VERSION=${SOC_VERSION}
cmake --build build -j
cmake --install build
