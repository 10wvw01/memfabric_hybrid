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

## 17. 4-rank 跨卡 import 507899（已解决）

**现象**：4-rank HBM 测试中跨卡 `aclrtMemImportFromShareableHandle` 返回 507899（`ACL_ERROR_RT_DRV_INTERNAL_ERROR`）。同卡 import 成功，跨卡 import 全部失败。

**复现路径**：
- `310p-p0-verify/p3_min_repro.c`（4-rank 纯 ACL 串行，用 CANN 头文件）→ **PASS**
- `examples/cross_card_import_test/main.cpp` Phase 1（4-rank 纯 ACL 串行，用魔术数 4）→ **FAIL 507899**
- 生产代码 `hybm_vmm_based_segment.cpp` 的 BM `MallocFromDevice` → **FAIL 507899**（`hybm_mmap failed, result: -1`）

### 根因

**根因一：示例文件魔术数错误**

`cross_card_import_test/main.cpp:91` 使用了错误的 `memAttr` 魔术数：

```cpp
prop.memAttr = static_cast<aclrtMemAttr>(4); /* ACL_MEM_P2P_HUGE */
```

注释标注为 `ACL_MEM_P2P_HUGE`，但实际值 4 是 `ACL_HBM_MEM_HUGE`，**不是** `ACL_MEM_P2P_HUGE`（CANN 头文件值 16）。修复为 `ACL_MEM_P2P_HUGE` 后示例全部 PASS。

**根因二：生产代码自定义枚举与 CANN ABI 不匹配（核心根因）**

`src/hybm/csrc/under_api/dl_acl_api.h:188-193` 自定义了 `aclrtMemAttr` 枚举，跳过了 CANN `acl_rt.h:227-246` 中前面的 `ACL_DDR_MEM*` / `ACL_HBM_MEM*` 成员，从 0 开始编号：

```cpp
enum class aclrtMemAttr : uint32_t {
    ACL_MEM_NORMAL = 0,      // CANN 中 = 12
    ACL_MEM_HUGE = 1,        // CANN 中 = 13
    ACL_MEM_P2P_NORMAL = 3,  // CANN 中 = 15
    ACL_MEM_P2P_HUGE = 4,    // CANN 中 = 16  ← 实际传给 ACL 的值是 4，等于 CANN 的 ACL_HBM_MEM_HUGE
};
```

生产代码 `hybm_vmm_based_segment.cpp:228` 虽然写的是命名常量：

```cpp
prop.memAttr = static_cast<uint32_t>(aclrtMemAttr::ACL_MEM_P2P_HUGE);
```

但因为枚举定义错误，实际传给 ACL runtime 的值是 **4**（= CANN 的 `ACL_HBM_MEM_HUGE`，非 P2P），而不是 **16**（= CANN 的 `ACL_MEM_P2P_HUGE`，P2P）。

### 实测验证（2026-09-03）

在 `p3_store_repro.cpp` 中同时打印 CANN 头文件值和 BM 枚举值（`smem_set_log_level(1)` 开启 INFO 日志），4-rank 运行确认：

```
[R2] CANN header ACL_MEM_P2P_HUGE=16
[HYBM hybm_vmm_based_segment.cpp:233] BM enum aclrtMemAttr::ACL_MEM_P2P_HUGE=4 handleType=0 location.id=2 location.type=1
```

| 来源 | `ACL_MEM_P2P_HUGE` 实际值 | 说明 |
|------|---------------------------|------|
| CANN `acl_rt.h:244` | **16** | 正确的 P2P 内存属性 |
| 项目 `dl_acl_api.h:192` | **4** | 错误，等于 CANN 的 `ACL_HBM_MEM_HUGE` |
| 示例 `main.cpp:91`（已修复） | **16** | 修复后用 CANN 头文件常量 |

### 失败机制

- `ACL_HBM_MEM_HUGE`（值 4）分配的是**非 P2P 的本地 HBM 内存**。
- 同卡（如 dev0/dev1 在同一 NPU）共享 HBM，所以同卡 import 成功。
- 跨卡（如 dev0→dev2 在不同 NPU 上）需要 **P2P 内存属性**才能被 `aclrtMemImportFromShareableHandle` 接受。非 P2P 内存跨卡 import 被驱动拒绝，返回 507899。

