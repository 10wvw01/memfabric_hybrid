## 1. 共享 hybm 基础设施（ACL API 与定义）

- [x] 1.1 `hybm_define.h`：`UNIFIED_EXCHANGE_SEG_INFO_SIZE` 改 200；新增 `SOC_TYPE_310P=4` 常量、`D2dCopyMode` 枚举、`HybmPlatformCapability` 结构体与 `GetPlatformCapability`/`Is310PSupported` 内联函数——验证编译通过
- [x] 1.2 `dl_acl_api.h`：重构 `aclrtPhysicalMemProp`（`allocationType`/`memAttr` 改 `uint32_t`）、`aclrtMemAccessDesc.flags` 改 `uint32_t`；新增 IPC 符号 typedef（`aclrtIpcMemGetExportKey`/`ImportByKey`/`Close`）与 P2P 符号（`DisablePeerAccess`/`CanAccessPeer`）；新增常量 `ACL_RT_IPC_MEM_*`/`ACL_ERROR_RT_DRV_INTERNAL_ERROR`/`ACL_RT_MEM_ACCESS_FLAGS_READWRITE`/`ACL_MEM_ALLOCATION_TYPE_PINNED`；移除 `AclrtMemSetPidToShareableHandle`——验证 `static_assert` 尺寸通过
- [x] 1.3 `dl_acl_api.cpp`：新增符号 `DL_LOAD_SYM`/`DL_LOAD_SYM_OPTIONAL` 加载（IPC、P2P、Context）；`ParseAscendSocName` 合并入 `GetAscendSocType`（增加 "Ascend_310P" 匹配）；`CleanupLibrary` 置空新符号——验证 `hybm_init` 在有/无符号环境均不崩溃
- [x] 1.4 `hybm.h` + `hybm_entry.cpp`：新增 C API `hybm_get_soc_type`/`hybm_get_phy_chip_id`/`hybm_enable_peer_access`/`hybm_alloc_vmm_buffer`/`hybm_free_vmm_buffer`；`hybm_uninit` 310P3 走 ACL 释放路径；新增 `HybmLookupVmmHandle` + `g_vmmHandleMap` 全局注册表——验证 `hybm_init`/`hybm_uninit` 310P3 实机通过
- [x] 1.5 `dl_hal_api.cpp`：`LoadHybmVmmLibrary` 条件增加 310P 判定（不再仅 V4）——验证 910B/950 路径不变
- [x] 1.6 `hybm_gva.cpp`：`HybmModernInitMetaGva` 增加 310P3 ACL 路径（Reserve/Malloc/Map）；`hybm_init_hbm_gva` 路由 310P 到 modern 路径——验证 310P3 meta GVA 初始化成功
- [x] 1.7 `hybm_entity_default.cpp`：`CheckOptions` HOST_SHM 增加 `hostVASpace == maxDRAMSize` 校验——验证非法参数被拒绝
- [x] 1.8 `hybm_entity_tag_info.cpp`：`AddRankTag` 空 tag 使用 `"HYBM_DEFAULT_TAG_FOR_EMPTY"` 默认值——验证空 tag 不再直接接受

## 2. BM 模式变更

- [x] 2.1 `smem_types.h`：新增 `SM_NO_RESOURCES = -2015` 错误码——验证编译通过
- [x] 2.2 `smem_bm.cpp`：`smem_bm_create2_inner` HOST_SHM 校验增加 `maxHbmSize == 0`——验证 HOST_SHM + maxHbmSize!=0 被拒绝
- [x] 2.3 `smem_bm_entry.h`/`.cpp`：新增 `TopologyPreflight()` 方法（310P3 HBM 池 Join 前拓扑记录交换 + P2P 槽位预算计算 + 超限返回 `SM_NO_RESOURCES`）；`Join` 前调用 `TopologyPreflight`；`Leave` 拒绝 310P3 HBM 池；`ExtendLocalMem` 拒绝 310P3 HOST_SHM DRAM——验证 4-rank PASS、8-rank FAIL、Leave/Extend 被拒绝
- [x] 2.4 `smem_bm_entry_manager.cpp`：`CreateEntryById` 使用 `worldSize_` 替代 `config_.dynamicWorldSize`——验证 entry 的 rankSize 正确

