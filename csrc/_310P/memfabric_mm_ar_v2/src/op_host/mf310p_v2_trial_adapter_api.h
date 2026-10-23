/* 独立 V2 实验 ABI，当前不直接接入 vLLM 模型分发。 */
#pragma once
#include <stdint.h>
#if defined(__GNUC__)
#define MF310P_V2_API __attribute__((visibility("default")))
#else
#define MF310P_V2_API
#endif
#ifdef __cplusplus
extern "C" {
#endif
typedef struct mf310p_v2_trial mf310p_v2_trial_t;

/* 调用方必须先选择 310P3 设备并创建 ACL 上下文。 */
MF310P_V2_API int mf310p_v2_trial_create(int rank, const char* store_url,
                          uint32_t chunk_rows, mf310p_v2_trial_t** out);
/* 必须先通过 G0 探针，包含两次 AICore 启动的 IB 事件对复用验证。 */
MF310P_V2_API int mf310p_v2_trial_ib_probe(mf310p_v2_trial_t* handle,
                            uint32_t repetitions, void* acl_stream);
/* x/w/y 均为设备指针，形状依次为 [M,2048]、NZ[2048,2048]、[M,2048]。原型要求 M>0、M 能被 chunk_rows 整除且 M≤8192。两个 Rank 必须按相同的 M、chunk_rows、overlap 和调用顺序执行。接口只负责入队，验证时须同步 Stream 并检查 status。 */
MF310P_V2_API int mf310p_v2_trial_run(mf310p_v2_trial_t* handle, uint64_t x,
                        uint64_t w, uint64_t y, uint32_t m,
                        uint32_t overlap, void* acl_stream);
MF310P_V2_API int mf310p_v2_trial_check_status(mf310p_v2_trial_t* handle,
                                uint64_t* protocol_status);
MF310P_V2_API int mf310p_v2_trial_destroy(mf310p_v2_trial_t* handle);
#ifdef __cplusplus
}
#endif
