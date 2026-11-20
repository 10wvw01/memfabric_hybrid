# ----------------------------------------------------------------------------------------------------------
# Copyright (c) 2025 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------


# op_host 构建脚本：把本目录下的算子定义编译成自定义算子包的三个库。
# npu_op_code_gen 读取 SRC 中 OP_ADD 注册的算子类，生成 aclnn 适配层、
# 算子原型、tiling 回退层等自动生成代码到 ASCEND_AUTOGEN_PATH。
aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR} ops_srcs)
npu_op_code_gen(
    SRC ${ops_srcs}
    PACKAGE ${package_name}
    OUT_DIR ${ASCEND_AUTOGEN_PATH}
    JOIN_OP_DEF True
)

# cust_opapi：aclnn 接口库（宿主调用入口），由自动生成的 aclnn_*.cpp 组成。
file(GLOB autogen_aclnn_src ${ASCEND_AUTOGEN_PATH}/aclnn_*.cpp)
set_source_files_properties(${autogen_aclnn_src} PROPERTIES GENERATED TRUE)
npu_op_library(cust_opapi ACLNN
    ${autogen_aclnn_src}
)
target_compile_options(cust_opapi PRIVATE
    -fvisibility=hidden
)

# cust_op_proto：算子原型库（图构图时的算子信息），由算子定义源码与自动
# 生成的原型代码组成。
file(GLOB group_proto_src ${ASCEND_AUTOGEN_PATH}/group_op_proto/*.cc)
file(GLOB proto_src ${ASCEND_AUTOGEN_PATH}/op_proto.cc)
set_source_files_properties(${group_proto_src} PROPERTIES GENERATED TRUE)
set_source_files_properties(${proto_src} PROPERTIES GENERATED TRUE)
npu_op_library(cust_op_proto GRAPH
    ${ops_srcs}
    ${group_proto_src}
    ${proto_src}
)
target_compile_options(cust_op_proto PRIVATE
    -fvisibility=hidden
)

# cust_optiling：tiling 库（TilingFunc 所在），由算子定义源码与自动生成的
# 回退层组成。
file(GLOB fallback_src ${ASCEND_AUTOGEN_PATH}/fallback_*.cpp)
set_source_files_properties(${fallback_src} PROPERTIES GENERATED TRUE)
npu_op_library(cust_optiling TILING
    ${ops_srcs}
    ${fallback_src}
)
target_compile_options(cust_optiling PRIVATE
    -fvisibility=hidden
)

# 三个库一起打进同一个自定义算子包（vendor = ${package_name}）。
npu_op_package_add(${package_name}
    LIBRARY
        cust_optiling
        cust_opapi
        cust_op_proto
)
