#!/bin/bash
# Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
# MemFabric + Mooncake MF Transport 一键测试脚本
#
# 用法:
#   bash run.sh <工作目录> [选项]
#
# 示例:
#   bash run.sh /tmp/test_workdir                    # 全流程
#   bash run.sh /tmp/test_workdir --skip_build_mf    # 跳过 MemFabric 编译
#   bash run.sh /tmp/test_workdir --skip_test        # 只编译不测试
#   bash run.sh /tmp/test_workdir --only_test        # 只跑测试（需已编译）
#   bash run.sh /tmp/test_workdir --single single_write  # 只跑单个测试
#   bash run.sh /tmp/test_workdir --only_ttft_test   # 只跑 TTFT 基准测试（需已编译）
#
# 工作目录结构:
#   <工作目录>/
#   ├── memfabric_hybrid/     # MemFabric 源码（git clone）
#   ├── mooncake/             # Mooncake 源码（git clone + 打补丁）
#   ├── ylt_src/              # yalantinglibs 源码（git clone）
#   └── mooncake_pkg/         # Python 包目录
set -eo pipefail

readonly SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
readonly TEST_ROOT=$(cd "${SCRIPT_DIR}/.." && pwd)

WORK_DIR=""
SKIP_BUILD_MF=0
SKIP_BUILD_MOONCAKE=0
SKIP_TEST=0
ONLY_TEST=0
SKIP_VLLM_TEST=0
ONLY_VLLM_TEST=0
SKIP_TTFT_TEST=0
ONLY_TTFT_TEST=0
SINGLE_TEST=""
VLLM_MODEL_PATH="/home/models/Qwen3.5-2B-W8A8-310P"
VLLM_DEVICE=""
VLLM_PORT=8010
VLLM_TIMEOUT=360
MOONCAKE_REPO="https://gitcode.com/openFuyao/mooncake.git"
MOONCAKE_BRANCH="of-dev/v0.3.13.post1"
MEMFABRIC_REPO="https://gitcode.com/cnk700i/memfabric_hybrid.git"
MEMFABRIC_BRANCH="ljr-dev"
YLT_REPO="https://github.com/alibaba/yalantinglibs.git"
YLT_COMMIT="7801bc9ad9021781f15217552214e325a1cf7373"

show_help() {
    cat <<EOF
用法: $0 <工作目录> [选项]

必选参数:
  <工作目录>          所有源码、编译产物存放的根目录

可选参数:
  --skip_build_mf       跳过 MemFabric 编译（使用已有产物）
  --skip_build_mooncake 跳过 Mooncake 编译
  --skip_test           只编译不运行 PD 传输测试
  --skip_vllm_test      跳过 vLLM 集成测试
  --skip_ttft_test      跳过 TTFT 基准测试
  --only_test           只运行 PD 传输测试（跳过所有编译步骤）
  --only_vllm_test      只运行 vLLM 集成测试（跳过所有编译步骤）
  --only_ttft_test      只运行 TTFT 基准测试（跳过所有编译步骤）
  --single <test_id>    只运行指定测试（如 single_write）
  --vllm_model <path>   vLLM 测试模型路径（默认 /home/models/Qwen3.5-2B-W8A8-310P）
  --vllm_device <id>    vLLM 测试使用的 NPU 设备 ID（默认自动选择空闲设备）
  --vllm_port <port>    vLLM 测试端口（默认 8010）
  --mooncake_repo <url> Mooncake 仓库地址
  --mooncake_branch <b> Mooncake 分支
  --mf_repo <url>       MemFabric 仓库地址
  --mf_branch <b>       MemFabric 分支
  --help                显示帮助

测试列表:
  PD 传输测试:
    single_write, batch_write, single_read, batch_read,
    async_write, async_read, large_buffer, multi_segment,
    probe, register_unregister
  vLLM 集成测试:
    vllm_baseline, vllm_kvboth_mf, vllm_pd_disagg_mf, vllm_kv_offload_mf
  TTFT 基准测试:
    ttft_baseline, ttft_store_hit, ttft_offload
EOF
    exit 0
}

