cmake_minimum_required(VERSION 3.12)
project(310p3_trans_test)

set(MF_PATH $ENV{MEMFABRIC_HYBRID_HOME_PATH})
if ("${MF_PATH}" STREQUAL "")
    message(FATAL_ERROR "memfabric_hybrid path not found! Please set MEMFABRIC_HYBRID_HOME_PATH")
endif ()
set(ASCEND_CANN_PACKAGE_PATH $ENV{ASCEND_HOME_PATH})
if ("${ASCEND_CANN_PACKAGE_PATH}" STREQUAL "")
    message(FATAL_ERROR "ascend cann path not found! Please set ASCEND_HOME_PATH")
endif ()

if (CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64")
    set(TARGET_ARCH "x86_64")
elseif (CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
    set(TARGET_ARCH "aarch64")
else ()
    message(FATAL_ERROR "arch is not support!")
endif ()

if (EXISTS "${MF_PATH}/aarch64-linux/lib64/libmf_smem.so")
    set(SMEM_INCLUDE ${MF_PATH}/include/smem/host)
    set(HYBM_INCLUDE ${MF_PATH}/include/hybm)
    set(MF_LIB_DIR ${MF_PATH}/${TARGET_ARCH}-linux/lib64)
else ()
    set(SMEM_INCLUDE ${MF_PATH}/smem/include/host)
    set(HYBM_INCLUDE ${MF_PATH}/hybm/include)
    set(MF_LIB_DIR ${MF_PATH}/smem/lib64)
    list(APPEND MF_LIB_DIR ${MF_PATH}/hybm/lib64)
endif ()

add_executable(310p3_trans_test main.cpp)

target_compile_options(310p3_trans_test PRIVATE
    -O2 -D_GLIBCXX_USE_CXX11_ABI=0 -Wall -g -std=c++17
)

target_include_directories(310p3_trans_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${SMEM_INCLUDE}
    ${HYBM_INCLUDE}
    ${ASCEND_CANN_PACKAGE_PATH}/${TARGET_ARCH}-linux/include/
)

target_link_directories(310p3_trans_test PUBLIC
    ${MF_LIB_DIR}
    ${ASCEND_CANN_PACKAGE_PATH}/${TARGET_ARCH}-linux/lib64
)

find_package(MPI REQUIRED)
target_link_libraries(310p3_trans_test PRIVATE
    mf_smem
    mf_hybm_core
    ascendcl
    MPI::MPI_CXX
)

set_target_properties(310p3_trans_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/
)
