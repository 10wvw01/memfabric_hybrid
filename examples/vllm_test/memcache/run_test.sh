#!/bin/bash
# run_test.sh — Unified vLLM TTFT test runner
#
# Run MemCache workloads and vLLM integration tests.
#
# Usage:
#   bash run_test.sh --memcache-dir <DIR> [options]
#
# Environment variables:
#   MMC_TEST_VLLM_MODEL_PATH <PATH>          W8A8 model path (required for integration/
#                                           largecap/puredram; not needed for workload)
#
# Options:
#   --memcache-dir <DIR>                    memcache repo dir (required)
#   --type workload|integration|largecap|puredram  test type (default: workload)
#                                           puredram: pure-DRAM (host_shm, 0 HBM) 4-rank
#   --rank 1|2|4                            rank count: 1-rank (TP=1), 2-rank (TP=2) or 4-rank (TP=4) (integration, required for integration)
#   --suite integration|all              large-capacity test selection (default: all)
#   --protocol host_shm|device_sdma         protocol (integration only, default: config default)
#   --device-ids <a,b,...>                  NPU device IDs (default: 0 for 1-rank, 6,7 for 2-rank, 0,1,2,3 for 4-rank, 4,5,6,7 for largecap/puredram)
#   --filter <name>                         pytest -m marker filter (e.g. rank2, rank4, throughput, hitdecode)
#   --log                                   enable file logging to retest_logs/
#   --gpu-mem-util <float>                  GPU mem util (default: 0.80, largecap: 0.70, puredram: 0.60)
#   --skip-clean                            skip env cleanup before tests
#   --split                                 4-rank integration/puredram: run each test separately
#                                           with NPU cleanup between (recommended for 310P3)
#   --help                                  show help
#
# Examples:
#   # MemCache workload tests
#   bash run_test.sh --memcache-dir /path/to/memcache --type workload --device-ids 6,7
#
#   # vLLM 集成测试 1-rank (TP=1)
#   bash run_test.sh --memcache-dir /path/to/memcache --type integration --rank 1 --protocol device_sdma --device-ids 0 --log
#
#   # vLLM 集成测试 2-rank (TP=2)
#   bash run_test.sh --memcache-dir /path/to/memcache --type integration --rank 2 --protocol host_shm --device-ids 6,7 --log
#
#   # vLLM 集成测试 4-rank (TP=4)
#   bash run_test.sh --memcache-dir /path/to/memcache --type integration --rank 4 --protocol device_sdma --device-ids 0,1,2,3 --log
#
#   # Large-capacity test (TP=4, large HBM + DRAM)
#   bash run_test.sh --memcache-dir /path/to/memcache --type largecap --suite integration --device-ids 4,5,6,7 --log
#
#   # Pure-DRAM test (TP=4, host_shm, 0 HBM, 10GB DRAM)
#   bash run_test.sh --memcache-dir /path/to/memcache --type puredram --device-ids 4,5,6,7 --log

set -euo pipefail

readonly PKG_DIR="$(cd "$(dirname "$0")" && pwd)"

# ============================================================================
# Parameter parsing
# ============================================================================
MEMCACHE_DIR=""
TEST_TYPE="workload"
RANK=""
LARGECAP_SUITE="all"
PROTOCOL=""
DEVICE_IDS=""
TEST_FILTER=""
ENABLE_LOG=0
GPU_MEM_UTIL=""
SKIP_CLEAN=0
SPLIT_TESTS=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --memcache-dir)       MEMCACHE_DIR="$2"; shift 2 ;;
        --type)               TEST_TYPE="$2"; shift 2 ;;
        --rank)               RANK="$2"; shift 2 ;;
        --suite|--layer)      LARGECAP_SUITE="$2"; shift 2 ;;
        --protocol)           PROTOCOL="$2"; shift 2 ;;
        --device-ids)         DEVICE_IDS="$2"; shift 2 ;;
        --filter)             TEST_FILTER="$2"; shift 2 ;;
        --log)                ENABLE_LOG=1; shift ;;
        --gpu-mem-util)       GPU_MEM_UTIL="$2"; shift 2 ;;
        --skip-clean)         SKIP_CLEAN=1; shift ;;
        --split)              SPLIT_TESTS=1; shift ;;
        --help|-h)            sed -n '2,40p' "$0"; exit 0 ;;
        *) echo "[ERROR] 未知参数: $1"; exit 1 ;;
    esac
done

