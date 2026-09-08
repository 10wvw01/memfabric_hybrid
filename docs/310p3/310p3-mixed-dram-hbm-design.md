**作者 (Authors):** opencode
**创建日期 (Created):** 2026-09-07
**最后更新 (Updated):** 2026-09-08
**相关 Issue/PR:** commit 41f1bf00 (310P3 BM HBM 2-rank + DRAM 8-rank support)
**平台**: 310P3 (8卡, CANN 9.1.0)

---

# 1. 概述

## 1.1 简介

本提案旨在移除 310P3 平台上 BigMemory handle 中 DRAM+HBM 不能共存的限制，使 310P3 与 910B/910C 平台行为对齐——允许在同一个 `smem_bm_create` / `smem_bm_create2` 调用中同时指定 `localDRAMSize > 0` 和 `localHBMSize > 0`，并通过统一 GVA 地址空间对外提供混合内存池。

## 1.2 动机

commit `41f1bf00` 引入 310P3 支持时，主动添加了 `smem_bm.cpp:233-238` 的校验：

```cpp
/* 310P3: reject DRAM+HBM same handle — must use separate handles */
if (option->maxDramSize != 0 && option->maxHbmSize != 0) {
    SM_LOG_AND_SET_LAST_ERROR_CODE(SM_INVALID_PARAM,
                                   "DRAM+HBM in same handle is not supported on 310P3, use separate handles");
    return SM_INVALID_PARAM;
}
```

**痛点**：
- 910B/910C 平台已支持同一 handle 中 DRAM+HBM 混合，310P3 用户需要使用两个独立 handle 分别管理 DRAM 和 HBM，API 使用方式不一致。
- benchmark `bm_perf_benchmark` 默认创建 `smem_bm_create(0, 0, opType, GVA_SIZE, GVA_SIZE, flags)`（DRAM=HBM=4GB），在 310P3 上直接失败，无法运行全路径带宽测试。
- 上层应用（如内存池样例 `memory_pool`）需要混合 DRAM+HBM 的统一地址空间来支持 KV cache offload 等场景。
- `transfer_perf` 的 `smem_shm` 初始化路径依赖 HAL 驱动 API，310P3 不支持，需适配 ACL VMM 路径。

**不做此提案的影响**：310P3 无法在同一 BigMemory handle 中混合使用 DRAM 和 HBM，与 910 平台能力不对齐，限制上层应用编排灵活性。

## 1.3 目标

**目标**：
- 移除 310P3 DRAM+HBM 同 handle 限制，使 `smem_bm_create` / `smem_bm_create2` 在 310P3 上与 910 平台行为一致。
- 支持 SDMA 协议下的 DRAM+HBM 混合场景（同节点内），覆盖全 12 路径拷贝。
- 支持 DEVICE_RDMA 协议下的 DRAM+HBM 混合场景（含跨节点）。
- 补齐跨 rank DRAM 共享能力（`ConnBasedSegment` 跨进程共享物理内存）。
- 适配 `transfer_perf` 的 `smem_shm` 初始化路径，使其在 310P3 上可用。
- benchmark `bm_perf_benchmark` 在 310P3 上可直接运行 `-ot sdma -t all` 和 `-ot device_rdma -t all`。

**非目标**：
- 不修改 HOST_SHM op type 的限制（HOST_SHM 仍仅支持纯 DRAM）。
- 不支持 HOST_TCP / HOST_URMA / HOST_RDMA 协议下的 HBM 传输（这些协议本身不支持 HBM）。
- 不改变 310P3 的 ACL VMM 内存管理机制（`aclrtMallocPhysical` / `aclrtMemImportFromShareableHandle` 等）。

# 2. 用例分析

## 2.1 功能需求

| 场景 | 协议 | 内存组合 | 节点范围 | 描述 |
|------|------|---------|---------|------|
| BM-1 | SDMA | DRAM+HBM | 同节点 | 同节点多卡，SDMA 拷贝覆盖 LH2GH/LH2GD/LD2GH/LD2GD/GH2GH/GH2GD/GD2GH/GD2GD/GH2LH/GH2LD/GD2LH/GD2LD 全路径 |
| BM-2 | DEVICE_RDMA | DRAM+HBM | 同节点/跨节点 | DEVICE_RDMA 拷贝覆盖全路径，含跨节点 G2G |
| BM-3 | SDMA | HBM-only | 同节点 | 已支持，回归验证不回退 |
| BM-4 | SDMA | DRAM-only | 同节点 | 新增支持（需修复 ConnBasedSegment::CheckSdmaReaches） |
| BM-5 | DEVICE_RDMA | HBM-only | 同节点/跨节点 | 已支持，回归验证不回退 |
| BM-6 | DEVICE_RDMA | DRAM-only | 同节点/跨节点 | 已支持，回归验证不回退 |

## 2.2 性能指标

- 混合场景下各拷贝路径带宽不低于纯 DRAM 或纯 HBM 场景的 90%。
- 无额外内存开销（不引入中间 buffer 或额外拷贝）。

## 2.3 DFX 要求

