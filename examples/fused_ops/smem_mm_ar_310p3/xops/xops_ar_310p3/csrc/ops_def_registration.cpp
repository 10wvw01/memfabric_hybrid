#include <torch/extension.h>
#include <torch/library.h>

#include "../../../op_kernel/smem_mm_ar_310p3_tiling.h"

// smem_mm_ar_310p3：TP2 MatMul + AllReduce 通算融合算子，本地归约版
//（全量复制交换 + Vector 核归约加法并行处理分支本地相加，通信经
// MemFabric SDMA 池）。本文件只做 torch custom op 的算子签名注册。
// 形参顺序与 aclnnSmemMmAr310p3GetWorkspaceSize 完全一致：
//   (x, w, gva, symm, statusPtr, tracePtr, rank, epoch, rows, y)
// y 为调用方预分配的输出（签名中的 Tensor! 表示该输入会被原地写回）。
// SDMA 包长（1MiB）/通信组行数（1024）/矩阵乘核数（8）为 kernel 编译期
// 常量；组完成通知数组在池内 seats 区。
TORCH_LIBRARY(custom, m) {
    m.def("npu_smem_mm_ar_310p3(Tensor x, Tensor w, Tensor! y, int gva, "
          "int symm, int status_ptr, int trace_ptr, int rank, int epoch, "
          "int rows) -> Tensor");
}

// pybind 模块体：算子本体经 TORCH_LIBRARY 静态注册，这里只导出池契约
// 查询函数。数字同源于 op_kernel/smem_mm_ar_310p3_tiling.h 的布局推导段
//（唯一事实源）：会话封装层（session.py）据此程序化决定建池尺寸与行数
// 契约校验，外部不需要阅读任何布局文档，也不存在第二份手工常量。
PYBIND11_MODULE(TORCH_EXTENSION_NAME, m) {
    m.def("pool_floor_bytes", []() { return static_cast<int64_t>(kPoolFloor); },
          "Minimum per-rank symmetric pool bytes required by this operator");
    m.def("kernel_width", []() { return static_cast<int64_t>(kWidth); },
          "K == N width of the fixed kernel geometry");
    m.def("kernel_max_rows", []() { return static_cast<int64_t>(kMaxRows); },
          "Maximum rows M of one fused call");
    m.def("kernel_min_rows", []() { return static_cast<int64_t>(kMinRows); },
          "Minimum rows M of one fused call");
    m.def("kernel_rows_align", []() { return static_cast<int64_t>(kRowsAlign); },
          "Row alignment requirement of M");
}