[[ -z "$MEMCACHE_DIR" ]] && { echo "[ERROR] --memcache-dir <DIR> 是必选参数"; exit 1; }
[[ ! -d "$MEMCACHE_DIR" ]] && { echo "[ERROR] 目录不存在: $MEMCACHE_DIR"; exit 1; }

case "$TEST_TYPE" in
    workload|integration|largecap|puredram) ;;
    *) echo "[ERROR] --type 只能是 workload, integration, largecap 或 puredram"; exit 1 ;;
esac

# --rank validation (required for integration)
if [[ "$TEST_TYPE" == "integration" ]]; then
    if [[ -z "$RANK" ]]; then
        echo "[ERROR] --type $TEST_TYPE 需要指定 --rank 1、2 或 --rank 4"
        exit 1
    fi
fi
if [[ -n "$RANK" && "$RANK" != "1" && "$RANK" != "2" && "$RANK" != "4" ]]; then
    echo "[ERROR] --rank 只能是 1、2 或 4"
    exit 1
fi

# Device ID count must match rank count (for integration)
if [[ "$TEST_TYPE" == "integration" ]]; then
    DEV_COUNT=$(echo "$DEVICE_IDS" | tr ',' '\n' | wc -l)
    if [[ -n "$DEVICE_IDS" && "$DEV_COUNT" -ne "$RANK" ]]; then
        echo "[ERROR] --device-ids 设备数 ($DEV_COUNT) 与 --rank ($RANK) 不匹配"
        exit 1
    fi
fi

# Type-dependent defaults
if [[ -z "$DEVICE_IDS" ]]; then
    if [[ "$TEST_TYPE" == "largecap" || "$TEST_TYPE" == "puredram" ]]; then
        DEVICE_IDS="4,5,6,7"
    elif [[ "$TEST_TYPE" == "integration" || "$TEST_TYPE" == "bench" ]]; then
        if [[ "$RANK" == "4" ]]; then
            DEVICE_IDS="0,1,2,3"
        elif [[ "$RANK" == "1" ]]; then
            DEVICE_IDS="0"
        else
            DEVICE_IDS="6,7"
        fi
    else
        DEVICE_IDS="6,7"
    fi
fi
if [[ -z "$GPU_MEM_UTIL" ]]; then
    case "$TEST_TYPE" in
        largecap)    GPU_MEM_UTIL="0.70" ;;
        puredram)    GPU_MEM_UTIL="0.60" ;;
        *)           GPU_MEM_UTIL="0.80" ;;
    esac
fi

if [[ -n "$PROTOCOL" && "$PROTOCOL" != "host_shm" && "$PROTOCOL" != "device_sdma" ]]; then
    echo "[ERROR] --protocol 只能是 host_shm 或 device_sdma"
    exit 1
fi

# ============================================================================
# Logging setup
# ============================================================================
LOG_FILE=""
if [[ "$ENABLE_LOG" -eq 1 ]]; then
    LOG_DIR="$PKG_DIR/retest_logs"
    TIMESTAMP=$(date +%Y%m%d_%H%M%S)
    LOG_FILE="$LOG_DIR/${TEST_TYPE}_${DEVICE_IDS//,/_}_${TIMESTAMP}.log"
    mkdir -p "$LOG_DIR"
fi

log() {
    if [[ -n "$LOG_FILE" ]]; then
        echo "$@" | tee -a "$LOG_FILE"
    else
        echo "$@"
    fi
}

# ============================================================================
# Environment setup
# ============================================================================
export ASCEND_HOME_PATH="${ASCEND_HOME_PATH:-/usr/local/Ascend/cann}"
export LD_LIBRARY_PATH="$ASCEND_HOME_PATH/lib64:${LD_LIBRARY_PATH:-}"
export ASCEND_KERNEL_META_DIR="${ASCEND_KERNEL_META_DIR:-/tmp/ascend_kernel_meta}"
mkdir -p "$ASCEND_KERNEL_META_DIR"
export LD_LIBRARY_PATH="$MEMCACHE_DIR/output/memcache/lib64:${LD_LIBRARY_PATH}"

for mod in memcache_hybrid memfabric_hybrid; do
    libdir=$(python3 -c "import $mod, os; print(os.path.dirname($mod.__file__)+'/lib')" 2>/dev/null || echo "")
    [[ -n "$libdir" && -d "$libdir" ]] && export LD_LIBRARY_PATH="$libdir:${LD_LIBRARY_PATH}"
done