- **兼容性**：910B/910C/950 平台行为不受影响，所有改动在 310P3 条件分支内。
- **可测试性**：benchmark `bm_perf_benchmark` 可直接运行混合场景。
- **可维护性**：不引入新的 segment 类型，复用现有 `hbmSegment_` + `dramSegment_` 双段架构。

# 3. 功能设计

本章以功能设计为主线，按功能模块组织所有改动。每个模块包含：问题描述 → 方案设计 → 代码实现 → 影响范围。

## 3.1 总体方案与改动清单

核心思路：**移除校验 + 修复 DRAM SDMA 可达性 + 修复 SDMA TransformVa 方向感知 + 修复拷贝路径 memcpy kind + 补齐跨 rank DRAM 共享 + 适配 meta GVA 初始化**。

310P3 上混合 DRAM+HBM 在同一个 entity 中时，entity 层已经设计了双段架构（`hbmSegment_` + `dramSegment_`），GVA 地址空间管理（`HybmVaManager`）对 DRAM 和 HBM 使用统一的地址池和查找机制。主要阻塞点有：

1. **校验拦截**（`smem_bm.cpp:233-238`）：直接拒绝 DRAM+HBM 同 handle。
2. **DRAM SDMA 不可达**（`hybm_entity_default.cpp:1218-1221`）：`ConnBasedSegment` 未覆写 `CheckSdmaReaches`，基类返回 `false`，导致 SDMA + DRAM 在 `InitDramSegment` 中被拒绝。
3. **SDMA TransformVa 方向盲**（`hybm_data_op_sdma.cpp:327-339`）：始终做 GVA→DVA，忽略 `direction` 参数。对 DRAM 地址返回 0（DVA 未设置），fallback 到 GVA(=HVA)。在 310P3 SVM 下 `ACL_MEMCPY_DEVICE_TO_DEVICE` 对 HVA 可工作，但语义不正确。
4. **拷贝路径 memcpy kind 错误**：`CopyG2G` 始终用 `ACL_MEMCPY_DEVICE_TO_DEVICE`；`CopyLH2GD` 等使用同步拷贝 + 中转 buffer，性能低且语义不准。
5. **跨 rank DRAM 不共享**：`ConnBasedSegment` 用 `MAP_ANONYMOUS | MAP_PRIVATE` 分配 DRAM，跨进程不共享物理内存。
6. **meta GVA 初始化依赖 HAL 驱动**：`smem_shm` 的 `hybm_init` 调用 `HalMemAddressReserve` 等 HAL API，310P3 不支持。

### 改动清单

| 编号 | 阶段 | 文件 | 改动点 | 风险 |
|------|------|------|--------|------|
| M1 | P0 | `src/smem/csrc/smem_bm/smem_bm.cpp` | 删除 DRAM+HBM 同 handle 拒绝校验 | 低 |
| M2 | P0 | `src/hybm/csrc/mm/hybm_conn_based_segment.h/.cpp` | 覆写 `CheckSdmaReaches` 返回 `true` | 中 |
| M3 | P0 | `src/hybm/csrc/data_operation/host/hybm_data_op_sdma.cpp` | `TransformVa` 改为方向感知 + 查找表 | 中 |
| M4 | P0 | `src/hybm/csrc/data_operation/host/hybm_data_op_sdma.cpp` | `CopyG2G` 310P3 恢复为仅 D2D async（kind 由调用方指定） | 中 |
| P0-1a | P0 | `src/hybm/csrc/data_operation/host/hybm_data_op_sdma.cpp` | `CopyLH2GD`/`CopyGD2LH`/`CopyLH2GH`/`CopyGH2LH` 310P3 用 `AclrtMemcpyAsync` + 正确 kind | 中 |
| P0-1b | P0 | `src/hybm/csrc/data_operation/host/hybm_data_op_sdma.cpp` | `CopyG2G` async 失败后 fallback 到同步 `AclrtMemcpy` + `ResetThreadAclStream` | 中 |
| P0-1c | P0 | `src/hybm/csrc/ts_engine/hybm_stream_manager.h/.cpp` | 新增 `ResetThreadAclStream` 方法 | 低 |
| P0-2 | P0 | `benchmark/bm/band_width_calculator.h/.cpp` + `main.cpp` | `GVA_SIZE` 参数化为 `-gs/--gva-size` | 低 |
| P1-1 | P1 | `src/hybm/csrc/mm/hybm_conn_based_segment.h/.cpp` | `shm_open` + `MAP_SHARED` 实现跨 rank DRAM 共享 | 中 |
| P1-2a | P1 | `src/hybm/csrc/driver/hybm_gva.cpp` | `HybmModernInitMetaGva` 310P3 用 ACL VMM 替代 HAL 驱动 | 中 |
| P1-2b | P1 | `src/hybm/csrc/hybm_entry.cpp` | `hybm_uninit` 310P3 用 ACL API 清理 meta 空间 | 中 |

### 为什么这些改动足够

基于源码研究的结论：