while [[ "$#" -gt 0 ]]; do
    case "$1" in
        --skip_build_mf) SKIP_BUILD_MF=1; shift ;;
        --skip_build_mooncake) SKIP_BUILD_MOONCAKE=1; shift ;;
        --skip_test) SKIP_TEST=1; shift ;;
        --skip_vllm_test) SKIP_VLLM_TEST=1; shift ;;
        --skip_ttft_test) SKIP_TTFT_TEST=1; shift ;;
        --only_test) ONLY_TEST=1; SKIP_BUILD_MF=1; SKIP_BUILD_MOONCAKE=1; SKIP_VLLM_TEST=1; SKIP_TTFT_TEST=1; shift ;;
        --only_vllm_test) ONLY_VLLM_TEST=1; SKIP_BUILD_MF=1; SKIP_BUILD_MOONCAKE=1; SKIP_TEST=1; SKIP_TTFT_TEST=1; shift ;;
        --only_ttft_test) ONLY_TTFT_TEST=1; SKIP_BUILD_MF=1; SKIP_BUILD_MOONCAKE=1; SKIP_TEST=1; SKIP_VLLM_TEST=1; shift ;;
        --single) SINGLE_TEST="$2"; shift 2 ;;
        --vllm_model) VLLM_MODEL_PATH="$2"; shift 2 ;;
        --vllm_device) VLLM_DEVICE="$2"; shift 2 ;;
        --vllm_port) VLLM_PORT="$2"; shift 2 ;;
        --mooncake_repo) MOONCAKE_REPO="$2"; shift 2 ;;
        --mooncake_branch) MOONCAKE_BRANCH="$2"; shift 2 ;;
        --mf_repo) MEMFABRIC_REPO="$2"; shift 2 ;;
        --mf_branch) MEMFABRIC_BRANCH="$2"; shift 2 ;;
        --help) show_help ;;
        -*) echo "未知参数: $1"; show_help ;;
        *)
            if [[ -z "$WORK_DIR" ]]; then
                WORK_DIR="$1"
            else
                echo "多余参数: $1"; show_help
            fi
            shift ;;
    esac
done

if [[ -z "$WORK_DIR" ]]; then
    echo "错误: 必须指定工作目录"
    show_help
fi

WORK_DIR=$(mkdir -p "$WORK_DIR" && cd "$WORK_DIR" && pwd)
MOONCAKE_DIR="${WORK_DIR}/mooncake"
MEMFABRIC_DIR="${WORK_DIR}/memfabric_hybrid"
YLT_DIR="${WORK_DIR}/ylt_src"
MOONCAKE_PKG="${WORK_DIR}/mooncake_pkg"
MEMFABRIC_OUTPUT="${MEMFABRIC_DIR}/output"

log() { echo -e "\033[32m[TEST]\033[0m $*"; }
warn() { echo -e "\033[33m[WARN]\033[0m $*"; }
err() { echo -e "\033[31m[ERROR]\033[0m $*" >&2; }

# ============================================================================
# 1. 拉取源码
# ============================================================================
clone_repo() {
    local dir="$1" repo="$2" branch="$3"
    if [[ -d "$dir/.git" ]]; then
        log "仓库已存在，跳过 clone: $dir"
        return 0
    fi
    log "克隆仓库: $repo (分支: $branch) -> $dir"
    git clone --depth 1 -b "$branch" "$repo" "$dir"
}

clone_ylt() {
    if [[ -d "$YLT_DIR/.git" ]]; then
        log "yalantinglibs 已存在，跳过 clone: $YLT_DIR"
        return 0
    fi
    log "克隆 yalantinglibs: $YLT_REPO -> $YLT_DIR"
    git clone "$YLT_REPO" "$YLT_DIR"
    log "检出 yalantinglibs commit: $YLT_COMMIT"
    git -C "$YLT_DIR" checkout "$YLT_COMMIT"
}

