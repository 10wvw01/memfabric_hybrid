# 310P3 SHM Device Kernel GVA 访问问题

> 日期：2026-09-03
> 硬件：Atlas 300I Duo (310P3) / CANN 9.1.0

## 问题概述

SHM 模式的核心价值是 device kernel（MTE/AIV）通过 GVA 直接访问远端 rank 的 HBM。910B 上此能力已验证可用，但 310P3 上 device kernel 的 `DataCopy` 操作 GVA 地址无法跨 rank 访问。

## 910B 为什么能行

### 核心机制：`HalMemAddressReserve(expectPtr=GVA)` 让 device VA == GVA

910B 的地址统一从 `ReserveMemorySpace` 开始（`hybm_vmm_based_segment.cpp:73`）：

```cpp
// 910B 路径（hybm_vmm_based_segment.cpp:82-108）
totalVirtualSize_ = options_.rankCnt * options_.maxSize;      // 2 * 1GB = 2GB
auto totalLvaSize = totalVirtualSize_;                         // 非 56-bit 时 = totalVirtualSize_

// 第一步：VaManager 分配 GVA 地址（从 HYBM_GVM_START_ADDR=0x280040000000 起找空闲区）
auto gvaInfo = HybmVaManager::GetInstance().AllocReserveGva(
    options_.rankId, totalVirtualSize_, totalLvaSize, mem_type, options_.enable56BitsGva);
// gvaInfo.va[HVM_GVA] = 0x280040000000 (fabric 全局地址)
// gvaInfo.va[HVM_DVA] = 0x280040000000 (device VA，= GVA，因为非 56-bit 时 gva=lva)

// 第二步：用 HalMemAddressReserve 在 GVA 地址处预留 device VA
HalMemAddressReserve(&base, totalLvaSize, 0,
    reinterpret_cast<void*>(gvaInfo.va[HVM_DVA]),  // expectPtr = 0x280040000000
    MEM_RSV_TYPE_REMOTE_MAP);
// base = 0x280040000000 ← 预留成功！device VA 基地址 == GVA 基地址
```

`HalMemAddressReserve` 第 4 个参数 `addr` 是**指定期望预留地址**。910B 驱动支持此参数，在指定地址处预留 2GB device VA 窗口。

`AllocReserveGva` 内部（`hybm_va_manager.cpp:326-382`）：
```cpp
// 非 56-bit 路径：gva = lva（同一地址）
lva = AllocReserveLvaInner(localRankId, localSize, HVM_DVA);
// AllocReserveLvaInner 在 HYBM_GVM_START_ADDR(0x280040000000) ~ HYBM_GVM_END_ADDR 范围找空闲
gva = lva;  // ← GVA == DVA，二者相同
result.va[HVM_GVA] = gva;
result.va[HVM_DVA] = lva;  // ← 同一个值
```

### 本地分配：GVA == device VA（同一地址）

`AllocLocalMemory`（`hybm_vmm_based_segment.cpp:279-326`）：

```cpp
// 910B 路径（enable56BitsGva=false, Is310P3AclPath()=false）
auto localVirtualBase = localVirtualAddress_ + options_.maxSize * options_.rankId;
//                  = 0x280040000000       + 1GB * rankId
// rank 0: localVirtualBase = 0x280040000000
// rank 1: localVirtualBase = 0x280080000000

auto globalVirtualBase = globalVirtualAddress_ + options_.maxSize * options_.rankId;
//                  = 0x280040000000       + 1GB * rankId
// rank 0: globalVirtualBase = 0x280040000000
// rank 1: globalVirtualBase = 0x280080000000

auto allocAddr = localVirtualBase + allocatedSize_;  // device VA（实际分配地址）
auto gva       = globalVirtualBase + allocatedSize_; // GVA（全局地址）
// rank 0: allocAddr = 0x280040000000, gva = 0x280040000000  ← 相同！
// rank 1: allocAddr = 0x280080000000, gva = 0x280080000000  ← 相同！

// 注册到 VaManager
HybmVaManager::GetInstance().AddVaInfo(
    {gva, slice->vAddress_, slice->vAddress_, size, type}, options_.rankId);
//                         ↑ gva   ↑ allocAddr(device VA)
// AddVaInfo 内部（hybm_va_manager.cpp:55-92）：
//   allocatedMap_[HVM_GVA][gva] = info;       // GVA → info
//   allocatedMap_[HVM_DVA][allocAddr] = info; // device VA → info
// 因为 gva == allocAddr，两个 map 指向同一个 entry
```

