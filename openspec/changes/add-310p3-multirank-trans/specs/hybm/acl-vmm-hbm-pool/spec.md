## ADDED Requirements

### Requirement: 拓扑前置检查与 P2P 槽位预算

310P3 HBM 池 Join 前 SHALL 执行拓扑前置检查：各 rank 通过 store 发布拓扑记录（rankId:devId:phyChipId），超时等待全部 rank 记录就绪后计算每卡跨卡 P2P 入站槽位需求（每卡 devId/2 为卡索引，跨卡连接数 = 2×(N-2)，N 为 rank 总数）。当任一卡槽位需求超过 8（310P3 Duo HAL 约束）时，SHALL 返回 `SM_NO_RESOURCES` 并记录含 poolId、rankId、worldSize、cardId、required、limit 的 ERROR 日志，不执行任何导入。槽位预算可通过时 SHALL 返回成功并记录 PASS 日志。非 310P3 设备、非 SDMA 位 opType、`maxHBMSize == 0` 的池 SHALL 跳过前置检查直接返回成功。前置检查超时时间 SHALL 取 `controlOperationTimeout`。

#### Scenario: 4-rank 拓扑预算通过

- **WHEN** 4-rank（2 卡 × 2 die）310P3 HBM 池 Join
- **THEN** 每卡跨卡入站槽位需求 = 2×(4-2) = 4 ≤ 8，前置检查 PASS，Join 继续

#### Scenario: 8-rank 拓扑预算超限

- **WHEN** 8-rank（4 卡 × 2 die）310P3 HBM 池 Join
- **THEN** 每卡跨卡入站槽位需求 = 2×(8-2) = 12 > 8，前置检查返回 `SM_NO_RESOURCES`，不执行任何导入

#### Scenario: 非 310P3 跳过前置检查

- **WHEN** 910B 或 910C/950 设备创建 HBM 池
- **THEN** 不执行拓扑前置检查，行为与引入本能力之前一致

### Requirement: P2P 生命周期管理

HBM 段 SHALL 维护 `p2pLeases_` 集合跟踪已启用的对端物理设备 ID。导入跨卡 peer 时 SHALL 调用 `EnableRemotePeerAccess` 并将 peer 物理 ID 记入 `p2pLeases_`，同一 peer 重复导入 SHALL 跳过已启用的设备。`Unmap` 全量释放时 SHALL 对 `p2pLeases_` 中每个设备调用 `DisablePeerAccess`（310P3 走 `AclrtDeviceDisablePeerAccess`，其他 SoC 走 `RtDisableP2P`）并清空集合。`RemoveImported` 部分释放时 SHALL 仅释放不再被剩余导入引用的 peer P2P，仍在引用的 peer SHALL 保留。同卡 peer（`IsSameCard` 判定为 true）SHALL 不启用/禁用 P2P。

#### Scenario: 导入跨卡 peer 后释放

- **WHEN** rank0 导入 rank2（跨卡）后执行 Unmap
- **THEN** `DisablePeerAccess` 对 rank2 的物理设备 ID 调用一次，`p2pLeases_` 清空

#### Scenario: 部分释放保留仍在引用的 P2P

- **WHEN** rank0 导入 rank2 和 rank3（同卡），仅移除 rank2
- **THEN** rank3 的 P2P 保留不被禁用（rank3 仍在 imports_ 中）

### Requirement: BM 模式 Leave 与 Extend 限制

310P3 HBM 池（SDMA 位 opType 且 `maxHBMSize > 0`） SHALL 拒绝 `Leave`（动态缩容），返回 `SM_INVALID_PARAM` 并记录含 poolId、rankId 的 ERROR 日志。310P3 HOST_SHM DRAM 池 SHALL 拒绝 `ExtendLocalMem`（HOST 类型），返回 `SM_INVALID_PARAM` 并记录含 poolId 的 ERROR 日志。非 310P3 设备或非上述池类型 SHALL 保持原有行为不变。

#### Scenario: 310P3 HBM 池拒绝 Leave

- **WHEN** 310P3 HBM 池调用 Leave
- **THEN** 返回 `SM_INVALID_PARAM`，记录 ERROR 日志，不执行 GroupLeave

#### Scenario: 310P3 HOST_SHM 拒绝 Extend