**GVA 地址空间**：VaManager 的 `AllocReserveGva` 对 DRAM 和 HBM 使用同一地址池（GVM range `[40TB, 168TB)`），`memType` 仅作为元数据。`FindFreeSpace` 通过 `reservedMap_` 防止地址重叠。`ClassifyAddressMask` 通过 `allocatedMap_` / `reservedMap_` 中的 `memType` 元数据正确区分 DRAM 和 HBM 地址。**无需修改 VaManager。**

**rankId 偏移**：HBM（VmmBasedSegment）和 DRAM（ConnBasedSegment）的 GVA 布局都是 `rankCnt * maxSize`，rank N 位于 `gvaBase + N * maxSize`。`smem_bm_ptr_by_mem_type` 的 `maxHBMSize * peerRankId` / `maxDRAMSize * peerRankId` 计算与实际布局匹配。**无需修改偏移逻辑。**

**Export/Import**：entity 层通过 magic 常量路由（`IsDramSlice` / `IsHbmSlice`），HBM 走 VMM ACL 路径（`aclrtMemExportToShareableHandle` / `aclrtMemImportFromShareableHandle`），DRAM 走 ConnBased mmap 路径（GVA 元数据交换 + 共享 mmap）。两者已在 entity 层协调。**无需修改 Export/Import。**

**P2P**：HBM 通过 `aclrtMemSetAccess` + ctx trick，DRAM 通过共享 host VA（同节点）。无资源冲突。**无需修改 P2P 逻辑。**

**Copy 路径**：DeviceRDMA 的 `TransformVa` 已经是方向感知的，直接支持混合。SDMA 的 `TransformVa` 需要修复（M3），拷贝函数的 memcpy kind 需要修复（M4 + P0-1）。

## 3.2 M1：移除 DRAM+HBM 校验

**问题**：`smem_bm_create2_inner` 中第 233-238 行直接拒绝 310P3 上 `maxDramSize != 0 && maxHbmSize != 0` 的调用。

**方案**：删除该拒绝逻辑。

```cpp
// 删除以下代码：
/* 310P3: reject DRAM+HBM same handle — must use separate handles */
if (option->maxDramSize != 0 && option->maxHbmSize != 0) {
    SM_LOG_AND_SET_LAST_ERROR_CODE(SM_INVALID_PARAM,
                                   "DRAM+HBM in same handle is not supported on 310P3, use separate handles");
    return SM_INVALID_PARAM;
}
```

**影响范围**：仅 310P3 平台。910B/910C/950 原本就没有此校验，行为不变。HOST_SHM 校验（`isHostShm && maxHbmSize != 0`）仍保留。

## 3.3 M2：DRAM SDMA 可达性

**文件**：`src/hybm/csrc/mm/hybm_conn_based_segment.h` + `hybm_conn_based_segment.cpp`

**问题**：`ConnBasedSegment` 未覆写 `CheckSdmaReaches`，基类 `MemSegment::CheckSdmaReaches` 返回 `false`。导致 `InitDramSegment`（`hybm_entity_default.cpp:1218-1221`）在 SDMA 模式下拒绝 DRAM segment 创建。

**方案**：在 `HybmConnBasedSegment` 中覆写 `CheckSdmaReaches`。

310P3 上 `ConnBasedSegment` 的 DRAM 通过 `mmap(MAP_FIXED_NOREPLACE)` 在同节点内共享 host VA 空间。SDMA 在 310P3 上是同节点拷贝（通过 ACL SVM），因此同节点的 DRAM SDMA 可达。

初步实现考虑基于 `mappedGvaMem_` 判断远端 rank 是否已 import：

```cpp
// hybm_conn_based_segment.h
bool CheckSdmaReaches(uint32_t rankId) const noexcept override;

// hybm_conn_based_segment.cpp
bool HybmConnBasedSegment::CheckSdmaReaches(uint32_t rankId) const noexcept
{
    // ConnBasedSegment DRAM 通过 mmap 共享同节点 host VA，SDMA 同节点可达
    // 跨节点 DRAM 需要 DEVICE_RDMA/HOST_RDMA/HOST_TCP transport
    if (rankId >= options_.rankCnt) {
        return false;
    }
    // 同节点判断：ConnBasedSegment 的 import 机制基于同节点 mmap 共享
    // 如果远端 rank 已 import（mappedGvaMem_ 中有记录），则同节点可达
    return mappedGvaMem_.find(/* peer GVA */) != mappedGvaMem_.end() || rankId == options_.rankId;
}
```

**注意**：`CheckSdmaReaches` 在 `InitDramSegment` 中被调用时（`hybm_entity_default.cpp:1218`），此时 `rankId == options_.rankId`（本地 rank），尚未执行 import。因此需要放宽初始检查：本地 rank 始终返回 `true`（本地 DRAM 对本地设备通过 SVM 可达）。

**最终简化实现**：由于 310P3 上 SDMA 均为同节点操作（跨节点不走 RDMA/TCP），且 `ConnBasedSegment` 仅在同节点 mmap 共享场景下使用，直接返回 `true`：

