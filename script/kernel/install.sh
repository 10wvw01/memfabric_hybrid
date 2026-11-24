#!/usr/bin/env bash
set -euo pipefail

PACKAGE_NAME="cann-hybm-compat.tar.gz"
JSON_NAME="libcann_hybm_kernel.json"
VERSION_FILE="cann_hybm_kernel_version"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." 2>/dev/null && pwd || true)"
BACKUP_BASENAME=".hybm_aicpu_kernel_backup"

# 310P sdma orchestrator 产物(与 A3/A5 同 .run 包发布, 装主包 cust_aicpu_kernel/ 子目录)
SDMA_ORCH_SO="libmf_sdma_orch.so"
SDMA_ORCH_CUST_JSON="libmf_sdma_orch.json"
SDMA_ORCH_LAUNCH_JSON="libmf_sdma_orch_launch.json"
SDMA_ORCH_VERSION_FILE="sdma_orch_kernel_version"
SDMA_ORCH_BACKUP_BASENAME=".sdma_orch_kernel_backup"

ACTION=""
FORCE=0
INSTALL_FOR_ALL=0

usage() {
    cat <<'EOF'
Usage: install.sh [OPTIONS]

Install, uninstall, or inspect HYBM AICPU kernel artifacts in the CANN OPP tree.
Installation also updates ${ASCEND_HOME_PATH}/conf/ascend_package_load.ini.

Options:
  -h, --help          Show this help message
  --list              Print the list of files in the archive
  --check             Verify archive integrity and version dependency
  --noexec            (Handled by .run wrapper) Must be used with --extract=<path>; extract only without installing
  --extract=<path>    (Handled by .run wrapper) Extract payload to <path>
  --install           Perform installation (default when no option given)
    --install-for-all (Must be used with --install) Make files readable by all
                      users (directories 755, files 444)
    --force           (Must be used with --install) Overwrite existing installed files
  --uninstall         Remove installed files and restore backups
  --version           Show version information
EOF
}

# ---------------------------------------------------------------
# Locate source directory where payload files reside
# ---------------------------------------------------------------
find_source_dir() {
    # Primary: run package scenario -- payload files co-located with this script
    if [[ -f "${SCRIPT_DIR}/${PACKAGE_NAME}" && -f "${SCRIPT_DIR}/${JSON_NAME}" && -f "${SCRIPT_DIR}/${VERSION_FILE}" ]]; then
        echo "${SCRIPT_DIR}"
    # Fallback: source-tree debugging scenario -- only when SCRIPT_DIR ends with /script/kernel
    # and PROJECT_ROOT is valid and build output exists
    elif [[ "${SCRIPT_DIR}" == */script/kernel ]] && [[ -n "${PROJECT_ROOT:-}" ]] \
        && [[ -f "${PROJECT_ROOT}/output/hybm/aicpu_kernel/${PACKAGE_NAME}" \
        && -f "${PROJECT_ROOT}/output/hybm/aicpu_kernel/${JSON_NAME}" \
        && -f "${PROJECT_ROOT}/output/hybm/aicpu_kernel/${VERSION_FILE}" ]]; then
        echo "WARNING: Running from source tree, using build output at ${PROJECT_ROOT}/output/hybm/aicpu_kernel" >&2
        echo "${PROJECT_ROOT}/output/hybm/aicpu_kernel"
    else
        echo ""
    fi
}

