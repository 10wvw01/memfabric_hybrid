// smem_mm_ar_310p3 PTA 实现：透传给 libcust_opapi.so 的 aclnnSmemMmAr310p3
// 两段式符号。
// 实参顺序必须与 aclnnSmemMmAr310p3GetWorkspaceSize 的形参顺序完全一致：
//   (x, w, 13 个 int64 属性, y)；末尾 workspaceSize/executor 由宏自动补齐。
#include <torch/library.h>

#include "ops_common.h"

namespace custom {

at::Tensor npu_smem_mm_ar_310p3(const at::Tensor& x, const at::Tensor& w, at::Tensor& y,
                                int64_t gva, int64_t peerGva, int64_t symm, int64_t ibPtr,
                                int64_t statusPtr, int64_t tracePtr, int64_t rank,
                                int64_t epoch, int64_t packet, int64_t rows,
                                int64_t commRows, int64_t aicCount, int64_t enabled)
{
    // w 的物理布局是 FRACTAL_NZ（npu_format_cast(29) 产物），但 OpDef 原型声明为
    // ND，且 torch_npu 的 ConvertType 无法为"ND 标记 + NZ 存储"的组合构造 aclTensor。
    // 手工构造：逻辑 shape/strides + ACL_FLOAT16 + ACL_FORMAT_ND + NZ 存储基址；
    // kernel 只消费 GM 基址。EXEC_NPU_CMD_V1 结束时经 ReleaseConvertTypes 调
    // aclDestroyTensor 释放。
    static const auto aclCreateTensorFn = GET_OP_API_FUNC(aclCreateTensor);
    TORCH_CHECK(aclCreateTensorFn != nullptr, "aclCreateTensor not found");
    const int64_t n = w.size(0), k = w.size(1);
    const int64_t dims[2] = {n, k};
    const int64_t strides[2] = {k, 1};
    aclTensor *w_acl = aclCreateTensorFn(dims, 2, ACL_FLOAT16, strides, 0,
                                         ACL_FORMAT_ND, dims, 2,
                                         const_cast<void *>(w.const_data_ptr()));
    TORCH_CHECK(w_acl != nullptr, "aclCreateTensor(w) failed");
    EXEC_NPU_CMD_V1(aclnnSmemMmAr310p3, x, w_acl, gva, peerGva, symm, ibPtr,
                   statusPtr, tracePtr, rank, epoch, packet, rows, commRows, aicCount,
                   enabled, y);
    return y;
}

at::Tensor npu_smem_mm_ar_310p3_meta(const at::Tensor& x, const at::Tensor& w,
                                     at::Tensor& y,
                                     int64_t gva, int64_t peerGva, int64_t symm,
                                     int64_t ibPtr, int64_t statusPtr, int64_t tracePtr,
                                     int64_t rank, int64_t epoch, int64_t packet,
                                     int64_t rows, int64_t commRows, int64_t aicCount,
                                     int64_t enabled)
{
    return y;
}

}  // namespace custom

TORCH_LIBRARY_IMPL(custom, PrivateUse1, m)
{
    m.impl("npu_smem_mm_ar_310p3", &custom::npu_smem_mm_ar_310p3);
}

TORCH_LIBRARY_IMPL(custom, Meta, m)
{
    m.impl("npu_smem_mm_ar_310p3", &custom::npu_smem_mm_ar_310p3_meta);
}