### CANN aclrtMemAttr 枚举对照

| 枚举值 | CANN 数值 | 项目自定义值 | 是否匹配 |
|--------|-----------|-------------|----------|
| `ACL_DDR_MEM` | 0 | — | — |
| `ACL_HBM_MEM` | 1 | — | — |
| `ACL_HBM_MEM_HUGE` | **4** | — | — |
| `ACL_MEM_NORMAL` | 12 | 0 | ❌ |
| `ACL_MEM_HUGE` | 13 | 1 | ❌ |
| `ACL_MEM_P2P_NORMAL` | 15 | 3 | ❌ |
| `ACL_MEM_P2P_HUGE` | **16** | **4** | ❌ |

### 解决方案

**示例文件**（已修复）：`cross_card_import_test/main.cpp:91` 的 `static_cast<aclrtMemAttr>(4)` 改为 `ACL_MEM_P2P_HUGE`。

**生产代码**（待修复）：`src/hybm/csrc/under_api/dl_acl_api.h:188-193` 的自定义 `aclrtMemAttr` 枚举值改为与 CANN `acl_rt.h` 一致：

```cpp
enum class aclrtMemAttr : uint32_t {
    ACL_MEM_NORMAL = 12,
    ACL_MEM_HUGE = 13,
    ACL_MEM_P2P_NORMAL = 15,
    ACL_MEM_P2P_HUGE = 16,
};
```

修复后示例所有 5 个 Phase 全部 PASS：

| Phase | 场景 | 修复前 | 修复后 |
|-------|------|--------|--------|
| Phase 1 | 主线程，无 smem_bm | FAIL 507899 | ✅ PASS |
| Phase 2 | smem_bm_init + create2 后 | FAIL 507899 | ✅ PASS |
| Phase 3 | smem_bm_join 后，主线程 | FAIL 507899 | ✅ PASS |
| Phase 4 | smem_bm_join 后，子线程 | FAIL 507899 | ✅ PASS |
| Phase 5 | 预启用 P2P 后，子线程 | FAIL 507899 | ✅ PASS |

### 经验教训

1. CANN 枚举值不要用魔术数，必须用命名常量。
2. 项目自定义的 CANN 类型/枚举必须与 CANN 头文件完全一致，否则会因 ABI 不匹配导致运行时行为异常。
3. `aclrtMemAttr` 枚举中 `ACL_HBM_MEM_HUGE`(4) 与 `ACL_MEM_P2P_HUGE`(16) 语义完全不同，前者是本地 HBM，后者才是可跨卡 P2P 内存。
4. 之前 memory 第 17 节中分析的"异步时序"、"listen 线程竞争"等方向全是误导，根因就是这个枚举值错误。

---

## 18. BM TopologyPreflight 配对算法错误

**现象**：8-rank HBM 测试 preflight 计算每卡 P2P slot 数为 16，但实际应为 12。4-rank 碰巧通过（4≤8）。

**根因**：原算法用 `phyChipId`（per-die 编号 0/1）分组配对，跨组配对数 = 4×4 = 16。但 `phyChipId` 不是卡标识，只是 chip 在卡内编号。

**修正一：改用 `devId/2` 判断同卡**（与 `hybm_entry.cpp:222` 的 `hybm_enable_peer_access` 一致）。

**修正二：计费模型从"跨组配对总数"改为"每卡入向 P2P 连接数"**。HAL 文档（`ascend_hal_base.h:1665-1715`）明确：310P3 Duo 两 die **共享 8 个入向 P2P 授权槽位**（per-card inbound），不是 per-device。

正确算法：对每个 rank 的 devId，计算跨卡连接数（其他卡的设备数之和），累加到该卡。全连接 N-rank、每卡 2 die：
```
每卡入向 = 2 × (N - 2)    （同卡 2 个 die 各连接 N-2 个跨卡 peer）
N=4: 2×2=4 ≤ 8  ✅     N=6: 2×4=8 ≤ 8  ✅(饱和)     N=8: 2×6=12 > 8  ❌
```

