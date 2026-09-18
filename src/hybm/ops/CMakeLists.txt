cmake_minimum_required(VERSION 3.12.0)

# ---- 公共路径计算(standalone 与主构建接入两种模式通用): src/hybm/ops → 上三级 = 仓库根
#      (修正历史 off-by-one: 原 "../.."链算到仓库父目录, 构建期被掩盖, install 期爆发) ----
get_filename_component(_OPS_DIR "${CMAKE_CURRENT_SOURCE_DIR}" REALPATH)
get_filename_component(_REPO_ROOT "${_OPS_DIR}/../../.." REALPATH)

# ---- ASCEND_HOME_PATH 解析: 显式 -D 优先, 缺省回退环境变量(修复"必须 -D 传"限制);
#      仍为空则致命错(AICPU 交叉编译器与头文件都在其下, 缺失无法继续) ----
if(NOT DEFINED ASCEND_HOME_PATH)
    set(ASCEND_HOME_PATH "$ENV{ASCEND_HOME_PATH}")
endif()
if("${ASCEND_HOME_PATH}" STREQUAL "")
    message(FATAL_ERROR "[hybm_ops] ASCEND_HOME_PATH not set (pass -DASCEND_HOME_PATH=<cann-dir> or export it)")
endif()

# ---- 主构建接入模式(add_subdirectory 自 src/CMakeLists)下的变量缺省:
#      顶层未定义这三个变量, 不设会导致 include 路径/安装规则错位 ----
if(NOT DEFINED HYBM_KERNEL_PROJECT_ROOT)
    set(HYBM_KERNEL_PROJECT_ROOT "${_REPO_ROOT}")
endif()
if(NOT DEFINED PROJECT_HYBM_SRC_BASE)
    set(PROJECT_HYBM_SRC_BASE "${_REPO_ROOT}/src/hybm")
endif()
if(NOT DEFINED PROJECT_UTIL_SRC_BASE)
    set(PROJECT_UTIL_SRC_BASE "${_REPO_ROOT}/src/util/csrc")
endif()

# ---- Standalone project mode: invoked directly as cmake -S src/hybm/ops -B ... ----
# When added via add_subdirectory from the top-level project, CMAKE_SOURCE_DIR differs.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(hybm_ops LANGUAGES C CXX)

    set(HYBM_KERNEL_PROJECT_ROOT "${_REPO_ROOT}" CACHE PATH "Repo root for hybm kernel build")
    set(PROJECT_HYBM_SRC_BASE "${_REPO_ROOT}/src/hybm" CACHE PATH "hybm src base")
    set(PROJECT_UTIL_SRC_BASE "${_REPO_ROOT}/src/util/csrc" CACHE PATH "util src base")
    if(NOT TARGET_INSTALL_DIR)
        set(TARGET_INSTALL_DIR "${_REPO_ROOT}/output" CACHE PATH "Install destination")
    endif()
    set(CMAKE_INSTALL_PREFIX "${TARGET_INSTALL_DIR}")

    # AICPU kernel source files for cross-compilation (standalone mode)
    set(AICPU_KERNEL_SRC
        "${CMAKE_CURRENT_SOURCE_DIR}/hybm_kernel/hybm_batch_transfer.cc"
        CACHE STRING "Source files for AICPU kernel cross-compilation")

    message(STATUS "[hybm_ops] Standalone build")
    message(STATUS "[hybm_ops] HYBM_KERNEL_PROJECT_ROOT = ${HYBM_KERNEL_PROJECT_ROOT}")
    message(STATUS "[hybm_ops] TARGET_INSTALL_DIR       = ${TARGET_INSTALL_DIR}")
endif()

# Include dlog/slog config if present (两种模式一致; 未找到 slog 库时优雅跳过)
if(EXISTS "${HYBM_KERNEL_PROJECT_ROOT}/cmake/kernel/config_slog.cmake")
    include("${HYBM_KERNEL_PROJECT_ROOT}/cmake/kernel/config_slog.cmake")
endif()

add_subdirectory(hybm_kernel)