# ---------------------------------------------------------------
# Detect target chip platform.
# 按文档 docs/installation_aicpu_kernel.md: 310P 只需 libmf_sdma_orch.so,
# A3/A5 只需 cann-hybm-compat.tar.gz。两份都装会在 310P 上触发设备签名校验
# 失败(verifyFlag=Only Huawei, 包无签名), 导致 aclrtSetDevice 507033。
# 故安装前探测平台, 只装对应那一份。
# 返回值(写到全局 PLATFORM_DETECTED): "310P" | "A3A5" | "unknown"
# ---------------------------------------------------------------
detect_platform() {
    PLATFORM_DETECTED="unknown"
    local soc_name=""
    # 优先 npu-smi: 310P 的 Chip 列为 "310P3"(不带 Ascend 前缀), A3/A5 为 Ascend9xx
    if command -v npu-smi &>/dev/null; then
        soc_name=$(npu-smi info 2>/dev/null | grep -oE "Ascend9[0-9]+[A-Za-z]*|310P[0-9]*" | head -n1)
    fi
    if [[ -n "${soc_name}" ]]; then
        case "${soc_name}" in
            *310P*)
                PLATFORM_DETECTED="310P" ;;
            *)
                PLATFORM_DETECTED="A3A5" ;;
        esac
        return
    fi
    # lspci 兜底(PCI 设备 ID: d802=A2, d803=A3, d806=A5; 310P 无对应 ID, 不在此识别)
    if [[ $(lspci 2>/dev/null | grep -ci "Processing.*Huawei.*d80[236]") -gt 0 ]]; then
        PLATFORM_DETECTED="A3A5"
    fi
}

# ---------------------------------------------------------------
# Read version metadata from source/payload
# ---------------------------------------------------------------
read_version() {
    local src_dir="$1"
    local ver_file="${src_dir}/${VERSION_FILE}"
    if [[ -f "$ver_file" ]]; then
        cat "$ver_file"
    else
        echo "WARNING: ${VERSION_FILE} not found in payload" >&2
        echo "mf version info:"
        echo "mf version: unknown"
        echo "git: unknown"
    fi
}

# ---------------------------------------------------------------
# --check : verify payload integrity
# ---------------------------------------------------------------
check_payload() {
    local src_dir="$1"
    local pkg="${src_dir}/${PACKAGE_NAME}"
    local json="${src_dir}/${JSON_NAME}"
    local ver="${src_dir}/${VERSION_FILE}"
    local rc=0

    echo "=== Payload integrity check ==="

    # Package archive
    if [[ ! -f "$pkg" ]]; then
        echo "ERROR: ${PACKAGE_NAME} not found" >&2
        rc=1
    elif [[ ! -s "$pkg" ]]; then
        echo "ERROR: ${PACKAGE_NAME} is empty" >&2
        rc=1
    else
        local size
        size=$(stat -c%s "$pkg" 2>/dev/null || echo "?")
        echo "OK: ${PACKAGE_NAME} (${size} bytes)"
        if tar -tzf "$pkg" &>/dev/null; then
            echo "OK: ${PACKAGE_NAME} is a valid tar.gz archive"
        else
            echo "ERROR: ${PACKAGE_NAME} is not a valid tar.gz archive" >&2
            rc=1
        fi
    fi

    # JSON config
    if [[ ! -f "$json" ]]; then
        echo "ERROR: ${JSON_NAME} not found" >&2
        rc=1
    elif [[ ! -s "$json" ]]; then
        echo "ERROR: ${JSON_NAME} is empty" >&2
        rc=1
    else
        local size
        size=$(stat -c%s "$json" 2>/dev/null || echo "?")
        echo "OK: ${JSON_NAME} (${size} bytes)"
        if command -v python3 &>/dev/null; then
            if python3 -c "import json; json.load(open('${json}'))" 2>/dev/null; then
                echo "OK: ${JSON_NAME} is valid JSON"
            else
                echo "ERROR: ${JSON_NAME} is not valid JSON" >&2
                rc=1
            fi
        else
            echo "INFO: python3 not available, skip JSON validation"
        fi
    fi

    # Version
    echo ""
    if [[ -f "$ver" ]]; then
        echo "--- Version ---"
        cat "$ver"
    else
        echo "WARNING: ${VERSION_FILE} not found in payload"
    fi

    # CANN path information
    echo ""
    if [[ -n "${ASCEND_HOME_PATH:-}" ]]; then
        if [[ -d "$ASCEND_HOME_PATH" ]]; then
            echo "OK: ASCEND_HOME_PATH=${ASCEND_HOME_PATH} (exists)"
        else
            echo "WARNING: ASCEND_HOME_PATH=${ASCEND_HOME_PATH} does not exist"
        fi
        # Ini config path
        local ini_conf_dir="${ASCEND_HOME_PATH}/conf"
        echo "INI config path: ${ini_conf_dir}/ascend_package_load.ini"
        if [[ -d "$ini_conf_dir" ]]; then
            if [[ -w "$ini_conf_dir" ]]; then
                echo "OK: ${ini_conf_dir} (exists, writable)"
            else
                echo "WARNING: ${ini_conf_dir} exists but is not writable"
            fi
        elif [[ -d "${ASCEND_HOME_PATH}" ]]; then
            if [[ -w "${ASCEND_HOME_PATH}" ]]; then
                echo "OK: ${ini_conf_dir} can be created (parent writable)"
            else
                echo "WARNING: ${ASCEND_HOME_PATH} exists but is not writable, cannot create ${ini_conf_dir}"
            fi
        fi
    else
        echo "WARNING: ASCEND_HOME_PATH is not set (required for install/uninstall)"
    fi

    return $rc
}