- **WHEN** 310P3 HOST_SHM DRAM 池调用 ExtendLocalMem(HOST, size)
- **THEN** 返回 `SM_INVALID_PARAM`，记录 ERROR 日志

### Requirement: DRAM 池 POSIX 共享内存跨进程

310P3 DRAM 池（HOST_SHM opType） SHALL 使用 POSIX `shm_open` 创建跨进程共享内存（名称格式 `/mf_dram_<pid>_<rankId>`），经 `ftruncate` 设大小后 `mmap(MAP_FIXED|MAP_SHARED)` 映射。导出信息 SHALL 携带 `shmName`（64 字节字段）。对端导入时 SHALL 通过 `shm_open(name, O_RDWR)` 打开并 `mmap(MAP_FIXED|MAP_SHARED)` 映射到导入 GVA 地址。`Unmap` 时 SHALL 对每个映射执行 `munmap` 并 `shm_unlink` 本地名称。非 310P3 设备 SHALL 保持原有匿名 mmap / `shmFd` 路径不变。

#### Scenario: 310P3 跨进程 DRAM 共享

- **WHEN** rank0 创建 DRAM 池，rank1 导入 rank0 的 DRAM 分片
- **THEN** rank1 通过 `shm_open` 打开 rank0 的共享内存名并 mmap 到 GVA 地址，数据可读写

#### Scenario: 非 310P3 DRAM 路径不变

- **WHEN** 910B/950 设备创建 DRAM 池
- **THEN** 使用原有匿名 mmap 或 shmFd 路径，不创建 POSIX shm_name

### Requirement: 310P3 Meta GVA ACL 初始化

`hybm_init` 的 meta GVA 初始化在 310P3 上 SHALL 走 ACL 路径：`AclrtReserveMemAddress` 预留 VA → `AclrtMallocPhysical(ACL_MEM_P2P_HUGE)` 分配物理内存 → `AclrtMapMem` 映射。`hybm_uninit` 在 310P3 上 SHALL 走 ACL 释放路径：`AclrtUnmapMem` → `AclrtFreePhysical` → `AclrtReleaseMemAddress`。非 310P3 设备 SHALL 保持原有 HAL 路径（`HalMemAddressReserve`/`HalMemMap`/`HalMemUnmap`/`HalMemRelease`/`HalMemAddressFree`）不变。

#### Scenario: 310P3 meta GVA 初始化

- **WHEN** 在 310P3 上执行 `hybm_init`
- **THEN** meta 空间经 ACL 路径分配并映射成功，返回 `BM_OK`

#### Scenario: 310P3 meta GVA 释放

- **WHEN** 在 310P3 上执行 `hybm_uninit`
- **THEN** meta 空间经 ACL 路径释放（UnmapMem/FreePhysical/ReleaseMemAddress），无 HAL 调用

### Requirement: 同卡判定与跨卡 P2P 使能

系统 SHALL 通过 `IsSameCard(peerId)` 判定对端设备是否在同一物理卡上。310P3 使用 `deviceId/2` 作为卡索引（dev0/dev1→card0，dev2/dev3→card1），`IsSameCard` 返回 `(deviceId/2 == peerId/2)`。其他 SoC 使用 `physicalChipId` 直接比较。`EnableRemotePeerAccess` 在 310P3 上 SHALL 调用 `AclrtDeviceEnablePeerAccess`，同卡返回 `ACL_ERROR_RT_DRV_INTERNAL_ERROR`(507899) 时 SHALL 容忍并返回成功（同卡 die 共享 PCIe，P2P 已内部配置），跨卡返回 507899 SHALL 报错失败。`CanLocalHostReaches` 在 310P3 上 SHALL 始终返回 true（可达性由段映射状态判定）。

#### Scenario: 同卡跨 die P2P 使能被容忍

- **WHEN** 310P3 上 dev0 对 dev1（同卡）调用 `EnableRemotePeerAccess`
- **THEN** `AclrtDeviceEnablePeerAccess` 返回 507899，系统判定为同卡并返回成功

#### Scenario: 跨卡 P2P 使能成功

- **WHEN** 310P3 上 dev0 对 dev2（跨卡）调用 `EnableRemotePeerAccess`
- **THEN** `AclrtDeviceEnablePeerAccess` 返回 0，P2P 使能成功

### Requirement: 新增 C API 查询与 P2P 接口