### 导出：export info 记录 GVA == device VA

`ExportInner`（`hybm_vmm_based_segment.cpp:480-546`）：

```cpp
info.gva       = gvaInfo.base.va[HVM_GVA]; // 0x280080000000 (rank 1)
info.deviceVa  = gvaInfo.base.va[HVM_DVA]; // 0x280080000000 (rank 1)
// gva == deviceVa ← 相同！
```

### 导入 peer：在同一个预留窗口内映射

`ReserveLva`（`hybm_vmm_based_segment.cpp:658-692`）：

```cpp
// 910B 路径（非 56-bit, 非 310P3）
if (!options_.enable56BitsGva) {
    return im.deviceVa;  // 直接返回 peer 的 device VA = 0x280080000000
}
// 不需要额外 HalMemAddressReserve！
// 因为 ReserveMemorySpace 已经预留了 2GB 全窗口（含所有 rank 的 slot）
// peer 的 device VA 0x280080000000 就在本进程已预留的窗口范围内
```

`Mmap`（`hybm_vmm_based_segment.cpp:834-847`）：

```cpp
// 910B 路径
HalMemImport(MEM_HANDLE_TYPE_FABRIC, &im.shareHandle, logicDeviceId_, &handle);
// 导入 peer 的物理内存 handle

HalMemMap(reinterpret_cast<void*>(lva), im.size, 0, handle, 0);
// lva = im.deviceVa = 0x280080000000
// 在本进程的 device VA 0x280080000000 处映射 peer 的物理内存
```

导入后注册到 VaManager（`hybm_vmm_based_segment.cpp:859-861`）：

```cpp
HybmVaManager::GetInstance().AddVaInfoFromExternal(
    {im.gva, lva, 0, im.size, memType}, options_.rankId, im.rankId);
//  im.gva = 0x280080000000, lva = 0x280080000000 ← 相同！
//  allocatedMap_[HVM_GVA][0x280080000000] = imported info
//  allocatedMap_[HVM_DVA][0x280080000000] = imported info
```

### 910B 完整布局示例（2-rank SHM，每 rank 1GB）

```
VaManager AllocReserveGva:
  在 0x280040000000 起找 2GB 空闲空间
  → gva = lva = 0x280040000000

ReserveMemorySpace (rank 0):
  HalMemAddressReserve(2GB, expectPtr=0x280040000000)
  → base = 0x280040000000  ✓ 预留 2GB device VA 窗口 [0x280040000000, 0x280080000000)

AllocLocalMemory (rank 0):
  localVirtualBase = 0x280040000000 + 1GB * 0 = 0x280040000000
  allocAddr (DVA)  = 0x280040000000
  gva              = 0x280040000000
  → MallocPhysical + HalMemMap(0x280040000000) → rank 0 自己的 HBM 映射到 0x280040000000
  → AddVaInfo(gva=0x280040000000, dva=0x280040000000)

AllocLocalMemory (rank 1, 同样逻辑):
  localVirtualBase = 0x280040000000 + 1GB * 1 = 0x280080000000
  allocAddr (DVA)  = 0x280080000000
  gva              = 0x280080000000
  → rank 1 自己的 HBM 映射到 0x280080000000

Import (rank 0 imports rank 1):
  ReserveLva → return im.deviceVa = 0x280080000000（不额外预留，已在 2GB 窗口内）
  HalMemImport + HalMemMap(0x280080000000) → rank 1 的 HBM 映射到 0x280080000000
  → AddVaInfoFromExternal(gva=0x280080000000, dva=0x280080000000)

最终 rank 0 的地址视图：
  ┌─────────────────────────────────────────────────────────────┐
  │ device VA 空间 (也是 GVA 空间，两者重合)                      │
  ├──────────────────┬──────────────────┤
  │ 0x280040000000   │ 0x280080000000   │
  │ rank 0 HBM       │ rank 1 HBM       │
  │ (local alloc+map)│ (imported+map)   │
  │ GVA=DVA=同地址   │ GVA=DVA=同地址   │
  └──────────────────┴──────────────────┘
  ↑ 任何地址既是 GVA 也是 device VA
```

