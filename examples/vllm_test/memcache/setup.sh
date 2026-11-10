#!/bin/bash
# setup.sh — vLLM TTFT 测试环境部署脚本
#
# 指定 memcache 工作目录，不存在则从远程拉取，然后应用补丁、构建安装。
#
# 用法:
#   bash setup.sh --memcache-dir <DIR> [选项]
#
# 选项:
#   --memcache-dir <DIR>        memcache 工作目录（不存在则 clone 到此路径）
#   --memcache-repo <URL>       memcache 远程仓库地址（默认 GDD_ESCC/memcache）
#   --vllm-dir <DIR>            vLLM 0.24 源码目录（集成测试必选）
#   --vllm-ascend-dir <DIR>     vLLM-Ascend 目录（默认自动探测）
#   --skip-build                跳过构建
#   --skip-patch-vllm           跳过 vLLM-Ascend 补丁
#   --skip-install-libs         跳过 wheel 安装
#   --yes|-y                    非交互模式，自动确认
#   --help                      显示帮助

set -euo pipefail

# ============================================================================
# 常量
# ============================================================================
MEMCACHE_DEFAULT_REPO="https://gitcode.com/GDD_ESCC/memcache.git"
MEMCACHE_BASE_BRANCH="trunk_for_escc_v1.2.1"
MEMFABRIC_HYBRID_BRANCH="trunk_for_escc_v1.2.0"
MEMFABRIC_HYBRID_FORK_REPO="https://gitcode.com/GDD_ESCC/memfabric_hybrid.git"
VLLM_ASCEND_BASE_COMMIT="b19127c7e855af6a1ee6a67d3825205086a87a8e"
CANN_DEFAULT_PATH="/usr/local/Ascend/cann-9.1.0"

readonly SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# ============================================================================
# 参数解析
# ============================================================================
MEMCACHE_DIR=""
MEMCACHE_REPO="$MEMCACHE_DEFAULT_REPO"
VLLM_ASCEND_DIR=""
VLLM_DIR=""
SKIP_BUILD=0
SKIP_PATCH_VLLM=0
SKIP_INSTALL_LIBS=0
ASSUME_YES=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --memcache-dir)       MEMCACHE_DIR="$2"; shift 2 ;;
        --memcache-repo)      MEMCACHE_REPO="$2"; shift 2 ;;
        --vllm-ascend-dir)    VLLM_ASCEND_DIR="$2"; shift 2 ;;
        --vllm-dir)           VLLM_DIR="$2"; shift 2 ;;
        --skip-build)         SKIP_BUILD=1; shift ;;
        --skip-patch-vllm)    SKIP_PATCH_VLLM=1; shift ;;
        --skip-install-libs)  SKIP_INSTALL_LIBS=1; shift ;;
        --yes|-y)             ASSUME_YES=1; shift ;;
        --help|-h)            sed -n '2,20p' "$0"; exit 0 ;;
        *) echo "[ERROR] 未知参数: $1"; exit 1 ;;
    esac
done

[[ -z "$MEMCACHE_DIR" ]] && { echo "[ERROR] --memcache-dir <DIR> 是必选参数"; exit 1; }

# ============================================================================
# 辅助
# ============================================================================
log_info()  { echo -e "\033[32m[INFO]\033[0m  $*"; }
log_warn()  { echo -e "\033[33m[WARN]\033[0m  $*"; }
log_error() { echo -e "\033[31m[ERROR]\033[0m $*"; }
log_step()  { echo -e "\n\033[36m========== $* ==========\033[0m"; }