step_clone() {
    log "========== 步骤 1/5: 拉取源码 =========="
    clone_repo "$MEMFABRIC_DIR" "$MEMFABRIC_REPO" "$MEMFABRIC_BRANCH"
    clone_repo "$MOONCAKE_DIR" "$MOONCAKE_REPO" "$MOONCAKE_BRANCH"
    clone_ylt
    log "源码拉取完成"
}

# ============================================================================
# 2. 编译 MemFabric
# ============================================================================
step_build_mf() {
    if [[ $SKIP_BUILD_MF -eq 1 ]]; then
        log "跳过 MemFabric 编译"
        if [[ ! -f "$MEMFABRIC_OUTPUT/smem/lib64/libmf_smem.so" ]]; then
            local fallback="/home/l30023043/memfabric_hybrid/output"
            if [[ -f "$fallback/smem/lib64/libmf_smem.so" ]]; then
                log "使用已有 MemFabric 产物: $fallback -> $MEMFABRIC_OUTPUT"
                mkdir -p "$MEMFABRIC_OUTPUT"
                cp -a "$fallback"/* "$MEMFABRIC_OUTPUT/"
            else
                err "MemFabric 产物不存在 ($MEMFABRIC_OUTPUT 或 $fallback)，请去掉 --skip_build_mf 重新编译"
                return 1
            fi
        fi
        return 0
    fi
    log "========== 步骤 2/5: 编译 MemFabric =========="
    if [[ ! -f "$MEMFABRIC_DIR/script/build_and_pack_run.sh" ]]; then
        err "MemFabric 构建脚本不存在: $MEMFABRIC_DIR/script/build_and_pack_run.sh"
        return 1
    fi
    log "初始化 MemFabric 子模块"
    git -C "$MEMFABRIC_DIR" submodule update --recursive --init || warn "子模块初始化警告（可能已存在）"
    log "编译 MemFabric (RELEASE, Python ON, NPU)"
    bash "$MEMFABRIC_DIR/script/build_and_pack_run.sh" --build_mode RELEASE --build_python ON --xpu_type NPU --build_hcom OFF --build_hcom_rdma OFF
    log "MemFabric 编译完成: $MEMFABRIC_OUTPUT"
    if [[ ! -f "$MEMFABRIC_OUTPUT/smem/lib64/libmf_smem.so" ]]; then
        err "libmf_smem.so 未生成，MemFabric 编译可能失败"
        return 1
    fi
}

# ============================================================================
# 3. 打补丁 + 编译 Mooncake
# ============================================================================
step_patch_mooncake() {
    log "========== 步骤 3/5: 打补丁 Mooncake =========="
    local patch_file="${TEST_ROOT}/patches/mooncake/tag_v0.3.13.patch"
    if [[ ! -f "$patch_file" ]]; then
        err "补丁文件不存在: $patch_file"
        return 1
    fi
    log "还原 Mooncake 工作区"
    git -C "$MOONCAKE_DIR" checkout -- . 2>/dev/null || true
    git -C "$MOONCAKE_DIR" clean -fd 2>/dev/null || true
    log "应用补丁: $patch_file"
    git -C "$MOONCAKE_DIR" apply "$patch_file" 2>&1 || {
        err "打补丁失败"
        return 1
    }
    log "修正 yalantinglibs 路径: /tmp/ylt_src -> $YLT_DIR"
    sed -i "s|SOURCE_DIR /tmp/ylt_src|SOURCE_DIR $YLT_DIR|g" \
        "$MOONCAKE_DIR/mooncake-common/FindYLT.cmake"
    log "补丁应用成功"
}

step_build_mooncake() {
    if [[ $SKIP_BUILD_MOONCAKE -eq 1 ]]; then
        log "跳过 Mooncake 编译"
        return 0
    fi
    log "========== 步骤 4/5: 编译 Mooncake =========="
    local build_dir="${MOONCAKE_DIR}/build"
    rm -rf "$build_dir"
    mkdir -p "$build_dir"
    log "CMake 配置 (USE_MF=ON)"
    cmake -S "$MOONCAKE_DIR" -B "$build_dir" \
        -DUSE_MF=ON \
        -DWITH_STORE_RUST=OFF \
        -DWITH_P2P_STORE=OFF \
        -DWITH_STORE_GO=OFF \
        -DWITH_EP=OFF \
        -DBUILD_UNIT_TESTS=OFF \
        -DBUILD_BENCHMARK=OFF \
        -DCMAKE_BUILD_TYPE=Release \
        2>&1 | tail -5
    log "编译 Mooncake (make -j$(nproc))"
    make -C "$build_dir" -j"$(nproc)" 2>&1 | tail -5
    log "Mooncake 编译完成: $build_dir"

    log "组装 Python 包"
    mkdir -p "${MOONCAKE_PKG}/mooncake"
    local py_tag
    py_tag=$(python3 -c "import sys; print(f'cpython-{sys.version_info.major}{sys.version_info.minor}')")
    local arch_tag
    arch_tag=$(uname -m)
    local so_suffix="${py_tag}-${arch_tag}-linux-gnu"
    cp "${build_dir}/mooncake-integration/engine.${so_suffix}.so" \
       "${MOONCAKE_PKG}/mooncake/engine.so"
    cp "${build_dir}/mooncake-integration/store.${so_suffix}.so" \
       "${MOONCAKE_PKG}/mooncake/store.so"
    touch "${MOONCAKE_PKG}/mooncake/__init__.py"
    log "Python 包就绪: $MOONCAKE_PKG"
}

# ============================================================================
# 4. 运行测试
# ============================================================================
step_test() {
    if [[ $SKIP_TEST -eq 1 ]]; then
        log "跳过测试"
        return 0
    fi
    log "========== 步骤 5/5: 运行测试 =========="
    local demo="${TEST_ROOT}/tests/demo_pd_transfer.py"
    if [[ ! -f "$demo" ]]; then
        err "测试脚本不存在: $demo"
        return 1
    fi

    export MEMFABRIC_OUTPUT="$MEMFABRIC_OUTPUT"
    export MOONCAKE_BUILD="${MOONCAKE_DIR}/build"
    export PYTHONPATH="${MOONCAKE_PKG}:${PYTHONPATH:-}"
    export GLOG_logtostderr=1
    export GLOG_minloglevel=2
    export MF_STORE_URL="tcp://127.0.0.1:8570"
    export MF_DRAM_SIZE=$((1 << 30))
    export MF_HBM_SIZE=0
    export MF_OP_TYPE=device_sdma
    export MF_PD_OP_TYPE=device_sdma
    export MF_LOG_LEVEL=1

    # MemFabric 库路径
    export LD_LIBRARY_PATH="${MEMFABRIC_OUTPUT}/aarch64-linux/lib64:${MEMFABRIC_OUTPUT}/smem/lib64:${MEMFABRIC_OUTPUT}/hybm/lib64:${MEMFABRIC_OUTPUT}/acc_links/lib64:${MEMFABRIC_OUTPUT}/acc_offload/lib64:${MOONCAKE_DIR}/build/mooncake-common:${MOONCAKE_DIR}/build/mooncake-transfer-engine/src/transport/mf_transport:${LD_LIBRARY_PATH:-}"

    if [[ -n "$SINGLE_TEST" ]]; then
        log "运行单个测试: $SINGLE_TEST"
        python3 -u "$demo" "$SINGLE_TEST"
    else
        log "运行全部 10 个测试"
        python3 -u "$demo"
    fi
}

# ============================================================================
# 5. 运行 vLLM 集成测试
# ============================================================================
pick_free_device() {
    # 解析 npu-smi info 输出，选择内存使用率最低的设备
    python3 -c "
import subprocess, re
out = subprocess.run(['npu-smi','info'], capture_output=True, text=True).stdout
best_dev, best_ratio = None, 1.0
for line in out.splitlines():
    m = re.match(r'\|\s+(\d+)\s+(\d+)\s+\|\s+[\d:]+\.\d+\s+\|\s+\d+\s+(\d+)\s*/\s*(\d+)', line)
    if m:
        dev = int(m.group(2))
        used = int(m.group(3))
        total = int(m.group(4))
        ratio = used / total if total > 0 else 1.0
        if ratio < best_ratio and total > 40000:
            best_ratio, best_dev = ratio, dev
print(best_dev if best_dev is not None else 0)
" 2>/dev/null || echo 0
}