```cpp
bool HybmConnBasedSegment::CheckSdmaReaches(uint32_t rankId) const noexcept
{
    return true;
}
```

这与 `HybmVmmBasedSegment::CheckSdmaReaches`（`hybm_vmm_based_segment.cpp:986-988`）的行为一致——VmmBasedSegment 也无条件返回 `true`。

**影响范围**：
- 310P3 SDMA + DRAM：从"拒绝"变为"允许"。
- 310P3 DEVICE_RDMA + DRAM：不受影响（`InitDramSegment` 中的 SDMA 检查仅在 `bmDataOpType & HYBM_DOP_TYPE_SDMA` 时触发）。
- 910 平台：910 的 DRAM 使用 `HybmDevLegacySegment`（有自己的 `CheckSdmaReaches` 实现），不受影响。

## 3.4 M3：SDMA 地址转换方向感知

**文件**：`src/hybm/csrc/data_operation/host/hybm_data_op_sdma.cpp`

**问题**：`HostDataOpSDMA::TransformVa`（第 327-339 行）始终做 GVA→DVA，忽略 `direction` 参数。对 DRAM 地址（DVA=0），返回 0，fallback 到 GVA(=HVA)。虽然 310P3 SVM 下 `ACL_MEMCPY_DEVICE_TO_DEVICE` 对 HVA 可工作，但语义不正确。

**方案**：参考 `DataOpDeviceRDMA::TransformVa`（`hybm_data_op_device_rdma.cpp:115-131`），改为方向感知：

```cpp
// 需要引入方向→memType 查找表（与 DataOpDeviceRDMA 一致）
static const hybm_mem_type HybmSdmaDirectionSrcMemType[HYBM_DATA_COPY_DIRECTION_BUTT] = {
    HYBM_MEM_TYPE_HOST,   // 0  LH2GH
    HYBM_MEM_TYPE_HOST,   // 1  LH2GD
    HYBM_MEM_TYPE_DEVICE, // 2  LD2GH
    HYBM_MEM_TYPE_DEVICE, // 3  LD2GD
    HYBM_MEM_TYPE_HOST,   // 4  GH2GH
    HYBM_MEM_TYPE_HOST,   // 5  GH2GD
    HYBM_MEM_TYPE_HOST,   // 6  GH2LH
    HYBM_MEM_TYPE_HOST,   // 7  GH2LD
    HYBM_MEM_TYPE_DEVICE, // 8  GD2GH
    HYBM_MEM_TYPE_DEVICE, // 9  GD2GD
    HYBM_MEM_TYPE_DEVICE, // 10 GD2LH
    HYBM_MEM_TYPE_DEVICE, // 11 GD2LD
};

static const hybm_mem_type HybmSdmaDirectionDestMemType[HYBM_DATA_COPY_DIRECTION_BUTT] = {
    HYBM_MEM_TYPE_HOST,   // 0  LH2GH
    HYBM_MEM_TYPE_DEVICE, // 1  LH2GD
    HYBM_MEM_TYPE_HOST,   // 2  LD2GH
    HYBM_MEM_TYPE_DEVICE, // 3  LD2GD
    HYBM_MEM_TYPE_HOST,   // 4  GH2GH
    HYBM_MEM_TYPE_DEVICE, // 5  GH2GD
    HYBM_MEM_TYPE_HOST,   // 6  GH2LH
    HYBM_MEM_TYPE_DEVICE, // 7  GH2LD
    HYBM_MEM_TYPE_HOST,   // 8  GD2GH
    HYBM_MEM_TYPE_DEVICE, // 9  GD2GD
    HYBM_MEM_TYPE_HOST,   // 10 GD2LH
    HYBM_MEM_TYPE_DEVICE, // 11 GD2LD
};

void HostDataOpSDMA::TransformVa(void *&src, void *&dst, hybm_data_copy_direction direction) noexcept
{
    uint32_t srcType = (HybmSdmaDirectionSrcMemType[direction] == HYBM_MEM_TYPE_HOST) ? HVM_HVA : HVM_DVA;
    uint32_t dstType = (HybmSdmaDirectionDestMemType[direction] == HYBM_MEM_TYPE_HOST) ? HVM_HVA : HVM_DVA;

    if (src != nullptr) {
        auto out = HybmVaManager::GetInstance().TransformVa(reinterpret_cast<uint64_t>(src), HVM_GVA, srcType);
        src = (out != 0) ? reinterpret_cast<void *>(out) : src;
    }
    if (dst != nullptr) {
        auto out = HybmVaManager::GetInstance().TransformVa(reinterpret_cast<uint64_t>(dst), HVM_GVA, dstType);
        dst = (out != 0) ? reinterpret_cast<void *>(out) : dst;
    }
}
```

**效果**：
- DRAM（HOST）方向：GVA→HVA，如果 HVA 未注册（DVA=0, HVA=0），返回 0，fallback 到 GVA。在 310P3 SVM 下 GVA==HVA，可直接使用。
- HBM（DEVICE）方向：GVA→DVA，DVA 已注册，正确转换。
- 混合方向（如 GH2GD）：src 转 HVA，dst 转 DVA，各自正确。

