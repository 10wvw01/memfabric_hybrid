# 310P3 MemFabric 实施问题与解决方案记录

> 日期：2026-09-01
> 硬件：Atlas 300I Duo (310P3) × 4 卡 = 8 dev / CANN 9.1.0 / aarch64

---

## 1. SoC 识别缺失

**现象**：310P3 设备在 `hybm_init` 阶段被 VA Manager 拒绝（`ASCEND_UNKNOWN`），所有 segment 创建失败。

**根因**：`AscendSocType` 枚举只有 `ASCEND_910b/910C/950`，`GetAscendSocType()` 用字符串匹配 `aclrtGetSocName()` 返回值，310P3 的 SoC name `Ascend310P3` 不在匹配列表中。

**解决方案**：
- `hybm_define.h`：枚举增加 `ASCEND_310P1`、`ASCEND_310P3`
- `dl_acl_api.cpp`：`GetAscendSocType()` 增加 `Ascend310P3`/`Ascend310P1` 字符串匹配
- 同时修复 pre-existing bug：`ASCEND_910B`（大写 B）应为 `ASCEND_910b`（小写 b），原代码依赖 `-fpermissive` 通过编译但运行时 `SOC_TYPE` 恒为 0

---

## 2. GVA Version 检测路径错误

**现象**：`hybm_init` 报 "driver version is empty"，GVA version 检测失败。

**根因**：CMake 构建 `BUILD_UT=ON` 时定义 `UT_ENABLED` 宏，`CastDriverVersion` 使用 `ASCEND_HOME_PATH`（如 `/usr/local/Ascend/cann-9.1.0`）拼接 `/driver/version.info`，但实际驱动版本信息在 `/usr/local/Ascend/driver/version.info`。

**解决方案**：构建时使用 `cmake -DBUILD_UT=OFF`（`UT_ENABLED` 仅用于 UT 场景）。

**长期修复方向**：`CastDriverVersion` 应优先读 `/etc/ascend_install.info` 中的 `Driver_Install_Path_Param=`（生产路径），`UT_ENABLED` 只影响 UT mock 路径。

---

## 3. SDID/serverId/superPodId 不可用

**现象**：`InitDeviceInfo` 在 `RtGetDeviceInfo(INFO_TYPE_SDID)` 处返回 207000 后直接失败退出。

**根因**：310P3 不支持 `INFO_TYPE_SDID`、`INFO_TYPE_SERVER_ID`、`INFO_TYPE_SUPER_POD_ID` 三个 info type。

**解决方案**：将这三处查询从 `ERROR + return` 改为 `WARN + fallback`（sdid=0, serverId=0, superPodId=invalidSuperPodId）。

---

## 4. HAL 层 `halMemCreate` 不支持 HBM 类型

**现象**：`HalMemCreate(MEM_DEV_SIDE, MEM_HBM_TYPE)` 返回 ret=6（OOM），但 `MEM_HOST_SIDE` 和 `MEM_DDR_TYPE` 成功。

**根因**：310P3 的 HAL 层 `halMemCreate` 不支持 `MEM_DEV_SIDE + MEM_HBM_TYPE` 组合。能力矩阵实测 ACL wrapper `aclrtMallocPhysical(ACL_MEM_P2P_HUGE)` 成功。

**解决方案**：`MallocFromDevice` 中 310P3 走 ACL wrapper 路径（`aclrtMallocPhysical`），不走 HAL 路径。整个 VMM segment 的分配/导入/映射/释放全部改用 ACL API。

---

## 5. `aclrtPhysicalMemProp` 结构体 ABI 不匹配

**现象**：`aclrtMallocPhysical` 返回 100000（参数错误）。

**根因**：HYBM 定义的 `aclrtPhysicalMemProp` 字段顺序为 `{handleType, allocationType, location, memAttr}`，但 CANN 实际结构体为 `{handleType, allocationType, memAttr, location}`。字段顺序不匹配导致 ACL runtime 读取到错误的 location/memAttr 值。

**解决方案**：修正字段顺序为 `{handleType, allocationType, memAttr, location, reserve}`，与 CANN ABI 一致。

---

## 6. 同卡跨 die `EnablePeerAccess` 返回 507899

**现象**：同卡双 die（dev0/dev1）的 `aclrtDeviceEnablePeerAccess` 返回 507899（`ACL_ERROR_RT_DRV_INTERNAL_ERROR`）。

**根因**：310P3 同卡双 die 共享 PCIe 设备，内核 `devdrv_p2p_para_check` 显式拒绝同芯片 P2P。ACL runtime 的 `aclrtMemImportFromShareableHandle` 内部调用 `EnableP2PWithOtherDevice`，检查当前 ctx 设备与导出方设备的 P2P，同芯片被拒。

