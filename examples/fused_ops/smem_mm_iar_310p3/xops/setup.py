import os
import glob
from setuptools import setup, find_packages

from torch.utils.cpp_extension import BuildExtension

import torch_npu
from torch_npu.utils.cpp_extension import NpuExtension

PYTORCH_NPU_INSTALL_PATH = os.path.dirname(os.path.abspath(torch_npu.__file__))
BASE_DIR = os.path.dirname(os.path.realpath(__file__))

# 收集 csrc 下全部 cpp：ops_common.cpp（公共机制）/ ops_def_registration.cpp
#（算子签名注册）/ npu_smem_mm_ar_310p3.cpp（算子宿主入口）。
source_files = glob.glob(os.path.join(BASE_DIR, "xops_iar_310p3/csrc", "*.cpp"))

# 扩展只做注册与透传：编译期只需要 ACL 头文件路径，aclnn 符号全部在
# 运行时经 dlopen/dlsym 解析，因此不写 libraries=。
ext = NpuExtension(
    name="xops_iar_310p3.xops_lib",
    sources=source_files,
    extra_compile_args=[
        '-I' + os.path.join(PYTORCH_NPU_INSTALL_PATH, "include/third_party/acl/inc"),
    ],
)

setup(
    name="xops_smem_mm_iar_310p3",
    version='1.0',
    ext_modules=[ext],
    packages=find_packages(),
    cmdclass={"build_ext": BuildExtension},
)
