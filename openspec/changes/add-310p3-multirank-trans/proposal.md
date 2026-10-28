## Why

310P3（Ascend 310P3，Atlas 300I Duo）此前仅支持 2-rank HBM 池（单卡双 die），且 TRANS 模式缺乏拓扑前置检查与内存反注册能力。随着多卡多 rank 场景需求出现，需要：(1) 在 BM 和 TRANS 两种模式下引入 P2P 槽位预算拓扑前置检查，防止超限导致建池/注册后才发现 P2P 不可用；(2) 将 310P HBM 段从 `HybmAclVmmSegment` 迁移到 `HybmVmmBasedSegment` 统一路径，支持跨卡 P2P 导入；(3) 补齐 TRANS 模式的 `SMEM_TRANS_BOTH` 角色支持、内存反注册、310P 量化拷贝拒绝等能力。

## What Changes

### BM 模式

- 新增 `SmemBmEntry::TopologyPreflight()`：310P3 HBM 池 Join 前通过 store 交换拓扑记录（rankId/devId/phyChipId），计算每卡跨卡 P2P 入站槽位需求，超过 8 上限返回 `SM_NO_RESOURCES`
- 新增 `SM_NO_RESOURCES`(-2015) 错误码
- 310P3 HBM 池拒绝 `Leave`（动态缩容不支持）与 `ExtendLocalMem`（HOST_SHM DRAM 池已满）
- `smem_bm_create2_inner` HOST_SHM 校验增加 `maxHbmSize == 0` 检查
- `SmemBmEntryManager::CreateEntryById` 使用 `worldSize_` 替代 `config_.dynamicWorldSize`
- **BREAKING**：310P HBM 段分派从 `HybmAclVmmSegment` 改为 `HybmVmmBasedSegment`，移除 `HybmAclVmmSegment` 的 `MallocPhysicalWithFallback`（P2P_HUGE→HBM_HUGE 回退），改为直接 `ACL_MEM_P2P_HUGE` 分配

### TRANS 模式

- 新增 `SmemTransEntry::TopologyPreflight()`：310P3 TRANS 注册内存前通过 store 交换拓扑记录，计算 P2P 槽位预算，超限返回 `SM_NO_RESOURCES`
- 新增 `SmemTransEntry::DeregisterLocalMemory()`：实现 `smem_trans_deregister_mem`（此前为空实现直接返回 SM_OK），通过 `hybm_free_local_memory` 释放 slice 并从 `registeredSlices_` 移除
- `SMEM_TRANS_BOTH` 角色修复：`AddRemoteInfo`/`JoinImport`/`UpdateHandle` 中 BOTH 角色现在记录/导入/交换所有 peer（此前跳过同角色 peer）
- 310P 拒绝 `BatchQuantTransfer`（量化拷贝不支持）

### 共享 hybm 基础设施（BM 和 TRANS 共同依赖）

