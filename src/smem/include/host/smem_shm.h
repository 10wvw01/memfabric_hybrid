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
#ifndef __MEMFABRIC_SMEM_SHM_H__
#define __MEMFABRIC_SMEM_SHM_H__

#include "smem.h"
#include "smem_shm_def.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize smem_shm_config_t, i.e. set to default value
 *
 * @param config           [in] config to be initialized
 * @return 0 if successful
 */
int32_t smem_shm_config_init(smem_shm_config_t *config);

/**
 * @brief Initialize shm library with global config store
 * all processes need to call this function before creating a shm object,
 * all processes will connect to a global config store with specified ipPort,
 * this function will finish when all processes connected or timeout;
 * the global config store will be used to exchange information about shm object and team
 *
 * @param configStoreIpPort[in] url of config store, e.g. tcp://ip:port, tcp://[ip]:port
 * @param worldSize        [in] size of processes
 * @param rankId           [in] local rank id in world size
 * @param deviceId         [in] device npu id
 * @param config           [in] config, see @smem_shm_config_t
 * @return 0 if successfully, negative value if failed, use @ref smem_get_last_error_msg to get last err msg
 */
int32_t smem_shm_init(const char *configStoreIpPort, uint32_t worldSize, uint32_t rankId, uint16_t deviceId,
                      smem_shm_config_t *config);

/**
 * @brief Un-initialize shm library with destroy all things
 *
 * @param flags            [in] optional flags, set to 0
 */
void smem_shm_uninit(uint32_t flags);

/**
 * @brief Query supported data operation type
 * @return the set of smem_shm_data_op_type
 */
uint32_t smem_shm_query_support_data_operation(void);

/**
 * @brief Create shm object peer by peer
 *
 * @param id               [in] id of the shm object
 * @param rankSize         [in] rank count
 * @param rankId           [in] my rank id
 * @param localSize        [in] local memory contributed to the shm object, all ranks must the same size
 * @param dataOpType       [in] data operation engine type, i.e. MTE, SDMA, RDMA etc
 * @param flags            [in] optional flags
 * @param gva              [out] global virtual address created, it can be passed to kernel to data operations
 * @return shm object created if successful, null if failed, use @ref smem_get_last_error_msg to get last error message
 */
smem_shm_t smem_shm_create(uint32_t id, uint32_t rankSize, uint32_t rankId, uint64_t localSize,
                           smem_shm_data_op_type dataOpType, uint32_t flags, void **gva);

/**
 * @brief Query symmetric_size, that address of the i-th rank is gva + symmetric_size * i
 * @param handle           [in] the shm object
 * @return symmetric_size
 */
uint64_t smem_shm_get_symmetric_size(smem_shm_t handle);

/**
 * @brief Destroy shm object
 *
 * @param handle           [in] the shm object to be destroyed
 * @param flags            [in] optional flags
 * @return 0 if successful
 */
int32_t smem_shm_destroy(smem_shm_t handle, uint32_t flags);

/**
 * @brief Set user extra context of shm object
 *
 * @param handle           [in] the shm object to be set
 * @param context          [in] extra context ptr
 * @param size             [in] extra context size (max is 64K)
 * @return 0 if successful
 */
int32_t smem_shm_set_extra_context(smem_shm_t handle, const void *context, uint32_t size);

/**
 * @brief Get local rank of a shm object
 *
 * @param handle           [in] the shm object
 * @return local rank in the input object, return UINT32_MAX if error
 */
uint32_t smem_shm_get_global_rank(smem_shm_t handle);

/**
 * @brief Get rank size of a shm object
 *
 * @param handle           [in] the shm object
 * @return rank size in the input object, return UINT32_MAX if error
 */
uint32_t smem_shm_get_global_rank_size(smem_shm_t handle);

/**
 * @brief Do barrier on a shm object, using control network
 *
 * @param handle           [in] the shm object
 * @return 0 if successful, other is error
 */
int32_t smem_shm_control_barrier(smem_shm_t handle);

/**
 * @brief Do all gather on a shm object, using control network
 *
 * @param handle           [in] the shm object
 * @param sendBuf          [in] input data buf
 * @param sendSize         [in] input data buf size
 * @param recvBuf          [in] output data buf
 * @param recvSize         [in] output data buf size
 * @return 0 if successful
 */
int32_t smem_shm_control_allgather(smem_shm_t handle, const char *sendBuf, uint32_t sendSize, char *recvBuf,
                                   uint32_t recvSize);

/**
 * @brief Do barrier operation with sub partition of world on a shm object using control network,
 * there is no need to setup sub group firstly, just need to make sure the key is following the rule:
 * a) key is a string
 * b) key should be the same for all participators (i.e. same in the sub group)
 * c) key should be different with other sub group, otherwise it will be messed up
 *
 * @param handle           [in] the shm object
 * @param key              [in] key name for this barrier, which should be same in sub group but unique in the world
 * @param rankSize         [in] the size of sub group
 * @param rankId           [in] rank id in sub group
 * @return 0 if successful, other is error
 */
int32_t smem_shm_subgroup_barrier(smem_shm_t handle, const char *key, uint32_t rankSize, uint32_t rankId);