与 `310p-capability_matrix/07_d2d_matrix` 实测数据完全吻合：`T20/07: r0 六个 PEER + r1 六个 PEER = 12 > 8`。

---

## 19. AllocLocalMemory 循环越界（`rankCnt * 2`）

**现象**：8-rank HBM 测试 `AclrtMemSetAccess(RW) dev:8~15 failed:507899`，这些设备不存在。

**根因**：`hybm_vmm_based_segment.cpp:365` 原代码 `for (uint32_t dev = 0; dev < options_.rankCnt * 2; dev++)`，310P3 Duo 上 `rankCnt` 已等于设备数（deviceId = rankId），`* 2` 导致遍历不存在的设备 8~15。

**解决方案**：
1. `rankCnt * 2` → `rankCnt`（去掉多余的乘 2）
2. 新增 `dev / 2U == deviceId_ / 2U` 同卡跳过（同卡 die 不需要 SetAccess）

---

## 20. BM TestHbm 多写者并发竞态

**现象**：4-rank HBM 测试数据验证失败（G2H/G2L verify FAIL at word 0）。

**根因**：原测试逻辑每个 rank 遍历所有 peer 并发执行 L2G 写入同一目标 GVA，多个 rank 同时写同一地址导致最后写入者覆盖前面的数据。2-rank 恰好 1:1 配对无竞态，4-rank 有 N-1 个 rank 同时写同一 GVA。

**解决方案**：改为 round-robin 串行化（参考 `TestDram` 的模式）：每轮指定一个 writer 写一个 target，4 个 phase 串行执行（L2G → G2H → G2L → G2G）。循环前每个 rank 先用 L2G 初始化自己的 GVA，确保 G2G 阶段 writer 的 GVA 有真实数据。

---

## 21. SHM 元数据初始化失败（`HalMemCreate` 不支持 310P3）

**现象**：`smem_shm_init` 调用 `hybm_init` 设置 `HYBM_FLAG_INIT_SHMEM_META`，触发 `HybmModernInitMetaGva` → `HalMemCreate` 返回错误 6（`DRV_ERROR_NOT_SUPPORT`）。

**根因**：SHM 模式在 `smem_shm_init` 时设置 `HYBM_FLAG_INIT_SHMEM_META`（bit 63），初始化设备侧元数据区域（32MB，位于 `SVM_END_ADDR - 1GB` 处）。该区域用于存储 rank/rankSize/symmetricSize/QP/extra context 等信息，供 device kernel 通过固定地址读取。

原始 `HybmModernInitMetaGva` 用 HAL 层 API 分配设备 HBM 物理内存：
```
HalMemAddressReserve  → 预留 VA（1GB）
HalMemCreate(MEM_HBM_TYPE)  → 分配物理内存
HalMemMap              → 映射 VA→物理内存
```

310P3 的 HAL 层 `halMemCreate` 不支持 `MEM_DEV_SIDE + MEM_HBM_TYPE` 组合（与第 4 节同一根因），返回错误 6。BM 模式不设置 `HYBM_FLAG_INIT_SHMEM_META`，跳过此路径，所以不受影响。

**解决方案**：`HybmModernInitMetaGva` 增加 310P3 ACL VMM 路径分支：

| 操作 | 原路径（HAL） | 310P3 路径（ACL VMM） |
|------|-------------|---------------------|
| 预留 VA | `HalMemAddressReserve` | `AclrtReserveMemAddress` |
| 分配物理内存 | `HalMemCreate(MEM_HBM_TYPE)` | `AclrtMallocPhysical(ACL_MEM_P2P_HUGE)` |
| 映射 | `HalMemMap` | `AclrtMapMem` |
| 取消映射 | `HalMemUnmap` | `AclrtUnmapMem` |
| 释放物理 | `HalMemRelease` | `AclrtFreePhysical` |
| 释放 VA | `HalMemAddressFree` | `AclrtReleaseMemAddress` |

`ACL_MEM_P2P_HUGE`（值 16，2MB 粒度）是能力矩阵实测验证过的唯一可用 memAttr（与第 4/17 节一致）。