# ---------------------------------------------------------------
# --list : list archive files
# ---------------------------------------------------------------
do_list() {
    local src_dir="$1"
    echo "Archive files:"
    for f in "${PACKAGE_NAME}" "${JSON_NAME}" "${VERSION_FILE}"; do
        if [[ -f "${src_dir}/${f}" ]]; then
            local sz
            sz=$(stat -c%s "${src_dir}/${f}" 2>/dev/null || echo "0")
            printf "  %-40s %s bytes\n" "${f}" "${sz}"
        fi
    done
    echo ""
    if [[ -f "${src_dir}/${PACKAGE_NAME}" ]]; then
        echo "Contents of ${PACKAGE_NAME}:"
        tar -tzf "${src_dir}/${PACKAGE_NAME}" | sed 's/^/  /'
    fi
    if [[ -n "${ASCEND_HOME_PATH:-}" ]]; then
        echo ""
        echo "Target install paths (ASCEND_HOME_PATH=${ASCEND_HOME_PATH}):"
        echo "  ${ASCEND_HOME_PATH}/opp/vendors/cust/op_impl/aicpu/kernel/${PACKAGE_NAME}"
        echo "  ${ASCEND_HOME_PATH}/opp/vendors/cust/op_impl/aicpu/config/${JSON_NAME}"
        echo "  ${ASCEND_HOME_PATH}/opp/vendors/cust/op_impl/aicpu/config/${VERSION_FILE}"
        echo ""
        echo "INI config file:"
        echo "  ${ASCEND_HOME_PATH}/conf/ascend_package_load.ini"
        echo ""
        echo "INI config content to write:"
        echo "  name:${PACKAGE_NAME}"
        echo "  install_path:2"
        echo "  optional:true"
        echo "  package_path:opp/vendors/cust/op_impl/aicpu/kernel"
        echo "  load_as_per_soc:false"
    fi
}

