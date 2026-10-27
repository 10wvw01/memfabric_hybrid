# 310P3 V2 Ascend C 直调工程：正式与诊断源代码分别管理。
# 为保持既有 G0/G1/T1/T2/G3 试验 ABI，诊断模块默认开启。
# 生产验证可设置 -DVLLM_ASCEND_310P_V2_BUILD_DIAGNOSTICS=OFF，
# 此时仅编译正式 T0/Host/Serving 与 Serving 必需的 IB 自检。
option(VLLM_ASCEND_310P_V2_BUILD_DIAGNOSTICS
    "Build V2 optional diagnostic kernels, adapters and Torch debug op" ON)

set(MF310P_V2_KERNEL_SOURCE
    "${CMAKE_CURRENT_LIST_DIR}/op_kernel/mf310p_v2_kernel.asc")
set(MF310P_V2_KERNEL_SOURCES ${MF310P_V2_KERNEL_SOURCE})
set(MF310P_V2_HOST_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/op_host/mf310p_v2_trial_adapter.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/torch_adapter/mf310p_v2_serve.cpp")
set(MF310P_V2_HOST_INCLUDE_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/op_host"
    "${CMAKE_CURRENT_LIST_DIR}/torch_adapter")

if(VLLM_ASCEND_310P_V2_BUILD_DIAGNOSTICS)
    list(APPEND MF310P_V2_KERNEL_SOURCES
        "${CMAKE_CURRENT_LIST_DIR}/op_kernel/mf310p_v2_diagnostics.asc")
    list(APPEND MF310P_V2_HOST_SOURCES
        "${CMAKE_CURRENT_LIST_DIR}/op_host/mf310p_v2_trial_diagnostics.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/torch_adapter/mf310p_v2_serve_diagnostics.cpp")
endif()
