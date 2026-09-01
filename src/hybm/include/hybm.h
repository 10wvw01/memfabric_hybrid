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
#ifndef MEM_FABRIC_HYBRID_HYBM_H
#define MEM_FABRIC_HYBRID_HYBM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize hybrid big memory library
 *
 * @param deviceId         [in] npu device id
 * @param flags            [in] optional flags
 * @return 0 if successful
 */
int32_t hybm_init(uint16_t deviceId, uint64_t flags);

/**
 * @brief UnInitialize hybrid big memory library
 */
void hybm_uninit(void);

/**
 * @brief Set external log function, if not set, log message will be instdout
 *
 * @param logger           [in] logger function
 * @return 0 if successful
 */
void hybm_set_extern_logger(void (*logger)(int level, const char *msg));

/**
 * @brief Set alarm log function, user can set customized logger function
 *
 * @param alarm             [in] alarm logger function
 * @param resume            [in] resume logger function
 */
void hybm_set_extern_alarm(void (*alarm)(uint16_t code, const char *msg), void (*resume)(uint16_t code));

/**
 * @brief Set log print level
 *
 * @param level           [in] log level, 0:debug 1:info 2:warn 3:error
 * @return 0 if successful
 */
int32_t hybm_set_log_level(int level);

/**
 * @brief Get error message by error code
 *
 * @param errCode          [in] error number returned by other functions
 * @return error string if the error code exists, null if the error is invalid
 */
const char *hybm_get_error_string(int32_t errCode);

/**
 * @brief Get Ascend SoC type
 *
 * @return SoC type value (0=unknown, 1=910B, 2=910C, 3=950, 4=310P1, 5=310P3)
 */
int32_t hybm_get_soc_type(void);

/**
 * @brief Get device physical chip id
 *
 * @param deviceId        [in] device id
 * @return chip id >= 0 on success, < 0 on failure
 */
int32_t hybm_get_phy_chip_id(uint32_t deviceId);

/**
 * @brief Enable P2P access to a peer device (bidirectional)
 *
 * @param localDeviceId   [in] local device id
 * @param peerDeviceId    [in] peer device id
 * @return 0 if successful
 */
int32_t hybm_enable_peer_access(uint32_t localDeviceId, uint32_t peerDeviceId);

#ifdef __cplusplus
}
#endif

#endif // MEM_FABRIC_HYBRID_HYBM_H