`hybm_uninit` 的清理路径同步修改：310P3 用 `AclrtUnmapMem` + `AclrtFreePhysical` + `AclrtReleaseMemAddress`，其他 SoC 仍用 HAL 路径。

---

## 22. SHM GVA 不能用 host 侧 `aclrtMemcpy` 直接访问

**现象**：SHM `smem_shm_create` 成功返回 GVA `0x280040000000`，但 `aclrtMemcpy(gva, ...)` SIGSEGV（段错误）。

**根因**：SHM 返回的 GVA 是 **fabric 全局地址**，不是设备 VA。310P3 上 GVA 和设备 VA 是不同地址空间：

```
GVA (fabric):       0x280040000000  ← smem_shm_create 返回的
deviceVa (本地):     0x17ffc0000000  ← AclrtReserveMemAddress 预留的
imported deviceVa:   0x12c140000000  ← AclrtMapMem 映射的 peer HBM
```

`aclrtMemcpy` 需要设备 VA，不认识 GVA。GVA→DVA 翻译由 VaManager 管理，`aclrtMemcpy` 不做这个翻译。

在 910B/GVA V4 上 GVA 可能直接映射到设备 VA 空间（host 可寻址），但 310P3 的 GVA 是独立的 fabric 地址空间，只有 AI core MTE 引擎（通过设备 kernel 内的 `DataCopyPad`）才能直接用 GVA 寻址。

**结论**：SHM 的数据访问验证需要 device kernel（MTE `DataCopyPad`），不能用 host 侧 `aclrtMemcpy`。当前测试验证 SHM 的 **生命周期和控制操作**（create/destroy/barrier/topology/extra_context），数据验证留给后续 device kernel 测试。

---

## 23. SHM `topology_can_reach` 返回 MTE 不可达

**现象**：SHM 2-rank/4-rank 测试中 `smem_shm_topology_can_reach` 返回 `reachInfo=0x0`（MTE=no）。

**根因**：`smem_shm_entry.cpp:299-331` 的 `GetReachInfo` 调用 `hybm_entity_reach_types`，该函数查询 entity 的可达性。310P3 上 entity 初始化时 `bmDataOpType=1`（MTE），但 `hybm_entity_reach_types` 内部可能依赖 transport/SDMA 等下层能力判断，而 SHM 模式不初始化 transport 和 data operator（`InitDataOperator` 跳过 AI_CORE_INITIATE 类型）。

**状态**：待排查。不影响 pool 生命周期和控制操作，只影响 `topology_can_reach` 查询结果。

---

## 24. SHM `topology_can_reach` 返回 MTE 不可达（已解决）

**现象**：SHM 2-rank/4-rank 测试中 `smem_shm_topology_can_reach` 返回 `reachInfo=0x0`（MTE=no）。

**根因**：`CanReachDataOperators`（`hybm_entity_default.cpp:1300`）中 MTE 可达性绑定在 `HYBM_DOP_TYPE_SDMA` 分支内。SHM 模式 `bmDataOpType` 只配 `HYBM_DOP_TYPE_MTE`（不含 SDMA），所以不进入该分支。这是原始设计逻辑（非 310P3 回归）：MTE 可达性在所有平台都依赖 SDMA 标志。

**解决方案**：`smem_shm_create` 中对 310P3 额外设置 `HYBM_DOP_TYPE_SDMA`，让现有判断逻辑生效。`bmDataOpType = HYBM_DOP_TYPE_MTE | HYBM_DOP_TYPE_SDMA`。

无副作用验证：
- `InitDataOperator`：`AI_CORE_INITIATE` 类型直接跳过
- `InitTransManager`：`composeTransFlags` 不含 `HYBM_DOP_TYPE_SDMA`，不会触发 transport 初始化
- `LoadExtendLibrary`：只检查 `HYBM_DOP_TYPE_AIV_SDMA`，不检查 `HYBM_DOP_TYPE_SDMA`

修复后 `topology_can_reach` 返回 `reachInfo=0x3 (MTE=yes)`。

---

