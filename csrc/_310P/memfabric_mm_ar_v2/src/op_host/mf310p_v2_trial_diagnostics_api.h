#pragma once
// 仅用于验证的附加 C ABI；正式 Serving 不包含此头文件。
#include "mf310p_v2_trial_adapter_api.h"
#ifdef __cplusplus
extern "C" {
#endif
/* T1 原始 NZ 权重直接 ND 输出：精确支持 M=1～256，不切片权重、不暂存输入；MM/Add 核数独立取 1/2/4/8。仍为实验 C API，不由 vLLM 自动路由。 */
MF310P_V2_API int mf310p_v2_trial_run_nz_direct_small(
    mf310p_v2_trial_t* handle, uint64_t x, uint64_t w, uint64_t y,
    uint32_t m, uint32_t mm_cores, uint32_t add_cores,
    void* acl_stream);
/* T2 非融合基线：框架原生 MM 在同一 ACL Stream 上先生成连续 ND FP16 的 native_partial[M,2048]。AR-only 先暂存进对称 C，再经 MemFabric signal/wait、Vector Add 与波级 DONE 完成规约。支持 M=1～8192、Add 核数 1/2/4/8，完整计入暂存成本；禁止 native_partial 与 output 重叠，或同一上下文跨 Stream 并发。 */
MF310P_V2_API int mf310p_v2_trial_run_ar_only(
    mf310p_v2_trial_t* handle, uint64_t native_partial, uint64_t output,
    uint32_t m, uint32_t add_cores, void* acl_stream);


/* 测试辅助：同步 D2H 读取本 Rank 的 SEND 对称区；该 DDR 视图与 MemFabric AICPU SDMA 引擎可见内容一致。 */
MF310P_V2_API int mf310p_v2_trial_read_send_arena(
    mf310p_v2_trial_t* handle, void* host_dst, uint32_t bytes);
/* 测试辅助：分阶段隔离融合流水线。diag=0 为每核单 Chunk MM 直接写 y，不通信；diag=1 为 MM 写 ownC 后执行一次 signal/wait 与 Add，跳过核间 IB 会合（Core0 自身 MM 完成后即可继续）。 */
MF310P_V2_API int mf310p_v2_trial_diag(mf310p_v2_trial_t* handle, uint64_t x,
                         uint64_t w, uint64_t y, uint32_t diag,
                         void* acl_stream);

#ifdef __cplusplus
}
#endif
