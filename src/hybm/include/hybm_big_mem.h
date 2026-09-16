/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
*/
#ifndef MEM_FABRIC_HYBRID_HYBM_BIG_MEM_C_H
#define MEM_FABRIC_HYBRID_HYBM_BIG_MEM_C_H

#include <stddef.h>
#include "hybm.h"
#include "hybm_def.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create Hybrid Big Memory(HyBM) entity at local side
 *
 * @param id               [in] identify for new entity
 * @param options          [in] options of the HyBM entity
 * @param flags            [in] optional flags, default value 0
 * @return entity if created, nullptr if failed to create
 */
hybm_entity_t hybm_create_entity(uint16_t id, const hybm_options *options, uint32_t flags);

/**
 * @brief Destroy HyBM entity
 *
 * @param e                [in] entity created by hybm_create_entity
 * @param flags            [in] optional flags, default value 0
 */
void hybm_destroy_entity(hybm_entity_t e, uint32_t flags);

/**
 * @brief Reserve virtual memory space at local side
 *
 * @param e                [in] entity created by hybm_create_entity
 * @param flags            [in] optional flags, default value 0
 * @return 0 if reserved successful
 */
int32_t hybm_reserve_mem_space(hybm_entity_t e, uint32_t flags);

/**
 * @brief Un-reserve virtual memory space at local side
 *
 * @param e                [in] entity created by hybm_create_entity
 * @param flags            [in] optional flags, default value 0
 * @return 0 if reserved successful
 */
int32_t hybm_unreserve_mem_space(hybm_entity_t e, uint32_t flags);

/**
 *
 * @param e                [in] entity created by hybm_create_entity
 * @param mType            [in] memory type, device or host
 * @return reserve memory ptr if successful, null ptr if failed
 */
void *hybm_get_memory_ptr(hybm_entity_t e, hybm_mem_type mType);

/**
 * @brief Allocate memory at local side(mmap local memory)
 *
 * @param e                [in] entity created by hybm_create_entity
 * @param mType            [in] memory type, device or host
 * @param size             [in] size of memory
 * @param flags            [in] optional flags, default value 0
 * @return mem slice handle if successful, null ptr if failed
 */
hybm_mem_slice_t hybm_alloc_local_memory(hybm_entity_t e, hybm_mem_type mType, uint64_t size, uint32_t flags);

/**
 * @brief Free memory at local side
 *
 * @param e                [in] entity created by hybm_create_entity
 * @param slice            [in] pointer of mem slice array, could be null if free all
 * @param count            [in] count of mem slice array, could be 0 if free all
 * @param flags            [in] HYBM_FREE_PARTIAL_SLICE or HYBM_FREE_ALL_SLICE
 * @return 0 if successful, error code if failed
 */
int32_t hybm_free_local_memory(hybm_entity_t e, hybm_mem_slice_t slice, uint32_t count, uint32_t flags);

/**
 * @brief get virtual address from slice
 *
 * @param e                [in] entity created by hybm_create_entity
 * @param slice             [in] mf slice ptr
 * @return non-null pointer to the virtual address of the slice or fail
 */
void *hybm_get_slice_va(hybm_entity_t e, hybm_mem_slice_t slice);

/**
 * @brief Register memory at local side, registered memory can be accessed by remote.
 * @param e                [in] entity created by hybm_create_entity
 * @param ptr              [in] local memory start address
 * @param size             [in] size of memory
 * @param flags            [in] optional flags, default value 0
 * @return mem slice handle if successful, null ptr if failed
 */
hybm_mem_slice_t hybm_register_local_memory(hybm_entity_t e, const void *ptr, uint64_t size, uint32_t flags);

/**
 * @brief Export exchange info for peer to import
 *
 * @param e                [in] entity created by hybm_create_entity
 * @param slice            [in] slice to export, could be null if export all
 * @param flags            [in] HYBM_FLAG_EXPORT_ENTITY: export entity instead of slice.
 * @param exInfo           [out] exchange info to be filled in
 * @return 0 if successful, error code if failed
 */
int32_t hybm_export(hybm_entity_t e, hybm_mem_slice_t slice, uint32_t flags, hybm_exchange_info *exInfo);

/**
 * @brief Import batch of exchange info of other HyBM entities
 *
 * @param e                [in] entity created by hybm_create_entity
 * @param allExInfo        [in] ptr of entities array
 * @param count            [in] count of entities
 * @param addresses        [out] import slices got local virtual address, can be null if not care
 * @param flags            [in] optional flags, default  0, HYBM_FLAG_EXPORT_ENTITY: export entity instead of slice.
 * @return 0 if successful
 */
int32_t hybm_import(hybm_entity_t e, const hybm_exchange_info allExInfo[], uint32_t count, void *addresses[],
                    uint32_t flags);

/**
 * @brief Mmap all memory which is imported
 *
 * @param e                [in] entity created by hybm_create_entity
 * @param flags            [in] optional flags, default value 0
 * @return 0 if successful, error code if failed
 */