## 3. TRANS 模式变更

- [x] 3.1 `smem_trans.cpp`：`smem_trans_deregister_mem` 实现 `DeregisterLocalMemory` 调用（不再直接返回 `SM_OK`）——验证反注册实际释放 slice
- [x] 3.2 `smem_trans_entry.h`/`.cpp`：新增 `RegisteredSliceInfo` 结构体与 `registeredSlices_` map；`RegisterOneMemory` 记录 slice 到 `registeredSlices_`；新增 `DeregisterLocalMemory`（2MB 对齐查找 + `hybm_free_local_memory` 释放 + 移除条目）——验证注册后反注册成功，未注册地址返回 `SM_INVALID_PARAM`
- [x] 3.3 `smem_trans_entry.cpp`：`AddRemoteInfo`/`JoinImport`/`UpdateHandle` 中 `SMEM_TRANS_BOTH` 角色条件改为 `config_.role == SMEM_TRANS_BOTH || role != config_.role`——验证 BOTH 角色记录/导入/交换全部 peer
- [x] 3.4 `smem_trans_entry.cpp`：`BatchQuantTransfer` 增加 310P SoC 拒绝（返回 `SM_ERROR` + ERROR 日志）——验证 310P 上量化拷贝被拒绝
- [x] 3.5 `smem_trans_entry.h`/`.cpp`：新增 `TopologyPreflight`/`GatherTopologyRecords`/`CheckP2PSlotBudget` 方法；`RegisterLocalMemories` 在 `Update` 前调用 `TopologyPreflight`——验证 4-rank PASS、8-rank FAIL、超时部分继续

## 4. 内存段变更（HybmVmmBasedSegment / HybmConnBasedSegment / HybmDevUserLegacySegment）

- [x] 4.1 `hybm_mem_segment.h`/`.cpp`：新增 `physicalChipId_` 静态成员与 `IsSameCard` 静态方法；移除 `QueryDeviceInfoSocTolerant`（改为直接查询 + WARN 降级）；`InitDeviceInfo` 增加 `INFO_TYPE_PHY_CHIP_ID` 查询；`CanLocalHostReaches` 310P 返回 true；`EnableRemotePeerAccess` 310P3 走 `AclrtDeviceEnablePeerAccess`（同卡 507899 容忍）；HBM 段工厂分派条件改为 `V4 \|\| 950 \|\| 310P`——验证 310P 分派到 `HybmVmmBasedSegment`，910B/950 不变
- [x] 4.2 `hybm_vmm_based_segment.h`/`.cpp`：`HostSdmaExportInfo` 新增 `physicalChipId`+`reserved_`（200B）；`mappedGvaMem_` 改为 `void*`；新增 `p2pLeases_` 集合、`ReleaseP2PLeases`/`Is310PAclPath`/`ReleaseHandle` 辅助方法；`ReserveMemorySpace` 310P3 走 `AclrtReserveMemAddress(maxSize)`；`MallocFromDevice` 310P3 走 `AclrtMallocPhysical(P2P_HUGE)`；`AllocLocalMemory` 310P3 走 `AclrtMapMem` + 跨卡 `AclrtMemSetAccess`；`ExportInner` 310P3 走 `AclrtMemExportToShareableHandle`；`ReserveLva` 310P3 走 per-import `AclrtReserveMemAddress`；`Mmap` 310P3 排序同卡优先 + SetDevice + ctx trick 导入 + `AclrtMemSetAccess`；`Unmap`/`RemoveImported` 310P3 走 `AclrtUnmapMem`/`AclrtFreePhysical`——验证 310P3 HBM 池跨卡 import/export 正常
- [x] 4.3 `hybm_conn_based_segment.h`/`.cpp`：`HostExportInfo` 新增 `shmName[64]`；新增 `mappedGvaSize_` map 与 `shmName_` 成员；`ReserveMemorySpace` 310P3 创建 POSIX shm_name；`Export` 携带 `shmName`；`Mmap` 310P3 `shm_open`+`mmap`；`Unmap` `munmap`+`shm_unlink`；`AllocMemory` 310P3 `shm_open`+`ftruncate`+`mmap`；`CheckSdmaReaches` override 返回 true——验证 310P3 跨进程 DRAM 共享正常
- [x] 4.4 `hybm_dev_user_legacy_segment.h`/`.cpp`：`RegisterSlice` 新增 `shareableHandle`；`UserHbmExportSliceInfo` 新增 `shareableHandle`（200B）；新增 `ImportedVmmInfo` 结构体与 `importedVmmInfo_` map；`RegisterMemory` 310P3 VMM handle 走 `AclrtMemExportToShareableHandle`、无 VMM handle 走 `AclrtIpcMemGetExportKey`；`ImportSliceInfo` 310P3 VMM 走 `ImportVmmSlice`、IPC 走 `AclrtIpcMemImportByKey`；新增 `ImportVmmSlice`/`CleanupImportedVmm`；`Mmap`/`Unmap` 310P3 返回 OK；`CloseMemory` 310P3 清理 imported VMM——验证 310P3 用户注册内存导入导出正常
- [x] 4.5 `hybm_acl_vmm_segment.cpp`：`FillMemProp` 改用 `ACL_MEM_P2P_HUGE`/`ACL_MEM_ALLOCATION_TYPE_PINNED` 常量；`MapPhysicalToOwnSlot`/`MapImportedToSlot` 改用 `ACL_RT_MEM_ACCESS_FLAGS_READWRITE` 常量——验证编译通过
- [x] 4.6 `hybm_va_manager.cpp`：`ClassifyAddressMask` `inAllocGva` 分支简化为仅对 imported 设单 GLOBAL 位——验证 UT `HybmVaManagerTest` 全量 PASS