/**
 * @brief Do all-gather operation with sub partition of world on a shm object using control network,
 * there is no need to setup sub group firstly, just need to make sure the key is following the rule:
 * a) key is a string
 * b) key should be the same for all participators (i.e. same in the sub group)
 * c) key should be different with other sub group, otherwise it will be messed up
 *
 * @param handle           [in] the shm object
 * @param key              [in] key name for this all-gather, which should be same in sub group but unique in the world
 * @param rankSize         [in] the size of sub group
 * @param rankId           [in] rank id in sub group
 * @param sendBuf          [in] input data buf
 * @param sendSize         [in] input data buf size
 * @param recvBuf          [in] output data buf
 * @param recvSize         [in] output data buf size
 * @return 0 if successful
 */
int32_t smem_shm_subgroup_allgather(smem_shm_t handle, const char *key, uint32_t rankSize, uint32_t rankId,
                                    const char *sendBuf, uint32_t sendSize, char *recvBuf, uint32_t recvSize);

/**
 * @brief Query if remote rank can ranch
 *
 * @param handle           [in] shm object
 * @param remoteRank       [in] remote rank
 * @param reachInfo        [out] reach info, the set of smem_shm_data_op_type
 * @return 0 if successful
 */
int32_t smem_shm_topology_can_reach(smem_shm_t handle, uint32_t remoteRank, uint32_t *reachInfo);

/**
 * @brief Allocate a global number in the shm object which begin from zero, more description:
 * 1) the number is unique within the scope of the shm object
 * 2) recorded in config store along with shm object
 * 3) starts from 0
 * 4) need to release after used
 *
 * @param handle           [in] shm object
 * @param limit            [in] the returned number must be less than 'limit' (limit <= SMEM_SHM_ATOMIC_NUM_LIMIT)
 * @param value            [out] allocated number
 * @return 0 if successful
 */
int32_t smem_shm_atomic_alloc_value(smem_shm_t handle, uint32_t limit, int32_t *value);

/**
 * @brief Release the global number which is allocated by <i>smem_shm_atomic_alloc_value</i>
 *
 * @param handle           [in] shm object
 * @param value            [in] the number
 * @return 0 if successful
 */
int32_t smem_shm_atomic_release_value(smem_shm_t handle, int32_t value);

/**
 * @brief Register function of exit
 *
 * @param exit             [in] global exit option, every rank will apply this function
 *                              to complete global exit
 * @param handle           [in] shm object
 * @return 0 if successful
 */
int32_t smem_shm_register_exit(smem_shm_t handle, void (*exit)(int));

/**
 * @brief Wait for all ranks exit
 *
 * @param handle           [in] shm object
 * @param status           [in] int
 */
void smem_shm_global_exit(smem_shm_t handle, int status);

/**
 * @brief [310P AICore×AICPU 协同模式] 提交一次跨端协同搬运
 *
 * 语义(八步闭环的发起侧, 详见 docs 与 310p 计划): 本端 AICore kernel 先产数据并写
 * 协同邮箱(设备侧头文件 smem_shm_aicore_overlap.h 的 smem_shm_overlap_notify),
 * 本接口触发的 AICPU 编排 kernel 轮询邮箱后提交 SQE 对(数据→dstGva +
 * 8B 标志→flagDoneGva), FIFO 保序保证"标志到即数据到"; 对端 AICore kernel 用
 * smem_shm_overlap_poll_flag 轮询标志后消费。
 *
 * 前置: smem_shm_create 时选用 SDMA 引擎, 环境变量 MF_310P_SHM_OVERLAP=1,
 * 且初始化冒烟通过(否则本接口返回错误, 上层以 smem_get_last_err_msg 排障)。
 *
 * @param handle           [in] shm object
 * @param dstGva           [in] 数据目的地址(对端池 GVA + 数据偏移)
 * @param flagDoneGva      [in] 标志目的地址(对端池 GVA 协同保留区, 见设备头文件槽位约定)
 * @return 0 if successful
 */
int32_t smem_shm_overlap_launch(smem_shm_t handle, void *dstGva, void *flagDoneGva);

/**
 * @brief [310P 协同模式] 等待最近一次协同搬运完成
 * @param handle           [in] shm object
 * @return 0 if successful
 */
int32_t smem_shm_overlap_wait(smem_shm_t handle);

/**
 * @brief [310P 协同模式] 读取最近一次搬运的 kernel 结果(诊断)
 * @param handle           [in] shm object
 * @param mainRet          [out] kernel 主返回码(0=成功)
 * @param stage            [out] 状态机阶段(5=完成)
 * @param headAfter        [out] SQ head(TSFW 消费证据)
 * @return 0 if successful
 */
int32_t smem_shm_overlap_get_result(smem_shm_t handle, uint32_t *mainRet, uint32_t *stage, uint32_t *headAfter);

/**
 * @brief [310P 协同模式] 查询协同工作区地址(host 侧获取后作为参数传给 AICore kernel)
 * @param handle           [in] shm object
 * @return 工作区设备地址; 协同模式未就绪时返回 NULL
 */
void *smem_shm_overlap_get_workspace(smem_shm_t handle);

#ifdef __cplusplus
}
#endif

#endif // __MEMFABRIC_SMEM_SHM_H__