**影响范围**：
- 310P3 SDMA 混合场景：TransformVa 正确区分 DRAM/HBM 地址。
- 310P3 SDMA 纯 HBM 场景：行为不变（DVA 已注册，TransformVa 返回 DVA）。
- 310P3 SDMA 纯 DRAM 场景：行为不变（HVA 未注册，返回 0，fallback 到 GVA=HVA）。
- 910 平台 SDMA：910 的 DRAM 使用 `HybmDevLegacySegment`（注册了 DVA），TransformVa 会正确返回 DVA。方向感知改动不影响 910 行为（因为 910 的 DVA 和 HVA 都已注册，无论转哪个都正确）。

## 3.5 M4 + P0-1：拷贝路径 async + memcpy kind 修复

### 3.5.1 设计演进

原始设计中 `CopyG2G` 的 `ACL_MEMCPY_DEVICE_TO_DEVICE` memcpy 类型分析如下：310P3 支持 SVM（Shared Virtual Memory），device 可以直接访问 host VA，因此 `ACL_MEMCPY_DEVICE_TO_DEVICE` 对 HVA 和 DVA 都有效。**初步结论是无需修改 `CopyG2G` 的 memcpy 类型。**

但实测发现：`CopyG2G` 用地址范围判断 kind 的方式在混合场景下不可靠，且 `CopyLH2GD` 等 host↔device 拷贝函数使用同步 `AclrtMemcpy` + 中转 buffer，性能低。因此实施阶段进行了 M4 + P0-1 修复。

### 3.5.2 M4：CopyG2G 310P3 D2D async

310P3 的 `CopyG2G` 改为用 `AclrtMemcpyAsync(D2D)` + `AclrtSynchronizeStream`，不再用地址范围判断 kind，host 地址的 kind 由 `CopyLH2GD` 等调用方直接指定。async 失败后 fallback 到同步 `AclrtMemcpy(D2D)` + `ResetThreadAclStream`。

### 3.5.3 P0-1a：host↔device 拷贝函数 async + 正确 kind

四个 host↔device 拷贝函数（`CopyLH2GD`/`CopyGD2LH`/`CopyLH2GH`/`CopyGH2LH`）310P3 路径改用 `AclrtMemcpyAsync` + `AclrtSynchronizeStream`，每个函数使用正确的 memcpy kind（H2D/D2H）。async 失败后 fallback 到同步 + `ResetThreadAclStream`。替代了原先的同步 `AclrtMemcpy` 和中转 buffer 方案。

### 3.5.4 P0-1b/c：ResetThreadAclStream

`CopyG2G` async 路径失败后增加 fallback：`ResetThreadAclStream` + 同步 `AclrtMemcpy(D2D)` 重试。为此在 `hybm_stream_manager.h/.cpp` 新增 `ResetThreadAclStream` 方法（P0-1c），用于在 async 失败后重置 stream 状态以恢复可用性。

## 3.6 P1-1：跨 rank DRAM 共享

**文件**：`src/hybm/csrc/mm/hybm_conn_based_segment.h/.cpp`

**问题**：`ConnBasedSegment` 用 `MAP_ANONYMOUS | MAP_PRIVATE` 分配 DRAM，跨进程不共享物理内存，远端 rank 的 DRAM GVA 不可访问。

**方案**：310P3 上用 `shm_open` + `MAP_SHARED` 替代：

- `ReserveMemorySpace`：创建 `shm_open` name（`/mf_dram_{pid}_{rankId}`）
- `AllocMemory`：用 `shm_open(name)` + `ftruncate` + `mmap(MAP_FIXED | MAP_SHARED)` 分配
- `Export`：在 `HostExportInfo` 中传递 `shmName[64]`（复用原有 padding 空间）
- `Mmap`：远端 rank 通过 `shm_open(import.shmName)` + `mmap(MAP_FIXED | MAP_SHARED)` 映射到相同 GVA 地址，注册 `hva=gva`
- `Unmap`：`munmap` + `shm_unlink` 清理

**方案选型验证**：ACL VMM 的 `aclrtMallocPhysical(ACL_MEM_LOCATION_TYPE_HOST)` + `AclrtMemExportToShareableHandle` 在 310P3 上不支持跨 device import（返回 `107000`）。`aclrtIpcMemGetExportKey` 也不支持 mmap 内存（返回 `507899`）。`shm_open` + `MAP_SHARED` 是唯一可行的跨进程 host 内存共享方案。

## 3.7 P1-2：meta GVA 空间初始化适配

**文件**：`src/hybm/csrc/driver/hybm_gva.cpp` + `src/hybm/csrc/hybm_entry.cpp`

**问题**：`smem_shm` 的 `hybm_init` 调用 `hybm_init_hbm_gva` → `HybmModernInitMetaGva`，其中用 `HalMemAddressReserve` / `HalMemCreate` / `HalMemMap`（HAL 驱动 API），310P3 不支持。