MF_OUTPUT="$MEMCACHE_DIR/3rdparty/memfabric_hybrid/output"
for dir in "$MF_OUTPUT/acc_offload/lib64" "$MF_OUTPUT/hybm/lib64" "$MF_OUTPUT/smem/lib64"; do
    [[ -d "$dir" ]] && export LD_LIBRARY_PATH="$dir:$LD_LIBRARY_PATH"
done

export MMC_MEMCACHE_DIR="$MEMCACHE_DIR"
export MMC_META_CONFIG_PATH="$MEMCACHE_DIR/config/mmc-meta.conf"

# ============================================================================
# Config selection + protocol switch
# ============================================================================
resize_shm_for_dram() {
    local min_gb="${1:-50}"
    local target_gb="${2:-100}"
    local SHM_SIZE SHM_SIZE_GB
    SHM_SIZE=$(df --output=size -B1 /dev/shm | tail -1)
    SHM_SIZE_GB=$((SHM_SIZE / 1024 / 1024 / 1024))
    if [[ "$SHM_SIZE_GB" -lt "$min_gb" ]]; then
        log "[WARN] /dev/shm is ${SHM_SIZE_GB}GB, need >= ${min_gb}GB for 4 ranks x 10GB DRAM"
        log "[INFO] Resizing /dev/shm to ${target_gb}GB..."
        mount -o remount,size=${target_gb}G /dev/shm
        SHM_SIZE_GB=$(df --output=size -B1 /dev/shm | tail -1 | awk '{printf "%d", $1/1024/1024/1024}')
    fi
    log "[INFO] /dev/shm size: ${SHM_SIZE_GB}GB"
}

case "$TEST_TYPE" in
    workload)
        export MMC_LOCAL_CONFIG_PATH="$PKG_DIR/cases/config/mmc-local-workload.conf"
        ;;
    integration|bench|hitdecode)
        if [[ "$RANK" == "4" ]]; then
            BASE_CONFIG="$PKG_DIR/cases/config/mmc-local-integration-4rank.conf"
            export MMC_TEST_TENSOR_PARALLEL_SIZE="4"
        elif [[ "$RANK" == "1" ]]; then
            BASE_CONFIG="$PKG_DIR/cases/config/mmc-local-integration-1rank.conf"
            export MMC_TEST_TENSOR_PARALLEL_SIZE="1"
        else
            BASE_CONFIG="$PKG_DIR/cases/config/mmc-local-integration.conf"
            export MMC_TEST_TENSOR_PARALLEL_SIZE="2"
        fi
        export MMC_TEST_GPU_MEM_UTIL="$GPU_MEM_UTIL"
        if [[ -n "$PROTOCOL" ]]; then
            TEMP_CONF="$PKG_DIR/cases/config/.mmc-local-integration-${PROTOCOL}.conf"
            if [[ "$PROTOCOL" == "host_shm" ]]; then
                sed -e "s/^ock\.mmc\.local_service\.protocol.*/ock.mmc.local_service.protocol = host_shm/" \
                    -e "s/^ock\.mmc\.local_service\.hbm\.size.*/ock.mmc.local_service.hbm.size = 0/" \
                    -e "s/^ock\.mmc\.local_service\.dram\.size.*/ock.mmc.local_service.dram.size = 2GB/" \
                    "$BASE_CONFIG" > "$TEMP_CONF"
            else
                sed -e "s/^ock\.mmc\.local_service\.protocol.*/ock.mmc.local_service.protocol = device_sdma/" \
                    "$BASE_CONFIG" > "$TEMP_CONF"
            fi
            export MMC_LOCAL_CONFIG_PATH="$TEMP_CONF"
        else
            export MMC_LOCAL_CONFIG_PATH="$BASE_CONFIG"
        fi
        ;;
    largecap)
        resize_shm_for_dram 50 100
        export MMC_LOCAL_CONFIG_PATH="$PKG_DIR/cases/config/mmc-local-largecap-4rank.conf"
        export MMC_TEST_TENSOR_PARALLEL_SIZE="4"
        export MMC_TEST_GPU_MEM_UTIL="$GPU_MEM_UTIL"
        ;;
    puredram)
        resize_shm_for_dram 50 100
        export MMC_LOCAL_CONFIG_PATH="$PKG_DIR/cases/config/mmc-local-puredram-4rank.conf"
        export MMC_TEST_TENSOR_PARALLEL_SIZE="4"
        export MMC_TEST_GPU_MEM_UTIL="$GPU_MEM_UTIL"
        ;;
