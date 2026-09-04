# MemFabric 三种模式跨 rank 数据传输机制

> 硬件：Atlas 300I Duo (310P3) / CANN 9.1.0
> 地址值取自实际 2-rank 测试日志（dev0/dev1 同卡 die 对）

## 总览

| 模式 | 内存来源 | 共享机制 | 数据搬运 | 310P3 |
|------|---------|---------|---------|-------|
| BM | 框架内部分配 | VMM shareable handle | host CPU → SDMA | ✅ |
| TRANS | 用户 `aclrtMalloc` | ACL IPC key | host CPU → SDMA | ✅ |
| SHM | 框架分配对称 HBM | VMM shareable handle | device kernel → MTE | ❌ GVA≠DVA |

---

## 一、TRANS 模式

### 场景：2-rank，rank0 向 rank1 写 16MB 数据

```
rank0 (dev0, pid=1159702)         rank1 (dev1, pid=1159703)
```

### 步骤 1：两端各自 `aclrtMalloc` + `register_mem`

```c
// rank0 和 rank1 各自分配设备内存
void *devMem = NULL;
aclrtMalloc(&devMem, 16MB, ACL_MEM_MALLOC_HUGE_FIRST);
// 两进程各自得到 devMem = 0x12c001000000（不同进程的相同 VA，映射不同物理页）

// 两端各自注册到 TRANS entity
smem_trans_register_mem(handle, devMem, 16MB, 0);
```

register 内部（`HybmDevUserLegacySegment::RegisterMemory`）：

```c
// 310P3: ACL 层 IPC 导出
AclrtIpcMemGetExportKey(devMem, 16MB, key, 65, DISABLE_PID_VALIDATION);
// key 示例: "0011b2160000000000000040..."（65 字节，含 exporter 的 phyId 信息）

// VaManager 注册 GVA↔DVA（GVA 由 VaManager 分配，DVA = devMem 本身）
// rank0: gva=0x1000000000000000  dva=0x12c001000000
// rank1: gva=0x1000002000000000  dva=0x12c001000000
AddVaInfo({gva, dva=devMem, dva=devMem, 16MB, DEVICE});
```

### 步骤 2：config store 交换 IPC key + peer import

```c
// rank0 通过 config store 发布自己的 exchange info（含 IPC key）
// rank1 收到后调 hybm_import → ImportSliceInfo
```

ImportSliceInfo 内部（`HybmDevUserLegacySegment::ImportSliceInfo`）：

```c
// rank1 import rank0 的内存：
//   同卡 die（dev0/2 == dev1/2 == card0），用 DEFAULT flag
AclrtIpcMemImportByKey(&importedDVA, rank0_key, ACL_RT_IPC_MEM_IMPORT_FLAG_DEFAULT);
// importedDVA = 0x12c140000000（rank0 的物理内存在 rank1 进程中的映射地址）

// VaManager 注册 rank0 的 GVA↔importedDVA
AddVaInfoFromExternal({gva=0x1000000000000000, dva=0x12c140000000, 16MB, DEVICE});
```

### 步骤 2 后的地址分布

#### rank0 进程的地址视图

| 描述 | GVA | DVA | 物理内存 |
|------|-----|-----|---------|
| rank0 本地（自分配） | 0x1000000000000000 | 0x12c001000000 | rank0 dev0 HBM |
| rank1 远端（imported） | 0x1000002000000000 | 0x12c140000000 | rank1 dev1 HBM |

#### rank1 进程的地址视图

| 描述 | GVA | DVA | 物理内存 |
|------|-----|-----|---------|
| rank1 本地（自分配） | 0x1000002000000000 | 0x12c001000000 | rank1 dev1 HBM |
| rank0 远端（imported） | 0x1000000000000000 | 0x12c140000000 | rank0 dev0 HBM |

#### SmemTransEntry 的 remoteSlices_ 映射表（rank0 视角）

```
remoteSlices_[rank1_WorkerId]:
  key=0x12c001000000（rank1 的本地地址）→ GVA=0x1000002000000000（rank1 的 GVA）
```

### 步骤 3：rank0 向 rank1 写数据

