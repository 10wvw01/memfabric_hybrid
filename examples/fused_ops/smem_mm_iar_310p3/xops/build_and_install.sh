#!/bin/bash
# 构建 xops_smem_mm_iar_310p3 wheel 并安装。前置条件：
#   1) source CANN set_env.sh（ASCEND_HOME_PATH 已设置）
#   2) smem_mm_iar_310p3 算子包已构建，且 ASCEND_CUSTOM_OPP_PATH 指向
#      vendors/smem_mm_iar_310p3（运行期 aclnn 符号解析来源）
set -e
cd "$(dirname "$0")"
rm -rf build                       # 不删会命中旧的编译缓存
python3 setup.py build bdist_wheel
pip3 install dist/*.whl -I         # -I 不可省，否则 pip 复用已装版本