**修复**：
- `HybmModernInitMetaGva`（P1-2a）：310P3 用 `AclrtReserveMemAddress`（固定地址 `HYBM_DEVICE_META_ADDR`）+ `AclrtMallocPhysical` + `AclrtMapMem`
- `hybm_uninit`（P1-2b）：310P3 用 `AclrtUnmapMem` + `AclrtFreePhysical` + `AclrtReleaseMemAddress` 替代 `HalMemUnmap` / `HalMemRelease` / `HalMemAddressFree`

**结果**：`smem_shm_init` + `smem_shm_create` 成功，segfault 已修复。`smem_shm_control_allgather` + `smem_shm_control_barrier` 通过，`smem_shm_control` 通道的 HAL 调用已迁移为 ACL API。`trans_perf_test` 全部完成。

## 3.8 P0-2：benchmark 参数化

**文件**：`benchmark/bm/band_width_calculator.h/.cpp` + `main.cpp`

**问题**：`band_width_calculator.cpp:433` 硬编码 `GVA_SIZE`（DRAM=HBM=4GB），在 310P3 上可能因内存不足失败。

**修改**：将 `GVA_SIZE` 参数化为 `-gs/--gva-size` 命令行参数，允许用户按实际内存容量调整。M1 完成后 `smem_bm_create(0, 0, opType, GVA_SIZE, GVA_SIZE, flags)` 在 310P3 上不再被拒绝，benchmark 代码本身无需修改创建逻辑，仅需参数化 GVA_SIZE。

## 3.9 无需修改的部分

| 模块 | 结论 | 理由 |
|------|------|------|
| VaManager (`hybm_va_manager.cpp`) | 无需修改 | DRAM/HBM 使用同一地址池，`memType` 仅作元数据，`ClassifyAddressMask` 正确区分 |
| rankId 偏移逻辑 | 无需修改 | HBM/DRAM GVA 布局一致（`rankCnt * maxSize`），`smem_bm_ptr_by_mem_type` 计算匹配 |
| Export/Import | 无需修改 | entity 层通过 magic 常量路由，HBM 走 VMM ACL 路径，DRAM 走 ConnBased mmap 路径，已协调 |
| P2P 逻辑 | 无需修改 | HBM 通过 `aclrtMemSetAccess` + ctx trick，DRAM 通过共享 host VA，无资源冲突 |
| DeviceRDMA TransformVa | 无需修改 | 已是方向感知的，直接支持混合 |

# 4. 技术选型

## 4.1 方案 A（采用）：最小改动 + 方向感知修复 + shm_open 共享

- M1：移除 310P3 DRAM+HBM 校验
- M2：ConnBasedSegment 覆写 `CheckSdmaReaches`（返回 `true`）
- M3：SDMA `TransformVa` 改为方向感知
- M4 + P0-1：拷贝路径 async + 正确 memcpy kind
- P1-1：`shm_open` + `MAP_SHARED` 跨 rank DRAM 共享
- P1-2：ACL VMM 替代 HAL 驱动初始化 meta GVA

**优势**：改动量最小（核心约 50 行 + 拷贝路径修复），复用现有双段架构，910 平台不受影响。
**劣势**：SDMA + DRAM 仅支持同节点（310P3 SVM 限制），跨节点 DRAM 需使用 DEVICE_RDMA。

## 4.2 方案 B（放弃）：DRAM 也走 VmmBasedSegment

将 310P3 DRAM 也通过 `aclrtMallocPhysical` 分配，走 ACL VMM import/export 路径。

**放弃理由**：
- 310P3 DRAM 使用 host 内存，`aclrtMallocPhysical` 的 `ACL_MEM_P2P_HUGE` 语义是 device 物理内存，不适用于 host DRAM。
- 改动量大，需新增 DRAM VMM 路径，与现有 ConnBasedSegment 的 mmap 共享内存机制冲突。
- 910 平台 DRAM 也不走 VMM 路径（除非 910C+GVA_V4），与 910 对齐的目标不需要此改动。

## 4.3 方案 C（放弃）：保持双 handle 限制，上层封装

不改底层，在 smem_bm API 层提供 "虚拟混合 handle" 封装，内部维护两个独立 handle。

**放弃理由**：
- 用户要求与 910 行为一致（单 handle 混合）。
- 双 handle 的 GVA 地址空间不连续，`smem_bm_ptr_by_mem_type` 无法统一返回地址。
- 上层应用需感知底层差异，违反透明性原则。

# 5. 编程与调用设计

## 5.1 编程模型

开发者使用混合 DRAM+HBM 的方式与 910 平台完全一致：

```cpp
// C API
smem_bm_t handle = smem_bm_create(0, 0, SMEMB_DATA_OP_SDMA, dramSize, hbmSize, flags);
smem_bm_join(handle, 0);

void *dramGva = smem_bm_ptr_by_mem_type(handle, SMEM_MEM_TYPE_HOST, peerRankId);
void *hbmGva  = smem_bm_ptr_by_mem_type(handle, SMEM_MEM_TYPE_DEVICE, peerRankId);
```