### 910B kernel 如何识别和访问 rank 1 的 GVA

kernel **不需要"识别"**——地址 `0x280080000000` 直接就是 device VA，device 硬件的 MMU/页表已经映射了该地址到 rank 1 的物理 HBM。整个过程：

1. **host 传入 GVA**：`smem_shm_create` 返回 `gva=0x280040000000`，host 把它作为参数传给 kernel

2. **kernel 从固定地址读元数据**获取 rank/rankSize/symSize（不经过 host 参数）：
```cpp
// smem_shm_aicore_base_meta.h
// SMEM_SHM_DEVICE_META_ADDR = 0x17FFBE000000 (固定地址，由 hybm_init_hbm_gva 映射)
// kernel 直接读 *(uint32_t*)(META_ADDR + shmemId*128 + offset)
uint32_t rank = smem_shm_get_global_rank();      // = 0
uint32_t rankSize = smem_shm_get_global_rank_size(); // = 2
uint64_t symSize = smem_shm_get_symmetric_size();   // = 1GB
```

3. **kernel 用 GVA 算术计算 peer slot 地址**：
```cpp
// shm_all_shift.cpp:86-91
uint64_t offset = smem_shm_get_symmetric_size();  // = 0x40000000 (1GB)
uint64_t dst = reinterpret_cast<uint64_t>(gva) + offset * targetRank;
// rank 0, targetRank=1: dst = 0x280040000000 + 1GB * 1 = 0x280080000000
```

4. **kernel 用 DataCopyPad 直接访问该地址**：
```cpp
// smem_shm_aicore_base_copy.h:18-33
AscendC::GlobalTensor<T> gmTensor;
gmTensor.SetGlobalBuffer(reinterpret_cast<__gm__ T*>(dstGva)); // dstGva = 0x280080000000
AscendC::DataCopyPad(gmTensor, ubTensor, dataCopyParams);
```

**为什么能访问？** 因为 host 在 `Mmap` 阶段已经通过 `HalMemMap(0x280080000000, peer_handle)` 在 device VA `0x280080000000` 处建立了到 rank 1 物理 HBM 的映射。device 硬件的页表中有这个映射项，MTE 引擎发 `DataCopyPad` 时通过 device VA 查页表找到 rank 1 的物理内存。

**关键点**：kernel 不做任何地址翻译。`gva + symSize * rank` 算出来的地址直接就是合法的 device VA，因为 `HalMemAddressReserve(expectPtr=GVA)` 让两个地址空间完全重合。

### 910B host 侧 SDMA 的 GVA→DVA 翻译（恒等映射）

BM 模式 host 侧 `smem_bm_copy` 调 `HostDataOpSDMA::TransformVa`（`hybm_data_op_sdma.cpp:327-338`）：

```cpp
void HostDataOpSDMA::TransformVa(void *&src, void *&dst, ...) {
    out = HybmVaManager::GetInstance().TransformVa(src, HVM_GVA, HVM_DVA);
    // TransformVa 内部（hybm_va_manager.cpp:144-172）：
    //   在 allocatedMap_[HVM_GVA] 中查找 src
    //   offset = src - info.va[HVM_GVA]
    //   outputVa = info.va[HVM_DVA] + offset
    //   return outputVa
    // 因为 va[HVM_GVA] == va[HVM_DVA]，offset 不变，outputVa == src
    // → 恒等映射，翻译前后地址不变
    src = (out != 0) ? out : src;  // src 不变
}
```

## 310P3 为什么不行

