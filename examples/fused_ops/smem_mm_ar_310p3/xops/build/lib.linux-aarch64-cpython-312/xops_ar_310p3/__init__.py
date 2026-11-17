import warnings

import torch
import torch_npu

# 加载编译产物，触发 csrc 里 TORCH_LIBRARY / TORCH_LIBRARY_IMPL 的静态注册
from . import xops_lib

# NOTE: 仅 eager 模式（enforce-eager），不注册 FX->GE converter。

# 把 torch.ops.custom.npu_smem_mm_ar_310p3 镜像到 torch_npu，使
# torch_npu.npu_smem_mm_ar_310p3(...) 等价可用。
# NOTE: torch.ops.custom 是 OpNamespaceProxy，dir() 不会列出算子名，用静态名单。
_CUSTOM_OP_NAMES = ("npu_smem_mm_ar_310p3",)
custom_ops_module = getattr(torch.ops, 'custom', None)
if custom_ops_module is None:
    warnings.warn("torch.ops.custom not found, mount custom ops to torch_npu failed. "
                  "Please use torch.ops.custom.xxx instead of torch_npu.xxx.")
else:
    for op_name in _CUSTOM_OP_NAMES:
        setattr(torch_npu, op_name, getattr(custom_ops_module, op_name))
