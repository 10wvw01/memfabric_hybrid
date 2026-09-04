cmake_minimum_required(VERSION 3.16)
project(310p3_shm_kernel_test)

set(SOC_VERSION "Ascend310P3" CACHE STRING "system on chip type")
set(ASCEND_CANN_PACKAGE_PATH $ENV{ASCEND_HOME_PATH})
if ("${ASCEND_CANN_PACKAGE_PATH}" STREQUAL "")
    message(FATAL_ERROR "ascend cann path not found! Please set ASCEND_HOME_PATH")
endif ()
set(ASCENDC_CMAKE_DIR ${ASCEND_CANN_PACKAGE_PATH}/compiler/tikcpp/ascendc_kernel_cmake)
include(${ASCENDC_CMAKE_DIR}/ascendc.cmake)

set(MF_PATH $ENV{MEMFABRIC_HYBRID_HOME_PATH})
if ("${MF_PATH}" STREQUAL "")
    message(FATAL_ERROR "memfabric_hybrid path not found! Please set MEMFABRIC_HYBRID_HOME_PATH")
endif ()

# Device headers are in source tree (not installed to output/)
set(MF_SRC_PATH ${MF_PATH})
if (NOT EXISTS "${MF_SRC_PATH}/src/smem/include/device/smem_shm_aicore_base_api.h")
    set(MF_SRC_PATH ${CMAKE_CURRENT_SOURCE_DIR}/../..)
endif ()
set(SMEM_DEVICE_INCLUDE ${MF_SRC_PATH}/src/smem/include/device)

if (CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64")
    set(TARGET_ARCH "x86_64")
elseif (CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
    set(TARGET_ARCH "aarch64")
else ()
    message(FATAL_ERROR "arch is not support!")
endif ()

ascendc_library(shm_kernel_test SHARED ${CMAKE_CURRENT_LIST_DIR}/shm_kernel.cpp)
ascendc_include_directories(shm_kernel_test
        PUBLIC
        ${SMEM_DEVICE_INCLUDE}/
)

add_executable(310p3_shm_kernel_test ${CMAKE_CURRENT_SOURCE_DIR}/main_kernel.cpp)

target_compile_options(310p3_shm_kernel_test PRIVATE
        -O2 -D_GLIBCXX_USE_CXX11_ABI=0 -Wall -g -std=c++17
)

target_include_directories(310p3_shm_kernel_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${MF_PATH}/include/smem/host
        ${MF_PATH}/include/hybm
        ${ASCEND_CANN_PACKAGE_PATH}/${TARGET_ARCH}-linux/include/
)

target_link_directories(310p3_shm_kernel_test PUBLIC
        ${MF_PATH}/${TARGET_ARCH}-linux/lib64
        ${MF_PATH}/smem/lib64
        ${MF_PATH}/hybm/lib64
        ${ASCEND_CANN_PACKAGE_PATH}/${TARGET_ARCH}-linux/lib64
)

find_package(MPI REQUIRED)
target_link_libraries(310p3_shm_kernel_test PRIVATE
        shm_kernel_test
        mf_smem
        ascendcl
        MPI::MPI_CXX
)

set_target_properties(310p3_shm_kernel_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)