**解决方案**：ctx 技巧 — import 前先 `AclrtSetDevice(exporterDev)` 切换到导出方 context，使 `phyDeviceId == peerPhyDeviceId`，跳过 P2P 检查，然后 `AclrtSetDevice(localDev)` 切回。

```cpp
if (sameCard) {
    AclrtSetDevice(exporterDev);  // ctx trick
}
AclrtMemImportFromShareableHandle(shareable, devId, &handle);
if (sameCard) {
    AclrtSetDevice(deviceId_);  // restore
}
```

---

## 7. `INFO_TYPE_PHY_CHIP_ID` 返回 per-die 值

**现象**：`IsSameCard` 误判同卡为不同卡，导致同卡 import 错误地调用 `EnablePeerAccess`。

**根因**：310P3 的 `INFO_TYPE_PHY_CHIP_ID` 返回 per-die 值（dev0=0, dev1=1），而非 per-card 值。dev0 和 dev1 在同一张卡上但 `physicalChipId` 不同。

**解决方案**：310P3 改用 `deviceId / 2` 作为同卡判断（dev0/dev1→card0, dev2/dev3→card1, ...）。

---

## 8. `HalMemAddressReserve` 与 `AclrtMapMem` 不兼容

**现象**：HBM copy 数据验证失败 — `AclrtMapMem` 映射后 `rtMemcpyAsync(D2D)` 写入的数据不是对端的物理内存。

**根因**：`HalMemAddressReserve` 是 HAL 层接口，预留的地址空间不被 ACL runtime 识别。`AclrtMapMem` 需要 `AclrtReserveMemAddress` 预留的地址空间。

**解决方案**：
- `ReserveMemorySpace`：310P3 改用 `AclrtReserveMemAddress`（只预留本地 `maxSize`，不是全部 `totalLvaSize`）
- `ReserveLva`：每个 imported segment 独立调 `AclrtReserveMemAddress` 预留新 VA
- `UnReserveMemorySpace`：改用 `AclrtReleaseMemAddress`

---

## 9. `aclrtPhysicalMemProp` 的 `location.id` 用 `logicDeviceId` 而非 `deviceId`

**现象**：`aclrtMallocPhysical` 返回 100000。

**根因**：`AllocLocalMemory` 调用 `MallocFromDevice(size, logicDeviceId_, &handle)`，但 `aclrtMallocPhysical` 的 `prop.location.id` 需要是 `aclrtSetDevice` 时使用的设备号（`deviceId_`），不是逻辑设备号。

**解决方案**：310P3 路径改为 `prop.location.id = deviceId_`。

---

## 10. HOST_SHM `localDRAMSize != maxDramSize` 导致 GVA 地址不匹配

**现象**：DRAM 2-rank 测试中 `Direction mismatch`，peer GHA 被识别为 `LOCAL_HOST` 而非 `GLOBAL_HOST`。

**根因**：`smem_bm_ptr_by_mem_type` 用 `maxDRAMSize` 计算偏移（1 GiB），但 `HybmHostShmSegment` 用 `localDRAMSize`（256 MB）预留 GVA 槽位。两者不一致导致 `peerGva` 地址不在 VA manager 注册的范围内。

**解决方案**：在 `MemEntityDefault::CheckOptions` 中新增 HOST_SHM 校验 `localDRAMSize == maxDramSize`。

---

## 11. `RtMemcpyAsync` vs `AclrtMemcpyAsync`

**现象**：HBM G2G D2D copy 部分 word 数据不一致。

**根因**：`CopyG2G` 的 310P3 路径使用 `RtMemcpyAsync`（runtime 层 `rtMemcpyAsyncWithoutCheckKind`），但测试用的是 `aclrtMemcpyAsync`（ACL 层）。两者行为可能不同。

**解决方案**：310P3 路径统一改用 `AclrtMemcpyAsync` + `ACL_MEMCPY_DEVICE_TO_DEVICE`。

---

## 12. `localVirtualBase` 超出预留范围

**现象**：rank 1 的 `allocAddr` 超出了 `AclrtReserveMemAddress` 预留的 1GB 范围。

**根因**：`AllocLocalMemory` 中 `localVirtualBase = localVirtualAddress_ + maxSize * rankId`，但 310P3 的 `ReserveMemorySpace` 只预留了 `maxSize`（1GB），不是 `totalLvaSize`（2GB）。rank 1 的 `localVirtualBase` 偏移 1GB 后超出预留范围。