# ---------------------------------------------------------------
# --install (and --install-for-all) : copy files to target
# ---------------------------------------------------------------
do_install() {
    local src_dir="$1"
    local kern_dir="$2"
    local config_dir="$3"
    local backup_dir="$4"

    mkdir -p "${kern_dir}" "${config_dir}"

    local pkg_target="${kern_dir}/${PACKAGE_NAME}"
    local json_target="${config_dir}/${JSON_NAME}"
    local ver_target="${config_dir}/${VERSION_FILE}"

    local existing=0
    [[ -f "$pkg_target" ]] && existing=1
    [[ -f "$json_target" ]] && existing=1
    [[ -f "$ver_target" ]] && existing=1

    if [[ $existing -eq 1 && $FORCE -eq 0 ]]; then
        echo "Error: target files already exist. Use --force to overwrite, or --uninstall first." >&2
        exit 1
    fi

    # Backup originals once (only if manifest does not exist yet)
    local manifest="${backup_dir}/manifest.txt"
    if [[ ! -f "$manifest" ]]; then
        local INI_FILE="${ASCEND_HOME_PATH}/conf/ascend_package_load.ini"
        # Backup ini file before modification (if it exists)
        if [[ -f "$INI_FILE" ]]; then
            mkdir -p "${backup_dir}/conf"
            cp -a "$INI_FILE" "${backup_dir}/conf/"
            echo "ascend_package_load.ini" >> "$manifest"
            echo "Backed up: ${INI_FILE}"
        fi
        # Backup existing target files
        if [[ $existing -eq 1 ]]; then
            mkdir -p "${backup_dir}/kernel" "${backup_dir}/config"
            if [[ -f "$pkg_target" ]]; then
                cp -a "$pkg_target" "${backup_dir}/kernel/"
                echo "${PACKAGE_NAME}" >> "$manifest"
                echo "Backed up: ${pkg_target}"
            fi
            if [[ -f "$json_target" ]]; then
                cp -a "$json_target" "${backup_dir}/config/"
                echo "${JSON_NAME}" >> "$manifest"
                echo "Backed up: ${json_target}"
            fi
            if [[ -f "$ver_target" ]]; then
                cp -a "$ver_target" "${backup_dir}/config/"
                echo "${VERSION_FILE}" >> "$manifest"
                echo "Backed up: ${ver_target}"
            fi
        fi
    fi

    cp -f "${src_dir}/${PACKAGE_NAME}" "$pkg_target"
    cp -f "${src_dir}/${JSON_NAME}" "$json_target"
    cp -f "${src_dir}/${VERSION_FILE}" "$ver_target"

    echo "Installed: ${pkg_target}"
    echo "Installed: ${json_target}"
    echo "Installed: ${ver_target}"

    # Write/update ascend_package_load.ini
    local INI_FILE="${ASCEND_HOME_PATH}/conf/ascend_package_load.ini"
    mkdir -p "$(dirname "$INI_FILE")"
    # Remove any existing block for our package
    while grep -q '^name:cann-hybm-compat.tar.gz$' "$INI_FILE" 2>/dev/null; do
        sed -i '/^name:cann-hybm-compat.tar.gz$/,+4d' "$INI_FILE"
    done
    # Preserve existing content: start a new line if the file has no trailing newline.
    if [[ -s "$INI_FILE" ]] && [[ "$(tail -c 1 "$INI_FILE" | od -An -tx1 | tr -d ' \n')" != "0a" ]]; then
        printf '\n' >> "$INI_FILE"
    fi
    # Append our 5-line config (no extra fields)
    cat >> "$INI_FILE" <<'INIEOF'
name:cann-hybm-compat.tar.gz
install_path:2
optional:true
package_path:opp/vendors/cust/op_impl/aicpu/kernel
load_as_per_soc:false
INIEOF
    echo "Updated: ${INI_FILE}"

    # Set appropriate file and directory permissions
    local file_perm=440
    local dir_perm=750
    if [[ $INSTALL_FOR_ALL -eq 1 ]]; then
        file_perm=444
        dir_perm=755
    fi
    chmod "$file_perm" "$pkg_target" "$json_target" "$ver_target"
    chmod "$dir_perm" "${kern_dir}" "${config_dir}"
}

