#!/usr/bin/env bash
# Fetch the specified upstream branches, then build and install the test environment.
set -euo pipefail
readonly SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/source_repos.sh"
MEMCACHE_DIR=""
VLLM_ASCEND_DIR=""
VLLM_DIR=""
SKIP_BUILD=0
SKIP_INSTALL_LIBS=0
LAYER2_ONLY=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --memcache-dir) MEMCACHE_DIR="$2"; shift 2 ;;
        --vllm-ascend-dir) VLLM_ASCEND_DIR="$2"; shift 2 ;;
        --vllm-dir) VLLM_DIR="$2"; shift 2 ;;
        --skip-build) SKIP_BUILD=1; shift ;;
        --skip-install-libs) SKIP_INSTALL_LIBS=1; shift ;;
        --layer2-only) LAYER2_ONLY=1; shift ;;
        --yes|-y) shift ;;
        --help|-h)
            echo "Usage: bash setup.sh --memcache-dir DIR --vllm-dir DIR --vllm-ascend-dir DIR"
            echo "Options: --layer2-only --skip-build --skip-install-libs --yes"
            exit 0 ;;
        *) echo "[ERROR] Unknown argument: $1" >&2; exit 2 ;;
    esac
done
[[ -n "$MEMCACHE_DIR" ]] || { echo "[ERROR] --memcache-dir is required" >&2; exit 2; }
MEMCACHE_DIR=$(realpath -m "$MEMCACHE_DIR")
if [[ -n "$VLLM_DIR" ]]; then
    VLLM_DIR=$(realpath -m "$VLLM_DIR")
fi
export PYTHON_HOME="${PYTHON_HOME:-$(python3 -c 'import sys; print(sys.prefix)')}"
log_info() { echo "[INFO] $*"; }
log_warn() { echo "[WARN] $*"; }
log_error() { echo "[ERROR] $*" >&2; }
log_step() { echo "=== $* ==="; }

sync_source_repo "$MEMCACHE_DIR" "$MEMCACHE_REPO" "$MEMCACHE_BRANCH"
MFH_DIR="$MEMCACHE_DIR/3rdparty/memfabric_hybrid"
git -C "$MEMCACHE_DIR" submodule init 3rdparty/memfabric_hybrid
git -C "$MEMCACHE_DIR" config submodule.3rdparty/memfabric_hybrid.url "$MEMFABRIC_REPO"
git -C "$MEMCACHE_DIR" config submodule.3rdparty/memfabric_hybrid.branch "$MEMFABRIC_BRANCH"
sync_source_repo "$MFH_DIR" "$MEMFABRIC_REPO" "$MEMFABRIC_BRANCH"
git -C "$MEMCACHE_DIR" submodule absorbgitdirs 3rdparty/memfabric_hybrid
if [[ "$LAYER2_ONLY" -eq 0 ]]; then
    [[ -n "$VLLM_ASCEND_DIR" ]] || VLLM_ASCEND_DIR="$(dirname "$MEMCACHE_DIR")/vllm-ascend"
    VLLM_ASCEND_DIR=$(realpath -m "$VLLM_ASCEND_DIR")
    sync_source_repo "$VLLM_ASCEND_DIR" "$VLLM_ASCEND_REPO" "$VLLM_ASCEND_BRANCH"
    if [[ -z "$VLLM_DIR" || ! -e "$VLLM_DIR/.git" ]]; then
        log_error "--vllm-dir must point to the original vLLM 0.24 repository"
        exit 1
    fi
    if [[ -n "$(git -C "$VLLM_DIR" status --porcelain --untracked-files=no)" ]]; then
        log_error "vLLM source has uncommitted changes: $VLLM_DIR"
        exit 1
    fi
fi

if [[ "$SKIP_BUILD" -eq 1 ]]; then
    log_step "Step 5: 跳过构建（--skip-build）"