系统 SHALL 导出以下 C API 供 smem 层调用：`hybm_get_soc_type()` 返回 SoC 类型值（4=310P）；`hybm_get_phy_chip_id(deviceId)` 返回设备物理 chip ID（≥0 成功，<0 失败）；`hybm_enable_peer_access(localDeviceId, peerDeviceId)` 使能 P2P 访问（同卡 deviceId/2 相同返回 0 跳过）；`hybm_alloc_vmm_buffer(deviceId, worldSize, size)` 分配 VMM 设备缓冲（`AclrtMallocPhysical` + `AclrtReserveMemAddress` + `AclrtMapMem` + 跨卡 `AclrtMemSetAccess`），返回映射 VA 或 NULL；`hybm_free_vmm_buffer(addr, size)` 释放 VMM 缓冲（`AclrtUnmapMem` + `AclrtReleaseMemAddress` + `AclrtFreePhysical`）。缓冲句柄 SHALL 经全局 `g_vmmHandleMap` 注册以供 `HybmLookupVmmHandle` 查询。

#### Scenario: 查询 SoC 类型

- **WHEN** 调用 `hybm_get_soc_type()` 在 310P3 设备上
- **THEN** 返回 4（`SOC_TYPE_310P`）

#### Scenario: 分配与释放 VMM 缓冲

- **WHEN** 调用 `hybm_alloc_vmm_buffer(0, 4, 8388608)` 后调用 `hybm_free_vmm_buffer(addr, 8388608)`
- **THEN** 缓冲成功分配（含跨卡 SetAccess）并释放，`g_vmmHandleMap` 中对应条目被移除

## MODIFIED Requirements

### Requirement: 310P SOC 识别与 HBM 段分派

系统 SHALL 识别 soc 字符串包含 "Ascend310P" 或 "Ascend_310P" 的设备为 ASCEND_310P 类型。在 310P 设备上创建 HBM 内存池（SDMA 位 opType）时，系统 SHALL 使用 `HybmVmmBasedSegment` 构建段（与 910C/950 共享 VMM 段路径，通过 `Is310PAclPath()` 内部分派 ACL/HAL 调用），不再使用 `HybmAclVmmSegment`。HBM 段分派条件 SHALL 为 `GVA V4 || 950 || 310P` 合取判定，310P 不再单独优先于 GVA 版本判断。在 910B/910C/950 设备上，段分派与建池行为 SHALL 保持与引入本能力之前完全一致。

#### Scenario: 310P 上创建 HBM 池

- **WHEN** 在 soc 字符串为 "Ascend310P3" 的设备上以 SDMA 位 opType 创建 HBM 内存池
- **THEN** 建池成功，段经由 `HybmVmmBasedSegment` ACL 路径构建（AclrtReserveMemAddress/AclrtMallocPhysical/AclrtMapMem 链路）

#### Scenario: 非 310P 设备行为不变

- **WHEN** 在 910B 或 910C/950 设备上创建 HBM 内存池
- **THEN** 段分派结果与建池行为和引入本能力之前一致，无任何行为变化

### Requirement: ACL VMM 可选符号缺失容忍

系统 SHALL 以 OPTIONAL 方式动态加载 ACL VMM 相关的 `aclrt*` 符号。新增 IPC 符号（`aclrtIpcMemGetExportKey`/`aclrtIpcMemImportByKey`/`aclrtIpcMemClose`）与 P2P 符号（`aclrtDeviceDisablePeerAccess`/`aclrtDeviceCanAccessPeer`）SHALL 以 OPTIONAL 加载。Context 符号（`aclrtCreateContext`/`aclrtSetCurrentContext`/`aclrtDestroyContext`）SHALL 以 OPTIONAL 加载。`aclrtMemSetPidToShareableHandle` SHALL 被移除（不再加载）。当宿主机器的 libascendcl.so 不导出这些符号（如 910B/950 环境）时，`hybm_init` SHALL 正常完成，仅记录 WARN 日志，对应函数指针保持空值；不得因符号缺失导致初始化失败或崩溃。

#### Scenario: 符号缺失的机器上初始化

- **WHEN** 在不导出 ACL VMM 符号的 CANN 环境上执行 `hybm_init`
- **THEN** 初始化成功返回，仅出现符号加载 WARN 日志，进程不崩溃

### Requirement: 段地址空间布局