int32_t hybm_mmap(hybm_entity_t e, uint32_t flags);

/**
 * @brief Unmap the entity
 *
 * @param e                [in] entity created by hybm_create_entity
 * @param flags            [in] optional flags, default value 0
 */
void hybm_unmap(hybm_entity_t e, uint32_t flags);

/**
 * @brief Determine whether various channels from local rank to the remote is reachable.
 * @param e                [in] entity created by hybm_create_entity
 * @param rank             [in] remote rank
 * @param reachTypes       [out] Which data operators can be represented in bits.
 * @param flags            [in] optional flags, default value 0
 * @return 0 if successful, error code if failed
 */
int32_t hybm_entity_reach_types(hybm_entity_t e, uint32_t rank, hybm_data_op_type &reachTypes, uint32_t flags);

/**
 * @brief Remove one rank after imported
 *
 * @param e                [in] entity created by hybm_create_entity
 * @param rank             [in] leave rank
 * @param flags            [in] optional flags, default value 0
 * @return 0 if successful, error code if failed
 */
int32_t hybm_remove_imported(hybm_entity_t e, uint32_t rank, uint32_t flags);

/**
 * @brief Write user extra context into the fixed addr of the hbm
 *
 * @param e                [in] entity created by hybm_create_entity
 * @param context          [in] user extra context addr
 * @param size             [in] user extra context size
 * @return 0 if successful
 */
int32_t hybm_set_extra_context(hybm_entity_t e, const void *context, uint32_t size);

/**
 * @brief Convert GVA (Global Virtual Address) to VA (Virtual Address) with specified memory type
 *
 * @param gva              [in] Global Virtual Address to convert
 * @param vaMemType        [in] Output va memory type, HYBM_MEM_TYPE_DEVICE or HYBM_MEM_TYPE_HOST
 * @param va               [out] Converted Virtual Address
 * @return 0 if successful, error code otherwise
 */
int32_t hybm_gva_to_va(uint64_t gva, hybm_mem_type vaMemType, uint64_t *va);

/**
 * @brief 310P AICore×AICPU 协同模式信息(进程级, 详见 310p-aicore-aicpu-开发进展.md)
 */
typedef struct {       /* 进程级协同模式就绪信息(HybmOverlapPrep 单例的查询快照) */
    uint32_t sqId;      /* CP 流 SQ id(kernel 内活查询用) */
    int32_t streamId;   /* CP 流 id(SQE 的 rt_stream_id) */
    uint32_t dieId;     /* die 号 = 设备号 % 2(CP1 内 HAL devId) */
    uint64_t workspace; /* 协同工作区设备地址(mailbox/sentinel/result/ovf) */
} hybm_overlap_info_t; /* 供 host 装配 AICore kernel 参数与诊断用 */

/**
 * @brief 查询协同模式就绪信息(未就绪返回非 0)
 * @param info            [out] 就绪信息
 * @return 0 if successful
 */
int32_t hybm_overlap_get_info(hybm_overlap_info_t *info); // 未就绪(开关关/初始化失败)返回非 0

/**
 * @brief 提交一次协同搬运(流水语义: AICPU 轮询本端邮箱 → 逐 chunk SQE 对
 *        [数据 src(邮箱描述符)→dstAddr+c*chunkBytes + 标志 哨兵→flagDoneAddr+c*8]
 *        → 每 chunk 一次 doorbell)。邮箱由本端 AICore kernel 写
 *        (smem_shm_aicore_overlap.h 的 notify 原语)。FIFO 保序: 等待最后一块
 *        标志到达即全部数据就位。chunks=1 即单块闭环语义。
 * @param dstAddr         [in] 数据目的基址(池 GVA, kernel 内按 chunk 递增)
 * @param flagDoneAddr    [in] 标志数组目的基址(池 GVA, 每槽 8B × chunks)
 * @param chunks          [in] chunk 数(1~64, 受邮箱槽数约束)
 * @return 0 if successful
 */
int32_t hybm_overlap_submit(uint64_t dstAddr, uint64_t flagDoneAddr, uint32_t chunks); // 流水提交

/**
 * @brief 等待协同搬运完成(SynchronizeStream)
 * @return 0 if successful
 */
int32_t hybm_overlap_wait(void); // SynchronizeStream 等编排 kernel 完成

/**
 * @brief 读取最近一次协同搬运的 kernel 结果(诊断: mainRet/stage/headAfter)
 * @param mainRet         [out] kernel 主返回码(0=成功, 13xx/14xx 段见计划附录 A.6)
 * @param stage           [out] 状态机阶段(5=STAGE_DONE)
 * @param headAfter       [out] SQ head(TSFW 消费证据)
 * @return 0 if successful
 */
int32_t hybm_overlap_get_result(uint32_t *mainRet, uint32_t *stage, uint32_t *headAfter); // 诊断读回

#ifdef __cplusplus
}
#endif

#endif // MEM_FABRIC_HYBRID_HYBM_BIG_MEM_C_H