# ---------------------------------------------------------------
# --uninstall : remove installed files and restore backups
# ---------------------------------------------------------------
do_uninstall() {
    local kern_dir="$1"
    local config_dir="$2"
    local backup_dir="$3"

    local pkg_target="${kern_dir}/${PACKAGE_NAME}"
    local json_target="${config_dir}/${JSON_NAME}"
    local ver_target="${config_dir}/${VERSION_FILE}"
    local manifest="${backup_dir}/manifest.txt"

    local removed=0

    if [[ -f "$pkg_target" ]]; then
        rm -f "$pkg_target"
        echo "Removed: ${pkg_target}"
        removed=1
    fi
    if [[ -f "$json_target" ]]; then
        rm -f "$json_target"
        echo "Removed: ${json_target}"
        removed=1
    fi
    if [[ -f "$ver_target" ]]; then
        rm -f "$ver_target"
        echo "Removed: ${ver_target}"
        removed=1
    fi

    local INI_FILE="${ASCEND_HOME_PATH}/conf/ascend_package_load.ini"

    if [[ -f "$manifest" ]]; then
        echo "Restoring from backup..."
        while IFS= read -r fname; do
            if [[ "$fname" == "${PACKAGE_NAME}" ]]; then
                local bkp="${backup_dir}/kernel/${fname}"
                if [[ -f "$bkp" ]]; then
                    cp -a "$bkp" "$pkg_target"
                    echo "Restored: ${pkg_target}"
                    removed=1
                fi
            elif [[ "$fname" == "${JSON_NAME}" ]]; then
                local bkp="${backup_dir}/config/${fname}"
                if [[ -f "$bkp" ]]; then
                    cp -a "$bkp" "$json_target"
                    echo "Restored: ${json_target}"
                    removed=1
                fi
            elif [[ "$fname" == "${VERSION_FILE}" ]]; then
                local bkp="${backup_dir}/config/${fname}"
                if [[ -f "$bkp" ]]; then
                    cp -a "$bkp" "$ver_target"
                    echo "Restored: ${ver_target}"
                    removed=1
                fi
            elif [[ "$fname" == "ascend_package_load.ini" ]]; then
                local bkp="${backup_dir}/conf/${fname}"
                if [[ -f "$bkp" ]]; then
                    mkdir -p "$(dirname "$INI_FILE")"
                    cp -a "$bkp" "$INI_FILE"
                    echo "Restored: ${INI_FILE}"
                    removed=1
                fi
            fi
        done < "$manifest"
        rm -f "$manifest"
    fi

    # If ini still has our block (no backup existed), remove it
    if [[ -f "$INI_FILE" ]] && grep -q '^name:cann-hybm-compat.tar.gz$' "$INI_FILE" 2>/dev/null; then
        while grep -q '^name:cann-hybm-compat.tar.gz$' "$INI_FILE" 2>/dev/null; do
            sed -i '/^name:cann-hybm-compat.tar.gz$/,+4d' "$INI_FILE"
        done
        if [[ -s "$INI_FILE" ]]; then
            echo "Cleaned: ${INI_FILE}"
        else
            rm -f "$INI_FILE"
            echo "Removed empty: ${INI_FILE}"
        fi
        removed=1
    fi

    # Clean up empty backup dirs / stale backup files
    rm -f "${backup_dir}/kernel/${PACKAGE_NAME}" "${backup_dir}/config/${JSON_NAME}" "${backup_dir}/config/${VERSION_FILE}" "${backup_dir}/conf/ascend_package_load.ini" 2>/dev/null || true
    rmdir "${backup_dir}/kernel" "${backup_dir}/config" "${backup_dir}/conf" "${backup_dir}" 2>/dev/null || true

    if [[ $removed -eq 0 ]]; then
        echo "Nothing to uninstall."
    fi
}

# ---------------------------------------------------------------
# 310P sdma orchestrator: resolve install dir
# 优先装到 venv 内 memfabric_hybrid 包目录下 cust_aicpu_kernel/ 子目录;
# rank 运行时加载 venv 内两级布局 <pkg>/lib/libmf_hybm_core.so, FindInstalledJson()
# 上溯两级到 <pkg> 找 <pkg>/cust_aicpu_kernel/, 故产物装此处与之匹配。
# venv 包不可用(如纯 run-install 三级布局场景)时回退 MEMFABRIC_HYBRID_HOME_PATH/cust_aicpu_kernel/。
# 该目录收纳所有 host 侧主动 CUST 迁移加载的 AICPU kernel(与 A3/A5 走 CANN OPP 自动加载区分),
# 当前装 sdma orchestrator 一组, 后续同类 kernel 往同目录平铺。
# ---------------------------------------------------------------
resolve_sdma_orch_install_dir() {
    # 优先: venv 内 memfabric_hybrid 包目录(两级布局, FindInstalledJson 两级回溯命中)
    if command -v python &>/dev/null; then
        local pkg_dir
        pkg_dir=$(python - <<'PYEOF' 2>/dev/null
import os
try:
    import memfabric_hybrid as m
    p = os.path.dirname(os.path.abspath(m.__file__))
    print(p)
except Exception:
    pass
PYEOF
        )
        if [[ -n "${pkg_dir}" && -d "${pkg_dir}" ]]; then
            echo "${pkg_dir}/cust_aicpu_kernel"
            return
        fi
    fi
    # 回退: MEMFABRIC_HYBRID_HOME_PATH(三级布局, run-install 场景)
    local home="${MEMFABRIC_HYBRID_HOME_PATH:-}"
    if [[ -n "${home}" && -d "${home}" ]]; then
        echo "${home}/cust_aicpu_kernel"
        return
    fi
    echo "/usr/local/memfabric_hybrid/latest/cust_aicpu_kernel"
}

