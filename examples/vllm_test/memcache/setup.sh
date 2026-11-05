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
#   --memcache-repo <URL>       memcache 远程仓库地址（默认 gitcode.com/Ascend/memcache.git）
#   --vllm-ascend-dir <DIR>     vLLM-Ascend 目录（默认自动探测）
#   --skip-build                跳过构建
#   --skip-patch-vllm           跳过 vLLM-Ascend 补丁
#   --skip-install-libs         跳过库替换
#   --yes|-y                    非交互模式，自动确认
#   --help                      显示帮助

set -euo pipefail

# ============================================================================
# 常量
# ============================================================================
MEMCACHE_BASE_TAG="v1.2.1"
MEMCACHE_DEFAULT_REPO="https://gitcode.com/Ascend/memcache.git"
MEMFABRIC_HYBRID_BRANCH="dev-v1.2.0"
MEMFABRIC_HYBRID_FORK_REPO="https://gitcode.com/GDD_ESCC/memfabric_hybrid.git"
VLLM_ASCEND_BASE_COMMIT="f2f74a16c3c50a76f4349d807918e83edec1e35c"
CANN_DEFAULT_PATH="/usr/local/Ascend/cann-9.1.0"

readonly SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# ============================================================================
# 参数解析
# ============================================================================
MEMCACHE_DIR=""
MEMCACHE_REPO="$MEMCACHE_DEFAULT_REPO"
VLLM_ASCEND_DIR=""
SKIP_BUILD=0
SKIP_PATCH_VLLM=0
SKIP_INSTALL_LIBS=0
ASSUME_YES=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --memcache-dir)       MEMCACHE_DIR="$2"; shift 2 ;;
        --memcache-repo)      MEMCACHE_REPO="$2"; shift 2 ;;
        --vllm-ascend-dir)    VLLM_ASCEND_DIR="$2"; shift 2 ;;
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
# fix_memfabric_hybrid — 将子模块切到 dev-v1.2.0 分支并应用补丁
#
# .gitmodules 指向 Ascend/memfabric_hybrid.git（master 分支），dev-v1.2.0 分支在
# GDD_ESCC/memfabric_hybrid.git 上，已包含 IsSameCard + 107002 修复。
# 207001 P2P_HUGE 回退补丁尚未合入，需本地应用。
# git submodule update 会把子模块 reset 回父仓库记录的 commit，所以每次
# submodule update 后都需要重新执行此函数。
# ============================================================================
fix_memfabric_hybrid() {
    local mfh_dir="$MEMCACHE_DIR/3rdparty/memfabric_hybrid"

    cd "$mfh_dir"

    # 清除工作区残留（上次应用的补丁未 commit，submodule update 后残留）
    git stash 2>/dev/null || true
    git reset --hard 2>/dev/null || true
    git clean -fd 2>/dev/null || true

    # origin 默认指向 Ascend/memfabric_hybrid.git（master 分支），dev-v1.2.0 分支在 GDD_ESCC 仓库上
    # 已包含 IsSameCard + 107002 + 207001 P2P_HUGE 回退修复
    if ! git remote get-url fork &>/dev/null; then
        git remote add fork "$MEMFABRIC_HYBRID_FORK_REPO"
    fi
    git fetch fork "$MEMFABRIC_HYBRID_BRANCH" 2>/dev/null
    git checkout -B "$MEMFABRIC_HYBRID_BRANCH" "fork/$MEMFABRIC_HYBRID_BRANCH"

    log_info "memfabric_hybrid: dev-v1.2.0 ($(git rev-parse --short HEAD)) ✓"
    cd "$MEMCACHE_DIR"
}

# ============================================================================
# Step 1: 准备 memcache 仓库（不存在则 clone）
# ============================================================================
log_step "Step 1: 准备 memcache 仓库"

if [[ -d "$MEMCACHE_DIR/.git" ]]; then
    log_info "目录已存在且为 git 仓库: $MEMCACHE_DIR"
    cd "$MEMCACHE_DIR"
    CURRENT_TAG=$(git describe --tags --exact-match 2>/dev/null || echo "")

    if [[ "$CURRENT_TAG" == "$MEMCACHE_BASE_TAG" ]]; then
        log_info "当前在 tag $MEMCACHE_BASE_TAG ✓"
    else
        CURRENT_COMMIT=$(git rev-parse --short HEAD 2>/dev/null || echo "unknown")
        log_warn "当前 tag/commit: ${CURRENT_TAG:-$CURRENT_COMMIT}（期望: $MEMCACHE_BASE_TAG）"
        log_warn "如果补丁应用失败，请先: git checkout $MEMCACHE_BASE_TAG"
        if [[ "$ASSUME_YES" -ne 1 ]]; then
            echo -n "  是否继续? [y/N] "
            read -r response
            [[ "$response" != "y" && "$response" != "Y" ]] && { log_info "用户取消"; exit 0; }
        fi
    fi