esac

# Device IDs
export MMC_TEST_DEVICE_IDS="$DEVICE_IDS"
IFS=',' read -ra _devs <<< "$DEVICE_IDS"
export MMC_TEST_DEVICE_ID="${_devs[0]}"
export MMC_TEST_DEVICE_ID_2="${_devs[1]:-${_devs[0]}}"
export MMC_TEST_DEVICE_ID_3="${_devs[2]:-${_devs[1]:-${_devs[0]}}}"
export MMC_TEST_DEVICE_ID_4="${_devs[3]:-${_devs[2]:-${_devs[1]:-${_devs[0]}}}}"

# ============================================================================
# Print info
# ============================================================================
log "[INFO] === vLLM TTFT test runner ==="
log "[INFO]   PKG_DIR          = $PKG_DIR"
log "[INFO]   MEMCACHE_DIR     = $MEMCACHE_DIR"
log "[INFO]   TEST_TYPE        = $TEST_TYPE"
log "[INFO]   RANK             = ${RANK:-<n/a>}"
log "[INFO]   DEVICE_IDS       = $DEVICE_IDS"
log "[INFO]   PROTOCOL         = ${PROTOCOL:-<config default>}"
log "[INFO]   FILTER           = ${TEST_FILTER:-<none>}"
log "[INFO]   TP               = ${MMC_TEST_TENSOR_PARALLEL_SIZE:-<auto>}"
log "[INFO]   GPU_MEM_UTIL     = $GPU_MEM_UTIL"
log "[INFO]   MMC_LOCAL_CONFIG = $MMC_LOCAL_CONFIG_PATH"
log "[INFO]   ASCEND_HOME_PATH = ${ASCEND_HOME_PATH:-<unset>}"
[[ -n "$LOG_FILE" ]] && log "[INFO]   LOG_FILE         = $LOG_FILE"
log ""

# ============================================================================
# NPU detection
# ============================================================================
check_npu() {
    local total
    total=$(ls -d /dev/davinci[0-9]* 2>/dev/null | wc -l || true)
    [[ "$total" -eq 0 ]] && return 1
    # 不用 npu-smi info：驱动异常时该命令会卡死。设备文件存在即认为 NPU 存在，
    # 设备健康度由后续 check_env.sh 预检判断。
    return 0
}
if ! check_npu; then
    log "[SKIP] No NPU detected"
    exit 0
fi
log "[INFO] NPU detected"

# ============================================================================
# Pre-flight: 驱动健康预检
# 驱动异常时 put_from_layers 的 ioctl 退化数千倍，导致 workload 单次 prefill
# 从 ~4ms 退化到 ~27s。提前做一次 256MB D2D 拷贝，超 50ms 即中止，避免浪费时间。
# ============================================================================
log "[INFO] 预检目标设备驱动健康: $DEVICE_IDS"
if ! bash "$PKG_DIR/check_env.sh" --device-ids "$DEVICE_IDS" 2>&1 | while IFS= read -r line; do log "$line"; done; then
    log "[ERROR] 预检失败，中止测试。排查: dmesg | grep 'devmm.*Map address failed'"
    exit 1
fi

# Verify the repository revisions and runtime packages before starting tests.
VERIFY_ARGS=(verify --memcache-dir "$MEMCACHE_DIR")
if [[ "$TEST_TYPE" == integration || "$TEST_TYPE" == puredram ]]; then
    VERIFY_ARGS+=(--require-vllm)
fi
python3 "$PKG_DIR/source_manifest.py" "${VERIFY_ARGS[@]}"

# ============================================================================
# Clean stale environment
# ============================================================================
CLEAN_SCRIPT="$MEMCACHE_DIR/debug/test/clean_stale_env.sh"
if [[ "$SKIP_CLEAN" -eq 1 || "$TEST_TYPE" == "integration" ]]; then
    log "[INFO] 跳过全局环境清理"
elif [[ -f "$CLEAN_SCRIPT" ]]; then
    log "[INFO] 清理残留进程和 IPC 资源..."
    if [[ -n "$LOG_FILE" ]]; then
        bash "$CLEAN_SCRIPT" 2>&1 | tee -a "$LOG_FILE"
    else
        bash "$CLEAN_SCRIPT"
    fi
else
    log "[WARN] clean_stale_env.sh 未找到: $CLEAN_SCRIPT"
    log "[WARN] 建议手动清理: pkill -9 -f 'mmc_meta_service|VLLM::'; rm -f /dev/shm/mf_dram_*"
