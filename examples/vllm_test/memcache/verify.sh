#!/bin/bash
# verify.sh — 验证 vLLM TTFT 测试环境部署
#
# 用法: bash verify.sh --memcache-dir <DIR> [--vllm-ascend-dir <DIR>]

set -uo pipefail

readonly PKG_DIR="$(cd "$(dirname "$0")" && pwd)"

MEMCACHE_DIR=""
VLLM_ASCEND_DIR=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --memcache-dir)    MEMCACHE_DIR="$2"; shift 2 ;;
        --vllm-ascend-dir) VLLM_ASCEND_DIR="$2"; shift 2 ;;
        *) shift ;;
    esac
done

[[ -z "$MEMCACHE_DIR" ]] && { echo "用法: bash verify.sh --memcache-dir <DIR> [--vllm-ascend-dir <DIR>]"; exit 1; }

PASS=0
FAIL=0

check() {
    if [[ "$2" == "1" ]]; then
        echo -e "  \033[32m[PASS]\033[0m $1"
        PASS=$((PASS + 1))
    else
        echo -e "  \033[31m[FAIL]\033[0m $1"
        FAIL=$((FAIL + 1))
    fi
}

echo "========== vLLM TTFT 测试环境验证 =========="
echo ""

echo "--- 1. 打包目录测试文件 ---"
check "cases/config/mmc-local-workload.conf 存在" \
    "$([[ -f "$PKG_DIR/cases/config/mmc-local-workload.conf" ]] && echo 1 || echo 0)"
check "cases/config/mmc-local-integration.conf 存在" \
    "$([[ -f "$PKG_DIR/cases/config/mmc-local-integration.conf" ]] && echo 1 || echo 0)"
check "cases/python/memcache/workload/ 目录存在" \
    "$([[ -d "$PKG_DIR/cases/python/memcache/workload" ]] && echo 1 || echo 0)"
check "cases/python/memcache/integration/ 目录存在" \
    "$([[ -d "$PKG_DIR/cases/python/memcache/integration" ]] && echo 1 || echo 0)"
check "run_test.sh 存在" \
    "$([[ -f "$PKG_DIR/run_test.sh" ]] && echo 1 || echo 0)"
check "patches/memcache_modified_files.patch 存在" \
    "$([[ -f "$PKG_DIR/patches/memcache_modified_files.patch" ]] && echo 1 || echo 0)"
check "patches/vllm_ascend_310p_kv_transfer.patch 存在" \
    "$([[ -f "$PKG_DIR/patches/vllm_ascend_310p_kv_transfer.patch" ]] && echo 1 || echo 0)"

echo ""
echo "--- 2. 辅助脚本 ---"
check "aux_scripts/clean_stale_env.sh 存在" \
    "$([[ -f "$PKG_DIR/aux_scripts/clean_stale_env.sh" ]] && echo 1 || echo 0)"
check "aux_scripts/repro_store_init_207001.py 存在" \
    "$([[ -f "$PKG_DIR/aux_scripts/repro_store_init_207001.py" ]] && echo 1 || echo 0)"

echo ""
echo "--- 3. memcache 仓库补丁 ---"
# v1.2.1 已合入补丁，检测源码是否包含变更
check "pymmc.cpp 已修改（pybind11 enum cast）" \
    "$(grep -q 'static_cast<int32_t>(SMEMB_COPY' "$MEMCACHE_DIR/src/memcache/csrc/python_wrapper/pymmc.cpp" 2>/dev/null && echo 1 || echo 0)"
check "python_wrapper/CMakeLists.txt 已修改" \
    "$(grep -q 'find_program(PYTHON_EXECUTABLE' "$MEMCACHE_DIR/src/memcache/csrc/python_wrapper/CMakeLists.txt" 2>/dev/null && echo 1 || echo 0)"