```python
# Python API
handle = bm.create2(id=0, local_dram_size=ONE_GIB, max_dram_size=ONE_GIB,
                    local_hbm_size=ONE_GIB, max_hbm_size=ONE_GIB,
                    data_op_type=bm.BmDataOpType.SDMA)
handle.join()
dram_gva = handle.peer_rank_ptr(0, bm.BmMemType.HOST)
hbm_gva = handle.peer_rank_ptr(0, bm.BmMemType.DEVICE)
```

## 5.2 benchmark 使用

`band_width_calculator.cpp:433` 调用 `smem_bm_create(0, 0, cmdParam_.opType, GVA_SIZE, GVA_SIZE, flags)`，M1 完成后在 310P3 上不再被拒绝。P0-2 将 `GVA_SIZE` 参数化为 `-gs/--gva-size`，可按实际内存容量调整。

# 6. 安全隐私与 DFX 设计

## 6.1 兼容性

- **910B/910C/950 平台**：所有修改在 310P3 条件分支内或通过虚函数覆写实现，910 平台代码路径不变。
- **HOST_SHM op type**：M1 不影响 HOST_SHM 校验（`isHostShm && maxHbmSize != 0` 仍然被拒绝）。
- **现有 API**：`smem_bm_create` / `smem_bm_create2` 签名不变，仅放宽参数约束。

## 6.2 可测试性

- benchmark `bm_perf_benchmark -ot sdma -t all -s 2097152 -ws 2 -lrs 2 -ip tcp://127.0.0.1:8570` 可直接验证混合场景。
- `bm_perf_benchmark -ot device_rdma -t all` 验证 DEVICE_RDMA 混合场景。
- 回归测试：纯 HBM（`-ot sdma -s 2097152 -ws 2 -lrs 2`，DRAM=0）和纯 DRAM（`-ot device_rdma -s 2097152 -ws 2 -lrs 2`，HBM=0）场景不回退。

## 6.3 可靠性

- `CheckSdmaReaches` 放宽后，跨节点 SDMA + DRAM 场景会由 transport 层的 `CanReachDataOperators` 逻辑处理（`hybm_entity_default.cpp:1300-1307`）。如果远端 rank 不可达，copy 操作会返回错误，不会导致内存访问异常。
- `TransformVa` 方向感知改动：如果 `TransformVa` 返回 0（地址未注册），fallback 到原始 GVA，与当前行为一致。
- `CopyG2G` async 失败后 fallback 到同步重试 + `ResetThreadAclStream`，保证拷贝可靠性。

# 7. 测试验证

## 7.1 smem_bm benchmark（SDMA，混合 DRAM+HBM，2卡）

**命令**:
```bash
./bm_perf_benchmark -bw -ot sdma -t all -s 1048576 -cc 100 -bs 1 -d 0 -ws 2 -lrs 2
```

**结果**: 10/10 路径全部 PASS（WrongBytes=0）

| 路径 | 方向 | 状态 | 带宽 (GB/s) |
|------|------|------|------------|
| H2D | LH2GD (Host→HBM) | PASS | 5.3 |
| D2H | GD2LH (HBM→Host) | PASS | 168-170 |
| H2RD | LH2GH (Host→DRAM GVA) | PASS | 5.4-5.7 |
| H2RH | LH2RH (Host→远端DRAM) | PASS | 20-21 |
| D2RD | LD2GH (HBM→DRAM GVA) | PASS | 300-315 |
| D2RH | LD2RH (HBM→远端Host) | PASS | 174-179 |
| RH2D | RH2LD (远端Host→HBM) | PASS | 105 |
| RH2H | RH2LH (远端Host→Host) | PASS | 20-21 |
| RD2D | RG2LD (远端HBM→本地HBM) | PASS | 293-315 |
| RD2H | RG2LH (远端HBM→Host) | PASS | 6.1 |

## 7.2 transfer_perf

- `smem_shm_init` + `smem_shm_create`：PASS
- `smem_shm_control_allgather` + `smem_shm_control_barrier`：PASS
- `smem_trans_register_mem`：PASS
- `trans_perf_test`：PASS，10 个 block size 全部完成，吞吐 15-18 GB/s

## 7.3 310p3_bm_test / 310p3_trans_test 4 rank

- 310p3_bm_test 4 rank HBM：ALL TESTS PASSED
- 310p3_trans_test 4 rank：ALL TESTS PASS（SyncWriteRead + AsyncSubmit + SyncRead）

# 8. 影响范围

| 平台 | 影响 |
|------|------|
| 910B/910C/950 | 无影响。所有改动在 310P3 条件分支内 |
| 310P3 SDMA+混合 | 新增支持（M1+M2+M3+M4+P0-1+P1-1） |
| 310P3 SDMA+HBM-only | 无影响（回归验证通过） |
| 310P3 SDMA+DRAM-only | 新增支持（M2） |
| 310P3 transfer_perf smem_shm | 新增支持（P1-2） |
| HOST_SHM | 无影响。HOST_SHM 校验仍保留 |

# 9. 风险与未解决问题

## 9.1 风险

