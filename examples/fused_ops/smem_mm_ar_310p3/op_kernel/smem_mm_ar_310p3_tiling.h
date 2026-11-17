#ifndef SMEM_MM_AR310_P3_TILING_H
#define SMEM_MM_AR310_P3_TILING_H
#include <cstdint>

// SmemMmAr310p3（smem_mm_ar_310p3）tiling 数据。字段顺序与 op_host TilingFunc
// 的属性写入顺序一致；相比研发期 SmemMmArInlineReduceTilingData 删除了 flavor
// 字段——本算子即原 smem_mm_ar_inline_reduce 的 flavor 10（本地归约）部署档，
// 协议在编译期固定，不再运行时选择。
struct SmemMmAr310p3TilingData {
    uint64_t gva;
    uint64_t peerGva;
    uint64_t symm;
    uint64_t ibPtr;
    uint64_t statusPtr;
    uint64_t tracePtr;
    uint32_t rank;
    uint32_t epoch;
    uint32_t packet;
    uint32_t rows;
    uint32_t commRows;
    uint32_t aicCount;
    uint32_t enabled;
};

#endif // SMEM_MM_AR310_P3_TILING_H