step_vllm_test() {
    if [[ $SKIP_VLLM_TEST -eq 1 ]]; then
        log "跳过 vLLM 集成测试"
        return 0
    fi
    log "========== 运行 vLLM 集成测试 =========="
    local demo="${TEST_ROOT}/tests/demo_vllm_pd.py"
    if [[ ! -f "$demo" ]]; then
        err "vLLM 测试脚本不存在: $demo"
        return 1
    fi

    # 检查 vLLM 是否可用
    if ! python3 -c "import vllm" 2>/dev/null; then
        warn "vLLM 未安装，跳过 vLLM 集成测试"
        return 0
    fi

    # 自动修复 vLLM 混合注意力 KV cache tuple 处理 bug
    # (vLLM 0.26.0 get_transfer_cache_regions 对 tuple cache 的兼容性)
    _patch_vllm_hybrid_kv

    # 修复 MooncakeStore 相关问题（store.so 缺失符号 + client_service 不支持 mf 协议）
    _fix_mooncake_store

    # 修复 vLLM store/worker.py: 按组分配段地址 + SDMA 传输重试
    _patch_vllm_store_worker

    # 检查模型路径
    if [[ ! -d "$VLLM_MODEL_PATH" ]]; then
        err "模型路径不存在: $VLLM_MODEL_PATH"
        return 1
    fi

    # 检查 mooncake 编译产物
    if [[ ! -f "${MOONCAKE_PKG}/mooncake/engine.so" ]]; then
        err "Mooncake Python 包不存在: ${MOONCAKE_PKG}/mooncake/engine.so"
        err "请先编译 Mooncake（去掉 --skip_build_mooncake）"
        return 1
    fi

    # 检查 MemFabric 产物
    if [[ ! -f "${MEMFABRIC_OUTPUT}/smem/lib64/libmf_smem.so" ]]; then
        err "MemFabric 产物不存在: ${MEMFABRIC_OUTPUT}/smem/lib64/libmf_smem.so"
        return 1
    fi

    # 自动选择空闲设备
    if [[ -z "$VLLM_DEVICE" ]]; then
        VLLM_DEVICE=$(pick_free_device)
        log "自动选择 NPU 设备: $VLLM_DEVICE"
    fi

    export VLLM_MODEL_PATH="$VLLM_MODEL_PATH"
    export VLLM_DEVICE="$VLLM_DEVICE"
    export VLLM_PORT="$VLLM_PORT"
    export VLLM_TIMEOUT="$VLLM_TIMEOUT"
    export MEMFABRIC_OUTPUT="$MEMFABRIC_OUTPUT"
    export MOONCAKE_PKG="$MOONCAKE_PKG"
    export MOONCAKE_BUILD="${MOONCAKE_DIR}/build"

    log "vLLM 模型: $VLLM_MODEL_PATH"
    log "NPU 设备: $VLLM_DEVICE, 端口: $VLLM_PORT"
    python3 -u "$demo"
}