## 25. SHM device kernel `DataCopyPad` 在 310P3 不支持

**现象**：编译 SHM device kernel（`shm_kernel.cpp`）时，CANN 编译器报 `DataCopyPad is an unsupported API on current device`。

**根因**：310P3 是 dav-m200 架构（推理卡），`DataCopyPad` 是 dav-m220（训练卡 910B）的 MTE API。`smem_shm_aicore_base_copy.h` 全部使用 `DataCopyPad`，310P3 不支持。

**解决方案**：kernel 改用 `AscendC::DataCopy`（非 Pad 版本），`DataCopy` 在 310P3 上可用。

---

## 26. SHM device kernel `DataCopy` 无法通过 GVA 跨 rank 访问（核心限制）

**现象**：kernel 编译运行成功，但 `DataCopy` 操作 GVA 地址（`gva + symSize * targetRank`）时，数据未到达 peer rank——output 读取到的是自己的数据，不是 peer 写来的数据。

**根因**：310P3 上 GVA 和 device VA 是**完全分离的地址空间**：

```
rank 0 local:  GVA=0x280040000000  deviceVa=0x17ffc0000000
rank 1 local:  GVA=0x280080000000  deviceVa=0x17ffc0000000  (相同 deviceVa，不同进程)
rank 0 import rank1: GVA=0x280080000000  deviceVa=0x12c140000000 (imported)
```

- 910B（GVA V4）：`ReserveMemorySpace` 用 `HalMemAddressReserve` 预留 `totalLvaSize`（所有 rank 的 GVA 窗口），GVA 直接映射到 device VA 空间，kernel `DataCopy` 可以直接用 GVA
- 310P3：每个 rank 的 local VA 只预留 `maxSize`（自己的），imported peer 的 VA 是独立的 `AclrtReserveMemAddress` 块。GVA (`0x2800...`) 与 device VA (`0x17ffc...`/`0x12c14...`) 完全分离。device kernel 的 `DataCopy` 只认识 device VA，不认识 fabric GVA

**影响**：SHM 模式的核心价值是 device kernel（MTE/AIV）通过 GVA 直接访问远端 HBM。310P3 上 GVA 无法被 device kernel 直接寻址，SHM 的设备侧数据访问能力受限。

**当前验证状态**：
- SHM pool 生命周期（create/destroy）：PASS
- 控制操作（barrier/topology_can_reach/extra_context）：PASS
- P2P preflight：PASS
- Device kernel 通过 GVA 跨 rank 数据访问：**不支持**（GVA ≠ device VA）

**潜在解决方向**（未验证）：
1. Device kernel 中使用 imported device VA 而非 GVA（需要将 imported DVA 传入 kernel，破坏 GVA 对称性）
2. 探索 310P3 是否有 fabric address translation 机制让 device kernel 直接寻址 GVA
3. 使用 BM 模式的 host-initiated SDMA（已验证可用）替代 SHM 的 device-initiated MTE

---

## 27. TRANS 模式 external HBM 共享路径验证

> 日期：2026-09-04
> 验证文件：`310p-capability_matrix/17_trans_verify/`

**背景**：TRANS 模式注册用户已分配的 `aclrtMalloc` 设备内存进行跨进程传输。当前使用 `HybmDevUserLegacySegment`（runtime IPC：`RtIpcSetMemoryName`/`RtIpcOpenMemory`），310P3 同卡 die import 返回 507899。

**根因**：runtime 层 IPC `RtIpcOpenMemory` 内部调用 `EnableP2PWithOtherDevice`，同卡 die 被 `devdrv_p2p_para_check` 拒绝（507899）。ACL 层 IPC `aclrtIpcMemImportByKey(DEFAULT)` 不触发 P2P 前置检查。

**验证三条路径**：

| 路径 | Export API | Import API | 结果 |
|------|-----------|-----------|------|
| A. ACL IPC | `aclrtIpcMemGetExportKey` | `aclrtIpcMemImportByKey(DEFAULT)` | ✅ PASS — 直接接受 `aclrtMalloc` VA，同卡 die 不触发 P2P |
| B. VMM export | `aclrtMemExportToShareableHandle` | — | ❌ 不可用 — 需要 `aclrtDrvMemHandle`，`aclrtMalloc` 不返回句柄 |
| C. VMM baseline | `aclrtMallocPhysical` + Export | `ImportFromShareableHandle(ctx trick)` | ✅ PASS — 但需重新分配内存，破坏 TRANS "注册外部内存"语义 |

