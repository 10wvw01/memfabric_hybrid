## Context

前序变更（`add-310p-2rank-vmm-pool`，已归档）建立了 310P 单卡双 die 2-rank HBM 池能力，使用独立 `HybmAclVmmSegment` 走 ACL VMM 路径。当前待提交变更将该能力扩展到多卡多 rank 场景，并补齐 TRANS 模式缺失能力。

关键约束与实测事实（来自代码与注释）：

- 310P3 Duo 单卡双 die，`deviceId/2` 为卡索引（dev0/dev1→card0，dev2/dev3→card1）
- HAL 约束：每卡 8 个 MAX enable-number（入站 P2P 授权槽位），全 mesh N-rank 每卡入站 = 2×(N-2)，N=6 时饱和（8/8），N=8 超限（12>8）
- ACL `AclrtMemImportFromShareableHandle` 在组监听线程上需显式 `AclrtSetDevice`（device context 线程局部）
- 同卡跨 die `AclrtDeviceEnablePeerAccess` 返回 507899（`ACL_ERROR_RT_DRV_INTERNAL_ERROR`），内核拒绝同卡 P2P（共享 PCIe），安全忽略
- `ACL_MEM_P2P_HUGE` 分区上限 ~4088MB，前序变更有 P2P_HUGE→HBM_HUGE 回退；本变更迁移到 `HybmVmmBasedSegment` 后未携带该回退
- `aclrtPhysicalMemProp` 的 `allocationType`/`memAttr` 字段在 CANN acl_rt.h 中为 `uint32_t`（非枚举类），前序变更为枚举类曾导致语义漂移风险

## Goals / Non-Goals

**Goals:**

- BM 与 TRANS 两种模式下 310P3 多卡多 rank 拓扑前置检查（P2P 槽位预算），超限提前失败
- 310P HBM 段统一到 `HybmVmmBasedSegment`（与 910C/950 共享段类型，通过 `Is310PAclPath()` 分派 ACL/HAL）
- 跨卡 P2P 导入导出（`AclrtMemSetAccess` 多设备、`AclrtMemExportToShareableHandle`、ctx trick 导入）
- TRANS 内存反注册实际实现（`smem_trans_deregister_mem` 不再为空）
- TRANS `SMEM_TRANS_BOTH` 角色全 peer 语义
- 拷贝算子真实异步 + `TransformVa` 真实翻译 + 同步回退安全网
- 910B/910C/950 零行为变化

**Non-Goals:**

- P2P_HUGE→HBM_HUGE 回退迁移到 `HybmVmmBasedSegment`（大池场景需应用层保证 ≤ P2P 分区上限，或 V2 补充）
- PID 白名单安全语义恢复（V2）
- 超过 6 rank 的 HBM 池支持（P2P 槽位硬限制）
- `HybmAclVmmSegment` 代码删除（保留文件，仅分派不再使用）

## Decisions

