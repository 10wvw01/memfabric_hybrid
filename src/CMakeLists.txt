# Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
# MemFabric_Hybrid is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#          http://license.coscl.org.cn/MulanPSL2
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
# See the Mulan PSL v2 for more details.

# added very strict compile options here, but other modules maybe not need these
add_compile_options(-Wunused-variable -Wunused-value -Wcast-align)
add_compile_options(-fpermissive -Wno-error)
add_compile_options(-Wcast-qual -Winvalid-pch -Wwrite-strings -Wsign-compare -Wfloat-equal -Wextra -Wno-deprecated-declarations)

# 310P sdma orchestrator 设备侧版本号(提前声明: hybm/csrc 的缺省 json 名经编译宏引用,
# 而 hybmm_objects 的求值早于 hybm/ops/hybm_kernel 的同名 CACHE set;
# hybm/ops/hybm_kernel 处亦有 set, CACHE 幂等不冲突)
set(MF_SDMA_ORCH_KVER "14" CACHE STRING "310P sdma orchestrator kernel device-side version (CP1 dlopen cache busting)")

add_subdirectory(acc_links/csrc)
add_subdirectory(util/csrc/ptracer)
add_subdirectory(smem/csrc/config_store)
add_subdirectory(smem/csrc)
add_subdirectory(hybm/csrc)
add_subdirectory(acc_offload/csrc)

# ---- AICPU 编排 kernel(随主构建一并产出, 免独立构建步骤) ----
# 产物: <build>/src/hybm/ops/hybm_kernel/{libmf_sdma_orch.so, libmf_sdma_orch.json,
#       libmf_sdma_orch_v4.json} + A3 血统 libcann_hybm_kernel.*(安装至
#       ${TARGET_INSTALL_DIR}/hybm/aicpu_kernel/, 与运行时安装前缀发现路径一致)
# 门控: 仅 NPU 构建; CANN 交叉工具链(hcc)缺失时优雅跳过(如无 CANN 的 CI 环境),
#       不阻断主库构建
if(NOT DEFINED ASCEND_HOME_PATH)
    set(ASCEND_HOME_PATH "$ENV{ASCEND_HOME_PATH}")
endif()
if(XPU_TYPE STREQUAL "NPU"
        AND EXISTS "${ASCEND_HOME_PATH}/toolkit/toolchain/hcc/bin/aarch64-target-linux-gnu-g++")
    add_subdirectory(hybm/ops)
elseif(XPU_TYPE STREQUAL "NPU")
    message(STATUS "[src] skip hybm/ops: hcc toolchain not found (export ASCEND_HOME_PATH to enable)")
endif()