# ============================================================================
# Select the requested MemFabric revision without discarding local changes.
fix_memfabric_hybrid() {
    local mfh_dir="$MEMCACHE_DIR/3rdparty/memfabric_hybrid"
    cd "$mfh_dir"
    if ! git remote get-url fork &>/dev/null; then
        git remote add fork "$MEMFABRIC_HYBRID_FORK_REPO"
    fi
    git fetch fork "$MEMFABRIC_HYBRID_BRANCH"
    local mf_patch="$SCRIPT_DIR/patches/memfabric_310p_vmm_context.patch"
    if git merge-base --is-ancestor FETCH_HEAD HEAD && git apply --reverse --check "$mf_patch" 2>/dev/null; then
        log_info "保留基于 $MEMFABRIC_HYBRID_BRANCH 的已修复 MemFabric 提交"
    elif [[ "$(git rev-parse HEAD)" != "$(git rev-parse FETCH_HEAD)" ]]; then
        if [[ -n "$(git status --porcelain --untracked-files=no)" ]]; then
            log_error "memfabric_hybrid 有未提交的修改，不能切换版本: $mfh_dir"
            return 1
        fi
        git checkout --detach FETCH_HEAD
    fi
    if git apply --reverse --check "$mf_patch" 2>/dev/null; then
        log_info "MemFabric 310P 地址窗口和上下文补丁已应用"
    else
        git apply --check "$mf_patch"
        git apply "$mf_patch"
    fi
    log_info "memfabric_hybrid: $MEMFABRIC_HYBRID_BRANCH ($(git rev-parse --short HEAD))"
    cd "$MEMCACHE_DIR"
}

# ============================================================================
# Step 1: 准备 memcache 仓库（不存在则 clone）
# ============================================================================
log_step "Step 1: 准备 memcache 仓库"

if [[ -d "$MEMCACHE_DIR/.git" ]]; then
    log_info "目录已存在且为 git 仓库: $MEMCACHE_DIR"
    cd "$MEMCACHE_DIR"
    CURRENT_BRANCH=$(git branch --show-current)
    if ! git rev-parse --verify "$MEMCACHE_BASE_BRANCH^{commit}" >/dev/null 2>&1 ||
        ! git merge-base --is-ancestor "$MEMCACHE_BASE_BRANCH" HEAD; then
        log_error "MemCache 当前提交不基于 $MEMCACHE_BASE_BRANCH；请准备正确分支后重试"
        exit 1
    fi
    log_info "MemCache 分支: ${CURRENT_BRANCH:-detached HEAD} ($(git rev-parse --short HEAD))，基线 $MEMCACHE_BASE_BRANCH"

else
    log_info "目录不存在，从远程拉取: $MEMCACHE_REPO"
    log_info "  目标路径: $MEMCACHE_DIR"

    if [[ -d "$MEMCACHE_DIR" && -n "$(ls -A "$MEMCACHE_DIR" 2>/dev/null)" ]]; then
        log_error "目录已存在且非空，但不是 git 仓库: $MEMCACHE_DIR"
        log_error "请清空该目录或指定其他路径"
        exit 1
    fi

    mkdir -p "$(dirname "$MEMCACHE_DIR")"

    if git clone --branch "$MEMCACHE_BASE_BRANCH" "$MEMCACHE_REPO" "$MEMCACHE_DIR"; then
        log_info "clone 完成 ✓"
    else
        log_error "clone 失败，请检查仓库地址和网络: $MEMCACHE_REPO"
        exit 1
    fi

    cd "$MEMCACHE_DIR"
    log_info "checkout 到分支 $MEMCACHE_BASE_BRANCH..."
    git checkout "$MEMCACHE_BASE_BRANCH"
    log_info "已切换到分支 $MEMCACHE_BASE_BRANCH ✓"
fi

# ============================================================================
# Step 2: 应用 memcache 补丁（pymmc.cpp pybind11 枚举 static_cast 适配）
# ============================================================================
log_step "Step 2: 应用 memcache 补丁"

MEMCACHE_PATCH="$SCRIPT_DIR/patches/memcache_modified_files.patch"

PATCH_ALREADY=0
if grep -q 'static_cast<int32_t>(SMEMB_COPY' src/memcache/csrc/python_wrapper/pymmc.cpp 2>/dev/null; then
    PATCH_ALREADY=1
fi

if [[ "$PATCH_ALREADY" -eq 1 ]]; then
    log_info "补丁已应用，跳过"
else
    log_info "应用补丁: $MEMCACHE_PATCH"
    if git apply --check "$MEMCACHE_PATCH" 2>/dev/null; then
        git apply "$MEMCACHE_PATCH"
        log_info "补丁应用成功 ✓"
    else
        log_warn "git apply --check 失败，尝试 --3way..."
        if git apply --3way "$MEMCACHE_PATCH" 2>/dev/null; then
            log_info "补丁通过 --3way 应用成功 ✓"
        else
            log_error "补丁应用失败，请手动检查:"
            log_error "  cd $MEMCACHE_DIR && git apply --reject $MEMCACHE_PATCH"
            exit 1
        fi
    fi