**结论**：TRANS 在 310P3 上应改用 ACL 层 IPC API（`aclrtIpcMemGetExportKey`/`aclrtIpcMemImportByKey`），替代 runtime 层 `RtIpcSetMemoryName`/`RtIpcOpenMemory`。跨卡场景用 `ENABLE_PEER_ACCESS` flag（自动建立双向 P2P）。

**实施方案**：修改 `HybmDevUserLegacySegment`，310P3 分支：
- Export：`AclrtIpcMemGetExportKey` 替代 `RtIpcSetMemoryName`（DISABLE_PID_VALIDATION）
- Import：`AclrtIpcMemImportByKey` 替代 `RtIpcOpenMemory`（同卡 DEFAULT / 跨卡 PEER_ACCESS）
- Close：`AclrtIpcMemClose` 替代 `RtIpcCloseMemory`
- 跳过 `RtSetIpcMemorySuperPodPid`（ACL IPC DISABLE_PID_VALIDATION 已处理）
- 跳过 `RtIpcDestroyMemoryName`（ACL IPC key 不需要显式销毁）

**验证结果**（2026-09-04）：
- 2-rank 同卡 die 对（dev0/dev1）：**ALL TESTS PASS**
  - 同步 write + verify：PASS
  - 异步 write_submit + stream sync + verify：PASS
  - deregister_mem：PASS
- TopologyPreflight：PASS（P2P 槽位 0 ≤ 8）
- ACL IPC import 成功，同卡 die 不触发 P2P 前置检查
- **4-rank 跨卡（dev0-3）：FAIL** — `AclrtIpcMemImportByKey(ENABLE_PEER_ACCESS)` 对 `aclrtMalloc` 外部内存返回 507899（`devdrv_p2p_para_check` 拒绝跨卡 P2P）

**额外修复**：
1. `dl_acl_api.h/cpp`：新增 ACL IPC API 封装（`AclrtIpcMemGetExportKey`/`AclrtIpcMemImportByKey`/`AclrtIpcMemClose`）
2. `hybm_entity_tag_info.cpp`：`AddRankTag` 空 tag 使用默认 tag `HYBM_DEFAULT_TAG_FOR_EMPTY`（TRANS 场景 tag 为空导致 `GetRank2RankOpType` 返回 0）
3. `smem_trans_entry.cpp`：`JoinImport` entity import 条件增加 `config_.role == SMEM_TRANS_BOTH`（BOTH 角色两端 role 相同导致 entity import 被跳过）

---

## 28. TRANS 4-rank 跨卡：VMM baseline 路径实现

> 日期：2026-09-04
> 验证文件：`examples/310p3_trans_test/`

**背景**：ACL IPC 路径（§27）在 2-rank 同卡 die 场景 PASS，但 4-rank 跨卡场景 `AclrtIpcMemImportByKey(ENABLE_PEER_ACCESS)` 对 `aclrtMalloc` 外部内存返回 507899。BM 模式 4-rank 跨卡能 PASS 是因为 VMM 路径（`AclrtMemImportFromShareableHandle` + `AclrtMemSetAccess`）正确处理跨卡 P2P。

**根因**：`aclrtMalloc` 分配的内存不返回 `aclrtDrvMemHandle`，无法通过 `AclrtMemExportToShareableHandle` 导出 shareable handle。跨卡 P2P 需要 VMM 层的 `AclrtMemSetAccess` 为每个远端 device 授权访问权限，ACL IPC 路径不提供这一机制。

**方案**：VMM baseline 路径 — 用户通过 `hybm_alloc_vmm_buffer` 分配 VMM 内存（`AclrtMallocPhysical` + `AclrtMapMem` + `AclrtMemSetAccess`），`RegisterMemory` 检测到 VMM handle 时走 `AclrtMemExportToShareableHandle` 导出，`ImportSliceInfo` 检测到 shareable handle 时走 `AclrtMemImportFromShareableHandle` + `AclrtMapMem` + ctx trick 导入。

