
#include "ops_common.h"

// 自定义算子包（libcust_opapi.so）的候选搜索路径，由 ops_common.h 中的
// get_custom_lib_path() 依据 ASCEND_CUSTOM_OPP_PATH 等环境变量生成。
const std::vector<std::string> g_custom_lib_path = get_custom_lib_path();
// 兜底搜索路径（环境变量未配置时使用 CANN 默认安装位置）。
const std::vector<std::string> g_default_custom_lib_path = get_default_custom_lib_path();

// GetOpApiFuncAddrFromFeatureLib：在 CANN 的各特性库中查找 aclnn 符号。
// 仅当算子随特性库（而非自定义算子包）分发时才会命中；对自定义算子包
// 走 ops_common.h 里的主查找路径。逐个库尝试，找不到返回空指针。
void *GetOpApiFuncAddrFromFeatureLib(const char *api_name)
{
    GET_OP_API_FUNC_FROM_FEATURE_LIB(ops_infer_handler, "libaclnn_ops_infer.so", api_name);
    GET_OP_API_FUNC_FROM_FEATURE_LIB(ops_train_handler, "libaclnn_ops_train.so", api_name);
    GET_OP_API_FUNC_FROM_FEATURE_LIB(math_handler, "libaclnn_math.so", api_name);
    GET_OP_API_FUNC_FROM_FEATURE_LIB(sparse_handler, "libaclnn_sparse.so", api_name);
    GET_OP_API_FUNC_FROM_FEATURE_LIB(fft_handler, "libaclnn_fft.so", api_name);
    GET_OP_API_FUNC_FROM_FEATURE_LIB(rand_handler, "libaclnn_rand.so", api_name);
    return nullptr;
}

// array_to_small_vector：把 torch 的 IntArrayRef 逐元素复制成
// c10::SmallVector（torch_npu 多数接口的形状参数类型）。
c10::SmallVector<int64_t, SIZE> array_to_small_vector(c10::IntArrayRef shape)
{
    c10::SmallVector<int64_t, SIZE> shape_small_vec;
    for (uint64_t i = 0; i < shape.size(); i++) {
        shape_small_vec.emplace_back(shape[i]);
    }

    return shape_small_vec;
}