step_ttft_test() {
    if [[ $SKIP_TTFT_TEST -eq 1 ]]; then
        log "跳过 TTFT 基准测试"
        return 0
    fi
    log "========== 运行 TTFT 基准测试 =========="
    local demo="${TEST_ROOT}/tests/demo_vllm_ttft.py"
    if [[ ! -f "$demo" ]]; then
        err "TTFT 测试脚本不存在: $demo"
        return 1
    fi

    # 检查 vLLM 是否可用
    if ! python3 -c "import vllm" 2>/dev/null; then
        warn "vLLM 未安装，跳过 TTFT 基准测试"
        return 0
    fi

    # 应用与 vLLM 集成测试相同的补丁（函数内部幂等，已打补丁则跳过）
    _patch_vllm_hybrid_kv
    _fix_mooncake_store
    _patch_vllm_store_worker

    # 检查模型路径
    if [[ ! -d "$VLLM_MODEL_PATH" ]]; then
        err "模型路径不存在: $VLLM_MODEL_PATH"
        return 1
    fi

    # 检查 mooncake 编译产物
    if [[ ! -f "${MOONCAKE_PKG}/mooncake/engine.so" ]]; then
        err "Mooncake Python 包不存在: ${MOONCAKE_PKG}/mooncake/engine.so"
        err "请先编译 Mooncake（去掉 --skip_build_mooncake）"
        return 1
    fi

    # 检查 MemFabric 产物
    if [[ ! -f "${MEMFABRIC_OUTPUT}/smem/lib64/libmf_smem.so" ]]; then
        err "MemFabric 产物不存在: ${MEMFABRIC_OUTPUT}/smem/lib64/libmf_smem.so"
        return 1
    fi

    # 自动选择空闲设备
    if [[ -z "$VLLM_DEVICE" ]]; then
        VLLM_DEVICE=$(pick_free_device)
        log "自动选择 NPU 设备: $VLLM_DEVICE"
    fi

    export VLLM_MODEL_PATH="$VLLM_MODEL_PATH"
    export VLLM_DEVICE="$VLLM_DEVICE"
    export VLLM_PORT="$VLLM_PORT"
    export VLLM_TIMEOUT="$VLLM_TIMEOUT"
    export MEMFABRIC_OUTPUT="$MEMFABRIC_OUTPUT"
    export MOONCAKE_PKG="$MOONCAKE_PKG"
    export MOONCAKE_BUILD="${MOONCAKE_DIR}/build"

    log "TTFT 模型: $VLLM_MODEL_PATH"
    log "NPU 设备: $VLLM_DEVICE, 端口: $VLLM_PORT"
    python3 -u "$demo"
}

