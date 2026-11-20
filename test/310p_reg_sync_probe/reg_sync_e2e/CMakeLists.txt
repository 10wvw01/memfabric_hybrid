cmake_minimum_required(VERSION 3.16)
project(reg_sync_e2e)

# ---- 环境检查 ----
set(SOC_VERSION "Ascend310P3" CACHE STRING "system on chip type")
set(ASCEND_CANN_PACKAGE_PATH $ENV{ASCEND_HOME_PATH})
if ("${ASCEND_CANN_PACKAGE_PATH}" STREQUAL "")
    message(FATAL_ERROR "ascend cann path not found! source setenv.bash first")
endif ()

# ascendc_kernel_cmake (AICore kernel 编译)
set(ASCENDC_CMAKE_DIR ${ASCEND_CANN_PACKAGE_PATH}/compiler/tikcpp/ascendc_kernel_cmake)
include(${ASCENDC_CMAKE_DIR}/ascendc.cmake)

if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
endif ()
set(CMAKE_INSTALL_PREFIX ${CMAKE_CURRENT_LIST_DIR}/out)

# ---- AICore kernel (ascendc_library) ----
ascendc_library(reg_sync_aicore_kernel SHARED
    ${CMAKE_CURRENT_LIST_DIR}/reg_sync_aicore_dev.cpp
)

# ---- AICPU kernel (hcc 交叉编译 .so) ----
set(AICPU_COMPILER "${ASCEND_CANN_PACKAGE_PATH}/toolkit/toolchain/hcc/bin/aarch64-target-linux-gnu-g++")
set(AICPU_SO_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/libreg_sync_aicpu.so")

add_custom_command(
    OUTPUT ${AICPU_SO_OUTPUT}
    COMMAND ${AICPU_COMPILER}
        -shared -O2 -std=c++17 -fPIC
        -o ${AICPU_SO_OUTPUT}
        ${CMAKE_CURRENT_LIST_DIR}/reg_sync_aicpu_kernel.cc
    DEPENDS ${CMAKE_CURRENT_LIST_DIR}/reg_sync_aicpu_kernel.cc
    COMMENT "Cross-compiling AICPU kernel (libreg_sync_aicpu.so)"
)

add_custom_target(reg_sync_aicpu_so ALL DEPENDS ${AICPU_SO_OUTPUT})

# ---- 复制 launch json 到构建目录(运行时 REG_SYNC_AICPU_JSON 指向) ----
set(AICPU_JSON_OUT "${CMAKE_CURRENT_BINARY_DIR}/reg_sync_aicpu.json")
add_custom_command(
    OUTPUT ${AICPU_JSON_OUT}
    COMMAND ${CMAKE_COMMAND} -E copy
            ${CMAKE_CURRENT_LIST_DIR}/reg_sync_aicpu.json ${AICPU_JSON_OUT}
    DEPENDS ${CMAKE_CURRENT_LIST_DIR}/reg_sync_aicpu.json
    COMMENT "Copying AICPU launch json to build dir"
)

# ---- host main ----
add_executable(reg_sync_e2e ${CMAKE_CURRENT_LIST_DIR}/main.cpp)

target_compile_options(reg_sync_e2e PRIVATE
    -O2 -D_GLIBCXX_USE_CXX11_ABI=0 -Wall
)

target_link_libraries(reg_sync_e2e PRIVATE
    reg_sync_aicore_kernel
)

# host 依赖 AICPU so + json(构建顺序)
add_dependencies(reg_sync_e2e reg_sync_aicpu_so)
add_custom_target(copy_aicpu_json ALL DEPENDS ${AICPU_JSON_OUT})
add_dependencies(reg_sync_e2e copy_aicpu_json)

install(TARGETS reg_sync_e2e RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES ${AICPU_SO_OUTPUT} ${AICPU_JSON_OUT}
        DESTINATION ${CMAKE_INSTALL_BINDIR})