**新增 API**：

| API | 位置 | 说明 |
|-----|------|------|
| `hybm_alloc_vmm_buffer` | `hybm.h` / `hybm_entry.cpp` | VMM 设备内存分配（`AclrtMallocPhysical` + `AclrtReserveMemAddress` + `AclrtMapMem` + `AclrtMemSetAccess` 为所有跨卡 device 授权） |
| `hybm_free_vmm_buffer` | `hybm.h` / `hybm_entry.cpp` | VMM 设备内存释放（`AclrtUnmapMem` + `AclrtReleaseMemAddress` + `AclrtFreePhysical`） |

**修改的文件**：

| 文件 | 修改内容 |
|------|---------|
| `hybm.h` | 新增 `hybm_alloc_vmm_buffer` / `hybm_free_vmm_buffer` 公共 API 声明 |
| `hybm_entry.cpp` | 实现 VMM buffer 分配/释放 + 全局 handle 注册表 `g_vmmHandleMap` + `HybmLookupVmmHandle` 查找函数 |
| `hybm_dev_user_legacy_segment.h` | `RegisterSlice` 增加 `shareableHandle` 字段；`UserHbmExportSliceInfo` 增加 `shareableHandle` 字段（8B，重新排列 padding 保持 200B）；新增 `ImportedVmmInfo` 结构和 `importedVmmInfo_` map；新增 `ImportVmmSlice`/`CleanupImportedVmm` 方法声明 |
| `hybm_dev_user_legacy_segment.cpp` | `RegisterMemory`：检测 VMM handle → `AclrtMemExportToShareableHandle` 导出；`ImportSliceInfo`：检测 shareableHandle ≠ 0 → `ImportVmmSlice`（`AclrtReserveMemAddress` + ctx trick + `AclrtMemImportFromShareableHandle` + `AclrtMapMem` + `AclrtMemSetAccess`）；`ReleaseSliceMemory`/`RemoveSliceInfo`/`CloseMemory`：VMM 路径 cleanup；`Mmap`/`Unmap`：310P3 返回 OK |

**数据流**（VMM baseline 路径）：

```
Export side:
  hybm_alloc_vmm_buffer(devId, worldSize, size)
    → AclrtMallocPhysical(prop{NONE,PINNED,DEVICE,P2P_HUGE}) → aclrtDrvMemHandle
    → AclrtReserveMemAddress → lva
    → AclrtMapMem(lva, size, 0, handle)
    → AclrtMemSetAccess(lva, RW, {dev}) for local + all cross-card devs
    → g_vmmHandleMap[lva] = handle

  smem_trans_register_mem(handle, lva, size)
    → HybmDevUserLegacySegment::RegisterMemory
    → HybmLookupVmmHandle(lva) → drvHandle
    → AclrtMemExportToShareableHandle(drvHandle, NONE, DISABLE_PID_VALIDATION) → uint64 shareable
    → MemSlice{gva, vAddress=lva, size}
    → HybmVaManager::AddVaInfo(gva, lva, lva, size, onlyGva=true)
    → registerSlices_[idx] = {slice, name="", shareableHandle}

  Export(slice, exInfo)
    → UserHbmExportSliceInfo{gvaOffset, address=lva, size, shareableHandle}
    → serialize → 200B blob

Import side:
  ImportSliceInfo(info)
    → deserialize → UserHbmExportSliceInfo{shareableHandle != 0}
    → ImportVmmSlice(sliceInfo, address)
      → AclrtSetDevice(localDev, force=true)  // thread-local ctx
      → AclrtReserveMemAddress → lva
      → if sameCard: AclrtSetDevice(exporterDev, force=true)  // ctx trick
      → AclrtMemImportFromShareableHandle(shareable, localDev, &aclHandle)
      → if sameCard: AclrtSetDevice(localDev)  // restore
      → AclrtMapMem(lva, size, 0, aclHandle)
      → AclrtMemSetAccess(lva, RW, {localDev})
      → importedVmmInfo_[name] = {lva, aclHandle}
    → MemSlice{gva, vAddress=lva, size}
    → HybmVaManager::AddVaInfoFromExternal(gva, lva, 0, size, rankId, importedRankId)

Copy path (unchanged):
  smem_trans_write(handle, localAddr, peerName, remoteAddr, size)
    → TransformAddr: remoteAddr → peerGVA (via remoteSlices_)
    → ClassifyAddressMask: localAddr=LOCAL_DEVICE, peerGVA=GLOBAL_DEVICE
    → direction = LOCAL_DEVICE_TO_GLOBAL_DEVICE
    → TransformVa(GVA→DVA): peerGVA → importedLva
    → aclrtMemcpyAsync(importedLva, size, localAddr, D2D, stream)
```