```c
// remoteAddr 是 rank1 的本地地址 0x12c001000000
// remoteUniqueId 是 rank1 的 "127.0.0.1:13001"
smem_trans_write(handle, localDevMem, "127.0.0.1:13001", remoteAddr, 16MB, 0);
```

内部流程（`BatchSyncTransfer`）：

```c
// 3a. 地址翻译：peer本地地址 → GVA
//   remoteSlices_[rank1].lower_bound(0x12c001000000) → GVA=0x1000002000000000
mappedAddress[0] = TransformAddr(remoteAddr=0x12c001000000);
// mappedAddress[0] = 0x1000002000000000（GVA）

// 3b. 调 hybm 层
hybm_data_batch_copy(localDevMem, mappedAddress, 16MB, AUTO);
```

hybm 层（`hybm_data_op_entry.cpp` → `HostDataOpSDMA`）：

```c
// 3c. 方向推断
srcMask = ClassifyAddressMask(localDevMem);  // 0x12c001000000 → LOCAL_DEVICE
dstMask = ClassifyAddressMask(GVA);          // 0x1000002000000000 → GLOBAL_DEVICE
// direction = D2GD

// 3d. GVA→DVA 翻译
dstDVA = TransformVa(GVA=0x1000002000000000, GVA→DVA);
// 查 VaManager: gva=0x1000002000000000 → dva=0x12c140000000（rank1 的 imported DVA）

// 3e. DMA 拷贝
AclrtMemcpyAsync(dst=0x12c140000000, 16MB, src=0x12c001000000, 16MB, D2D, stream);
AclrtSynchronizeStream(stream);
// DMA 引擎从 0x12c001000000（rank0 本地 HBM）拷贝到 0x12c140000000（映射到 rank1 HBM）
```

### 完整数据流

```
rank0 调 smem_trans_write(localDVA=0x12c001000000, remoteAddr=0x12c001000000)

  remoteAddr ──TransformAddr──► GVA=0x1000002000000000  (查 remoteSlices_ 表)
  localDVA   ──────────────────► 0x12c001000000           (不翻译，本地地址)

  hybm_data_batch_copy:
    src=0x12c001000000  dst(GVA)=0x1000002000000000
      │                    │
      │              TransformVa(GVA→DVA)
      │                    │
      │              dstDVA=0x12c140000000  (查 VaManager，rank1 imported DVA)
      │                    │
      └────────────────────┴──► AclrtMemcpyAsync(D2D)
                                 src=0x12c001000000  →  dst=0x12c140000000
                                   (rank0 本地 HBM)      (映射到 rank1 HBM)
```

---

## 二、BM 模式

### 场景：2-rank，rank0 向 rank1 写 16MB 数据

```
rank0 (dev0)    rank1 (dev1)    每端 maxHBMSize = 1GB
```

### 步骤 1：create2 + join（框架内部分配 + 交换）

```c
smem_bm_create2(id=1, {maxHbmSize=1GB, dataOpType=SDMA});
smem_bm_join(handle, 0);
```

create 内部（`HybmVmmBasedSegment::ReserveMemorySpace`）：

```c
// 310P3: AclrtReserveMemAddress(auto-alloc) 预留本地 1GB VA
AclrtReserveMemAddress(&base, 1GB, 0, nullptr, 0);
// 两端都得到 localVA = 0x12c140000000

// VaManager 分配 GVA 窗口（rank0 和 rank1 各 1GB 槽位）
// rank0: gvaBase = 0x280040000000  (rank0 slot: 0x280040000000 ~ 0x280080000000)
// rank1: gvaBase = 0x280080000000  (rank1 slot: 0x280080000000 ~ 0x2800C0000000)
```

AllocLocalMemory（`AclrtMallocPhysical` + `AclrtMapMem`）：

```c
// rank0: AclrtMallocPhysical(1GB, ACL_MEM_P2P_HUGE) → physicalHandle
//        AclrtMapMem(localVA=0x12c140000000, 1GB, physicalHandle) → 映射本地 HBM
// rank1: 同上
```

join → Import + Mmap（ctx 技巧）：

