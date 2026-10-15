# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Huawei Technologies Co., Ltd. All Rights Reserved.

"""Runtime hooks that adapt MemFabric mm_ar to the 310P DFlash runner.

Keep the DFlash runner itself owned by the main development branch.  The
MemFabric customization only needs two lifecycle adjustments:

1. optionally wrap runner dummy/profile/capture runs in the existing
   MemFabric warmup fallback context; and
2. avoid device-wide synchronization once the long-lived MemFabric SDMA pool
   has started, because its supervised epoch kernel lives on a separate stream.

The hooks are installed lazily when an eligible mm_ar layer is selected, which
happens during model construction before profile/dummy runs.  Installation is
idempotent and preserves the complete DFlash implementation of ``_dummy_run``
(including FDO/FAP capture state and async metadata handling).
"""

from __future__ import annotations

from functools import wraps

import torch

_HOOKS_INSTALLED = False


def install_memfabric_mm_ar_runner_hooks() -> None:
    """Install the minimal MemFabric lifecycle adaptation on NPUModelRunner310."""

    global _HOOKS_INSTALLED
    if _HOOKS_INSTALLED:
        return

    from vllm_ascend._310p.model_runner_310p import NPUModelRunner310
    from vllm_ascend._310p.ops.memfabric_mm_ar import (
        memfabric_mm_ar_pool_started,
        memfabric_mm_ar_warmup_fallback,
    )

    # Preserve the DFlash-owned _dummy_run implementation wholesale.  The
    # wrapper only adds the MemFabric warmup context around it.
    original_dummy_run = NPUModelRunner310._dummy_run

    @wraps(original_dummy_run)
    def _dummy_run_with_memfabric(self, *args, **kwargs):
        with memfabric_mm_ar_warmup_fallback():
            return original_dummy_run(self, *args, **kwargs)

    def _sync_device_with_memfabric(self) -> None:
        # The MemFabric orchestrator runs a supervised epoch kernel on its own
        # launch stream for the lifetime of the pool.  A device-wide sync would
        # wait on that stream; after pool creation only drain the model stream.
        if memfabric_mm_ar_pool_started():
            torch.npu.current_stream().synchronize()
            return
        torch.npu.synchronize()

    NPUModelRunner310._dummy_run = _dummy_run_with_memfabric
    NPUModelRunner310._sync_device = _sync_device_with_memfabric
    _HOOKS_INSTALLED = True