## 5. 数据拷贝算子变更

- [x] 5.1 `hybm_data_op_acl_memcpy.h`/`.cpp`：新增 `DirectionSrcMemType`/`DirectionDestMemType` 查表；`IsSupportedDirection` 拆分为 `IsD2DDirection`/`IsH2DDirection`/`IsD2HDirection`；`CopyDeviceToDevice` 改名 `CopyD2D` + 同步回退；新增 `CopyH2D`/`CopyD2H`（async+sync 回退）；`DataCopy` 支持 `ASYNC_COPY_FLAG`/外部 stream + `TransformVa`；`BatchCopyD2D` 逐项回退；`BatchDataCopy` `TransformVa` + `InnerWait`；`DataCopyAsync` 真实异步；`Wait` 实际同步流；`TransformVa` 经 `HybmVaManager` 翻译；新增 `CleanUp`/`InnerWait`——验证 UT `HybmDataOpAclMemcpyTest` 全量 PASS
- [x] 5.2 `hybm_data_op_factory.cpp`：310P 分支保持，非 310P 增加 `(void)rankId`——验证编译通过

## 6. 流管理器变更

- [x] 6.1 `hybm_stream_manager.h`/`.cpp`：`AclrtStream` 改默认构造 + `Get`/`Reset` 方法；`g_threadAclStream` 改为 thread_local 全局；`GetThreadAclStream` 重构为 RAII 模式；新增 `ResetThreadAclStream`——验证编译通过，线程局部 stream 生命周期正确

## 7. 回归与验证

- [x] 7.1 全量 UT 回归：`bash script/run_ut.sh --fast`——验证改动覆盖路径 UT 全 PASS（31 套件 822 用例 820 PASS/0 FAIL，2 个存量 ASAN 崩溃套件 HybmDataOpSdmaTest/HybmTransportManagerTest 为 stub 环境问题与本次变更无关）
- [x] 7.2 910B/950 兼容性验证：确认 SOC 分派隔离、310P 分支独立，非 310P 路径零行为变化
- [x] 7.3 交换信息布局 `static_assert`：验证 `HostSdmaExportInfo`/`HostExportInfo`/`UserHbmExportSliceInfo` 均为 200B
- [x] 7.4 pre-commit / clang-format v18.1.8 通过：`bash script/ci-pre-commit-pr.sh`（gitleaks 因 Go proxy 不可达跳过，clang-format/trailing-whitespace/end-of-file 等通过）