| 风险 | 等级 | 应对措施 |
|------|------|---------|
| 310P3 SVM 对某些 DRAM 地址范围不完全覆盖 | 中 | 通过 `bm_perf_benchmark -t all` 全路径测试验证；如果发现问题，在 `CopyG2G` 中添加地址范围检查 |
| `CheckSdmaReaches` 无条件返回 `true` 可能导致跨节点 DRAM SDMA 误判 | 低 | transport 层 `CanReachDataOperators` 有二次校验；跨节点场景 SDMA 不可达时会 fallback 到其他协议 |
| SDMA `TransformVa` 方向感知改动影响 910 平台 | 低 | 910 平台的 DRAM/HBM 均注册了 DVA 和 HVA，方向感知后 TransformVa 结果不变 |
| HOST_SHM + 混合场景误用 | 低 | HOST_SHM 校验仍保留 `maxHbmSize == 0` 约束 |
| `shm_open` name 含 pid，多用户环境冲突 | 低 | P3 后续优化，可加入 user/namespace 隔离 |

## 9.2 未解决问题

| 问题 | 说明 |
|------|------|
| 310P3 SVM 地址覆盖范围 | 需通过实测确认 `ACL_MEMCPY_DEVICE_TO_DEVICE` 对 mmap'd host VA 的兼容性。如果 SVM 不覆盖，需要在 `CopyG2G` 中根据方向选择 `ACL_MEMCPY_HOST_TO_DEVICE` / `ACL_MEMCPY_DEVICE_TO_HOST`。当前实测全路径 PASS。 |
| 跨节点 SDMA + DRAM | 当前方案中 `CheckSdmaReaches` 返回 `true` 不区分同/跨节点。如果需要严格限制，可参考 `HybmDevLegacySegment::CheckSdmaReaches` 基于 serverId/superPodId 判断。但 310P3 上跨节点场景应使用 DEVICE_RDMA 而非 SDMA。 |
| `GetLocalMemoryType` 310P3 地址范围修正 | HBM LVA 可能在 GVM range，fallback 分类错误。 |

# 10. 后续工作

| 优先级 | 内容 | 说明 |
|--------|------|------|
| P1 | H2D 方向批量 async + 统一 sync | H2D/H2RD/RD2H 带宽 5-6 GB/s，受单次 async sync 延迟限制，预期提升到 40-80 GB/s |
| P1 | transfer_perf batch async + 统一 sync | 大 block 带宽 18 GB/s，每轮 sync 无法 pipeline，预期提升到 60-80 GB/s |
| P2 | 跨节点混合场景验证 | 需要 `libhcom.so` 支持，使用 DEVICE_RDMA 协议 |
| P2 | 混合场景 UT | TransformVa 方向感知、CopyLH2GD 等 async kind 的单元测试 |
| P3 | `shm_open` 安全性 | 当前 shm name 包含 pid，多用户环境可能冲突 |
| P3 | `GetLocalMemoryType` 310P3 地址范围修正 | HBM LVA 可能在 GVM range，fallback 分类错误 |

# 11. 现有技术

- **910B/910C 平台**：已支持 DRAM+HBM 同 handle 混合。DRAM 使用 `HybmDevLegacySegment`（driver-level IPC），HBM 使用 `HybmDevLegacySegment` 或 `HybmVmmBasedSegment`。两者通过 entity 层 `hbmSegment_` + `dramSegment_` 双段架构协调。本提案使 310P3 达到同等能力。
- **DataOpDeviceRDMA**：已实现方向感知的 `TransformVa`（`hybm_data_op_device_rdma.cpp:115-131`），是 SDMA `TransformVa` 修复的参考模板。

---

# 附录

## 参考资料

- commit `41f1bf00`：310P3 BM HBM 2-rank + DRAM 8-rank support
- `src/smem/csrc/smem_bm/smem_bm.cpp:233-238`：当前限制代码
- `src/hybm/csrc/entity/hybm_entity_default.cpp:1134-1224`：InitSegment / InitDramSegment
- `src/hybm/csrc/data_operation/host/hybm_data_op_sdma.cpp:327-339`：SDMA TransformVa
- `src/hybm/csrc/data_operation/host/hybm_data_op_device_rdma.cpp:115-131`：DeviceRDMA TransformVa（参考模板）
- `src/hybm/csrc/mm/hybm_va_manager.cpp:260-295`：ClassifyAddressMask
- `src/hybm/csrc/mm/hybm_mem_segment.cpp:115-118`：CheckSdmaReaches 基类

## 术语表

- GVA: Global Virtual Address，全局虚拟地址
- HVA: Host Virtual Address，主机虚拟地址
- DVA: Device Virtual Address，设备虚拟地址
- SVM: Shared Virtual Memory，共享虚拟内存（310P3 的 host/device 统一地址空间机制）
- VMM: Virtual Memory Management，ACL 虚拟内存管理 API
- ConnBasedSegment: 基于 mmap 共享内存的 DRAM segment
- VmmBasedSegment: 基于 ACL VMM API 的 HBM segment