| # | 决策 | 依据与备选 |
|---|------|-----------|
| D1 | 310P HBM 段从 `HybmAclVmmSegment` 迁移到 `HybmVmmBasedSegment` | `HybmVmmBasedSegment` 已有 910C/950 的 HAL 路径，通过 `Is310PAclPath()` 增加 ACL 分支即可共享段生命周期管理；备选继续扩展 `HybmAclVmmSegment`——被否，两段类型导致工厂分派与 compose 路径分裂，维护成本高 |
| D2 | 拓扑前置检查在 BM `Join` 和 TRANS `RegisterLocalMemories` 中分别实现 | BM 和 TRANS 有不同的入口时序与 store 前缀；共享算法（P2P 槽位计算）内联在各自方法中，不抽取公共类——避免跨模块依赖 |
| D3 | 拓扑记录格式：BM 为 `rankId:devId:phyChipId`，TRANS 为 `rankId:devId` | TRANS 不需要 phyChipId（用 devId/2 判同卡）；BM 额外记录 phyChipId 供日志诊断 |
| D4 | 同卡导入用 SetDevice ctx trick（3 步），不再用 CreateContext 五步 | p3_store_repro 实测验证 SetDevice(force=true) 足够；CreateContext 五步复杂且有 context 泄漏风险；备选保持五步——被否，过度复杂 |
| D5 | 导入列表排序同卡优先 | ACL runtime P2P 状态管理要求同卡导入先于跨卡；`std::stable_sort` 按 `IsSameCard` + `rankId` 排序 |
| D6 | Mmap 线程强制 `AclrtSetDevice(local, force=true)` | 组监听线程（grp_listen_evt）无主线程 device context，不 SetDevice 则跨卡 import 返回 507899 |
| D7 | `aclrtPhysicalMemProp` 字段改 `uint32_t` | CANN acl_rt.h 中该结构体字段为 `uint32_t`，枚举类导致 `static_cast` 隐式转换风险；改后类型严格对齐 |
| D8 | `UNIFIED_EXCHANGE_SEG_INFO_SIZE` 192→200 | 新增 `physicalChipId`(4B) + `reserved_`(4B) = 8B；所有交换信息结构体经 `static_assert` 保证等长 |
| D9 | `ClassifyAddressMask` 简化：`inAllocGva` 分支仅对 imported 设单 GLOBAL | 前序变更设 LOCAL|GLOBAL 双位是为 910B 兼容，但 `reservedMap_` 路径已覆盖本地分配地址分类；双位设置与 `reservedMap_` 语义重叠，简化后行为等价 |
| D10 | P2P 生命周期 `p2pLeases_` 集合 | 跨卡 import 时 enable 并记入集合，Unmap/RemoveImported 时按引用计数 disable；避免重复 enable 与泄漏 |
| D11 | DRAM 池 310P3 走 POSIX `shm_open` | 310P3 无 `shmFd` 共享 fd 机制（无 HAL `HalMemAddressReserve`），改用 POSIX `shm_open` 跨进程共享 DRAM；名称格式 `/mf_dram_<pid>_<rankId>` |
| D12 | `HostDataOpAclMemcpy` 真实异步 + 同步回退 | 前序变更全方向等效同步（V1 简化）；本变更兑现外部 stream/ASYNC_COPY_FLAG 契约，async 失败时回退同步 `AclrtMemcpy` 作为安全网 |
| D13 | `TransformVa` 从恒等改为 `HybmVaManager::TransformVa` | `HybmVmmBasedSegment` 不保证 GVA==DVA==HVA（每 rank 独立 ReserveMemAddress），需经 VaManager 翻译；前序 `HybmAclVmmSegment` 整窗对称 GVA==DVA 故恒等 |
| D14 | `HybmStreamManager` thread_local 重构 | 原实现 `static thread_local void*` + `static thread_local AclrtStream` 双重静态局部变量有初始化顺序问题；改为全局 `thread_local AclrtStream g_threadAclStream` + `ResetThreadAclStream` 显式管理 |
| D15 | `hybm_entity_tag_info` 空 tag 用默认值 | 空 tag 直接接受会导致 store 中空字符串 key 冲突；用 `"HYBM_DEFAULT_TAG_FOR_EMPTY"` 默认值替代 |

## Risks / Trade-offs

- [移除 P2P_HUGE→HBM_HUGE 回退] → 大池（>4088MB）场景 `AclrtMallocPhysical(P2P_HUGE)` 会直接失败 207001；缓解：应用层限制池大小 ≤ P2P 分区上限，V2 补充回退到 `HybmVmmBasedSegment`
- [交换信息布局 192→200B 不兼容] → 旧版本进程与新版本进程交换信息时 `static_assert` 尺寸不匹配会编译失败，运行时不会跨版本混用；缓解：全集群同步升级
- [Mmap 线程 SetDevice 线程局部性] → 组监听线程 `AclrtSetDevice` 仅影响该线程，主线程 device context 不受影响；风险：如果其他回调也在该线程执行 ACL 操作需注意 device 状态
- [TRANS 拓扑超时 5 秒后部分继续] → 慢 rank 可能导致槽位预算计算不准确（缺少部分 rank 的跨卡连接计数）；缓解：实际槽位需求只会偏低不会偏高，超限 rank 仍会在 import 时失败
- [同卡 507899 容忍] → 如果跨卡也返回 507899（非同卡场景），系统会报错失败；但如果 `deviceId/2` 判定错误（如非标准拓扑），可能误将跨卡判定为同卡而忽略真实 P2P 失败；缓解：日志记录 sameCard 判定结果便于诊断
- [`ClassifyAddressMask` 简化影响 910B] → `inAllocGva` 分支不再设 LOCAL 位，本地分配地址的分类依赖 `reservedMap_` 路径；需全量 UT 回归验证 910B 地址分类行为等价
- [TRANS `DeregisterLocalMemory` 地址对齐] → 2MB 页对齐后查找 `registeredSlices_`，如果注册时传入未对齐地址，反注册可能查不到；缓解：`RegisterOneMemory` 已记录对齐后地址到 `registeredSlices_`

## Migration Plan

本变更为待提交代码的规格化（代码已实现），无增量部署。回滚即 revert 对应未提交改动；910B/950 路径不受影响。交换信息布局变更要求全集群同步升级（192B→200B 不兼容）。

## Open Questions

无——实现已完成，残余改进项（P2P_HUGE 回退、PID 白名单）已登记为 V2 TODO。