fi
log ""

# ============================================================================
# MetaService management
# ============================================================================
readonly META_SERVICE_PORT=5000
readonly META_BOOTSTRAP_SECONDS=3
META_PID=""
META_STARTED_BY_US=false

is_port_listening() {
    local port="$1"
    if command -v ss &>/dev/null; then
        ss -tlnp 2>/dev/null | grep -q ":$port " && return 0
    elif command -v netstat &>/dev/null; then
        netstat -tlnp 2>/dev/null | grep -q ":$port " && return 0
    else
        timeout 2 bash -c "echo >/dev/tcp/127.0.0.1/$port" 2>/dev/null && return 0
    fi
    return 1
}

start_meta_service() {
    log "[INFO] Starting MetaService..."
    local meta_bin="$MEMCACHE_DIR/output/memcache/bin/mmc_meta_service"
    if [[ -f "$meta_bin" ]]; then
        "$meta_bin" &
    else
        python3 -c "from memcache_hybrid import MetaService; MetaService.main()" &
    fi
    META_PID=$!
    META_STARTED_BY_US=true
    sleep "$META_BOOTSTRAP_SECONDS"
    if kill -0 "$META_PID" 2>/dev/null; then
        log "[INFO] MetaService started (PID: $META_PID)"
        return 0
    else
        log "[ERROR] MetaService failed to start"
        return 1
    fi
}

stop_meta_service() {
    if $META_STARTED_BY_US && [[ -n "$META_PID" ]] && kill -0 "$META_PID" 2>/dev/null; then
        log "[INFO] Stopping MetaService (PID: $META_PID)..."
        kill "$META_PID" 2>/dev/null || true
        wait "$META_PID" 2>/dev/null || true
        META_STARTED_BY_US=false
    fi
}

cleanup_meta() {
    stop_meta_service
    if [[ "$TEST_TYPE" != "integration" ]]; then
        pkill -9 -f 'VLLM::' 2>/dev/null || true
    fi
}
trap cleanup_meta EXIT

ensure_meta_service() {
    if is_port_listening "$META_SERVICE_PORT"; then
        log "[INFO] MetaService already running on port $META_SERVICE_PORT"
    else
        start_meta_service || { log "[ERROR] MetaService failed to start"; exit 1; }
    fi
}

# ============================================================================
# Run pytest (continue on failure to collect all errors)
# ============================================================================
run_pytest() {
    log "[INFO] pytest $*"
    cd "$PKG_DIR"
    set +e
    if [[ -n "$LOG_FILE" ]]; then
        python3 -m pytest "$@" 2>&1 | tee -a "$LOG_FILE"
    else
        python3 -m pytest "$@"
    fi
    local exit_code=$?
    set -e
    log "[INFO] pytest exit code: $exit_code"
    return $exit_code
}

# ============================================================================
# Test type implementations
# ============================================================================

# Shared cleanup between split tests: kill stale processes + restart MetaService.
cleanup_between_tests() {
    if [[ "$TEST_TYPE" == "integration" ]]; then
        return
    fi
    if [[ -f "$CLEAN_SCRIPT" ]]; then
        log "[INFO] Cleaning NPU between split tests..."
        if [[ -n "$LOG_FILE" ]]; then
            bash "$CLEAN_SCRIPT" 2>&1 | tee -a "$LOG_FILE"
        else
            bash "$CLEAN_SCRIPT"
        fi
    fi
    stop_meta_service
    sleep 10
    ensure_meta_service
}

# Run pytest tests one-by-one with NPU cleanup between each.
# Args: test_file test_name1 test_name2 ...
run_pytest_split() {
    local test_file="$1"; shift
    local -a test_names=("$@")
    local overall=0
    for tname in "${test_names[@]}"; do
        log ""
        log "[INFO] === Split run: $tname ==="
        run_pytest "$test_file" -v -s -k "$tname"
        local rc=$?
        [[ $rc -ne 0 ]] && overall=$rc
        cleanup_between_tests
    done
    return $overall
}

run_workload() {
    ensure_meta_service
    local test_dir="$PKG_DIR/cases/python/memcache/workload/"
    if [[ -n "$TEST_FILTER" ]]; then
        run_pytest "$test_dir" -v -s -m "$TEST_FILTER"
    else
        run_pytest "$test_dir" -v -s -m workload
    fi
}