# ---------------------------------------------------------------
# 310P sdma orchestrator: --install
# 三个产物(.so + 双 json)装到同一目录(与 FindInstalledJson 期望路径一致)
# ---------------------------------------------------------------
# 首次备份 install_dir 下已存在的 310P 产物并登记 manifest;
# 后续 --force 覆盖不重复备份, --uninstall 时据此还原。
backup_existing_sdma_orch() {
    local install_dir="$1"
    shift
    local backup_dir="${install_dir}/${SDMA_ORCH_BACKUP_BASENAME}"
    local manifest="${backup_dir}/manifest.txt"
    [[ -f "$manifest" ]] && return
    mkdir -p "${backup_dir}"
    for f in "$@"; do
        local target="${install_dir}/${f}"
        [[ -f "$target" ]] && cp -a "$target" "${backup_dir}/" && echo "$f" >> "$manifest"
    done
}

do_install_sdma_orch() {
    local src_dir="$1"
    local install_dir="$2"

    local artifacts=("${SDMA_ORCH_SO}" "${SDMA_ORCH_CUST_JSON}" "${SDMA_ORCH_LAUNCH_JSON}" "${SDMA_ORCH_VERSION_FILE}")
    # 源文件存在检查: .run 包构建时已强制要求 310P 产物齐全, 缺失说明 .run 包损坏, 硬失败
    for f in "${artifacts[@]}"; do
        if [[ ! -f "${src_dir}/${f}" ]]; then
            echo "Error: 310P sdma orchestrator artifact '${f}' not found in ${src_dir}" >&2
            exit 1
        fi
    done

    mkdir -p "${install_dir}"

    local existing=0
    for f in "${artifacts[@]}"; do
        [[ -f "${install_dir}/${f}" ]] && existing=1
    done

    if [[ $existing -eq 1 && $FORCE -eq 0 ]]; then
        echo "Error: 310P sdma orchestrator target files already exist at ${install_dir}." >&2
        echo "       Use --force to overwrite, or --uninstall first." >&2
        exit 1
    fi

    backup_existing_sdma_orch "${install_dir}" "${artifacts[@]}"

    local targets=()
    for f in "${artifacts[@]}"; do
        cp -f "${src_dir}/${f}" "${install_dir}/${f}"
        echo "Installed: ${install_dir}/${f}"
        targets+=("${install_dir}/${f}")
    done

    local file_perm=440
    local dir_perm=750
    if [[ $INSTALL_FOR_ALL -eq 1 ]]; then
        file_perm=444
        dir_perm=755
    fi
    chmod "$file_perm" "${targets[@]}"
    chmod "$dir_perm" "${install_dir}"
}