在 310P 上创建 HBM 池时，系统 SHALL 以 `AclrtReserveMemAddress(maxSize)` 为每个 rank 的本地段预留独立 VA 块（非整窗 `rankCnt × maxSize` 预留）。本地分配的 VA 基址偏移 SHALL 为 0（非 `rankId × maxSize`）。导入的对端段 SHALL 在 `ReserveLva` 中各自调用 `AclrtReserveMemAddress` 获取独立 LVA。全局 GVA 仍按 `base + rankId × maxSize` 槽位布局。在 910C/950 上 SHALL 保持原有整窗预留 + `HalMemAddressReserve` 路径不变。物理内存分配 SHALL 按 VMM granularity（2MB）对齐。

#### Scenario: 310P 本地段独立 VA 预留

- **WHEN** 2-rank 各 8MB 在 310P 上建立 HBM 池
- **THEN** 每个 rank 的本地段经 `AclrtReserveMemAddress(8MB)` 独立预留，本地 VA 基址偏移为 0，GVA 按 `base + rankId × 8MB` 布局

#### Scenario: 910C/950 整窗预留不变

- **WHEN** 910C 上创建 HBM 池
- **THEN** 使用原有 `HalMemAddressReserve(rankCnt × maxSize)` 整窗预留路径

#### Scenario: 整窗预留与槽位布局

- **WHEN** 2-rank 各 8MB 在非 310P 设备（910C/950）上建立 HBM 池
- **THEN** 系统一次预留 16MB 窗口，rank0/rank1 分片分别位于 base 与 base+8MB，GVA==DVA==HVA

### Requirement: 跨进程导出与导入

HBM 分片导出信息 SHALL 使用 magic `0xAABB1234FFFFee05`（ACL VMM 段专用），序列化布局 200 字节（新增 `physicalChipId` + `reserved_` 字段，原 192B → 200B）；导出 SHALL 携带 gva、slotIndex、devId、pid、rankId、size、shareableHandle、physicalChipId。导出 shareable handle 时 SHALL 固定携带 `ACL_RT_VMM_EXPORT_FLAG_DISABLE_PID_VALIDATION` 标志。

对端导入时：同卡 die 对（`deviceId/2` 相同）SHALL 采用 SetDevice ctx trick（`AclrtSetDevice(exporter, force=true)` → `AclrtMemImportFromShareableHandle` → `AclrtSetDevice(local)` 恢复），不再使用 CreateContext 五步序列。跨卡场景 SHALL 在本进程上下文直接导入。导入前 SHALL 在 Mmap 线程强制 `AclrtSetDevice(local, force=true)`（组监听线程无主线程 device context）。导入列表 SHALL 排序为同卡优先、跨卡在后。导入后 SHALL 对导入 VA 调用 `AclrtMemSetAccess(RW, localDevice)` 设置本地设备访问权限。本地分配的 HBM 段 SHALL 对所有跨卡 peer 调用 `AclrtMemSetAccess(RW)` 设置访问权限（同卡 peer 跳过）。

#### Scenario: 跨 die 导入

- **WHEN** rank0（dev0）导出分片，rank1（dev1，同卡 die 对）执行导入
- **THEN** SetDevice ctx trick 三步序列成功，导入地址有效，随后跨 die D2D 拷贝逐字校验通过

#### Scenario: 未知 magic 的导出块被正确识别

- **WHEN** 传输层收到 magic 为 `0xAABB1234FFFFEE05` 的交换块
- **THEN** 该块被识别为 HBM 分片并进入导入流程，不被静默跳过（避免后续读取错位断链）

#### Scenario: 跨卡导入

- **WHEN** rank0（dev0，card0）导出分片，rank2（dev2，card1）执行导入
- **THEN** 在本进程上下文直接 `AclrtMemImportFromShareableHandle` 成功，导入 VA 经 `AclrtMapMem` 映射

#### Scenario: 导入列表同卡优先排序

- **WHEN** rank0 同时导入 rank1（同卡）和 rank2（跨卡）
- **THEN** rank1（同卡）的导入先于 rank2（跨卡）执行

### Requirement: 池内地址分类