run_integration() {
    if ! python3 -c "import vllm" 2>/dev/null; then
        log "[SKIP] vLLM not available, skipping integration tests"
        exit 0
    fi
    local test_file="$PKG_DIR/cases/python/memcache/integration/vllm_integration_test.py"
    if [[ -n "$TEST_FILTER" ]]; then
        run_pytest "$test_file" -v -s -m "$TEST_FILTER"
    elif [[ "$RANK" == "1" && "$SPLIT_TESTS" -eq 1 ]]; then
        # 1-rank: run each test separately with NPU cleanup between.
        run_pytest_split "$test_file" \
            "test_full_miss_baseline_1rank" \
            "test_50pct_hit_1rank" \
            "test_100pct_hit_1rank" \
            "test_cache_source_correctness" \
            "test_native_prefix_cache_benchmark"
    elif [[ "$RANK" == "4" && "$SPLIT_TESTS" -eq 1 ]]; then
        # 4-rank: run each test separately with NPU cleanup between,
        # because 310P3 vLLM exit leaves orphan NPU context (HBM not released).
        run_pytest_split "$test_file" \
            "test_full_miss_baseline_4rank" \
            "test_50pct_hit_4rank" \
            "test_100pct_hit_4rank" \
            "test_cache_source_correctness" \
            "test_native_prefix_cache_benchmark"
    else
        local marker="rank2"
        case "$RANK" in
            1) marker="rank1" ;;
            4) marker="rank4" ;;
        esac
        run_pytest "$test_file" -v -s -m "$marker"
    fi
}

run_puredram() {
    if ! python3 -c "import vllm" 2>/dev/null; then
        log "[SKIP] vLLM not available, skipping pure-DRAM tests"
        exit 0
    fi
    ensure_meta_service
    local test_file="$PKG_DIR/cases/python/memcache/integration/pure_dram_vllm_test.py"
    if [[ -n "$TEST_FILTER" ]]; then
        log "[INFO] Starting pure-DRAM tests (filter: $TEST_FILTER)..."
        run_pytest "$test_file" -v -s -m "$TEST_FILTER"
    elif [[ "$SPLIT_TESTS" -eq 1 ]]; then
        run_pytest_split "$test_file" \
            "test_puredram_full_miss_baseline_4rank" \
            "test_puredram_50pct_hit_4rank" \
            "test_puredram_100pct_hit_4rank"
    else
        log "[INFO] Starting pure-DRAM tests (TP=4, host_shm, 10GB DRAM)..."
        run_pytest "$test_file" -v -s -m rank4
    fi
}

run_largecap_integration() {
    if ! python3 -c "import vllm" 2>/dev/null; then
        log "[SKIP] vLLM not available, skipping vLLM integration tests"
        return 0
    fi
    ensure_meta_service
    local test_file="$PKG_DIR/cases/python/memcache/integration/large_capacity_vllm_test.py"
    if [[ -n "$TEST_FILTER" ]]; then
        log "[INFO] Starting large-capacity vLLM integration tests (filter: $TEST_FILTER)..."
        run_pytest "$test_file" -v -s -m "$TEST_FILTER"
    else
        log "[INFO] Starting large-capacity vLLM integration tests..."
        run_pytest "$test_file" -v -s -m rank4
    fi
    local test_exit=$?
    pkill -9 -f 'VLLM::' 2>/dev/null || true
    return "$test_exit"
}

# ============================================================================
# Main dispatch
# ============================================================================
OVERALL_EXIT=0

case "$TEST_TYPE" in
    workload)
        run_workload || OVERALL_EXIT=$?
        ;;
    integration)
        run_integration || OVERALL_EXIT=$?
        ;;
    largecap)
        case "$LARGECAP_SUITE" in
            integration|layer3|l3|all)
                run_largecap_integration || OVERALL_EXIT=$?
                ;;
            *)
                log "[ERROR] Unknown suite: $LARGECAP_SUITE (use: integration, all)"
                exit 1
                ;;
        esac
        ;;
    puredram)
        run_puredram || OVERALL_EXIT=$?
        # Cleanup vLLM processes after pure-DRAM tests
        pkill -9 -f "vllm serve" 2>/dev/null || true
        sleep 3
        pkill -9 -f "VLLM::" 2>/dev/null || true
        ;;
esac

log "[INFO] Done. Exit code: $OVERALL_EXIT"
[[ -n "$LOG_FILE" ]] && log "[INFO] Full log: $LOG_FILE"
exit $OVERALL_EXIT
