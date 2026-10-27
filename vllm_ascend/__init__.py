#
# Copyright (c) 2025 Huawei Technologies Co., Ltd. All Rights Reserved.
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
# This file is a part of the vllm-ascend project.
#

import logging

_GLOBAL_PATCH_APPLIED = False
_logger = logging.getLogger(__name__)


def _install_fla_compat_alias() -> None:
    """Alias vllm.model_executor.layers.fla onto its vLLM v0.27+ location.

    vLLM v0.27.0 moved the FLA ops package to
    vllm.third_party.flash_linear_attention. vllm-ascend patch modules import
    and rebind symbols through the v0.26 lane dotted path; register that path
    as an alias of the real package so both vLLM lanes work and attribute
    patches land on the real modules.
    """
    import importlib
    import sys
    import types

    legacy_root = "vllm.model_executor.layers.fla"
    real_root = "vllm.third_party.flash_linear_attention"
    if legacy_root in sys.modules:
        return
    try:
        importlib.import_module(legacy_root)
        return
    except ModuleNotFoundError:
        pass

    suffixes = (
        "",
        ".ops",
        ".ops.index",
        ".ops.utils",
        ".ops.l2norm",
        ".ops.layernorm_guard",
        ".ops.fused_recurrent",
    )
    root_alias = types.ModuleType(legacy_root)
    root_alias.__path__ = []
    for suffix in suffixes:
        target = importlib.import_module(real_root + suffix)
        sys.modules[legacy_root + suffix] = root_alias if suffix == "" else target
        if suffix:
            head, _, leaf = suffix.rpartition(".")
            parent = root_alias if head == "" else sys.modules[legacy_root + head]
            setattr(parent, leaf, target)
    import vllm.model_executor.layers as _layers_pkg

    _layers_pkg.fla = root_alias


def _ensure_global_patch():
    """Apply process-wide vLLM patches before engine-core initialization.

    vLLM loads general plugins in engine-core subprocesses. E2E test
    conftest hooks do not run there, so global patches that affect scheduler
    and engine code must also be applied through these plugin entry points.
    """
    global _GLOBAL_PATCH_APPLIED
    if _GLOBAL_PATCH_APPLIED:
        return

    from vllm_ascend.utils import adapt_patch

    adapt_patch(is_global_patch=True)
    _GLOBAL_PATCH_APPLIED = True


def register():
    """Register the NPU platform."""

    return "vllm_ascend.platform.NPUPlatform"


def register_connector():
    _ensure_global_patch()

    from vllm_ascend.distributed.kv_transfer import register_connector
    from vllm_ascend.distributed.weight_transfer import register_engine

    register_connector()
    register_engine()


def register_model_loader():
    _ensure_global_patch()

    from .model_loader.netloader import register_netloader
    from .model_loader.rfork import register_rforkloader

    register_netloader()
    register_rforkloader()


def register_service_profiling():
    _ensure_global_patch()

    from .profiling_config import generate_service_profiling_config

    generate_service_profiling_config()


def register_model():
    import transformers

    if hasattr(transformers, "HunYuanVLProcessor"):
        from vllm_ascend.patch.hunyuan_vl_processor_compat import (
            install_hunyuan_vl_processor_compat,
        )

        install_hunyuan_vl_processor_compat()
    else:
        _logger.info(
            "Skipping optional HunyuanVL processor compatibility patch: transformers.HunYuanVLProcessor is unavailable."
        )
    from .models import register_model

    register_model()


import vllm_ascend.logger  # noqa: E402, F401
