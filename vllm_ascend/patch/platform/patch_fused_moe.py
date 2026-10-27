#
# Copyright (c) 2025 Huawei Technologies Co., Ltd. All Rights Reserved.
# This file is a part of the vllm-ascend project.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

# Patch vllm's MoE factory to use AscendMoERunner by default.
#
# vLLM v0.26.0 exports the factory as FusedMoE, while vLLM v0.27.0 renamed it
# to FusedMoEFactory. We patch the exact binding for the active version lane
# in both the package __init__ and layer module before any model is imported.
#
# Import order in worker.__init__:
#   1. adapt_patch()  ->  this file runs  ->  MoE factory patched
#   2. from vllm_ascend import ops
#   3. model loading  ->  deepseek_v2 imported  ->  gets patched factory  ✓

import vllm.model_executor.layers.fused_moe as _fused_moe_pkg
import vllm.model_executor.layers.fused_moe.layer as _fused_moe_layer

from vllm_ascend.utils import is_310p

# Capture the real original before fused_moe.py's module-level code runs.
_original_FusedMoE = getattr(_fused_moe_layer, "FusedMoE", None)
if _original_FusedMoE is None:
    _original_FusedMoE = _fused_moe_layer.FusedMoEFactory
    _factory_attr = "FusedMoEFactory"
else:
    _factory_attr = "FusedMoE"

if is_310p():
    from vllm_ascend._310p.fused_moe.fused_moe import AscendMoERunner310 as _DefaultAscendMoERunner
else:
    from vllm_ascend.ops.fused_moe.fused_moe import AscendMoERunner as _DefaultAscendMoERunner


def _ascend_FusedMoE(*args, runner_cls=None, runner_args=None, **kwargs):
    if runner_cls is None:
        runner_cls = _DefaultAscendMoERunner
    # 'hash' is a DeepSeek V4 flag already consumed before FusedMoE is called;
    # 'tid2eid' is Ascend-specific and must reach AscendMoERunner via runner_args.
    kwargs.pop("hash", None)
    tid2eid = kwargs.pop("tid2eid", None)
    if tid2eid is not None:
        runner_args = dict(runner_args) if runner_args is not None else {}
        runner_args["tid2eid"] = tid2eid
    return _original_FusedMoE(*args, runner_cls=runner_cls, runner_args=runner_args, **kwargs)


setattr(_fused_moe_layer, _factory_attr, _ascend_FusedMoE)
setattr(_fused_moe_pkg, _factory_attr, _ascend_FusedMoE)