# ---------------------------------------------------------------
# 310P sdma orchestrator: --uninstall
# ---------------------------------------------------------------
do_uninstall_sdma_orch() {
    local install_dir="$1"

    local so_target="${install_dir}/${SDMA_ORCH_SO}"
    local cust_json_target="${install_dir}/${SDMA_ORCH_CUST_JSON}"
    local launch_json_target="${install_dir}/${SDMA_ORCH_LAUNCH_JSON}"
    local ver_target="${install_dir}/${SDMA_ORCH_VERSION_FILE}"

    local removed=0

    for f in "$so_target" "$cust_json_target" "$launch_json_target" "$ver_target"; do
        if [[ -f "$f" ]]; then
            rm -f "$f"
            echo "Removed: ${f}"
            removed=1
        fi
    done

    # Restore from backup if manifest exists
    local backup_dir="${install_dir}/${SDMA_ORCH_BACKUP_BASENAME}"
    local manifest="${backup_dir}/manifest.txt"
    if [[ -f "$manifest" ]]; then
        echo "Restoring 310P sdma orchestrator from backup..."
        while IFS= read -r fname; do
            local bkp="${backup_dir}/${fname}"
            if [[ -f "$bkp" ]]; then
                case "$fname" in
                    "${SDMA_ORCH_SO}")
                        cp -a "$bkp" "$so_target" && echo "Restored: ${so_target}" ;;
                    "${SDMA_ORCH_CUST_JSON}")
                        cp -a "$bkp" "$cust_json_target" && echo "Restored: ${cust_json_target}" ;;
                    "${SDMA_ORCH_LAUNCH_JSON}")
                        cp -a "$bkp" "$launch_json_target" && echo "Restored: ${launch_json_target}" ;;
                    "${SDMA_ORCH_VERSION_FILE}")
                        cp -a "$bkp" "$ver_target" && echo "Restored: ${ver_target}" ;;
                    *)
                        echo "WARNING: unknown backup entry: ${fname}" ;;
                esac
            fi
        done < "$manifest"
        rm -f "$manifest"
    fi

    # Clean up empty backup dirs / stale backup files
    rm -f "${backup_dir}/${SDMA_ORCH_SO}" "${backup_dir}/${SDMA_ORCH_CUST_JSON}" \
            "${backup_dir}/${SDMA_ORCH_LAUNCH_JSON}" "${backup_dir}/${SDMA_ORCH_VERSION_FILE}" 2>/dev/null || true
    rmdir "${backup_dir}" 2>/dev/null || true

    if [[ $removed -eq 0 ]]; then
        echo "Nothing to uninstall for 310P sdma orchestrator."
    fi
}

# ===============================================================
# Main
# ===============================================================

# Parse arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        --install)
            if [[ -n "$ACTION" && "$ACTION" != "install" ]]; then
                echo "Error: conflicting actions: --${ACTION} and --install" >&2
                usage >&2
                exit 1
            fi
            ACTION="install"; shift ;;
        --install-for-all) INSTALL_FOR_ALL=1; shift ;;
        --force) FORCE=1; shift ;;
        --uninstall)
            if [[ -n "$ACTION" ]]; then
                echo "Error: conflicting actions: --${ACTION} and --uninstall" >&2
                usage >&2
                exit 1
            fi
            ACTION="uninstall"; shift ;;
        --check)
            if [[ -n "$ACTION" ]]; then
                echo "Error: conflicting actions: --${ACTION} and --check" >&2
                usage >&2
                exit 1
            fi
            ACTION="check"; shift ;;
        --list)
            if [[ -n "$ACTION" ]]; then
                echo "Error: conflicting actions: --${ACTION} and --list" >&2
                usage >&2
                exit 1
            fi
            ACTION="list"; shift ;;
        --version)
            if [[ -n "$ACTION" ]]; then
                echo "Error: conflicting actions: --${ACTION} and --version" >&2
                usage >&2
                exit 1
            fi
            ACTION="version"; shift ;;
        -h|--help) usage; exit 0 ;;
        --extract=*|--noexec) shift ;;  # handled by .run wrapper
        *)
            echo "Unknown option: $1" >&2
            usage >&2
            exit 1
            ;;
    esac
done

# Default action: install when no option given.
# But --install-for-all and --force are not standalone actions.
if [[ -z "$ACTION" ]]; then
    if [[ $INSTALL_FOR_ALL -eq 1 || $FORCE -eq 1 ]]; then
        echo "Error: --install-for-all and --force must be used with --install" >&2
        usage >&2
        exit 1
    fi
    ACTION="install"
fi

# Ensure --install-for-all and --force are only used with --install
if [[ $INSTALL_FOR_ALL -eq 1 && "$ACTION" != "install" ]]; then
    echo "Error: --install-for-all must be used with --install" >&2
    usage >&2
    exit 1
fi
if [[ $FORCE -eq 1 && "$ACTION" != "install" ]]; then
    echo "Error: --force must be used with --install" >&2
    usage >&2
    exit 1
fi

# Locate source directory
SOURCE_DIR="$(find_source_dir)"
if [[ -z "$SOURCE_DIR" ]]; then
    echo "Error: Cannot locate ${PACKAGE_NAME}, ${JSON_NAME}, and ${VERSION_FILE}. Ensure they are in the same directory as this script." >&2
    exit 1
fi

