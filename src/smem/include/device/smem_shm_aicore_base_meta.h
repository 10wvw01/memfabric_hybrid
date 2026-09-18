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
#ifndef __MEMFABRIC_SMEM_AI_CORE_BASE_META_H__
#define __MEMFABRIC_SMEM_AI_CORE_BASE_META_H__

#include "smem_shm_aicore_base_define.h"

constexpr uint64_t SMEM_SHM_DEVICE_END_ADDR = 0x180000000000ULL - (1UL << 30UL);
constexpr uint64_t SMEM_SHM_DEVICE_PRE_META_SIZE = 128UL;                            // 128B
constexpr uint64_t SMEM_SHM_DEVICE_GLOBAL_META_SIZE = SMEM_SHM_DEVICE_PRE_META_SIZE; // 128B
constexpr uint64_t SMEM_OBJECT_NUM_MAX = 511UL;                                      // entity最大数量
constexpr uint64_t SMEM_SHM_DEVICE_META_SIZE =
    SMEM_SHM_DEVICE_PRE_META_SIZE * SMEM_OBJECT_NUM_MAX + SMEM_SHM_DEVICE_GLOBAL_META_SIZE; // 64K

constexpr uint64_t SMEM_SHM_DEVICE_USER_CONTEXT_PRE_SIZE = 64UL * 1024UL; // 64K
constexpr uint64_t SMEM_SHM_DEVICE_INFO_SIZE = SMEM_SHM_DEVICE_USER_CONTEXT_PRE_SIZE * SMEM_OBJECT_NUM_MAX +
                                               SMEM_SHM_DEVICE_META_SIZE; // 元数据+用户context,总大小32M, 对齐2M
constexpr uint64_t SMEM_SHM_DEVICE_META_ADDR = SMEM_SHM_DEVICE_END_ADDR - SMEM_SHM_DEVICE_INFO_SIZE;
constexpr uint64_t SMEM_SHM_DEVICE_USER_CONTEXT_ADDR = SMEM_SHM_DEVICE_META_ADDR + SMEM_SHM_DEVICE_META_SIZE;

constexpr uint64_t SMEM_SHM_DEVICE_META_OBJ_ID_OFFSET = 0;
constexpr uint64_t SMEM_SHM_DEVICE_META_RANK_OFFSET = SMEM_SHM_DEVICE_META_OBJ_ID_OFFSET + sizeof(uint32_t);
constexpr uint64_t SMEM_SHM_DEVICE_META_RANK_SIZE_OFFSET = SMEM_SHM_DEVICE_META_RANK_OFFSET + sizeof(uint32_t);
constexpr uint64_t SMEM_SHM_DEVICE_META_CONTEXT_OFFSET = SMEM_SHM_DEVICE_META_RANK_SIZE_OFFSET + sizeof(uint32_t);
constexpr uint64_t SMEM_SHM_DEVICE_META_SYMM_OFFSET = SMEM_SHM_DEVICE_META_CONTEXT_OFFSET + sizeof(uint32_t);
constexpr uint64_t SMEM_SHM_DEVICE_META_QP_INFO_OFFSET = SMEM_SHM_DEVICE_META_SYMM_OFFSET + sizeof(uint64_t);
constexpr uint64_t SMEM_SHM_DEVICE_META_SDMA_WORKSPACE_OFFSET = SMEM_SHM_DEVICE_META_QP_INFO_OFFSET + sizeof(uint64_t);
constexpr uint64_t SMEM_SHM_DEVICE_META_PHYS_SIZE_OFFSET = /* V5: 段物理大小槽 */
    SMEM_SHM_DEVICE_META_SDMA_WORKSPACE_OFFSET + sizeof(uint64_t);

SMEM_SHM_INLINE_AICORE uint32_t smem_shm_get_global_rank(uint32_t shmemId)
{
    if (shmemId >= SMEM_OBJECT_NUM_MAX) {
        return UINT32_MAX;
    }
    uint64_t metaAddr =
        SMEM_SHM_DEVICE_META_ADDR + SMEM_SHM_DEVICE_GLOBAL_META_SIZE + shmemId * SMEM_SHM_DEVICE_PRE_META_SIZE;
    return (*(__gm__ uint32_t *)(metaAddr + SMEM_SHM_DEVICE_META_RANK_OFFSET));
}

SMEM_SHM_INLINE_AICORE uint32_t smem_shm_get_global_rank_size(uint32_t shmemId)
{
    if (shmemId >= SMEM_OBJECT_NUM_MAX) {
        return UINT32_MAX;
    }
    uint64_t metaAddr =
        SMEM_SHM_DEVICE_META_ADDR + SMEM_SHM_DEVICE_GLOBAL_META_SIZE + shmemId * SMEM_SHM_DEVICE_PRE_META_SIZE;
    return (*(__gm__ uint32_t *)(metaAddr + SMEM_SHM_DEVICE_META_RANK_SIZE_OFFSET));
}

