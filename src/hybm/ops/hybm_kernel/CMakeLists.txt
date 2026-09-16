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
set(OVERLAP_SO_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/libmf_overlap_aicpu.so")
set(OVERLAP_JSON_CUST_SRC "${CMAKE_CURRENT_SOURCE_DIR}/libmf_overlap_aicpu.json")
set(OVERLAP_JSON_CUST_OUT "${CMAKE_CURRENT_BINARY_DIR}/libmf_overlap_aicpu.json")

# 设备侧 launch 版本号(缓存变量): launch json 的 kernelSo 与部署链的复制目标名都用它
set(MF_OVERLAP_KVER "2" CACHE STRING "310P overlap kernel device-side version (CP1 dlopen cache busting)")
set(OVERLAP_JSON_LAUNCH_OUT "${CMAKE_CURRENT_BINARY_DIR}/libmf_overlap_v${MF_OVERLAP_KVER}.json")

add_custom_command(
    OUTPUT ${OVERLAP_SO_OUTPUT}
    COMMAND ${AICPU_COMPILER}
        -shared -O2 -std=c++17 -fPIC
        -o ${OVERLAP_SO_OUTPUT}
        ${CMAKE_CURRENT_SOURCE_DIR}/hybm_overlap_kernel.cc
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/hybm_overlap_kernel.cc
            ${CMAKE_CURRENT_SOURCE_DIR}/hybm_overlap_contract.h
    COMMENT "Compiling 310P overlap AICPU kernel (libmf_overlap_aicpu.so)"
)

add_custom_command(
    OUTPUT ${OVERLAP_JSON_CUST_OUT}
    COMMAND ${CMAKE_COMMAND} -E copy ${OVERLAP_JSON_CUST_SRC} ${OVERLAP_JSON_CUST_OUT}
    DEPENDS ${OVERLAP_JSON_CUST_SRC}
    COMMENT "Copying overlap CUST json (deployer mode=1 migration)"
)

# launch json 按 MF_OVERLAP_KVER 配置期生成 (kernelSo 指向设备侧版本名)
file(WRITE "${OVERLAP_JSON_LAUNCH_OUT}"
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
add_custom_command(
    OUTPUT ${OVERLAP_JSON_LAUNCH_OUT}
    COMMAND ${CMAKE_COMMAND} -E touch ${OVERLAP_JSON_LAUNCH_OUT}
    DEPENDS ${OVERLAP_SO_OUTPUT}
    COMMENT "Overlap launch json (AICPUKernel cpuMode=0, v${MF_OVERLAP_KVER})"
)

add_custom_target(mf_overlap_kernel ALL
    DEPENDS ${OVERLAP_SO_OUTPUT} ${OVERLAP_JSON_CUST_OUT} ${OVERLAP_JSON_LAUNCH_OUT})

install(FILES ${OVERLAP_SO_OUTPUT}
        DESTINATION ${TARGET_INSTALL_DIR}/hybm/aicpu_kernel
        PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)

install(FILES ${OVERLAP_JSON_CUST_OUT} ${OVERLAP_JSON_LAUNCH_OUT}
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
