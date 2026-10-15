/*
 * Copyright (c) 2026 Huawei Technologies Co., Ltd. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 */

#include <torch/library.h>

#ifdef ASCEND_PLATFORM_310P
#include "memfabric_mm_ar_torch_adpt.h"

TORCH_LIBRARY_FRAGMENT(_C_ascend, ops)
{
    /* Fused matmul + TP=2 reduction through the installed MemFabric
     * public SHM/SDMA API. The op is always registered; feature-off builds
     * raise a clear error. */
    ops.def(
        "memfabric_mm_ar_allreduce("
        "Tensor x, Tensor weight, int tp_rank, int batch_basem_count) -> Tensor");
    ops.impl(
        "memfabric_mm_ar_allreduce",
        torch::kPrivateUse1,
        &vllm_ascend::memfabric_mm_ar_allreduce);

    /* Deterministic worker-lifetime teardown (also auto-registered via
     * atexit on first context creation). The schema is declared once and
     * implemented for both the PrivateUse1 key and a catch-all key so it is
     * callable without a tensor argument on any backend. */
    ops.def("memfabric_mm_ar_shutdown() -> ()");
    ops.impl(
        "memfabric_mm_ar_shutdown",
        torch::kPrivateUse1,
        &vllm_ascend::memfabric_mm_ar_shutdown);
    ops.impl(
        "memfabric_mm_ar_shutdown",
        c10::DispatchKey::CompositeExplicitAutograd,
        &vllm_ascend::memfabric_mm_ar_shutdown);

    /* Debug-only application snapshot (see torch_adpt.h). */
    ops.def("memfabric_mm_ar_debug_snapshot() -> Tensor");
    ops.impl(
        "memfabric_mm_ar_debug_snapshot",
        c10::DispatchKey::CompositeExplicitAutograd,
        &vllm_ascend::memfabric_mm_ar_debug_snapshot);
}
#endif
