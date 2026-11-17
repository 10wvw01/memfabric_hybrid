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
import gc
import os

import psutil
import torch
import torch_npu
from vllm.logger import logger
from vllm.utils.mem_constants import GiB_bytes
from vllm.utils.mem_utils import MemorySnapshot, memory_profiling
from vllm.utils.torch_utils import set_random_seed  # noqa: E402

from vllm_ascend._310p.model_runner_310p import NPUModelRunner310
from vllm_ascend.utils import is_rc_device
from vllm_ascend.worker.worker import NPUWorker, init_workspace_manager


class NPUWorker310(NPUWorker):
    def init_device(self):
        self.device = self._init_device()
        torch_npu.npu.set_compile_mode(jit_compile=False)

        init_workspace_manager(self.device, num_ubatches=1)

        self.model_runner = NPUModelRunner310(self.vllm_config, self.device)
        logger.info_once("Using NPUWorker310 and NPUModelRunner310.")

    def save_sharded_state(
        self,
        path: str,
        pattern: str | None = None,
        max_size: int | None = None,
    ) -> None:
        from vllm_ascend._310p.sharded_state_loader_310p import ShardedStateLoader310

        ShardedStateLoader310.save_model(
            self.model_runner.model,
            path,
            pattern=pattern,
            max_size=max_size,
        )

        ShardedStateLoader310.generate_quant_description(
            self.model_runner.model,
            path,
            self.vllm_config.quant_config,
        )

    @torch.inference_mode()
    def determine_available_memory(self) -> int:
        """Profiles the peak memory usage of the model to determine how much
        memory can be used for KV cache without OOMs.

        The engine will first conduct a profiling of the existing memory usage.
        Then, it calculates the free memory that can be used for KV cache in
        bytes.
        """
        if self.cache_config.kv_cache_memory_bytes:
            return super().determine_available_memory()

        GiB = lambda b: b / GiB_bytes
        # Execute a forward pass with dummy inputs to profile the memory usage
        # of the model.
        with memory_profiling(
            self.init_snapshot,
            weights_memory=int(self.model_runner.model_memory_usage),
        ) as profile_result:
            self.model_runner.profile_run()
            free_memory, total_memory = torch.npu.mem_get_info()
            # The host memory or device memory for RC devices refers to the available portion of memory
            # which cannot be obtained via torch.npu.mem_get_info()
            if is_rc_device():
                free_memory = psutil.virtual_memory().available
            torch_memory = torch.npu.memory_reserved()
            non_torch_memory_before_empty_cache = total_memory - free_memory - torch_memory

        self.non_torch_memory = profile_result.non_torch_increase
        self.peak_activation_memory = profile_result.torch_peak_increase
        non_torch_memory_cleared_by_empty_cache = non_torch_memory_before_empty_cache - self.non_torch_memory

        free_gpu_memory = profile_result.after_profile.free_memory
        assert self.init_snapshot.free_memory > free_gpu_memory, (
            "Error in memory profiling. "
            f"Initial free memory {GiB(self.init_snapshot.free_memory)} GiB, "
            f"current free memory {GiB(free_gpu_memory)} GiB. "
            "This happens when other processes sharing the same container "
            "release GPU memory while vLLM is profiling during initialization. "
            "To fix this, ensure consistent GPU memory allocation or "
            "isolate vLLM in its own container."
        )

        # Divide the available memory by 2, to reserved more memory for other operators workspace and other cache
        # This could avoid OOM with default gpu_memory_utilization
        # The 310P RC device shares the host memory and device memory.
        # Therefore, the space available for allocating KV cache and Mamba cache needs to be calculated
        # based on the already occupied space of the system memory.

        if is_rc_device():
            vm = psutil.virtual_memory()
            self.available_kv_cache_memory_bytes = (self.requested_memory - (vm.total - vm.available)) // 2
        else:
            self.available_kv_cache_memory_bytes = (
                self.requested_memory - profile_result.non_kv_cache_memory - non_torch_memory_cleared_by_empty_cache
            ) // 2

        logger.debug(profile_result)
        logger.info_once(
            "Available KV cache memory: %.2f GiB (halved for workspace)",
            GiB(self.available_kv_cache_memory_bytes),
            scope="local",
        )
        return int(self.available_kv_cache_memory_bytes)

    def _warm_up_atb(self):
        # 310p device do not support torch_npu._npu_matmul_add_fp32 atb ops
        logger.info_once("Skip warm-up atb ops for 310P device.")

    def _init_device(self):
        # Optional device offset so TP workers can land on a physical device
        # pair that avoids device 0 (e.g. VLLM_ASCEND_DEVICE_OFFSET=4 uses
        # physical 4/5). Required by smem_mm_310p3: the memfabric symmetric-window
        # VA layout is keyed by the device id pair, and this host's device 0
        # layout is asymmetric, which breaks the pool import. Do NOT combine
        # with ASCEND_RT_VISIBLE_DEVICES remapping — smem must receive the
        # real physical ids (user ids), which remapping makes impossible.
        device_offset = int(os.getenv("VLLM_ASCEND_DEVICE_OFFSET", "0"))
        device_index = self.local_rank + device_offset
        device = torch.device(f"npu:{device_index}")
        torch.npu.set_device(device)

        # smem_mm_310p3: reserve the MemFabric SDMA pool window as the FIRST NPU
        # allocation of this worker process. Both TP workers must reserve
        # the window at the same VA base (310P ACL path picks the base from
        # process VA state), so this must run before HCCL init / torch
        # allocator growth / weight loading diverge the two processes.
        # Operator selection (smem_mm_ar_310p3 local reduction /
        # smem_mm_iar_310p3 hardware reduction) happens inside; the two are
        # mutually exclusive per serve instance.
        try:
            from vllm_ascend.ops.smem_mm_310p3 import early_bootstrap_smem_mm_310p3
            early_bootstrap_smem_mm_310p3(device_index, self.rank,
                                          self.vllm_config)
        except Exception:
            logger.exception("smem_mm_310p3 early bootstrap failed; "
                             "continuing without it")

        # This lazy import avoids torch_npu re-initialization in patch
        # Note that this should be imported after torch.npu.set_device
        # to avoid repeated set_device in extra processes

        gc.collect()
        torch.npu.empty_cache()

        # take current memory snapshot
        self.init_snapshot = MemorySnapshot(device=device)
        self.requested_memory = self.init_snapshot.total_memory * self.cache_config.gpu_memory_utilization
        if is_rc_device():
            self.init_snapshot.free_memory = psutil.virtual_memory().available
            logger.info_once("Root Complex (RC) mode: host and device memory are shared.")
        if self.init_snapshot.free_memory < self.requested_memory:
            GiB = lambda b: round(b / GiB_bytes, 2)
            raise ValueError(
                f"Free memory on device "
                f"({GiB(self.init_snapshot.free_memory)}/"
                f"{GiB(self.init_snapshot.total_memory)} GiB) on startup "
                f"is less than desired GPU memory utilization "
                f"({self.cache_config.gpu_memory_utilization}, "
                f"{GiB(self.requested_memory)} GiB). Decrease GPU memory "
                f"utilization or reduce GPU memory used by other processes."
            )

        if (
            self.parallel_config.data_parallel_size > 1
            and self.parallel_config.data_parallel_size_local > 0
            and self.parallel_config.distributed_executor_backend not in ["ray", "external_launcher"]
            and self.vllm_config.parallel_config.data_parallel_backend != "ray"
            and self.vllm_config.parallel_config.nnodes_within_dp == 1
        ):
            visible_device_count = torch.npu.device_count() if torch.npu.is_available() else 0
            assert self.parallel_config.local_world_size <= visible_device_count, (
                f"local_world_size ({self.parallel_config.local_world_size}) must "
                f"be less than or equal to the number of visible devices "
                f"({visible_device_count})."
            )

        # Initialize the distributed environment.
        self._init_worker_distributed_environment()
        # Set random seed.
        set_random_seed(self.model_config.seed)

        return device