### 根因：`AclrtReserveMemAddress` 不支持指定地址

310P3 用 `AclrtReserveMemAddress`（ACL VMM API）替代 `HalMemAddressReserve`（HAL API）。两者**都不支持指定预留地址**：

- `AclrtReserveMemAddress(expectPtr=GVA)`：返回 ret=0 但 base=NULL
- `halMemAddressReserve(expectPtr=GVA)`：返回 ret=0 但 base=NULL（HAL 层不支持 HBM 地址预留）

只有 `AclrtReserveMemAddress(nullptr)` auto-alloc 可用。

**验证（独立 ACL+HAL API probe，2026-09-04）**：

```
[r0] ACL Reserve(fixed=0x100000000000) ret=0 base=(nil) → 不支持 expectPtr
[r0] HAL Reserve(gva=0x280040000000) ret=0 base=(nil) → HAL 无效
[r0] HAL Reserve(dva=0x100000000000) ret=0 base=(nil) → HAL 无效
[r0] HAL Reserve(auto) ret=0 base=(nil) → HAL 无效
[r0] ACL auto: ✓ auto=0x12c140000000 → ACL auto-alloc 可用
```

| API | 910B | 310P3 |
|-----|------|-------|
| `HalMemAddressReserve(expectPtr=GVA)` | ✓ 支持 | ✗ ret=0, base=NULL（HAL 层不支持 HBM 地址预留） |
| `AclrtReserveMemAddress(expectPtr=GVA)` | 未测试 | ✗ ret=0, base=NULL（不支持 expectPtr） |
| `AclrtReserveMemAddress(nullptr)` | 未测试 | ✓ auto-alloc |

### 310P3 地址空间分离

因为不能指定地址，310P3 的代码（`hybm_vmm_based_segment.cpp:91-100`）只能 auto-alloc 且只预留本地 `maxSize`（不是全窗口）：

```cpp
// 310P3 路径
ret = DlAclApi::AclrtReserveMemAddress(&base, options_.maxSize, 0, nullptr, 0);
// base = 0x17ffc0000000（驱动随机分配，与 GVA 无关）
// 只预留 1GB（本地），不是 2GB（全窗口）
```

`AllocLocalMemory` 中 310P3 不做 rankId 偏移（`hybm_vmm_based_segment.cpp:291`）：
```cpp
// 310P3 路径
auto localVirtualBase = localVirtualAddress_ + 0;  // Is310P3AclPath() ? 0 : maxSize * rankId
auto gva = globalVirtualAddress_ + maxSize * rankId;
// allocAddr (DVA) = 0x17ffc0000000  (本地 device VA)
// gva             = 0x280040000000  (fabric GVA)
// 两者不同！
```

导入时每个 peer 独立 auto-alloc（`hybm_vmm_based_segment.cpp:654-668`）：
```cpp
// 310P3 路径
AclrtReserveMemAddress(&lva, im.size, 0, nullptr, 0);
// lva = 0x12c140000000（另一个随机地址，与 GVA 无关）
```

### 310P3 完整布局示例（2-rank SHM，每 rank 1GB）与 910B 对比

