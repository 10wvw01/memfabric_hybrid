// smem_mm_iar_310p3 的 PyTorch 适配层（PTA）实现：Python 侧的
// torch_npu.npu_smem_mm_iar_310p3(...) 经 torch custom op 落到这里，
// 再透传给算子包 libcust_opapi.so 导出的两段式 aclnn 符号
//（aclnnSmemMmIar310p3 + aclnnSmemMmIar310p3GetWorkspaceSize）。
// 实参顺序必须与 aclnnSmemMmIar310p3GetWorkspaceSize 的形参顺序完全一致：
//   (x, w, 7 个 int64 属性, y)；末尾的 workspaceSize/executor 由宏自动补齐。
#include <torch/library.h>

#include "ops_common.h"

namespace custom {

// npu_smem_mm_iar_310p3：宿主侧算子入口。
// x 为本 rank 激活（ND fp16），w 为本 rank 权重分片（物理 NZ 布局），
// y 由调用方预分配。7 个 int64 属性原样透传给 aclnn，不做加工
//（SDMA 包长/通信子组行数/矩阵乘核数等协议常量已固化在 kernel 内；
// 组完成通知数组在池内 seats 区，ibPtr 已随池布局自治删除）。
at::Tensor npu_smem_mm_iar_310p3(const at::Tensor& x, const at::Tensor& w, at::Tensor& y,
                                 int64_t gva, int64_t symm,
                                 int64_t statusPtr, int64_t tracePtr, int64_t rank,
                                 int64_t epoch, int64_t rows)
{
    // w 的物理布局是 FRACTAL_NZ（npu_format_cast(29) 的产物），但 OpDef 原型
    // 声明为 ND，且 torch_npu 的 ConvertType 无法为“ND 标记 + NZ 存储”的组合
    // 构造 aclTensor。这里手工构造 aclTensor：逻辑 shape/strides 按 ND 填写，
    // 存储基址指向 NZ 物理数据；kernel 侧只消费全局内存基址，不检查布局。
    // EXEC_NPU_CMD_V1 结束时经 ReleaseConvertTypes 调 aclDestroyTensor 释放。
    static const auto aclCreateTensorFn = GET_OP_API_FUNC(aclCreateTensor);
    TORCH_CHECK(aclCreateTensorFn != nullptr, "aclCreateTensor not found");
    const int64_t n = w.size(0), k = w.size(1);
    const int64_t dims[2] = {n, k};
    const int64_t strides[2] = {k, 1};
    aclTensor *w_acl = aclCreateTensorFn(dims, 2, ACL_FLOAT16, strides, 0,
                                         ACL_FORMAT_ND, dims, 2,
                                         const_cast<void *>(w.const_data_ptr()));
    TORCH_CHECK(w_acl != nullptr, "aclCreateTensor(w) failed");
    EXEC_NPU_CMD_V1(aclnnSmemMmIar310p3, x, w_acl, gva, symm,
                   statusPtr, tracePtr, rank, epoch, rows, y);
    return y;
}

// npu_smem_mm_iar_310p3_meta：torch.compile / Meta 模式下的形状推导函数。
// 输出 y 由调用方预分配且形状不变，直接原样返回即可。
at::Tensor npu_smem_mm_iar_310p3_meta(const at::Tensor& x, const at::Tensor& w,
                                     at::Tensor& y,
                                     int64_t gva, int64_t symm,
                                     int64_t statusPtr, int64_t tracePtr,
                                     int64_t rank, int64_t epoch, int64_t rows)
{
    return y;
}

}  // namespace custom

// 下方两个 TORCH_LIBRARY_IMPL 分别把宿主实现挂到 NPU 后端（PrivateUse1）
// 与 Meta 后端（形状推导）。
TORCH_LIBRARY_IMPL(custom, PrivateUse1, m)
{
    m.impl("npu_smem_mm_iar_310p3", &custom::npu_smem_mm_iar_310p3);
}

TORCH_LIBRARY_IMPL(custom, Meta, m)
{
    m.impl("npu_smem_mm_iar_310p3", &custom::npu_smem_mm_iar_310p3_meta);
}
