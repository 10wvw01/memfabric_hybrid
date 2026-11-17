#include <torch/extension.h>
#include <torch/library.h>

// smem_mm_ar_310p3: TP2 MatMul + AllReduce 通算融合算子（本地归约：全量 COPY
// 交换 + AIV 车道本地加法，MemFabric SDMA 池）。形参顺序与
// aclnnSmemMmAr310p3GetWorkspaceSize 完全一致：
//   (x, w, gva, peerGva, symm, ibPtr, statusPtr, tracePtr, rank, epoch,
//    packet, rows, commRows, aicCount, enabled, y)
// y 为调用方预分配的输出（Tensor!）。
TORCH_LIBRARY(custom, m) {
    m.def("npu_smem_mm_ar_310p3(Tensor x, Tensor w, Tensor! y, int gva, int peer_gva, "
          "int symm, int ib_ptr, int status_ptr, int trace_ptr, int rank, int epoch, int packet, "
          "int rows, int comm_rows, int aic_count, int enabled) -> Tensor");
}

PYBIND11_MODULE(TORCH_EXTENSION_NAME, m) {}