```
=== 910B（GVA == device VA，统一地址空间）===

VaManager AllocReserveGva:
  在 0x280040000000 起找 2GB → gva = lva = 0x280040000000

ReserveMemorySpace (rank 0):
  HalMemAddressReserve(2GB, expectPtr=0x280040000000) → base = 0x280040000000 ✓
  预留全窗口 [0x280040000000, 0x280080000000)

AllocLocalMemory (rank 0):
  DVA = GVA = 0x280040000000  ← 相同

Import (rank 0 ← rank 1):
  ReserveLva → return im.deviceVa = 0x280080000000（在已预留窗口内，不需额外预留）
  HalMemMap(0x280080000000, peer_handle) → 映射 rank 1 HBM

rank 0 地址视图：
  0x280040000000 ── rank 0 HBM (local)
  0x280080000000 ── rank 1 HBM (imported)
  ↑ GVA == DVA，kernel DataCopy(0x280080000000) → 访问 rank 1 ✓

=== 310P3（GVA ≠ device VA，分离地址空间）===

VaManager AllocReserveGva:
  在 0x280040000000 起找 2GB → gva = 0x280040000000（fabric 地址）
  但 lva 无法在 GVA 地址处预留！

ReserveMemorySpace (rank 0):
  AclrtReserveMemAddress(1GB, nullptr) → base = 0x17ffc0000000（随机，≠ GVA）
  只预留本地 1GB，不是全窗口 2GB

AllocLocalMemory (rank 0):
  DVA = 0x17ffc0000000, GVA = 0x280040000000  ← 不同！
  AddVaInfo(gva=0x280040000000, dva=0x17ffc0000000) ← 注册映射关系

Import (rank 0 ← rank 1):
  AclrtReserveMemAddress(1GB, nullptr) → lva = 0x12c140000000（另一个随机地址）
  AclrtMapMem(0x12c140000000, peer_handle) → 映射 rank 1 HBM
  AddVaInfoFromExternal(gva=0x280080000000, dva=0x12c140000000) ← 注册映射关系

rank 0 地址视图：
  GVA:          0x280040000000  (fabric 地址，未映射到 device VA)
  device VA:    0x17ffc0000000  (rank 0 HBM, local, mapped)
  imported VA:  0x12c140000000  (rank 1 HBM, imported, mapped)
  ↑ 三个地址完全不同！

kernel 拿到 gva=0x280040000000：
  计算 peer slot = gva + symSize * 1 = 0x280080000000
  DataCopy(0x280080000000) → 0x280080000000 不在 device VA 映射中 → 无法寻址 ✗
```

### 310P3 host 侧 GVA→DVA 翻译（非恒等，需要查表）

BM 模式 host 侧 `TransformVa`（`hybm_data_op_sdma.cpp:327-338`）在 310P3 上：

```cpp
// host 调 smem_bm_copy(gva=0x280080000000, ...)
TransformVa(src=0x280080000000, HVM_GVA, HVM_DVA):
  在 allocatedMap_[HVM_GVA] 中查找 0x280080000000
  → 找到 imported info: va[HVM_GVA]=0x280080000000, va[HVM_DVA]=0x12c140000000
  offset = 0x280080000000 - 0x280080000000 = 0
  outputVa = 0x12c140000000 + 0 = 0x12c140000000
  → 返回 0x12c140000000（device VA）

// 然后 aclrtMemcpyAsync(0x12c140000000) → 访问 rank 1 HBM ✓
```

host 侧通过 VaManager 查表翻译成功，但 **device kernel 无法调用 VaManager**（它是 host 侧 C++ 对象）。

### DataCopyPad vs DataCopy

310P3 是 dav-m200 架构（推理卡），`DataCopyPad` 是 dav-m220（训练卡 910B）的 MTE API。310P3 只支持 `DataCopy`。但两者都只认识 device VA，不认识 GVA。

## ACL API 验证结果（2026-09-04 独立 probe）

### 验证环境
- 独立 C 程序，直接调 CANN ACL + HAL API，不依赖 MemFabric
- 2 rank（dev0/dev1 同卡 die 对）
- probe 源码：`/tmp/opencode/gva_probe/probe.cpp`

### Phase 1: AclrtReserveMemAddress 地址指定

| 测试 | 结果 | 说明 |
|------|------|------|
| `expectPtr=0x100000000000` | ✗ 507899 | 310P3 不支持指定预留地址 |
| `expectPtr=nullptr` (auto) | ✓ `0x12c140000000` | 自动分配可用 |

### Phase 2: HAL `halMemAddressReserve`

| 测试 | 结果 | 说明 |
|------|------|------|
| `halMemAddressReserve(addr=0x280040000000)` | ret=0, base=NULL | 返回成功但未分配 |
| `halMemAddressReserve(addr=0x100000000000)` | ret=0, base=NULL | 同上 |
| `halMemAddressReserve(addr=NULL)` (auto) | ret=0, base=NULL | 同上 |
| `halMemAddressReserve(addr=0x100..., flag=REMOTE_MAP)` | ret=0, base=NULL | 同上 |