else
    log_info "目录不存在，从远程拉取: $MEMCACHE_REPO"
    log_info "  目标路径: $MEMCACHE_DIR"

    if [[ -d "$MEMCACHE_DIR" && -n "$(ls -A "$MEMCACHE_DIR" 2>/dev/null)" ]]; then
        log_error "目录已存在且非空，但不是 git 仓库: $MEMCACHE_DIR"
        log_error "请清空该目录或指定其他路径"
        exit 1
    fi

    mkdir -p "$(dirname "$MEMCACHE_DIR")"

    if git clone "$MEMCACHE_REPO" "$MEMCACHE_DIR"; then
        log_info "clone 完成 ✓"
    else
        log_error "clone 失败，请检查仓库地址和网络: $MEMCACHE_REPO"
        exit 1
    fi

    cd "$MEMCACHE_DIR"
    log_info "checkout 到 tag $MEMCACHE_BASE_TAG..."
    git checkout "$MEMCACHE_BASE_TAG"
    log_info "已切换到 tag $MEMCACHE_BASE_TAG ✓"
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
# Step 3: 初始化 memfabric_hybrid 子模块并切到 dev-v1.2.0 + 应用 207001 补丁
# ============================================================================
log_step "Step 3: 初始化 memfabric_hybrid 子模块"

MFH_DIR="$MEMCACHE_DIR/3rdparty/memfabric_hybrid"

if [[ ! -e "$MFH_DIR/.git" ]] || [[ -z "$(ls -A "$MFH_DIR" 2>/dev/null | grep -v '^\.git$')" ]]; then
    cd "$MEMCACHE_DIR"
    git submodule update --init 3rdparty/memfabric_hybrid
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
        if git diff --name-only HEAD 2>/dev/null | grep "model_runner_310p.py" >/dev/null 2>&1; then
            VLLM_PATCH_ALREADY=1
        elif ! grep -q 'KV cache transfer is not supported' vllm_ascend/_310p/model_runner_310p.py 2>/dev/null; then
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

# ============================================================================
# Step 5: 构建 + 安装（可选）
# ============================================================================
if [[ "$SKIP_BUILD" -eq 1 ]]; then
    log_step "Step 5: 跳过构建（--skip-build）"
else
    log_step "Step 5: 构建 memcache + 安装"

    cd "$MEMCACHE_DIR"
    log_info "初始化构建所需子模块（跳过 test/ 子模块，BUILD_TEST=OFF 不需要）..."
    # git submodule update 会把 memfabric_hybrid reset 回父仓库记录的 commit，
    # 需要先清理工作区（Step 3 应用的补丁未 commit），再 update 后重新切回 dev-v1.2.0 + 应用补丁
    cd "$MEMCACHE_DIR/3rdparty/memfabric_hybrid" 2>/dev/null && git stash 2>/dev/null || true
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
    bash script/build_and_pack_run.sh --build_mode RELEASE --build_test OFF
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

        log_info "替换 memfabric_hybrid 库..."
        HYBRID_LIB_DIR=$(python3 -c "import memfabric_hybrid, os; print(os.path.dirname(memfabric_hybrid.__file__)+'/lib')" 2>/dev/null || echo "")
        if [[ -n "$HYBRID_LIB_DIR" && -d "$HYBRID_LIB_DIR" ]]; then
            [[ -f "$MFH_DIR/output/hybm/lib64/libmf_hybm_core.so" ]] && cp "$MFH_DIR/output/hybm/lib64/libmf_hybm_core.so" "$HYBRID_LIB_DIR/"
            [[ -f "$MFH_DIR/output/smem/lib64/libmf_smem.so" ]] && cp "$MFH_DIR/output/smem/lib64/libmf_smem.so" "$HYBRID_LIB_DIR/"
            if strings "$HYBRID_LIB_DIR/libmf_hybm_core.so" 2>/dev/null | grep "using 0, may affect" >/dev/null 2>&1; then
                log_info "SDID fallback 验证通过 ✓"
            else
                log_warn "SDID fallback 未检测到"
            fi
        else
            log_warn "未找到 memfabric_hybrid 库目录，跳过"
        fi
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
echo "      --type integration"
echo ""
echo "  测试前清理环境:"
echo "    bash $DEBUG_DIR/clean_stale_env.sh"
echo ""
echo "  详细说明: $SCRIPT_DIR/README.md"