- 新增 C API：`hybm_get_soc_type()`、`hybm_get_phy_chip_id()`、`hybm_enable_peer_access()`、`hybm_alloc_vmm_buffer()`、`hybm_free_vmm_buffer()`
- `hybm_uninit()` 310P3 走 ACL 路径（AclrtUnmapMem/AclrtFreePhysical/AclrtReleaseMemAddress）替代 HAL 路径
- `dl_acl_api` 重构：`aclrtPhysicalMemProp` 字段改为 `uint32_t`（消除枚举类隐式转换风险）；新增 `aclrtIpcMemGetExportKey`/`aclrtIpcMemImportByKey`/`aclrtIpcMemClose` IPC 符号；新增 `AclrtDeviceDisablePeerAccess`/`AclrtDeviceCanAccessPeer`；移除 `AclrtMemSetPidToShareableHandle`；`ParseAscendSocName` 合并入 `GetAscendSocType` 并增加 "Ascend_310P" 匹配
- `UNIFIED_EXCHANGE_SEG_INFO_SIZE` 从 192 增至 200（新增 `physicalChipId` 字段）
- `HybmVmmBasedSegment` 适配 310P3：ACL 路径 Reserve/Alloc/Map/Unmap/Release、跨卡 `AclrtMemSetAccess`、`AclrtMemExportToShareableHandle` 导出、同卡 ctx trick 导入、`p2pLeases_` P2P 生命周期管理
- `HybmConnBasedSegment` 适配 310P3 DRAM：POSIX `shm_open` 跨进程共享、`shmName` 导出、`munmap` 清理
- `HybmDevUserLegacySegment` 适配 310P3：VMM shareable handle 导入导出（`ImportVmmSlice`/`CleanupImportedVmm`）、IPC key 路径（`AclrtIpcMemGetExportKey`/`AclrtIpcMemImportByKey`）
- `MemSegment` 新增 `physicalChipId_`、`IsSameCard()`；移除 `QueryDeviceInfoSocTolerant`（改为直接查询 + warn 降级）；`EnableRemotePeerAccess` 310P3 走 `AclrtDeviceEnablePeerAccess`（同卡 507899 容忍）
- `HybmGetGvaVersion` / `hybm_init_hbm_gva` 310P3 走 ACL meta GVA 初始化路径
- `ClassifyAddressMask` 简化：imported 地址单 GLOBAL 位，本地分配不再设 LOCAL|GLOBAL 双位
- `HostDataOpAclMemcpy` 增强：真实异步支持（外部 stream / ASYNC_COPY_FLAG）、`TransformVa` 经 `HybmVaManager` 翻译 GVA→DVA/HVA、async 失败同步回退、`Wait` 实际同步流
- `HybmStreamManager` 重构：`g_threadAclStream` 改为 thread_local 全局 RAII、新增 `ResetThreadAclStream()`
- `hybm_entity_default` HOST_SHM 校验增加 `hostVASpace == maxDRAMSize` 检查
- `hybm_entity_tag_info` 空 tag 使用默认值替代直接接受

## Capabilities

### New Capabilities

- `smem/smem-trans`: TRANS 模式行为契约——拓扑前置检查、内存反注册、`SMEM_TRANS_BOTH` 角色全 peer 语义、310P 量化拷贝拒绝

### Modified Capabilities

- `hybm/acl-vmm-hbm-pool`: 310P HBM 段从 `HybmAclVmmSegment` 迁移到 `HybmVmmBasedSegment`；新增拓扑前置检查与 P2P 槽位预算；跨卡 P2P 导入导出；`physicalChipId` 同卡判定；P2P 生命周期管理；meta GVA ACL 路径；交换信息布局 192→200B；地址分类简化；设备信息查询降级改直接 fallback；`IsSameCard` 同卡判定；物理分配移除 P2P_HUGE→HBM_HUGE 回退
- `hybm/acl-memcpy-data-op`: 拷贝算子真实异步支持；`TransformVa` 经 `HybmVaManager` 翻译；async 失败同步回退；`Wait` 实际同步流；H2D/D2H 改异步+回退

## Impact

- **代码**：`src/smem/csrc/common/smem_types.h`、`src/smem/csrc/smem_bm/`（entry/entry_manager/bm）、`src/smem/csrc/smem_trans/`（trans/trans_entry）、`src/hybm/include/hybm.h`、`src/hybm/csrc/common/hybm_define.h`、`src/hybm/csrc/hybm_entry.cpp`、`src/hybm/csrc/under_api/dl_acl_api.{h,cpp}`、`src/hybm/csrc/under_api/dl_hal_api.cpp`、`src/hybm/csrc/driver/hybm_gva.cpp`、`src/hybm/csrc/entity/`（default/tag_info）、`src/hybm/csrc/mm/`（vmm_based_segment/conn_based_segment/dev_user_legacy_segment/mem_segment/acl_vmm_segment/va_manager）、`src/hybm/csrc/data_operation/host/`（acl_memcpy/factory）、`src/hybm/csrc/ts_engine/`（stream_manager）
- **兼容性**：910B/910C/950 行为零变化（SOC 分派隔离、310P 分支独立）；交换信息布局从 192B 变为 200B，旧版本与新版本不兼容（需同步升级）；`HostDataOpAclMemcpy` 的 `TransformVa` 从恒等改为真实翻译，影响 310P 池内拷贝地址处理
- **依赖**：libascendcl.so 导出 `aclrtIpcMemGetExportKey`/`aclrtIpcMemImportByKey`/`aclrtIpcMemClose`/`aclrtDeviceDisablePeerAccess`/`aclrtDeviceCanAccessPeer` 符号
- **已知限制**：310P3 HBM 池最大 6 rank（P2P 槽位 8 上限，2×(N-2)≤8）；DRAM HOST_SHM 池不支持 Extend；HBM 池不支持 Leave