**解决方案**：310P3 路径中 `localVirtualBase = localVirtualAddress_`（不偏移 `rankId`），因为每个 rank 的 `ReserveMemorySpace` 独立预留自己的 1GB VA。

---

## 13. `SmemBmEntryOptions::rankSize` 未正确设置

**现象**：`TopologyPreflight` 中 `rankSize:0` 和 `worldSize:0`，拓扑预检形同虚设。

**根因**：`smem_bm_entry_manager.cpp` 中 `SmemBmEntryOptions opt{id, config_.rankId, config_.dynamicWorldSize, ...}` — 第三个参数 `dynamicWorldSize` 是 bool（false=0），被用作 `rankSize`。

**解决方案**：改为 `SmemBmEntryOptions opt{id, config_.rankId, worldSize_, ...}`。

---

## 14. `HostDataOpSDMA::Wait` 调用 `GetThreadHybmStream` 导致资源耗尽

**现象**：100 次生命周期循环中 loop 63 `create2` 失败，后续全部失败。`HybmStream init failed: 3`（`HalSqCqAllocate` 返回 3）。

**根因**：
1. `HostDataOpSDMA::Wait` 调用 `GetThreadHybmStream`，但 310P3 的 copy 操作已在内部通过 `AclrtSynchronizeStream` 同步，不需要 HybmStream。
2. `HostDataOpSDMA::CleanUp` 调用 `DestroyAllThreadHybmStream`，在 `smem_bm_destroy` → `GroupLeave` → peer `LeaveHandle` → `RemoveImported` → `CleanUp` 中被触发。listen 线程的 HybmStream 被销毁，下次 create → `Wait` → `GetThreadHybmStream` 重新创建。多次创建后 SQ/CQ 硬件资源耗尽。

**解决方案**：
- `Wait`：310P3 直接返回 `BM_OK`（copy 已内部同步）
- `CleanUp`：310P3 跳过 `DestroyAllThreadHybmStream`（避免销毁共享资源）

---

## 15. BM entity id 超出范围

**现象**：lifecycle 测试 loop 63（id=64）`create2` 返回 `SM_INVALID_PARAM` (-2000)。

**根因**：`CreateEntryById` 检查 `id < HYBM_ENTITY_ID_SEGMENT_SIZE`（=64），`loop + 1 = 64` 超出范围 [0, 63]。

**解决方案**：测试中用 `(loop % 63) + 1` 循环复用 id。这是测试问题，非代码 bug。

---

## 16. `smem_bm_join` 后 peer JoinHandle 异步延迟

**现象**：`smem_bm_join` 返回后立即 copy 失败（`Direction mismatch`，peer GVA 未注册）。

**根因**：`GroupJoin` 在 self-join 后就返回，peer 的 `JoinHandle`（含 import + mmap）在 listen 线程中异步执行。如果 copy 在 peer import 完成前执行，VA manager 中没有 peer 的 import info。

**解决方案**：测试中 `smem_bm_join` 后加 `MPI_Barrier` + `sleep(1)` 确保 peer import 完成。这是 group engine 的异步行为，非 310P3 特有问题。

---

## 17. 4-rank 跨卡 import 507899（未解决）

**现象**：4-rank HBM 测试中跨卡 `aclrtMemImportFromShareableHandle` 返回 507899。

**根因**：`JoinHandle` 中同卡 import 的 ctx 技巧（`AclrtSetDevice(exporterDev)`）与跨卡 import 存在 ACL runtime 的 P2P 状态竞争。简化测试（串行 import、同卡优先）全部通过，HYBM 4-rank 异步时序下失败。

**已尝试的方案**：
- imports_ 排序（同卡优先）— 部分有效，但 `JoinHandle` 是 per-peer 的，每次只处理一个 peer
- `pendingImports_` defer + post-join retry — `GroupJoin` 在 self-join 后返回，无法等待所有 peer JoinHandle 完成
- 全局 mutex 序列化 — 无效
- `CreateContext` + `SetCurrentContext` — 无效
- 直接 `dlsym` 调 `aclrtMemImportFromShareableHandle` — 同进程中仍 507899

**简化测试结论**：纯 ACL API + `hybm_init` + `smem_bm_create2(HBM)` + `smem_bm_join(2-rank sub-pool)` + 同卡 import+map+setaccess → 跨卡 import 全部通过。根因在 HYBM 4-rank `JoinHandle` 的异步并发时序。

**当前状态**：2-rank 同卡完整可用，4-rank 跨卡需进一步排查 ACL runtime 内部 P2P 状态管理。
