// 仅用于验证：主动同步读取 V2 协议状态并提供诊断 Torch Op。
#include "mf310p_v2_serve_internal.h"
#include <c10/util/Exception.h>
#include <torch/library.h>
namespace vllm_ascend {
int64_t memfabric_mm_ar_v2_debug_status()
{
    auto& s = v2_serve_state();
    std::lock_guard<std::mutex> guard(s.mutex);
    TORCH_CHECK(s.handle != nullptr, "V2 diagnostic requires an initialized pool");
    const aclError sync = aclrtSynchronizeStream(s.execution_stream);
    TORCH_CHECK(sync == ACL_SUCCESS, "V2 diagnostic stream sync failed: ", sync);
    uint64_t protocol = ~0ULL;
    const int rc = mf310p_v2_trial_check_status(s.handle, &protocol);
    TORCH_CHECK(rc == 0, "V2 diagnostic status read failed rc=", rc);
    if (protocol != 0) {
        s.poisoned = true;
        s.failure = "device protocol status " + std::to_string(protocol);
    }
    return static_cast<int64_t>(protocol);
}


} // vllm_ascend 命名空间
TORCH_LIBRARY_FRAGMENT(_C_ascend, ops)
{
    ops.def("memfabric_mm_ar_v2_debug_status() -> int");
    ops.impl("memfabric_mm_ar_v2_debug_status",
             c10::DispatchKey::CompositeExplicitAutograd,
             &vllm_ascend::memfabric_mm_ar_v2_debug_status);
}