case "$ACTION" in
    install|uninstall)
        if [[ -z "${ASCEND_HOME_PATH:-}" ]]; then
            echo "Error: ASCEND_HOME_PATH is not set. Set it to the CANN root directory." >&2
            exit 1
        fi
        if [[ ! -d "$ASCEND_HOME_PATH" ]]; then
            echo "Error: ASCEND_HOME_PATH=${ASCEND_HOME_PATH} does not exist." >&2
            exit 1
        fi
        CANN_ROOT="${ASCEND_HOME_PATH%/}"
        KERNEL_DIR="${CANN_ROOT}/opp/vendors/cust/op_impl/aicpu/kernel"
        CONFIG_DIR="${CANN_ROOT}/opp/vendors/cust/op_impl/aicpu/config"
        BACKUP_DIR="${CANN_ROOT}/opp/vendors/cust/${BACKUP_BASENAME}"
        detect_platform
        if [[ "$PLATFORM_DETECTED" == "unknown" ]]; then
            echo "WARNING: cannot detect chip platform (npu-smi/lspci unavailable or not NPU host)." >&2
            echo "WARNING: installing both A3/A5 and 310P artifacts." >&2
            echo "WARNING: on 310P with verifyFlag=Only Huawei this may trigger aclrtSetDevice 507033;" >&2
            echo "         re-run on a host with npu-smi or pre-install only the matching platform." >&2
        else
            echo "INFO: detected platform = ${PLATFORM_DETECTED}"
        fi
        if [[ "$ACTION" == "install" ]]; then
            # A3/A5 产物(走 CANN OPP 自动加载, 注册进 ascend_package_load.ini)
            if [[ "$PLATFORM_DETECTED" == "A3A5" || "$PLATFORM_DETECTED" == "unknown" ]]; then
                echo "=== Installing A3/A5 AICPU kernel (libcann_hybm_kernel.so) to CANN OPP tree ==="
                do_install "$SOURCE_DIR" "$KERNEL_DIR" "$CONFIG_DIR" "$BACKUP_DIR"
            else
                echo "=== Skipping A3/A5 AICPU kernel on 310P (not required, avoids signature verify failure) ==="
            fi
            echo ""
            # 310P 产物(host 侧 CUST 迁移加载, 不走 CANN OPP 自动加载)
            if [[ "$PLATFORM_DETECTED" == "310P" || "$PLATFORM_DETECTED" == "unknown" ]]; then
                echo "=== Installing 310P sdma orchestrator AICPU kernel (libmf_sdma_orch.so) ==="
                SDMA_ORCH_INSTALL_DIR=$(resolve_sdma_orch_install_dir)
                do_install_sdma_orch "$SOURCE_DIR" "$SDMA_ORCH_INSTALL_DIR"
            else
                echo "=== Skipping 310P sdma orchestrator kernel on A3/A5 (not required) ==="
            fi
        else
            # uninstall 与 install 对称: 按平台只清对应那一份
            if [[ "$PLATFORM_DETECTED" == "A3A5" || "$PLATFORM_DETECTED" == "unknown" ]]; then
                echo "=== Uninstalling A3/A5 AICPU kernel (libcann_hybm_kernel.so) from CANN OPP tree ==="
                do_uninstall "$KERNEL_DIR" "$CONFIG_DIR" "$BACKUP_DIR"
            else
                echo "=== Skipping A3/A5 AICPU kernel uninstall on 310P (was not installed) ==="
            fi
            echo ""
            if [[ "$PLATFORM_DETECTED" == "310P" || "$PLATFORM_DETECTED" == "unknown" ]]; then
                echo "=== Uninstalling 310P sdma orchestrator AICPU kernel (libmf_sdma_orch.so) ==="
                SDMA_ORCH_INSTALL_DIR=$(resolve_sdma_orch_install_dir)
                do_uninstall_sdma_orch "$SDMA_ORCH_INSTALL_DIR"
            else
                echo "=== Skipping 310P sdma orchestrator kernel uninstall on A3/A5 (was not installed) ==="
            fi
        fi
        ;;
    check)
        check_payload "$SOURCE_DIR"
        ;;
    list)
        do_list "$SOURCE_DIR"
        ;;
    version)
        read_version "$SOURCE_DIR"
        ;;
esac