```c
// rank0 import rank1 的 shareable handle：
AclrtSetDevice(exporterDev=1);  // ctx trick：切到导出方，绕过 P2P 前置检查
AclrtMemImportFromShareableHandle(sh=0x1000007ab, devId=0, &handle);
AclrtSetDevice(0);              // 切回
AclrtReserveMemAddress(&importedVA, 1GB, 0, nullptr, 0);  // importedVA = 0x12c180000000
AclrtMapMem(importedVA, 1GB, handle);  // 映射 rank1 的 HBM 到 0x12c180000000
```

### 步骤 1 后的地址分布

#### rank0 进程的地址视图

| 描述 | GVA | DVA | 物理内存 |
|------|-----|-----|---------|
| rank0 本地（自己分配） | 0x280040000000 | 0x12c140000000 | rank0 dev0 HBM |
| rank1 远端（imported） | 0x280080000000 | 0x12c180000000 | rank1 dev1 HBM |

#### rank1 进程的地址视图

| 描述 | GVA | DVA | 物理内存 |
|------|-----|-----|---------|
| rank1 本地（自己分配） | 0x280080000000 | 0x12c140000000 | rank1 dev1 HBM |
| rank0 远端（imported） | 0x280040000000 | 0x12c180000000 | rank0 dev0 HBM |

### 步骤 2：rank0 向 rank1 写数据

```c
// 获取 rank1 的 GVA 指针
void *rank1Gva = smem_bm_ptr_by_mem_type(handle, DEVICE, peerRankId=1);
// rank1Gva = deviceGvaBase + maxHBMSize * 1 = 0x280040000000 + 1GB = 0x280080000000

// 本地数据
smem_copy_params_t params = {.src = localBuf, .dest = rank1Gva, .dataSize = 16MB};
smem_bm_copy(handle, &params, SMEMB_COPY_L2G, 0);
```

内部与 TRANS 完全相同（`hybm_data_batch_copy` → SDMA）：

```c
// 方向推断：src=LOCAL_DEVICE, dst=GLOBAL_DEVICE → D2GD
// GVA→DVA 翻译：
dstDVA = TransformVa(GVA=0x280080000000, GVA→DVA);
// 查 VaManager: gva=0x280080000000 → dva=0x12c180000000（rank1 imported DVA）

// DMA 拷贝
AclrtMemcpyAsync(dst=0x12c180000000, 16MB, src=localBufDVA, 16MB, D2D, stream);
AclrtSynchronizeStream(stream);
```

### 完整数据流

```
rank0 调 smem_bm_copy(src=localDVA, dst=smem_bm_ptr_by_mem_type(DEVICE, rank=1))

  dst = smem_bm_ptr_by_mem_type(DEVICE, 1)
      = deviceGvaBase + maxHBMSize * 1
      = 0x280040000000 + 0x40000000
      = 0x280080000000  (GVA)

  hybm_data_batch_copy:
    src=localDVA  dst(GVA)=0x280080000000
      │               │
      │         TransformVa(GVA→DVA)
      │               │
      │         dstDVA=0x12c180000000  (rank1 imported DVA)
      │               │
      └───────────────┴──► AclrtMemcpyAsync(D2D)
                            src=localDVA  →  dst=0x12c180000000
                              (rank0 本地)    (映射到 rank1 HBM)
```

### BM vs TRANS 对比

| 维度 | BM | TRANS |
|------|-----|-------|
| 内存来源 | 框架 `AclrtMallocPhysical` | 用户 `aclrtMalloc` |
| 共享机制 | VMM shareable handle | ACL IPC key |
| GVA 分配 | VaManager 按统一窗口分配（`gvaBase + maxHBMSize * rankId`） | VaManager 按注册顺序分配 |
| 用户拿到的地址 | GVA（`smem_bm_ptr_by_mem_type` 返回 GVA） | peer 本地地址（用户自己 `aclrtMalloc` 的 DVA） |
| 地址翻译 | GVA → DVA（1 步） | peer本地地址 → GVA → DVA（2 步） |
| SDMA 拷贝 | 相同 | 相同 |

---

## 三、SHM 模式

### 场景：2-rank，每端 256MB 对称 HBM

### 步骤 1：create（对称 HBM 池）

```c
smem_shm_create(id=0, worldSize=2, rankId=rank, symSize=256MB, opType=MTE, &gva);
```