**结论**：`halMemAddressReserve` 在 310P3 上返回 ret=0 但 base=NULL——函数符号存在但实际未执行地址预留。这与 `HalMemCreate(MEM_HBM_TYPE)` 返回错误 6 一致：310P3 的 HAL 层不支持 HBM 设备内存操作（包括地址预留）。HAL 库已加载但该函数对 HBM 相关操作无效。

### Phase 3: VMM 跨 die export/import + memcpy

| 步骤 | API | 结果 | 说明 |
|------|-----|------|------|
| 1 | `AclrtReserveMemAddress(auto)` | ✓ | local VA = 0x12c140000000 |
| 2 | `AclrtMallocPhysical(P2P_HUGE)` | ✓ | local HBM 物理内存 |
| 2 | `AclrtMapMem` | ✓ | VA→物理内存映射 |
| 2b | `AclrtMemSetAccess(peer die)` | 507899 | 同卡 die SetAccess 被拒（已知） |
| 3 | `AclrtMemExportToShareableHandle` | ✓ | flag=DISABLE_PID_VALIDATION |
| 5 | `AclrtMemImportFromShareableHandle` (ctx trick) | ✓ | SetDevice(exporter) 后 import 成功 |
| 6 | `AclrtReserveMemAddress + AclrtMapMem` (imported) | ✓ | imported VA = 0x12c150000000 |
| 7 | `aclrtMemcpy` H2D to peer's imported VA | ✓ | 写入 peer HBM |
| 8 | `aclrtMemcpy` D2H from own VA | ✓ | 读取 peer 写来的数据，验证通过 |

### 结论

1. **`AclrtReserveMemAddress` 不支持 `expectPtr`**——返回 ret=0 但 base=NULL，无法让 device VA == GVA
2. **`halMemAddressReserve` 在 310P3 上无效**——返回 ret=0 但 base=NULL，HAL 层不支持 HBM 地址预留
3. **VMM 跨 die export/import + host memcpy 完全可用**——BM 模式的 host-initiated 路径不受影响
4. **Device kernel 通过 GVA 直接访问不可行**——GVA ≠ device VA，kernel 无法寻址 GVA

## 影响范围

| 能力 | 910B | 310P3 | 原因 |
|------|------|-------|------|
| SHM create/destroy | ✓ | ✓ | VMM import/export 可用 |
| SHM barrier/topology | ✓ | ✓ | 控制面不依赖 GVA 数据访问 |
| Host SMEM_bm_copy (GVA→DVA) | ✓ | ✓ | BM 模式 host 侧有 TransformVa |
| Device kernel DataCopy(GVA) | ✓ | ✗ | 910B: GVA==DVA; 310P3: GVA≠DVA, kernel 无翻译 |
| Device kernel DataCopyPad(GVA) | ✓ | ✗ | 同上 + DataCopyPad 不支持 |

## 解决方向

### 方案 A：传入 device VA 而非 GVA

Host 在 kernel launch 前用 `TransformVa` 翻译 GVA→DVA，将 DVA 传入 kernel。

- 优点：不改 kernel 代码，`DataCopy(dva)` 直接可用
- 缺点：破坏 GVA 对称性（每个 rank 的 peer DVA 不同），kernel 需要接收 rank-specific 参数
- 限制：`TransformVa` 是 internal API，非公开 C API

### 方案 B：探索 310P3 fabric address translation

310P3 是否有 device 侧的 GVA 翻译机制？需验证 `aclrtMemSetAccess` 建立的 P2P 映射是否包含 GVA→物理地址翻译。

### 方案 C：用 BM 模式替代

放弃 SHM 的 device-initiated MTE，改用 BM 的 host-initiated SDMA（已验证可用）。BM 模式 `smem_bm_copy` 内部做 GVA→DVA 翻译后调 `aclrtMemcpyAsync`。

### 推荐

先验证方案 A（传入 device VA），因为代码改动最小，能验证 device kernel `DataCopy` 是否能通过 DVA 访问 peer HBM。