fi

# ============================================================================
# Step 3: 初始化 memfabric_hybrid 子模块并应用 310P 补丁
# ============================================================================
log_step "Step 3: 初始化 memfabric_hybrid 子模块"

MFH_DIR="$MEMCACHE_DIR/3rdparty/memfabric_hybrid"

if [[ ! -e "$MFH_DIR/.git" ]] || [[ -z "$(ls -A "$MFH_DIR" 2>/dev/null | grep -v '^\.git$')" ]]; then
    cd "$MEMCACHE_DIR"
    git submodule init 3rdparty/memfabric_hybrid
    git config submodule.3rdparty/memfabric_hybrid.url "$MEMFABRIC_HYBRID_FORK_REPO"
    git config submodule.3rdparty/memfabric_hybrid.branch "$MEMFABRIC_HYBRID_BRANCH"
    git submodule update --init --remote 3rdparty/memfabric_hybrid
fi

fix_memfabric_hybrid

# ============================================================================
# Step 4: 应用 vLLM-Ascend 补丁（可选）
# ============================================================================
if [[ "$SKIP_PATCH_VLLM" -eq 1 ]]; then
    log_step "Step 4: 跳过 vLLM-Ascend 补丁（--skip-patch-vllm）"
else
    log_step "Step 4: 应用 vLLM-Ascend 补丁"

    VLLM_PATCH="$SCRIPT_DIR/patches/vllm_ascend_310p_kv_transfer.patch"

    if [[ -z "$VLLM_ASCEND_DIR" ]]; then
        DETECTED=$(python3 -c "
import vllm_ascend, os
print(os.path.dirname(os.path.dirname(vllm_ascend.__file__)))
" 2>/dev/null || echo "")
        if [[ -n "$DETECTED" && -d "$DETECTED/.git" ]]; then
            VLLM_ASCEND_DIR="$DETECTED"
            log_info "自动探测到 vLLM-Ascend: $VLLM_ASCEND_DIR"
        else
            for c in /vllm-workspace/vllm-ascend /opt/vllm-ascend; do
                if [[ -d "$c/.git" ]]; then
                    VLLM_ASCEND_DIR="$c"
                    break
                fi
            done
        fi
    fi

    if [[ -z "$VLLM_ASCEND_DIR" || ! -d "$VLLM_ASCEND_DIR/.git" ]]; then
        log_warn "未找到 vLLM-Ascend 源码目录"
        log_warn "请指定 --vllm-ascend-dir，或加 --skip-patch-vllm 仅做 Layer 2"
    else
        cd "$VLLM_ASCEND_DIR"
        VLLM_PATCH_ALREADY=0
        if git apply --reverse --check "$VLLM_PATCH" 2>/dev/null; then
            VLLM_PATCH_ALREADY=1
        fi

        if [[ "$VLLM_PATCH_ALREADY" -eq 1 ]]; then
            log_info "vLLM-Ascend 补丁已应用，跳过"
        else
            log_info "应用补丁: $VLLM_PATCH"
            if git apply --check "$VLLM_PATCH" 2>/dev/null; then
                git apply "$VLLM_PATCH"
                log_info "vLLM-Ascend 补丁应用成功 ✓"
            elif git apply --3way "$VLLM_PATCH" 2>/dev/null; then
                log_info "vLLM-Ascend 补丁通过 --3way 应用成功 ✓"
            else
                log_error "vLLM-Ascend 补丁应用失败，请手动:"
                log_error "  cd $VLLM_ASCEND_DIR && git apply --reject $VLLM_PATCH"
                exit 1
            fi
        fi
    fi
fi

# vLLM stays on the unmodified 0.24 baseline; hardware adaptations live in vLLM-Ascend.
if [[ "$SKIP_PATCH_VLLM" -ne 1 ]]; then
    if [[ -z "$VLLM_DIR" || ! -e "$VLLM_DIR/.git" ]]; then
        log_error "请通过 --vllm-dir 指定原版 vLLM 0.24 源码目录"
        exit 1
    fi
    if ! git -C "$VLLM_DIR" diff --quiet HEAD -- vllm; then
        log_error "vLLM 源码存在本地修改，请使用原版 v0.24.0 工作区"
        exit 1
    fi
    log_info "使用原版 vLLM，不应用核心源码补丁"
fi

# ============================================================================
# Step 5: 构建 + 安装（可选）
# ============================================================================
if [[ "$SKIP_BUILD" -eq 1 ]]; then
    log_step "Step 5: 跳过构建（--skip-build）"
else
    log_step "Step 5: 构建 memcache + 安装"

    cd "$MEMCACHE_DIR"
    log_info "初始化构建所需子模块（跳过 test/ 子模块，BUILD_TEST=OFF 不需要）..."
    # 只更新其余依赖，保留已选择的 MemFabric 版本和补丁。
    cd "$MEMCACHE_DIR"

    # github.com 子模块在受限网络下不可达，替换为 gitcode.com 镜像
    # prometheus-cpp-lite: github.com/biaks → gitcode.com/Abstrey
    # mockcpp:              github.com/sinojelly → gitcode.com/Abstrey
    sed -i 's|https://github.com/biaks/prometheus-cpp-lite.git|https://gitcode.com/Abstrey/prometheus-cpp-lite.git|' .gitmodules
    sed -i 's|https://github.com/sinojelly/mockcpp.git|https://gitcode.com/Abstrey/mockcpp.git|' .gitmodules

    # 只初始化 3rdparty 构建所需子模块（跳过 test/ 子模块，BUILD_TEST=OFF 不需要）
    git submodule update --init \
        3rdparty/log/spdlog \
        3rdparty/nlohmann/json \
        3rdparty/msgpack-c \
        3rdparty/libzmq \
        3rdparty/prometheus-cpp-lite \
        2>&1 || { log_error "子模块 clone 失败"; exit 1; }

    fix_memfabric_hybrid

    log_info "构建 (RELEASE, no test)..."
    bash script/build_and_pack_run.sh --build_mode RELEASE --build_test OFF --build_ubsio OFF
    log_info "构建完成 ✓"

    if [[ "$SKIP_INSTALL_LIBS" -eq 0 ]]; then
        log_info "安装 wheel..."
        WHEEL=$(ls "$MEMCACHE_DIR"/output/memcache/wheel/memcache_hybrid-*.whl 2>/dev/null | head -1)
        if [[ -n "$WHEEL" ]]; then
            pip install --force-reinstall --no-deps "$WHEEL"
            log_info "wheel 安装完成 ✓"
        else
            log_warn "未找到 wheel，跳过"
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
            pip install --force-reinstall --no-deps dist/memfabric_hybrid-"$MEMFABRIC_VERSION"-*.whl
            installed_lib=$(python3 -c 'import memfabric_hybrid, pathlib; print(pathlib.Path(memfabric_hybrid.__file__).parent / "lib" / "libmf_hybm_core.so")')
            cmp "$MFH_DIR/output/hybm/lib64/libmf_hybm_core.so" "$installed_lib" || {
                log_error "安装的 MemFabric 库与本次构建产物不一致"
                exit 1
            }
        )

    fi
fi

# ============================================================================
# Step 6: 拷贝辅助脚本 + 输出指引
# ============================================================================
log_step "Step 6: 拷贝辅助脚本"

DEBUG_DIR="$MEMCACHE_DIR/debug/test"
mkdir -p "$DEBUG_DIR"
for f in "$SCRIPT_DIR"/aux_scripts/*; do
    [[ -f "$f" ]] && cp "$f" "$DEBUG_DIR/"
done
log_info "辅助脚本已拷贝到 $DEBUG_DIR ✓"

# ============================================================================
# 完成
# ============================================================================
echo ""
echo "================================================================"
echo "  部署完成！"
echo "================================================================"
echo ""
echo "  运行测试（在打包目录中执行）:"
echo "    bash $SCRIPT_DIR/run_test.sh \\"
echo "      --memcache-dir $MEMCACHE_DIR \\"
echo "      --type workload"
echo ""
echo "    bash $SCRIPT_DIR/run_test.sh \\"
echo "      --memcache-dir $MEMCACHE_DIR \\"
echo "      --type integration --rank 2 --protocol device_sdma --device-ids 6,7"
echo ""
echo "  详细说明: $SCRIPT_DIR/README.md"