echo ""
echo "--- 4. memfabric_hybrid 子模块 ---"
MFH_DIR="$MEMCACHE_DIR/3rdparty/memfabric_hybrid"
if [[ -d "$MFH_DIR" ]] && (cd "$MFH_DIR" && git rev-parse HEAD >/dev/null 2>&1); then
    MFH_BRANCH=$(cd "$MFH_DIR" && git rev-parse --abbrev-ref HEAD 2>/dev/null || echo "?")
    check "子模块为 git 仓库" "1"
    check "分支为 dev-v1.2.0 (当前: $MFH_BRANCH)" \
        "$([[ "$MFH_BRANCH" == "dev-v1.2.0" ]] && echo 1 || echo 0)"

    SDID_FIX=0
    if [[ -f "$MFH_DIR/output/hybm/lib64/libmf_hybm_core.so" ]]; then
        if strings "$MFH_DIR/output/hybm/lib64/libmf_hybm_core.so" 2>/dev/null | grep "using 0, may affect" >/dev/null 2>&1; then
            SDID_FIX=1
        fi
    fi
    if [[ "$SDID_FIX" -eq 0 ]]; then
        INSTALLED_SO=$(python3 -c "import memfabric_hybrid, os; print(os.path.dirname(memfabric_hybrid.__file__)+'/lib/libmf_hybm_core.so')" 2>/dev/null || echo "")
        if [[ -n "$INSTALLED_SO" && -f "$INSTALLED_SO" ]]; then
            strings "$INSTALLED_SO" 2>/dev/null | grep "using 0, may affect" >/dev/null 2>&1 && SDID_FIX=1
        fi
    fi
    check "SDID WARN fallback 修复已包含" "$SDID_FIX"
else
    check "子模块为 git 仓库" "0"
fi

echo ""
echo "--- 5. vLLM-Ascend 补丁 ---"
if [[ -z "$VLLM_ASCEND_DIR" ]]; then
    VLLM_ASCEND_DIR=$(python3 -c "
import vllm_ascend, os
print(os.path.dirname(os.path.dirname(vllm_ascend.__file__)))
" 2>/dev/null || echo "")
fi
if [[ -n "$VLLM_ASCEND_DIR" && -f "$VLLM_ASCEND_DIR/vllm_ascend/_310p/model_runner_310p.py" ]]; then
    MR="$VLLM_ASCEND_DIR/vllm_ascend/_310p/model_runner_310p.py"
    PW="$VLLM_ASCEND_DIR/vllm_ascend/distributed/kv_transfer/kv_pool/ascend_store/pool_worker.py"
    MB="$VLLM_ASCEND_DIR/vllm_ascend/distributed/kv_transfer/kv_pool/ascend_store/backend/memcache_backend.py"
    check "model_runner_310p.py: 移除 310P KV transfer 阻断" \
        "$(grep -q 'KV cache transfer is not supported' "$MR" 2>/dev/null && echo 0 || echo 1)"
    check "pool_worker.py: 2MB 对齐 assert 改为 warning" \
        "$(grep -q 'not 2MB-aligned' "$PW" 2>/dev/null && echo 1 || echo 0)"
    check "memcache_backend.py: 新增 _is_host_shm / _get_protocol" \
        "$(grep -q '_is_host_shm' "$MB" 2>/dev/null && grep -q '_get_protocol' "$MB" 2>/dev/null && echo 1 || echo 0)"
else
    echo -e "  \033[33m[SKIP]\033[0m vLLM-Ascend 未找到（仅 Layer 2 可跳过）"
fi

echo ""
echo "--- 6. 构建产物 ---"
check "output/memcache/ 目录存在" \
    "$([[ -d "$MEMCACHE_DIR/output/memcache" ]] && echo 1 || echo 0)"
check "output/memcache/bin/mmc_meta_service 存在" \
    "$([[ -f "$MEMCACHE_DIR/output/memcache/bin/mmc_meta_service" ]] && echo 1 || echo 0)"
WHEEL=$(ls "$MEMCACHE_DIR"/output/memcache/wheel/memcache_hybrid-*.whl 2>/dev/null | head -1)
check "memcache_hybrid wheel 存在" \
    "$([[ -n "$WHEEL" ]] && echo 1 || echo 0)"

echo ""
echo "========== 结果: PASS=$PASS  FAIL=$FAIL =========="
[[ "$FAIL" -gt 0 ]] && exit 1 || exit 0