内部按 init step 执行：

```c
// Step 1: hybm_create_entity（AI_CORE_INITIATE）
// Step 2: hybm_reserve_mem_space → ReserveMemorySpace
//   VaManager 分配 GVA 窗口: rank0 slot = 0x280040000000, rank1 slot = 0x280080000000
//   310P3: AclrtReserveMemAddress(auto) → localVA = 0x12c140000000（与 GVA 不同）
//   910B:   HalMemAddressReserve(GVA) → localVA = GVA（GVA == DVA）

// Step 3: hybm_alloc_local_memory → AclrtMallocPhysical + AclrtMapMem
//   rank0: 物理内存映射到 localVA = 0x12c140000000
//   rank1: 同上

// Step 4: AllGather + import + mmap（ctx 技巧，与 BM 相同）
//   rank0 import rank1 → importedDVA = 0x12c180000000
//   rank1 import rank0 → importedDVA = 0x12c180000000

// 返回 GVA 给用户
gva = 0x280040000000  (rank0)
gva = 0x280080000000  (rank1)
```

### 步骤 1 后的地址分布

#### 910B（GVA == DVA，kernel 可直接寻址）

| 描述 | GVA | DVA | 物理内存 |
|------|-----|-----|---------|
| rank0 本地 | 0x280040000000 | 0x280040000000 | rank0 HBM |
| rank1 远端 | 0x280080000000 | 0x280080000000 | rank1 HBM |

#### 310P3（GVA ≠ DVA，kernel 不可寻址）

| 描述 | GVA | DVA | 物理内存 |
|------|-----|-----|---------|
| rank0 本地 | 0x280040000000 | 0x12c140000000 | rank0 dev0 HBM |
| rank1 远端 | 0x280080000000 | 0x12c180000000 | rank1 dev1 HBM |

### 步骤 2：device kernel 通过 GVA 访问远端（910B）

```c
// host 传入 GVA
smem_shm_create(..., &gva);  // gva = 0x280040000000 (rank0)

// device kernel（smem_shm_aicore_base_copy.h）:
//   kernel 从固定地址读元数据获取 rank/rankSize/symSize
//   然后用 GVA 算术计算 peer slot 地址
uint64_t offset = symSize;           // 0x10000000 (256MB)
uint64_t dst = gva + offset * 1;     // 0x280080000000 (rank1 的 GVA slot)
// 910B: dst 既是 GVA 也是 device VA，页表已映射到 rank1 物理 HBM
AscendC::DataCopyPad(dst, srcTensor, params);  // ✅ 直接访问 rank1 HBM
```

### 步骤 2 的 310P3 限制

```c
// 同样的 kernel 代码：
uint64_t dst = gva + offset * 1;     // 0x280080000000 (rank1 的 GVA slot)
// 310P3: dst=GVA，但 device VA 是 0x12c180000000，GVA 不在设备页表中
AscendC::DataCopy(dst, srcTensor, params);  // ❌ 无法寻址 GVA
```

### 310P3 SHM 的 host 侧替代路径

```c
// host 侧可以用 smem_bm_copy（与 BM 相同的 SDMA 路径）：
smem_copy_params_t params = {.src = localDVA, .dest = gva_of_peer, .dataSize = 16MB};
smem_bm_copy(handle, &params, SMEMB_COPY_L2G, 0);
// 内部：TransformVa(GVA→DVA) → AclrtMemcpyAsync(D2D)  ← ✅ 可用
// 但这失去了 SHM 的核心价值（device kernel 直接访问 GVA）
```

### 完整数据流对比

```
=== 910B（GVA == DVA，kernel 直接寻址）===

device kernel:
  dst = gva + symSize * targetRank
      = 0x280040000000 + 0x10000000 * 1
      = 0x280080000000  (rank1 GVA == rank1 DVA)
      │
      └──► DataCopyPad(dst=0x280080000000)
             device MMU 页表 → rank1 物理 HBM  ✅

=== 310P3（GVA ≠ DVA，kernel 无法寻址）===

device kernel:
  dst = gva + symSize * targetRank
      = 0x280040000000 + 0x10000000 * 1
      = 0x280080000000  (rank1 GVA)
      │
      └──► DataCopy(dst=0x280080000000)
             0x280080000000 不在 device VA 映射中  ❌
             (实际 device VA = 0x12c180000000)

host 侧替代:
  smem_bm_copy(src=localDVA=0x12c140000000, dst=GVA=0x280080000000, L2G)
      │                    │
      │           TransformVa(GVA→DVA)
      │                    │
      │           dstDVA=0x12c180000000
      │                    │
      └────────────────────┴──► AclrtMemcpyAsync(D2D)  ✅
```

