cmake_minimum_required(VERSION 3.12)
project(310p3_aclrt_benchmark)

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

add_executable(310p3_aclrt_benchmark main.cpp)

target_compile_options(310p3_aclrt_benchmark PRIVATE
    -O2 -D_GLIBCXX_USE_CXX11_ABI=0 -Wall -g -std=c++17 -fpermissive
)

target_include_directories(310p3_aclrt_benchmark PRIVATE
    ${ASCEND_CANN_PACKAGE_PATH}/${TARGET_ARCH}-linux/include/
)

target_link_directories(310p3_aclrt_benchmark PUBLIC
    ${ASCEND_CANN_PACKAGE_PATH}/${TARGET_ARCH}-linux/lib64
)

find_package(MPI REQUIRED)
target_link_libraries(310p3_aclrt_benchmark PRIVATE
    ascendcl
    pthread
    MPI::MPI_CXX
)

set_target_properties(310p3_aclrt_benchmark PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/
)
