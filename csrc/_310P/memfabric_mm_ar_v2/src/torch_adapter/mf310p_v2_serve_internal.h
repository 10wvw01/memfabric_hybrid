#pragma once
// Serving 正式路径与调试状态检查共用的上下文布局。
#include <ATen/ATen.h>
#include <acl/acl_rt.h>
#include <mutex>
#include <string>
#include <vector>
#include "mf310p_v2_trial_adapter_api.h"
namespace vllm_ascend {
struct V2ServeState {
    std::mutex mutex;
    mf310p_v2_trial_t* handle = nullptr;
    int rank = -1;
    uint32_t chunk_rows = 0;
    aclrtStream execution_stream = nullptr;
    bool poisoned = false;
    std::string failure;
    // 每个输出槽的内存分配贯穿建池前后整个生命周期。返回的 narrow
    // 视图会额外持有 StorageImpl 引用，模型仍引用输出时
    // 禁止回收该槽，否则会覆盖尚未消费的结果。
    std::vector<at::Tensor> output_slots;
    at::Tensor input_stage;
};
V2ServeState& v2_serve_state();
} // vllm_ascend 命名空间