SMEM_SHM_INLINE_AICORE uint64_t smem_shm_get_symmetric_size(uint32_t shmemId)
{
    if (shmemId >= SMEM_OBJECT_NUM_MAX) {
        return 0;
    }
    uint64_t metaAddr =
        SMEM_SHM_DEVICE_META_ADDR + SMEM_SHM_DEVICE_GLOBAL_META_SIZE + shmemId * SMEM_SHM_DEVICE_PRE_META_SIZE;
    return (*(__gm__ uint64_t *)(metaAddr + SMEM_SHM_DEVICE_META_SYMM_OFFSET));
}

/* 工具链约束(dav-2002/bisheng 实测, 910B 后端无此限制):
 * ① 运行时整数 → __gm__ 指针 cast 会错位——void* 型恒 +0x2000, 变量绑定型 u64 在 -O2 错位;
 * ② __gm__ void** 解引用读取错误(-O0 读到 0xffff...0000, -O2 读零);
 * ③ 编译期常量地址的标量立即解引用与 kernel 参数传递的指针均正常。
 * 故本头地址类 getter 一律返回标量 uint64_t 设备地址: 调用方应以常量折叠加立即解引用使用,
 * 运行时地址须经 host 转为 kernel 参数传递, 勿在 kernel 内做整数→指针 cast。 */
SMEM_SHM_INLINE_AICORE uint64_t smem_shm_get_qp_info_address(uint32_t shmemId)
{
    if (shmemId >= SMEM_OBJECT_NUM_MAX) {
        return 0;
    }

    uint64_t metaAddr =
        SMEM_SHM_DEVICE_META_ADDR + SMEM_SHM_DEVICE_GLOBAL_META_SIZE + shmemId * SMEM_SHM_DEVICE_PRE_META_SIZE;
    return *(__gm__ uint64_t *)(metaAddr + SMEM_SHM_DEVICE_META_QP_INFO_OFFSET);
}

SMEM_SHM_INLINE_AICORE uint64_t smem_shm_get_sdma_workspace_address(uint32_t shmemId)
{
    if (shmemId >= SMEM_OBJECT_NUM_MAX) {
        return 0;
    }

    uint64_t metaAddr =
        SMEM_SHM_DEVICE_META_ADDR + SMEM_SHM_DEVICE_GLOBAL_META_SIZE + shmemId * SMEM_SHM_DEVICE_PRE_META_SIZE;
    return *(__gm__ uint64_t *)(metaAddr + SMEM_SHM_DEVICE_META_SDMA_WORKSPACE_OFFSET);
}

/* V5: 段物理大小(= 建池 localSize; 邮箱保留区定位用, host 建池时写入) */
SMEM_SHM_INLINE_AICORE uint64_t smem_shm_get_phys_size(uint32_t shmemId)
{
    if (shmemId >= SMEM_OBJECT_NUM_MAX) {
        return 0;
    }

    uint64_t metaAddr =
        SMEM_SHM_DEVICE_META_ADDR + SMEM_SHM_DEVICE_GLOBAL_META_SIZE + shmemId * SMEM_SHM_DEVICE_PRE_META_SIZE;
    return *(__gm__ uint64_t *)(metaAddr + SMEM_SHM_DEVICE_META_PHYS_SIZE_OFFSET);
}

SMEM_SHM_INLINE_AICORE uint64_t smem_shm_get_extra_context_addr(uint32_t shmemId)
{
    if (shmemId >= SMEM_OBJECT_NUM_MAX) {
        return 0;
    }
    uint64_t ctxAddr = SMEM_SHM_DEVICE_USER_CONTEXT_ADDR + shmemId * SMEM_SHM_DEVICE_USER_CONTEXT_PRE_SIZE;
    return ctxAddr;
}

SMEM_SHM_INLINE_AICORE uint32_t smem_shm_get_extra_context_size(uint32_t shmemId)
{
    if (shmemId >= SMEM_OBJECT_NUM_MAX) {
        return 0;
    }
    uint64_t metaAddr =
        SMEM_SHM_DEVICE_META_ADDR + SMEM_SHM_DEVICE_GLOBAL_META_SIZE + shmemId * SMEM_SHM_DEVICE_PRE_META_SIZE;
    return (*(__gm__ uint32_t *)(metaAddr + SMEM_SHM_DEVICE_META_CONTEXT_OFFSET));
}

#endif // __MEMFABRIC_SMEM_AI_CORE_BASE_META_H__