else
    log_step "Step 5: 构建 memcache + 安装"

    cd "$MEMCACHE_DIR"
    log_info "初始化构建所需子模块（跳过 test/ 子模块，BUILD_TEST=OFF 不需要）..."
    # 只更新其余依赖，保留已选择的 MemFabric 版本和补丁。
    cd "$MEMCACHE_DIR"

    git -c url.https://gitcode.com/Abstrey/.insteadOf=https://github.com/biaks/ \
        -c url.https://gitcode.com/Abstrey/.insteadOf=https://github.com/sinojelly/ \
        submodule update --init \
        3rdparty/log/spdlog 3rdparty/nlohmann/json 3rdparty/msgpack-c \
        3rdparty/libzmq 3rdparty/prometheus-cpp-lite

    log_info "构建 (RELEASE, no test)..."
    bash script/build_and_pack_run.sh --build_mode RELEASE --build_test OFF --build_ubsio OFF
    log_info "构建完成 ✓"

    if [[ "$SKIP_INSTALL_LIBS" -eq 0 ]]; then
        log_info "安装 wheel..."
        WHEEL=$(ls "$MEMCACHE_DIR"/output/memcache/wheel/memcache_hybrid-*.whl 2>/dev/null | head -1)
        if [[ -n "$WHEEL" ]]; then
            python3 -m pip install --force-reinstall --no-deps "$WHEEL"
            log_info "wheel 安装完成 ✓"
        else
            log_error "未找到本次构建的 MemCache wheel"
            exit 1
        fi

        log_info "安装由指定 MemFabric 分支构建的 Python 包..."
        MF_PY_ROOT="$MFH_DIR/src/smem/python/memfabric_hybrid"
        MF_PKG="$MF_PY_ROOT/memfabric_hybrid"
        MF_BUILD="$MEMCACHE_DIR/build/3rdparty/memfabric_hybrid/src"
        mkdir -p "$MF_PKG/lib"
        cp "$MFH_DIR/output/hybm/lib64/libmf_hybm_core.so" \
           "$MFH_DIR/output/smem/lib64/libmf_smem.so" \
           "$MFH_DIR/output/acc_offload/lib64/libmf_acc_offload.so" "$MF_PKG/lib/"
        cp "$MF_BUILD"/smem/csrc/python_wrapper/mk_transfer_adapter/_pymf_transfer*.so \
           "$MF_BUILD"/smem/csrc/python_wrapper/memfabric_hybrid/_pymf_hybrid*.so \
           "$MF_BUILD"/acc_offload/csrc/python_wrapper/_pymf_acc_offload*.so "$MF_PKG/"
        (
            cd "$MF_PY_ROOT"
            export MEMFABRIC_VERSION
            MEMFABRIC_VERSION=$(tr -d '[:space:]' < "$MFH_DIR/VERSION")
            python3 setup.py build --force bdist_wheel
            python3 -m pip install --force-reinstall --no-deps dist/memfabric_hybrid-"$MEMFABRIC_VERSION"-*.whl
            installed_lib=$(python3 -c 'import memfabric_hybrid, pathlib; print(pathlib.Path(memfabric_hybrid.__file__).parent / "lib" / "libmf_hybm_core.so")')
            cmp "$MFH_DIR/output/hybm/lib64/libmf_hybm_core.so" "$installed_lib" || {
                log_error "安装的 MemFabric 库与本次构建产物不一致"
                exit 1
            }
        )

    fi
fi


if [[ "$SKIP_BUILD" -eq 0 && "$SKIP_INSTALL_LIBS" -eq 0 && "$LAYER2_ONLY" -eq 0 ]]; then
    # CANN source-copy stamps can otherwise retain headers from the previous revision.
    if [[ -d "$VLLM_ASCEND_DIR/csrc/build/binary" ]]; then
        find "$VLLM_ASCEND_DIR/csrc/build/binary" -mindepth 2 -maxdepth 2 \
            -type d -name src -exec rm -rf -- {} +
    fi
    python3 -m pip install --no-deps --no-build-isolation -e "$VLLM_ASCEND_DIR"
fi

DEBUG_DIR="$MEMCACHE_DIR/debug/test"
mkdir -p "$DEBUG_DIR"
for helper in "$SCRIPT_DIR"/aux_scripts/*; do
    if [[ -f "$helper" ]]; then
        cp "$helper" "$DEBUG_DIR/"
    fi
done

MANIFEST_ARGS=()
if [[ "$SKIP_BUILD" -eq 1 || "$SKIP_INSTALL_LIBS" -eq 1 ]]; then
    MANIFEST_ARGS+=(--sources-only)
fi
python3 "$SCRIPT_DIR/source_manifest.py" write --memcache-dir "$MEMCACHE_DIR" \
    --vllm-ascend-dir "$VLLM_ASCEND_DIR" --vllm-dir "$VLLM_DIR" "${MANIFEST_ARGS[@]}"
if [[ ${#MANIFEST_ARGS[@]} -eq 0 ]]; then
    log_info "构建安装完成。运行 verify.sh 核对部署结果。"
else
    log_info "源码准备完成。完整构建安装后运行 verify.sh。"
fi
