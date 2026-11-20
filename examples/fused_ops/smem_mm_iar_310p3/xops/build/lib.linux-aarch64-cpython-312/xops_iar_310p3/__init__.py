import warnings

import torch
import torch_npu

# 本包的 Python 入口：加载 C++ 扩展并触发 csrc 里的 TORCH_LIBRARY /
# TORCH_LIBRARY_IMPL 静态注册（import 副作用完成一切）。
from . import xops_lib

# 会话封装层：建池/几何校验/通道分配/epoch 内管/发射全部收编于此，
# 外部只需 SmemSession(device, rank) + launch(x, w_nz, rows)。
# 契约常量（同源 op_kernel tiling.h，经 C 扩展程序化导出）一并再导出，
# 供外部做路由判断与版本核对。
from .session import (SmemSession, POOL_FLOOR_BYTES, KERNEL_WIDTH,
                      KERNEL_MAX_ROWS, KERNEL_MIN_ROWS, KERNEL_ROWS_ALIGN,
                      DEFAULT_POOL_BYTES)

# 说明：本算子只在 eager 模式（enforce-eager）下使用，不注册 FX 到 GE 的
# 图模式转换器。

# 把 torch.ops.custom.npu_smem_mm_iar_310p3 镜像到 torch_npu 命名空间，
# 使 torch_npu.npu_smem_mm_iar_310p3(...) 与 torch.ops.custom 调用等价可用。
# 注意：torch.ops.custom 是 OpNamespaceProxy，dir() 不会列出已注册算子名，
# 因此这里用静态名单逐个挂载。
_CUSTOM_OP_NAMES = ("npu_smem_mm_iar_310p3",)
custom_ops_module = getattr(torch.ops, 'custom', None)
if custom_ops_module is None:
    warnings.warn("torch.ops.custom not found, mount custom ops to torch_npu failed. "
                  "Please use torch.ops.custom.xxx instead of torch_npu.xxx.")
else:
    for op_name in _CUSTOM_OP_NAMES:
        setattr(torch_npu, op_name, getattr(custom_ops_module, op_name))