---

## 四、三种模式对比

### 地址翻译链

| 模式 | 用户传入 | 第 1 步翻译 | 第 2 步翻译 | DMA 用的地址 |
|------|---------|-----------|-----------|------------|
| BM | GVA（`ptr_by_mem_type` 返回） | — | TransformVa: GVA→DVA | DVA |
| TRANS | peer 本地 DVA | TransformAddr: peerDVA→GVA | TransformVa: GVA→DVA | imported DVA |
| SHM(910B) | GVA | — | — | GVA=DVA |
| SHM(310P3) | GVA | — | — | 不可达 |

### 2-rank 地址分布总表（310P3，实测值）

| 模式 | 进程 | 本地 GVA | 本地 DVA | peer GVA | peer DVA (imported) |
|------|------|---------|---------|---------|-------------------|
| BM | rank0 | 0x280040000000 | 0x12c140000000 | 0x280080000000 | 0x12c180000000 |
| BM | rank1 | 0x280080000000 | 0x12c140000000 | 0x280040000000 | 0x12c180000000 |
| TRANS | rank0 | 0x1000000000000000 | 0x12c001000000 | 0x1000002000000000 | 0x12c140000000 |
| TRANS | rank1 | 0x1000002000000000 | 0x12c001000000 | 0x1000000000000000 | 0x12c140000000 |
| SHM | rank0 | 0x280040000000 | 0x12c140000000 | 0x280080000000 | 0x12c180000000 |
| SHM | rank1 | 0x280080000000 | 0x12c140000000 | 0x280040000000 | 0x12c180000000 |

> 注：BM 和 SHM 的 GVA 基址相同（`0x280040000000`）因为都由 VaManager 从同一地址空间分配。TRANS 的 GVA 基址不同（`0x1000000000000000`）因为使用 56-bit GVA 空间（`enable56BitsGva=true`）。BM 的本地 DVA 和 imported DVA 在不同段（`0x12c140000000` vs `0x12c180000000`），TRANS 的本地 DVA 是用户 `aclrtMalloc` 的地址（`0x12c001000000`）。

### 共享机制对比

| 模式 | export API | import API | 310P3 同卡 die | 310P3 跨卡 |
|------|-----------|-----------|--------------|-----------|
| BM | `AclrtMemExportToShareableHandle` | `AclrtMemImportFromShareableHandle` + ctx trick | ✅ | ✅ |
| TRANS | `AclrtIpcMemGetExportKey` | `AclrtIpcMemImportByKey`(DEFAULT/PEER) | ✅ | ✅ |
| SHM | `AclrtMemExportToShareableHandle` | `AclrtMemImportFromShareableHandle` + ctx trick | ✅ | ✅ |

### SDMA 拷贝路径（三种模式 host 侧共用）

```
hybm_data_batch_copy(src, dst, AUTO)
  │
  ├─ ClassifyAddressMask(src, dst) → direction (如 D2GD)
  ├─ LocateAddrAndRank → {srcRank, dstRank}
  ├─ GetPrioritedDataOperators → HostDataOpSDMA
  │     ├─ TransformVa(GVA→DVA) for src and dst
  │     └─ AclrtMemcpyAsync(srcDVA, dstDVA, D2D) + AclrtSynchronizeStream
  │
  └─ 310P3: 不支持 COPY_EXTEND_FLAG（paramSpace_ 未分配）
```

BM 和 TRANS 从 `hybm_data_batch_copy` 起完全相同，差异仅在上层地址翻译（TRANS 多一步 peer本地地址→GVA）。SHM 在 310P3 上 host 侧路径相同，但 device kernel 路径不可用。
