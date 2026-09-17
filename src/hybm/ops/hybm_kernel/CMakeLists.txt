set(CMAKE_SKIP_RPATH TRUE)

# Allow standalone build: use HYBM_KERNEL_PROJECT_ROOT if defined, else fall back to PROJECT_SOURCE_DIR
if(NOT DEFINED HYBM_KERNEL_PROJECT_ROOT)
  set(HYBM_KERNEL_PROJECT_ROOT "${PROJECT_SOURCE_DIR}")
endif()
# hybm_pack_sign.cmake not needed: pack_targets_and_files() no longer used

set(HYBM_AICPU_PACKAGE_NAME cann-hybm-compat.tar.gz)
set(AICPU_COMPILER "${ASCEND_HOME_PATH}/toolkit/toolchain/hcc/bin/aarch64-target-linux-gnu-g++")
set(_ASCEND_INC_DIR "${ASCEND_HOME_PATH}/aarch64-linux/pkg_inc/base")

# Output artifacts in build directory (not directly to install dir)
set(AICPU_SO_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/libcann_hybm_kernel.so")
set(AICPU_JSON_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/libcann_hybm_kernel.json")
set(AICPU_TAR_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${HYBM_AICPU_PACKAGE_NAME}")

set(AICPU_INCLUDE_DIRS
    ${PROJECT_HYBM_SRC_BASE}/include
    ${PROJECT_HYBM_SRC_BASE}/csrc/common
    ${PROJECT_HYBM_SRC_BASE}/csrc/under_api
    ${_ASCEND_INC_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
)

message("==================hybm ai cpu ops compile====================================")

# Build include flags
set(AICPU_INC_FLAGS)
foreach(INC_DIR ${AICPU_INCLUDE_DIRS})
  list(APPEND AICPU_INC_FLAGS "-I${INC_DIR}")
endforeach()

if(NOT DEFINED AICPU_KERNEL_SRC)
  set(AICPU_KERNEL_SRC
      "${CMAKE_CURRENT_SOURCE_DIR}/hybm_batch_transfer.cc"
  )
endif()

set(AICPU_LINK_FLAGS "")

# ---- Copy static .json ----
add_custom_command(
    OUTPUT ${AICPU_JSON_OUTPUT}
    COMMAND ${CMAKE_COMMAND} -E copy
            ${CMAKE_CURRENT_SOURCE_DIR}/libcann_hybm_kernel.json
            ${AICPU_JSON_OUTPUT}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/libcann_hybm_kernel.json
)

# ---- Cross-compile .so ----
set(AICPU_KERNEL_DEPS
    ${CMAKE_CURRENT_SOURCE_DIR}/hybm_batch_transfer.h
    ${CMAKE_CURRENT_SOURCE_DIR}/hybm_kernel_log.h
    ${PROJECT_HYBM_SRC_BASE}/include/hybm_def.h
)

add_custom_command(
    OUTPUT ${AICPU_SO_OUTPUT}
    COMMAND ${AICPU_COMPILER}
        -shared -O2 -std=c++17 -fPIC
        ${AICPU_INC_FLAGS}
        ${AICPU_LINK_FLAGS}
        -o ${AICPU_SO_OUTPUT}
        ${AICPU_KERNEL_SRC}
    DEPENDS ${AICPU_KERNEL_SRC} ${AICPU_KERNEL_DEPS}
    COMMENT "Compiling AICPU kernel shared library with aarch64 cross-compiler"
)

# ---- Package .so into tar.gz (AICPUKernel mode) ----
set(AICPU_TAR_ROOT "aicpu_kernels_device")
set(AICPU_STAGING_DIR "${CMAKE_CURRENT_BINARY_DIR}/tar_staging")

add_custom_command(
    OUTPUT ${AICPU_TAR_OUTPUT}
    COMMAND ${CMAKE_COMMAND} -E make_directory "${AICPU_STAGING_DIR}/${AICPU_TAR_ROOT}"
    COMMAND ${CMAKE_COMMAND} -E copy "${AICPU_SO_OUTPUT}" "${AICPU_STAGING_DIR}/${AICPU_TAR_ROOT}/"
    COMMAND tar czf "${AICPU_TAR_OUTPUT}" -C "${AICPU_STAGING_DIR}" "${AICPU_TAR_ROOT}"
    DEPENDS ${AICPU_SO_OUTPUT}
    COMMENT "Packaging AICPU kernel into tar.gz (TAR_ROOT_DIR=${AICPU_TAR_ROOT})"
)

message("==================hybm ai cpu ops compile done====================================")

# ---- Default target: build all artifacts ----
add_custom_target(cann_hybm_kernel ALL DEPENDS ${AICPU_SO_OUTPUT} ${AICPU_JSON_OUTPUT} ${AICPU_TAR_OUTPUT})

# ============================================================================
# 310P AICore×AICPU 协同编排 kernel (310p-aicore-aicpu-开发计划.md P1-WP1)
# 独立 so: 与 libcann_hybm_kernel.so 互不影响; 设备侧版本名由 MF_OVERLAP_KVER 控制
# (CP1 按文件名 dlopen 缓存 —— 改 kernel 必 bump, 部署链详见计划附录 A.4)
# ============================================================================
set(OVERLAP_SO_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/libmf_overlap_aicpu.so") # overlap kernel so 产物(构建目录)
set(OVERLAP_JSON_CUST_SRC "${CMAKE_CURRENT_SOURCE_DIR}/libmf_overlap_aicpu.json") # CUST 迁移用 json 源(仓库静态文件)
set(OVERLAP_JSON_CUST_OUT "${CMAKE_CURRENT_BINARY_DIR}/libmf_overlap_aicpu.json") # CUST json 构建目录副本

# 设备侧 launch 版本号(缓存变量): launch json 的 kernelSo 与部署链的复制目标名都用它
set(MF_OVERLAP_KVER "3" CACHE STRING "310P overlap kernel device-side version (CP1 dlopen cache busting)")
# launch json 输出路径(文件名带版本号)
set(OVERLAP_JSON_LAUNCH_OUT "${CMAKE_CURRENT_BINARY_DIR}/libmf_overlap_v${MF_OVERLAP_KVER}.json")

add_custom_command( # 编译 overlap kernel so(hcc 交叉编译)
    OUTPUT ${OVERLAP_SO_OUTPUT} # 产物: 设备侧 so
    COMMAND ${AICPU_COMPILER} # aarch64 交叉编译器
        -shared -O2 -std=c++17 -fPIC # 共享库/O2/C++17/位置无关
        -o ${OVERLAP_SO_OUTPUT} # 输出目标
        ${CMAKE_CURRENT_SOURCE_DIR}/hybm_overlap_kernel.cc # kernel 源文件
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/hybm_overlap_kernel.cc # 依赖: kernel 源
            ${CMAKE_CURRENT_SOURCE_DIR}/hybm_overlap_contract.h # 依赖: wire 契约头
    COMMENT "Compiling 310P overlap AICPU kernel (libmf_overlap_aicpu.so)"
)

add_custom_command( # 复制 CUST json 到构建目录(deployer 迁移链用)
    OUTPUT ${OVERLAP_JSON_CUST_OUT} # 产物: CUST json 副本
    COMMAND ${CMAKE_COMMAND} -E copy ${OVERLAP_JSON_CUST_SRC} ${OVERLAP_JSON_CUST_OUT} # 原样复制
    DEPENDS ${OVERLAP_JSON_CUST_SRC} # 依赖: 仓库内源 json
    COMMENT "Copying overlap CUST json (deployer mode=1 migration)"
)

# launch json 按 MF_OVERLAP_KVER 配置期生成 (kernelSo 指向设备侧版本名)
file(WRITE "${OVERLAP_JSON_LAUNCH_OUT}" # 配置期生成 launch json(内容见下, 勿在串内加注释)
"{
    \"HybmOverlapRun\": {
        \"opInfo\": {
            \"opKernelLib\": \"AICPUKernel\",
            \"kernelSo\": \"libmf_overlap_v${MF_OVERLAP_KVER}.so\",
            \"functionName\": \"HybmOverlapRun\"
        }
    }
}
")
add_custom_command( # launch json 时间戳目标(让 file(WRITE) 产物纳入构建图)
    OUTPUT ${OVERLAP_JSON_LAUNCH_OUT} # 产物: launch json
    COMMAND ${CMAKE_COMMAND} -E touch ${OVERLAP_JSON_LAUNCH_OUT} # touch 刷新时间戳
    DEPENDS ${OVERLAP_SO_OUTPUT} # 依赖: 先产出 so
    COMMENT "Overlap launch json (AICPUKernel cpuMode=0, v${MF_OVERLAP_KVER})"
)

add_custom_target(mf_overlap_kernel ALL # 顶层目标: overlap 三产物聚合(so+双 json)
    DEPENDS ${OVERLAP_SO_OUTPUT} ${OVERLAP_JSON_CUST_OUT} ${OVERLAP_JSON_LAUNCH_OUT})

install(FILES ${OVERLAP_SO_OUTPUT} # 安装 overlap so(可执行权限)
        DESTINATION ${TARGET_INSTALL_DIR}/hybm/aicpu_kernel
        PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)

install(FILES ${OVERLAP_JSON_CUST_OUT} ${OVERLAP_JSON_LAUNCH_OUT} # 安装 overlap 双 json
        DESTINATION ${TARGET_INSTALL_DIR}/hybm/aicpu_kernel
        PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)

# ---- Install ----
install(FILES ${AICPU_SO_OUTPUT}
        DESTINATION ${TARGET_INSTALL_DIR}/hybm/aicpu_kernel
        PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)

install(FILES ${AICPU_JSON_OUTPUT}
        DESTINATION ${TARGET_INSTALL_DIR}/hybm/aicpu_kernel
        PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)

install(FILES ${AICPU_TAR_OUTPUT}
        DESTINATION ${TARGET_INSTALL_DIR}/hybm/aicpu_kernel
        PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)

install(PROGRAMS ${HYBM_KERNEL_PROJECT_ROOT}/script/kernel/install.sh
        DESTINATION ${TARGET_INSTALL_DIR}/hybm/aicpu_kernel
        PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
