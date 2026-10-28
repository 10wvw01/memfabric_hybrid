## Purpose

定义 TRANS 模式（`smem_trans`）在 310P3 设备上的行为契约：拓扑前置检查与 P2P 槽位预算、内存反注册、`SMEM_TRANS_BOTH` 角色全 peer 语义、310P 量化拷贝拒绝。

## ADDED Requirements

### Requirement: TRANS 拓扑前置检查

310P3 TRANS 模式在 `RegisterLocalMemories` 完成本地内存注册后、`Update` 之前 SHALL 执行拓扑前置检查。各 rank 通过 store 发布拓扑记录（rankId:devId），等待全部 rank 记录就绪（超时 5 秒，超时后以可用记录继续）。计算每卡跨卡 P2P 入站槽位需求（每卡 devId/2 为卡索引，跨卡连接数 = 2×(N-2)），超过 8 上限时 SHALL 返回 `SM_NO_RESOURCES` 并记录含 rankId、worldSize、cardId、required、limit 的 ERROR 日志，清空 `registedInfo_` 不执行 `Update`。rankSize ≤ 1 时 SHALL 跳过检查。非 310P3 设备或非 SDMA 位 opType SHALL 跳过检查直接返回成功。超时后可用记录 < 2 时 SHALL 返回 `SM_ERROR`。

#### Scenario: 4-rank TRANS 拓扑预算通过

- **WHEN** 4-rank（2 卡 × 2 die）310P3 TRANS 注册内存
- **THEN** 每卡跨卡入站槽位需求 = 2×(4-2) = 4 ≤ 8，前置检查 PASS，Update 继续

#### Scenario: 8-rank TRANS 拓扑预算超限

- **WHEN** 8-rank（4 卡 × 2 die）310P3 TRANS 注册内存
- **THEN** 每卡跨卡入站槽位需求 = 2×(8-2) = 12 > 8，前置检查返回 `SM_NO_RESOURCES`，`registedInfo_` 清空，不执行 Update

#### Scenario: 非 310P3 跳过检查

- **WHEN** 910B/950 设备 TRANS 注册内存
- **THEN** 不执行拓扑前置检查，行为不变

#### Scenario: 拓扑记录部分超时继续

- **WHEN** 5 秒内仅收到部分 rank 拓扑记录（≥2 但 < rankSize）
- **THEN** 记录 WARN 日志，以可用记录继续计算槽位预算

### Requirement: 内存反注册

`smem_trans_deregister_mem` SHALL 实现实际的内存反注册逻辑（不再为空实现直接返回 `SM_OK`）。反注册时 SHALL 将传入地址按 2MB 页对齐（`NPU_PAGE_SIZE = 2MB`，`NPU_PAGE_MASK` 向下对齐），在 `registeredSlices_` 中查找对齐后地址对应的 slice 信息。找到时 SHALL 调用 `hybm_free_local_memory` 释放 slice 并从 `registeredSlices_` 中移除，记录含 address、size、rank 的 INFO 日志。未找到对应地址时 SHALL 返回 `SM_INVALID_PARAM` 并记录含 address、alignedAddr 的 ERROR 日志。`hybm_free_local_memory` 失败时 SHALL 返回 `SM_ERROR` 并记录含 address、size、ret 的 ERROR 日志。handle 无效或 entry 未找到时 SHALL 返回对应错误码并记录 ERROR 日志。

#### Scenario: 正常反注册

- **WHEN** 用户对已注册的内存地址调用 `smem_trans_deregister_mem`
- **THEN** 地址按 2MB 对齐后查找 slice，`hybm_free_local_memory` 释放成功，`registeredSlices_` 移除条目，返回 `SM_OK`

#### Scenario: 地址未注册

- **WHEN** 用户对未注册的地址调用 `smem_trans_deregister_mem`
- **THEN** 在 `registeredSlices_` 中未找到，返回 `SM_INVALID_PARAM`，记录 ERROR 日志

#### Scenario: handle 无效

- **WHEN** 用户传入 null handle 或无法找到对应 entry
- **THEN** 返回错误码，记录含 handle、result 的 ERROR 日志

### Requirement: SMEM_TRANS_BOTH 角色全 peer 语义

当 `config_.role == SMEM_TRANS_BOTH` 时，`AddRemoteInfo` SHALL 记录所有 peer rank 的信息（不再跳过同角色 peer）；`JoinImport` SHALL 对所有 peer rank 执行 `hybm_import`（entity 级与 slice 级）；`UpdateHandle` SHALL 对所有 peer rank 交换地址信息。非 BOTH 角色（`SENDER`/`RECEIVER`）SHALL 保持原有行为：仅对角色不同的 peer 执行记录/导入/交换。

#### Scenario: BOTH 角色记录全部 peer

- **WHEN** `SMEM_TRANS_BOTH` 角色的 rank 收到所有 peer 的角色信息
- **THEN** 所有 peer 的 workerId 与 role 均被记录，不跳过任何 peer

#### Scenario: BOTH 角色导入全部 peer

- **WHEN** `SMEM_TRANS_BOTH` 角色的 rank 执行 `JoinImport`
- **THEN** 对所有 peer rank 执行 `hybm_import`（entity 级 `HYBM_FLAG_EXPORT_ENTITY` 与 slice 级）

#### Scenario: 非 BOTH 角色行为不变

- **WHEN** `SENDER` 角色的 rank 收到其他 `SENDER` 角色的 peer 信息
- **THEN** 跳过同角色 peer，行为与引入本能力之前一致

### Requirement: 310P 量化拷贝拒绝

`BatchQuantTransfer` 在 310P 设备上 SHALL 拒绝执行，返回 `SM_ERROR` 并记录含 socType 的 ERROR 日志（"quant write is not supported on 310P"）。非 310P 设备 SHALL 保持原有量化拷贝行为不变。

#### Scenario: 310P 拒绝量化拷贝

- **WHEN** 在 310P 设备上调用 `BatchQuantTransfer`
- **THEN** 返回 `SM_ERROR`，记录 ERROR 日志，不执行任何拷贝

#### Scenario: 非 310P 量化拷贝正常

- **WHEN** 在 910B/950 设备上调用 `BatchQuantTransfer`
- **THEN** 量化拷贝正常执行，行为不变