**验证结果**（2026-09-04）：

| 测试 | rankSize | 拓扑 | 结果 |
|------|----------|------|------|
| TestSyncWriteRead | 2 | 同卡 die 对 (dev0/dev1) | **PASS** — 4 轮 write+verify 全通过 |
| TestAsyncSubmit | 2 | 同卡 die 对 | **PASS** — async write_submit + stream sync + verify |
| TestDeregister | 2 | 同卡 die 对 | **PASS** |
| TestSyncWriteRead | 4 | 跨卡 2 卡 (dev0-3) | **PASS** — 4 轮 write+verify 全通过（同卡+跨卡混合） |
| TestAsyncSubmit | 4 | 跨卡 2 卡 | **PASS** — async write_submit + stream sync + verify |
| TestDeregister | 4 | 跨卡 2 卡 | **PASS** |

- TopologyPreflight：PASS（跨卡 P2P 槽位 4 ≤ 8）
- VMM import：同卡 die 走 ctx trick（`SetDevice(exporter)` + `ImportFromShareableHandle` + `SetDevice(local)` restore），跨卡走 `CanAccessPeer` + `ImportFromShareableHandle`，均成功
- `AclrtMemSetAccess` 在 `hybm_alloc_vmm_buffer` 时为所有跨卡 device 预授权，import 时为 local device 授权

**与 ACL IPC 路径的对比**：

| 维度 | ACL IPC 路径（§27） | VMM baseline 路径（§28） |
|------|-------------------|------------------------|
| 内存分配 | `aclrtMalloc`（用户自由分配） | `hybm_alloc_vmm_buffer`（需用专用 API） |
| Export | `AclrtIpcMemGetExportKey` → IPC key (64B string) | `AclrtMemExportToShareableHandle` → uint64 shareable |
| Import (同卡) | `AclrtIpcMemImportByKey(DEFAULT)` | `AclrtMemImportFromShareableHandle` + ctx trick |
| Import (跨卡) | `AclrtIpcMemImportByKey(ENABLE_PEER_ACCESS)` → **507899 FAIL** | `AclrtMemImportFromShareableHandle` → **PASS** |
| P2P 授权 | ACL IPC 内部处理（跨卡失败） | `AclrtMemSetAccess` 显式授权（成功） |
| 2-rank 同卡 | ✅ PASS | ✅ PASS |
| 4-rank 跨卡 | ❌ FAIL | ✅ PASS |
| 外部内存语义 | 保留（注册用户 `aclrtMalloc` 内存） | 需改用 `hybm_alloc_vmm_buffer` 分配 |

**注意事项**：
1. `hybm_alloc_vmm_buffer` 分配的内存地址必须用于 `smem_trans_register_mem` 注册，不能混用 `aclrtMalloc` 和 `hybm_alloc_vmm_buffer`
2. `AclrtMemSetAccess` 在分配时为所有 `worldSize` 个 device 预授权，需要传入正确的 `worldSize`
3. VMM baseline 路径和 ACL IPC 路径共存：`RegisterMemory` 通过 `HybmLookupVmmHandle` 检测地址是否为 VMM 内存，自动选择路径
4. ctx trick（同卡 die import 时切换到 exporter device context）是必须的，否则 `AclrtMemImportFromShareableHandle` 返回 507899