对于已登记在本地分配表中的池内地址：导入的对端地址 SHALL 分类为单一 GLOBAL 位（设备或主机类型按 `memType` 字段判定）。本端分配的地址在 `inAllocGva` 分支中不再设 LOCAL|GLOBAL 双位——该分支仅对 imported 地址设单 GLOBAL 位，本端地址的分类 SHALL 由 `reservedMap_` 路径或其他登记路径决定。该分类 SHALL 对 910B 场景行为等价。

#### Scenario: 导入地址分类为单 GLOBAL

- **WHEN** 用户对导入的对端 HBM 池地址发起拷贝
- **THEN** 地址被分类为单一 GLOBAL 位，方向判定依据 GLOBAL 位完成

#### Scenario: 本地池地址作拷贝源与目标

- **WHEN** 用户对本端分配的 HBM 池地址发起 G2H 或 H2G 方向拷贝
- **THEN** 地址经 `reservedMap_` 或其他登记路径分类，方向判定通过，拷贝正常执行

### Requirement: 310P 设备信息查询容忍降级

`hybm_init` 阶段的设备拓扑信息查询（SDID/SERVER_ID/SUPER_POD_ID）在 310P 上不被驱动支持（返回非零）时，系统 SHALL 容忍降级：记录 WARN 日志并将对应字段置为默认值（SDID=0、SERVER_ID=0、SUPER_POD_ID=invalidSuperPodId），初始化 SHALL 继续成功。系统 SHALL 额外查询 `INFO_TYPE_PHY_CHIP_ID` 获取 `physicalChipId_`，查询失败时置为 0 并记录 WARN。跨 rank 可达性判定在 310P 上 SHALL 始终返回 true（`CanLocalHostReaches` 对 310P 返回 true）。不再使用 `QueryDeviceInfoSocTolerant` 封装，改为直接调用 `RtGetDeviceInfo` 并按返回码降级。

#### Scenario: 310P 初始化不因拓扑查询失败而失败

- **WHEN** 在 310P 上执行 `hybm_init`，内部拓扑查询返回非零
- **THEN** 初始化成功，仅记录 WARN，SDID/SERVER_ID/SUPER_POD_ID 置为默认值，physicalChipId 查询失败时置为 0

### Requirement: aclrtMemAttr 枚举与 CANN 严格对齐

`dl_acl_api.h` 中 `aclrtMemAttr` 枚举 SHALL 与构建目标 CANN 版本（9.1.0）`acl/acl_rt.h` 的编号严格一致（全量 18 项，0~17），成员从 0 自增，不赋显式值。`aclrtPhysicalMemProp` 结构体的 `allocationType` 与 `memAttr` 字段 SHALL 为 `uint32_t` 类型（经 `static_cast` 从枚举值传入），而非枚举类类型，以消除枚举类隐式转换风险。`aclrtMemAccessDesc.flags` SHALL 为 `uint32_t` 类型。`ACL_RT_MEM_ACCESS_FLAGS_READWRITE` SHALL 定义为 `0x3U` 常量，`ACL_MEM_ALLOCATION_TYPE_PINNED` SHALL 定义为 `1U` 常量。

#### Scenario: 枚举值直传驱动语义正确

- **WHEN** 以 `ACL_MEM_P2P_HUGE`（值 16）构造 `aclrtPhysicalMemProp.memAttr`（uint32_t）调用 `AclrtMallocPhysical`
- **THEN** 驱动收到值 16，从 P2P 分区分配；池 ≤4GB 时分配成功，>4GB（分区上限 ~4088MB）返回 207001

#### Scenario: 非 310P 环境零影响

- **WHEN** 在 910B/910C/950 环境加载 libascendcl.so
- **THEN** 枚举与类型声明仅为本头文件使用，不影响宿主 CANN 行为

## REMOVED Requirements

### Requirement: 物理分配 P2P_HUGE 到 HBM_HUGE 回退

**Reason**: 310P HBM 段分派从 `HybmAclVmmSegment`（含 `MallocPhysicalWithFallback`）迁移到 `HybmVmmBasedSegment`（`MallocFromDevice` 直接使用 `ACL_MEM_P2P_HUGE`，无回退逻辑）。新路径不再维护 P2P_HUGE→HBM_HUGE 回退。
**Migration**: 大池场景（超过 P2P 分区 ~4088MB 上限）需在应用层确保 HBM 池大小 ≤ P2P 分区上限，或在 V2 中为 `HybmVmmBasedSegment::MallocFromDevice` 310P 路径补充回退逻辑。
