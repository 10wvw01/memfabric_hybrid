#ifndef REG_SYNC_AICORE_DEV_H
#define REG_SYNC_AICORE_DEV_H

#include <cstdint>
/* 注意: 此头被 host (main.cpp) 和 device (reg_sync_aicore_dev.cpp launch 封装段) 同时包含。
 * aclrtStream 类型在 device 侧编译时由 kernel_operator.h 间接提供, 不直接 include acl/acl.h。 */

#ifdef __cplusplus
extern "C" {
#endif

/* AICore kernel launch 封装(host 侧调用)。
 * stream 类型用 void* 代替 aclrtStream, 避免在 device 编译时拉入 acl/acl.h */
void RegSyncAicoreSend_do(void *stream, uint8_t *resultBuf, uint64_t expectCond);
void RegSyncAicoreRecv_do(void *stream, uint8_t *resultBuf, uint64_t slotOff);

#ifdef __cplusplus
}
#endif

#endif // REG_SYNC_AICORE_DEV_H