_patch_vllm_hybrid_kv() {
    # 修复 vLLM 0.26.0 对混合注意力模型 KV cache tuple 处理的 bug
    # 在 get_transfer_cache_regions 中，非 MambaSpec 层的 cache 可能是 tuple
    local utils_file
    utils_file=$(python3 -c "
import vllm.distributed.kv_transfer.kv_connector.utils as u
print(u.__file__)
" 2>/dev/null)
    if [[ -z "$utils_file" || ! -f "$utils_file" ]]; then
        warn "无法定位 vLLM utils.py，跳过 hybrid KV 修复"
        return 0
    fi
    if grep -q "Hybrid attention models" "$utils_file" 2>/dev/null; then
        log "vLLM hybrid KV tuple 修复已存在"
        return 0
    fi
    log "应用 vLLM hybrid KV tuple 修复: $utils_file"
    python3 -c "
import re
path = '$utils_file'
with open(path) as f:
    src = f.read()
old = '''        if isinstance(layer_spec, MambaSpec):
            # Register the whole kv cache shared tensor, including
            # SSM/Conv.
            conv, ssm = cache
            return [conv]

        # Check may be hacky but it's matching
        # \`_update_hybrid_attention_mamba_layout\`.
        if self.is_mamba and cache.shape[0] == 2:'''
new = '''        if isinstance(layer_spec, MambaSpec):
            # Register the whole kv cache shared tensor, including
            # SSM/Conv.
            conv, ssm = cache
            return [conv]

        # Hybrid attention models (e.g. Qwen3.5 with linear + full
        # attention) may pass KV cache as a tuple of (k, v) tensors.
        # Register each tensor separately.
        if isinstance(cache, tuple):
            return list(cache)

        # Check may be hacky but it's matching
        # \`_update_hybrid_attention_mamba_layout\`.
        if self.is_mamba and cache.shape[0] == 2:'''
if old in src:
    src = src.replace(old, new)
    with open(path, 'w') as f:
        f.write(src)
    print('patched')
else:
    print('pattern not found, skipping')
" 2>&1 | tail -1
}

_fix_mooncake_store() {
    # 1. Patch client_service.cpp to support protocol=mf (if not already patched)
    local cs_file="${MOONCAKE_DIR}/mooncake-store/src/client_service.cpp"
    if [[ -f "$cs_file" ]] && ! grep -q 'protocol == "mf"' "$cs_file" 2>/dev/null; then
        log "Patch mooncake store: add mf protocol support"
        sed -i 's/protocol == "ascend" || protocol == "ubshmem" ||/protocol == "ascend" || protocol == "ubshmem" || protocol == "mf" ||/' "$cs_file"
        # Rebuild store
        log "Rebuilding mooncake store with mf protocol support..."
        make -C "${MOONCAKE_DIR}/build" -j"$(nproc)" store 2>&1 | tail -3
        # Copy updated store.so
        local py_tag arch_tag so_suffix
        py_tag=$(python3 -c "import sys; print(f'cpython-{sys.version_info.major}{sys.version_info.minor}')")
        arch_tag=$(uname -m)
        so_suffix="${py_tag}-${arch_tag}-linux-gnu"
        cp "${MOONCAKE_DIR}/build/mooncake-integration/store.${so_suffix}.so" \
           "${MOONCAKE_PKG}/mooncake/store.so"
        log "store.so updated with mf protocol support"
    else
        log "mooncake store mf protocol patch already applied"
    fi

    # 2. Build libub_allocator.so (provides ub_allocate_memory/ub_free_memory)
    local ub_obj="${MOONCAKE_DIR}/build/mooncake-transfer-engine/src/transport/kunpeng_transport/CMakeFiles/ub_allocator.dir/ub_allocator.cpp.o"
    local ub_out="${MOONCAKE_DIR}/build/mooncake-transfer-engine/src/transport/mf_transport/libub_allocator.so"
    if [[ -f "$ub_obj" && ! -f "$ub_out" ]]; then
        log "Building libub_allocator.so..."
        g++ -shared -o "$ub_out" "$ub_obj" \
            -I"${MOONCAKE_DIR}/mooncake-transfer-engine/include" \
            -lnuma -lglog -lpthread
        log "libub_allocator.so built: $ub_out"
    elif [[ -f "$ub_out" ]]; then
        log "libub_allocator.so already exists"
    else
        warn "ub_allocator.cpp.o not found, store.so may have undefined symbols"
    fi

    # 3. Patch vLLM MooncakeStoreWorker._repr_tensor for hybrid attention tuple
    #    (same class of bug as utils.py — hybrid models pass KV cache as tuple)
    local sw_file
    sw_file=$(python3 -c "
import importlib.util
spec = importlib.util.find_spec('vllm')
if spec:
    import pathlib
    p = pathlib.Path(spec.origin).parent / 'distributed' / 'kv_transfer' / 'kv_connector' / 'v1' / 'mooncake' / 'store' / 'worker.py'
    print(p)
" 2>/dev/null)
    if [[ -z "$sw_file" || ! -f "$sw_file" ]]; then
        warn "无法定位 vLLM store/worker.py，跳过 _repr_tensor 修复"
        return 0
    fi
    if grep -q "Hybrid attention models (e.g. Qwen3.5" "$sw_file" 2>/dev/null; then
        log "vLLM store/worker.py _repr_tensor 修复已存在"
        return 0
    fi
    log "应用 vLLM store/worker.py _repr_tensor tuple 修复: $sw_file"
    python3 - "$sw_file" <<'PYEOF' 2>&1 | tail -1
import sys
path = sys.argv[1]
with open(path) as f:
    src = f.read()
old = '''        # underlying raw storage, so we take the first one.
        def _repr_tensor(v: torch.Tensor | list[torch.Tensor]) -> torch.Tensor:
            assert isinstance(v, torch.Tensor | list)
            return v if isinstance(v, torch.Tensor) else v[0]'''
new = '''        # underlying raw storage, so we take the first one.
        # Hybrid attention models (e.g. Qwen3.5) may pass KV cache as a
        # tuple of (k, v) tensors; handle both list and tuple.
        def _repr_tensor(v: torch.Tensor | list | tuple) -> torch.Tensor:
            assert isinstance(v, (torch.Tensor, list, tuple))
            return v if isinstance(v, torch.Tensor) else v[0]'''
if old in src:
    src = src.replace(old, new)
    with open(path, 'w') as f:
        f.write(src)
    print('patched')
else:
    print('pattern not found, skipping')
PYEOF
}

_patch_vllm_store_worker() {
    # 修复 vLLM store/worker.py:
    #   12. register_kv_caches 按组分配段地址（hybrid 模型必需）
    #   13. save_put / load_get SDMA 传输重试（310P3 首次传输可能失败）
    local sw_file
    sw_file=$(python3 -c "
import importlib.util
spec = importlib.util.find_spec('vllm')
if spec:
    import pathlib
    p = pathlib.Path(spec.origin).parent / 'distributed' / 'kv_transfer' / 'kv_connector' / 'v1' / 'mooncake' / 'store' / 'worker.py'
    print(p)
" 2>/dev/null)
    if [[ -z "$sw_file" || ! -f "$sw_file" ]]; then
        warn "无法定位 vLLM store/worker.py，跳过 per-group + retry 修复"
        return 0
    fi
    local patch_script="${TEST_ROOT}/scripts/patch_vllm_store_worker.py"
    if [[ ! -f "$patch_script" ]]; then
        warn "patch 脚不存在: $patch_script"
        return 0
    fi
    log "应用 vLLM store/worker.py 按组段分配 + 传输重试修复: $sw_file"
    python3 "$patch_script" "$sw_file"
}

# ============================================================================
# 主流程
# ============================================================================
main() {
    log "工作目录: $WORK_DIR"
    log "测试根目录: $TEST_ROOT"

    if [[ $ONLY_TEST -eq 0 && $ONLY_VLLM_TEST -eq 0 && $ONLY_TTFT_TEST -eq 0 ]]; then
        step_clone
        step_build_mf
        step_patch_mooncake
        step_build_mooncake
    else
        if [[ $ONLY_TEST -eq 1 ]]; then
            log "仅 PD 传输测试模式（跳过所有编译步骤）"
            if [[ ! -d "$MOONCAKE_DIR/build" ]]; then
                err "Mooncake 未编译，无法仅测试。请先完整运行一次。"
                exit 1
            fi
        fi
        if [[ $ONLY_VLLM_TEST -eq 1 ]]; then
            log "仅 vLLM 集成测试模式（跳过所有编译步骤）"
            if [[ ! -f "${MOONCAKE_PKG}/mooncake/engine.so" ]]; then
                err "Mooncake Python 包不存在，无法仅测试。请先完整运行一次。"
                exit 1
            fi
        fi
        if [[ $ONLY_TTFT_TEST -eq 1 ]]; then
            log "仅 TTFT 基准测试模式（跳过所有编译步骤）"
            if [[ ! -f "${MOONCAKE_PKG}/mooncake/engine.so" ]]; then
                err "Mooncake Python 包不存在，无法仅测试。请先完整运行一次。"
                exit 1
            fi
        fi
    fi
    if [[ $ONLY_VLLM_TEST -eq 0 && $ONLY_TTFT_TEST -eq 0 ]]; then
        step_test
    fi
    if [[ $ONLY_TTFT_TEST -eq 0 ]]; then
        step_vllm_test
    fi
    step_ttft_test
    log "全部流程完成！"
}

main "$@"
