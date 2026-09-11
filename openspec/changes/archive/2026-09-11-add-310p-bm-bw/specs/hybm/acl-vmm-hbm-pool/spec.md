## ADDED Requirements

### Requirement: aclrtMemAttr 枚举与 CANN 严格对齐

`dl_acl_api.h` 中本地声明的 `aclrtMemAttr` 枚举 SHALL 与构建目标 CANN 版本（9.1.0）`acl/acl_rt.h` 的编号严格一致（全量 18 项，0~17）。枚举值经 `aclrtMallocPhysical` 的 `aclrtPhysicalMemProp.memAttr` 直传驱动，本地自编号 SHALL 被禁止（错位会导致语义漂移：名为 P2P_HUGE 实发 HBM_HUGE）。枚举定义处 SHALL 以注释标明错位风险与实测教训。

#### Scenario: 枚举值直传驱动语义正确

- **WHEN** 以 `ACL_MEM_P2P_HUGE`（值 16）构造 memProp 调用 `AclrtMallocPhysical`
- **THEN** 驱动收到值 16，从 P2P 分区分配；池 ≤4GB 时分配成功，>4GB（分区上限 ~4088MB）返回 207001

#### Scenario: 非 310P 环境零影响

- **WHEN** 在 910B/910C/950 环境加载 libascendcl.so
- **THEN** 枚举仅为本头文件类型声明，不影响宿主 CANN 行为

### Requirement: 物理分配 P2P_HUGE 到 HBM_HUGE 回退

HBM 池物理分配（`MallocPhysicalWithFallback`）SHALL 先以 `ACL_MEM_P2P_HUGE` 申请；当且仅当返回码精确等于 207001（RT_ERROR_DRV_OUT_MEMORY，P2P 分区 OOM）时，SHALL 记录含 rankId、size、ret 的 WARN 日志并以 `ACL_HBM_MEM_HUGE`（AI 主池，~42GB）重试一次。其他错误码（如 100000 参数错、207000）SHALL 不回退、直接失败并在根因处记录 ERROR。回退按单次分配调用（每 slice）判定，`smem_bm` 建池为一次性整池分配，一个池的物理来源 SHALL 全 P2P 或全 HBM_HUGE，不混合。

#### Scenario: 大池触发回退

- **WHEN** 2-rank 各建 8GB/32GB HBM 池（超过 P2P 分区 ~4088MB 上限）
- **THEN** 双 rank 各打印一条 P2P partition exhausted WARN 后 HBM_HUGE 整池分配成功，建池/join/十方向拷贝校验全部通过

#### Scenario: 非分区 OOM 错误不回退

- **WHEN** `AclrtMallocPhysical(P2P_HUGE)` 返回 207001 以外的错误码
- **THEN** 不触发回退，错误逐层上抛（hybm_acl_vmm_segment 根因处已记录 ERROR）

#### Scenario: HBM_HUGE 也失败时最终失败

- **WHEN** 回退后 `AclrtMallocPhysical(HBM_HUGE)` 仍失败（如 AI 主池余量不足）
- **THEN** `MallocPhysicalWithFallback` 记录含 rankId/size/ret 的 ERROR 并返回失败，建池失败链路各层均记录日志
