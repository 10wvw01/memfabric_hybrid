# 310P3 数据路径带宽分析与优化

**平台**: 310P3 (CANN 9.1.0)
**测试条件**: 混合 DRAM+HBM (各1GB), 2卡 (dev0+dev1 同卡)

---

## 1. 迭代时间线总览

| 版本 | 日期 | 主题 | 带宽重点 |
|------|------|------|---------|
| 基线 | 2026-08-29 ~ 09-07 | 310P3 数据路径实现设计：驱动适配 → 内存管理 → 拷贝路径 → 跨 rank 共享 | 全 10 路径首次可测，同步 memcpy 带宽基线 |
| V1 | 2026-09-08 | SDMA 带宽优化：host↔device async + stream retry + DRAM shm + GVA meta | host↔device 5-6 GB/s，device 间 10-11 GB/s，H2RH/RH2H ~18-19 GB/s |

---

## 2. 基线：310P3 数据路径实现设计

### 2.1 设计背景

310P3 不支持 910 平台的 HAL 驱动 API，需要基于 ACL VMM（Virtual Memory Management）API 重新构建完整的内存管理与数据拷贝通路。设计目标是在 310P3 上实现与 910 平台对齐的混合 DRAM+HBM 同 handle 能力，覆盖 SDMA 协议下全 10 路径。

### 2.2 驱动适配架构

310P3 通过动态加载 ACL VMM 符号替代 HAL 驱动，建立驱动适配层：

- **SOC 识别**：`GetAscendSocType()` 匹配实测 SOC 名 `"Ascend310P3"`，识别为 `ASCEND_310P3`。
- **ACL VMM 符号层**：`dl_acl_api` 动态加载 14 个 `aclrt*` VMM 符号（`MallocPhysical`/`FreePhysical`/`ReserveMemAddress`/`MapMem`/`UnmapMem`/`MemSetAccess`/`ExportToShareableHandle`/`ImportFromShareableHandle` 等），910B/950 环境缺失仅 WARN 零影响。
- **实机探测结论**：`handleType` 须 NONE（POSIX 返回 100000）、`memAttr=ACL_MEM_P2P_HUGE` 可用、granularity=2MB、`export` 须带 `ACL_RT_VMM_EXPORT_FLAG_DISABLE_PID_VALIDATION`。

### 2.3 内存管理设计

采用 `hbmSegment_` + `dramSegment_` 双段架构，通过 `HybmVaManager` 统一管理 GVA 地址空间：

**HBM 内存池（`HybmAclVmmSegment`）**：

| 操作 | 实现方式 |
|------|---------|
| 物理内存分配 | `aclrtMallocPhysical(ACL_MEM_P2P_HUGE)` |
| 虚拟地址预留 | `AclrtReserveMemAddress` 替代 `HalMemAddressReserve` |
| 地址映射 | `AclrtMapMem` |
| 跨 rank 导出 | `AclrtMemExportToShareableHandle`（带 `DISABLE_PID_VALIDATION`） |
| 跨 rank 导入 | `AclrtMemImportFromShareableHandle` + ctx trick + `AclrtMapMem` |
| 访问权限 | `AclrtMemSetAccess(RW)` 在本地和 imported VA 上各执行一次 |
| 释放 | `AclrtUnmapMem` / `AclrtFreePhysical` / `AclrtReleaseMemAddress` |

**DRAM 内存池（`HybmConnBasedSegment`）**：

| 操作 | 实现方式 |
|------|---------|
| 内存分配 | `shm_open` + `ftruncate` + `mmap(MAP_FIXED | MAP_SHARED)` |
| 跨 rank 共享 | `shm_open(name)` + `mmap(MAP_FIXED | MAP_SHARED)` 映射到相同 GVA |
| 跨 rank 导出 | `HostExportInfo` 传递 `shmName[64]` |
| SDMA 可达性 | `CheckSdmaReaches` 覆写返回 `true`（310P3 SVM 同节点可达） |
| 释放 | `munmap` + `shm_unlink` |

**GVA 地址空间**：VaManager 对 DRAM 和 HBM 使用同一地址池（GVM range `[40TB, 168TB)`），`memType` 仅作元数据。`ClassifyAddressMask` 通过 `allocatedMap_`/`reservedMap_` 中的 `memType` 元数据区分 DRAM/HBM 地址。rank N 位于 `gvaBase + N * maxSize`，HBM/DRAM 布局一致。

> 设计选型说明：ACL VMM 的 `aclrtMallocPhysical(ACL_MEM_LOCATION_TYPE_HOST)` + `AclrtMemExportToShareableHandle` 在 310P3 上不支持跨 device import（返回 107000）；`aclrtIpcMemGetExportKey` 不支持 mmap 内存（返回 507899）。`shm_open` + `MAP_SHARED` 是唯一可行的跨进程 host 内存共享方案。

### 2.4 SDMA 拷贝路径设计

`HostDataOpSDMA` 实现 10 条拷贝路径，按方向分为三类：

| 类别 | 路径 | 基线 memcpy 方式 | 说明 |
|------|------|-----------------|------|
| **device 间 (D2D)** | D2RD, RD2D, D2RH, RH2D | `AclrtMemcpyAsync(D2D)` + `AclrtSynchronizeStream` | device 间走 D2D async，单流 10-11 GB/s |
| **D2H 方向** | D2H, RH2H, RD2H | 同步 `AclrtMemcpy(D2H)` | host↔device 走同步 memcpy，带宽受限 |
| **H2D 方向** | H2D, H2RD, H2RH | 同步 `AclrtMemcpy(H2D)` | 同上，且远端路径叠加 shm |

**`CopyG2G`**：device 间拷贝走 `AclrtMemcpyAsync(ACL_MEMCPY_DEVICE_TO_DEVICE)` + `AclrtSynchronizeStream`，`Wait` 同步 ACL stream。310P3 支持 SVM（Shared Virtual Memory），device 可直接访问 host VA，`ACL_MEMCPY_DEVICE_TO_DEVICE` 对 HVA 和 DVA 均有效。

**`CopyLH2GD`/`CopyGD2LH`/`CopyLH2GH`/`CopyGH2LH`**：host↔device 拷贝基线使用同步 `AclrtMemcpy`，memcpy kind 由方向决定（H2D/D2H）。每个函数使用正确的 memcpy kind：

| 函数 | kind | 说明 |
|------|------|------|
| CopyLH2GD | H2D | host→HBM GVA |
| CopyGD2LH | D2H | HBM GVA→host |
| CopyLH2GH | H2D | host→DRAM GVA (device 可访问) |
| CopyGH2LH | D2H | DRAM GVA→host |

**`TransformVa`**：GVA→DVA 地址转换，按拷贝方向选择目标 HVA/DVA（参考 `DataOpDeviceRDMA::TransformVa` 方向查找表）。DRAM 地址 DVA=0 时 fallback 到 GVA(=HVA)，在 SVM 下可直接使用。

**`Initialize`**：跳过 host register（`paramSpace_` 保持 null），`COPY_EXTEND_FLAG` 返回 `BM_NOT_SUPPORTED`。

### 2.5 跨 rank 共享与拓扑

**HBM 跨 rank 导入**：通过 `AclrtMemExportToShareableHandle` 导出 shareable handle，远端 rank 通过 `AclrtMemImportFromShareableHandle` + ctx trick（切换 device context）+ `AclrtMapMem` 导入，再 `AclrtMemSetAccess` 设置访问权限。import 列表按同卡优先排序。

**DRAM 跨 rank 共享**：通过 POSIX `shm_open` + `MAP_SHARED` 实现跨进程物理内存共享。`ReserveMemorySpace` 生成 `/mf_dram_<pid>_<rank>` shm 名称，远端 rank 通过 `shm_open` + `MAP_FIXED` 映射到相同 GVA 地址。

**TRANS 模式**：用 ACL IPC（`AclrtIpcMemGetExportKey`/`AclrtIpcMemImportByKey`）替代运行时 IPC（`RtIpcSetMemoryName`/`RtIpcOpenMemory`），避免同卡跨 die P2P 预检拒绝（错误 507899）。

**拓扑与 P2P 管理**：

- `TopologyPreflight`：通过 store publish/gather rank→deviceId 拓扑记录，import 前校验 per-card P2P slot budget（≤8）。
- `IsSameCard`：使用 `deviceId/2`（310P3 `physicalChipId` 返回 per-die 而非 per-card）。
- `EnableRemotePeerAccess`：使用 `AclrtDeviceEnablePeerAccess`。
- 同卡跨 die P2P 错误容忍：`CanAccessPeer` 预检，同卡跨 die 可能返回错误但实际可拷贝。
- `aclrtMemAttr` 枚举值对齐 CANN ABI（此前枚举不匹配导致跨卡 import 返回 507899）。

### 2.6 基线带宽数据

#### 2.6.1 bm_perf_benchmark（SDMA 全 10 路径）

**命令**:
```bash
export LD_LIBRARY_PATH=output/smem/lib64:output/hybm/lib64:output/acc_links/lib64:output/acc_offload/lib64:$LD_LIBRARY_PATH
export MF_CONFIG_STORE_URL=tcp://127.0.0.1:8570
./build/benchmark/bm/bm_perf_benchmark -bw -ot sdma -t all -s 1048576 -cc 100 -bs 1 -d 0 -ws 2 -lrs 2 -ip tcp://127.0.0.1:8570
```

> **注意**：`-ip` 参数指定 configStore 的 TCP 地址，rank 0 在此地址启动嵌入式 store server，其余 rank 作为 client 连接。barrier 端口由 store URL 端口替换为 21666 自动派生。`LD_LIBRARY_PATH` 需包含 `output/` 下的 `smem`、`hybm`、`acc_links`、`acc_offload` 库路径。

| 路径 | 方向 | 基线带宽 (GB/s) | memcpy 路径 |
|------|------|-----------------|------------|
| H2D | Host→HBM | 4.8-5.2 | `AclrtMemcpy(H2D)` 同步 |
| D2H | HBM→Host | 5.5-6.0 | `AclrtMemcpy(D2H)` 同步 |
| H2RD | Host→DRAM GVA | 5.6-6.3 | `AclrtMemcpy(H2D)` 同步 |
| H2RH | Host→远端DRAM | 2.9-3.0 | `AclrtMemcpy(H2D)` 同步 + shm |
| D2RD | HBM→DRAM GVA | 10-11 | `AclrtMemcpyAsync(D2D)` |
| D2RH | HBM→远端Host | 6-7 | `AclrtMemcpyAsync(D2D)` + shm |
| RH2D | 远端Host→HBM | 6-7 | `AclrtMemcpyAsync(D2D)` + shm |
| RH2H | 远端Host→Host | 3.0-3.3 | `AclrtMemcpy(D2H)` 同步 + shm |
| RD2D | 远端HBM→本地HBM | 10-11 | `AclrtMemcpyAsync(D2D)` |
| RD2H | 远端HBM→Host | 6.0-6.2 | `AclrtMemcpy(D2H)` 同步 |

**基线特征**：host↔device 路径使用同步 `AclrtMemcpy`，D2H 仅 5.5-6.0 GB/s，H2RH 仅 2.9-3.0 GB/s。device 间路径（D2D async）约 10-11 GB/s。

#### 2.6.2 transfer_perf（TRANS 路径）

**命令**:
```bash
export LD_LIBRARY_PATH=output/smem/lib64:output/hybm/lib64:output/acc_links/lib64:output/acc_offload/lib64:$LD_LIBRARY_PATH
export MF_CONFIG_STORE_URL=tcp://127.0.0.1:22052
./build/benchmark/transfer/perf_cpp/transfer_perf 2 0 0 1 tcp://127.0.0.1:22052 &
./build/benchmark/transfer/perf_cpp/transfer_perf 2 1 1 1 tcp://127.0.0.1:22052
```

| Block Size | Latency (us) | Throughput (GB/s) |
|-----------|-------------|-------------------|
| 32KB | 16.6 | 15.4 |
| 64KB | 14.8 | 16.9 |
| 128KB | 17.4 | 17.4 |
| 256KB | 23.5 | 17.8 |
| 512KB | 38.6 | 17.9 |
| 1MB | 68.0 | 18.0 |
| 2MB | 125.5 | 18.0 |
| 4MB | 241.9 | 18.0 |
| 8MB | 477.3 | 18.0 |
| 16MB | 948.7 | 18.1 |

> transfer_perf 走 `smem_trans_write` → `CopyG2G(D2D async)` 路径，每轮 100 次拷贝每次都 `AclrtSynchronizeStream`，无法利用 DMA pipeline 并行。大 block 下吞吐量稳定在 18 GB/s。此数据在基线与 V1 间无变化。

##### 裸 ACL D2D 基准对比

直接用裸 ACL API 测的 D2D async 带宽上限（100次提交后统一 sync），与 transfer_perf 对比：

| Block Size | 裸 D2D (GB/s) | transfer_perf (GB/s) | 比值 |
|-----------|--------------|----------------------|------|
| 32KB | 5.2 | 15.4 | 3.0x (trans batch 优化) |
| 128KB | 17.9 | 17.4 | 0.97x |
| 1MB | 58.3 | 18.0 | 0.31x |
| 16MB | 83.8 | 18.1 | 0.22x |

> 注：裸 D2D 基准使用裸 ACL API 直接测试（100 次提交后统一 sync），其大 block 下的高带宽（84 GB/s）来自 DMA 多任务 pipeline 重叠。transfer_perf 走 `CopyG2G`（D2D async）路径，但每轮拷贝后立即 sync，无法利用 pipeline。两者差异在于 sync 粒度，而非 DMA 路径本身。bm_perf_benchmark 的 D2RD/RD2D 路径（10-11 GB/s）与 transfer_perf（18 GB/s）的差异来自测试框架和聚合方式不同。

**结论**: transfer_perf 大 block 下远低于裸 D2D 的 84 GB/s，根因是每轮拷贝后立即 sync，无法 pipeline。

---

## 3. V1 (2026-09-08): SDMA 带宽优化

### 3.1 优化思路

基线带宽数据暴露出两类瓶颈：

**瓶颈一：host↔device 路径带宽极低（5-6 GB/s）**

基线中 `CopyLH2GD`/`CopyGD2LH`/`CopyLH2GH`/`CopyGH2LH` 四个 host↔device 拷贝函数使用同步 `AclrtMemcpy`，该调用阻塞调用线程直到 DMA 完成才返回。每次拷贝期间 DMA 引擎存在提交→执行→等待→再提交的串行空隙，无法形成 pipeline。受影响的路径包括 D2H（5.5-6.0 GB/s）、H2D（4.8-5.2 GB/s）、H2RH（2.9-3.0 GB/s）、RH2H（3.0-3.3 GB/s）等，均远低于 device 间路径的 10-11 GB/s。

优化方向：将同步 `AclrtMemcpy` 改为 `AclrtMemcpyAsync` + `AclrtSynchronizeStream`，使 DMA 提交立即返回，利用引擎内部不同阶段的重叠形成 pipeline。

**瓶颈二：CopyG2G async 路径不稳定（RD2D 仅 ~10 GB/s）**

基线中 `CopyG2G` 用地址范围判断 memcpy kind，在混合 DRAM+HBM 场景下判断不可靠，async 路径频繁 fallback 到同步 + stream 重建，导致 D2D async pipeline 无法稳定工作。RD2D 仅 ~10 GB/s，与 D2RD 持平。

优化方向：`CopyG2G` 恢复为仅 D2D async（kind 由调用方指定），改善 fallback 策略减少不必要的 stream 重建。

**配套需求**

async 路径需要可靠性保障（stream 异常后可恢复）和正确的地址转换（`TransformVa` 按方向选择 HVA/DVA），否则 async 失败后会永久降级到同步。

### 3.2 优化方案

#### 3.2.1 host↔device 拷贝改用 AclrtMemcpyAsync (`19eda1cc`)

`CopyLH2GD`/`CopyGD2LH`/`CopyLH2GH`/`CopyGH2LH` 四个函数从同步 `AclrtMemcpy` 改为 `AclrtMemcpyAsync` + `AclrtSynchronizeStream`，async 失败后 fallback 到同步 + `ResetThreadAclStream`：

```cpp
// 优化前：同步 AclrtMemcpy（阻塞直到 DMA 完成）
auto ret = DlAclApi::AclrtMemcpy(dst, count, src, count, ACL_MEMCPY_DEVICE_TO_HOST);

// 优化后：AclrtMemcpyAsync + AclrtSynchronizeStream + fallback
void *st = (stream != nullptr) ? stream : HybmStreamManager::GetThreadAclStream();
auto ret = DlAclApi::AclrtMemcpyAsync(dst, count, src, count, kind, st);
if (ret != 0 || DlAclApi::AclrtSynchronizeStream(st) != 0) {
    if (stream == nullptr) { HybmStreamManager::ResetThreadAclStream(); }
    ret = DlAclApi::AclrtMemcpy(dst, count, src, count, kind);  // fallback
}
```

每个函数使用正确的 memcpy kind（H2D/D2H），`CopyG2G` 恢复为仅 D2D async（不再用地址范围判断 kind），host 地址的 kind 由调用方直接指定。

#### 3.2.2 Stream reset retry + TransformVa 方向感知 (`7b5525a4`)

为 async 路径提供可靠性保障和正确的地址转换：

- **`TransformVa` 方向感知**：按拷贝方向选择目标 HVA/DVA（不再统一转 DVA），确保 async 路径拿到正确的源/目标地址。
- **`ResetThreadAclStream`**：`HybmStreamManager` 新增方法，async 失败后重建 stream 状态，使后续 async 提交可恢复（而非永久降级到同步）。
- **`AclrtStream` 延迟初始化**：改为默认构造 + Reset，避免初始化时序问题导致 stream 不可用。
- **`CopyG2G` fallback 优化**：async 失败后 fallback 到同步 `AclrtMemcpy(D2D)` + `ResetThreadAclStream`，减少不必要的 stream 重建频率。

### 3.3 结果及分析

#### 3.3.1 优化前后对比

**命令**:
```bash
export LD_LIBRARY_PATH=output/smem/lib64:output/hybm/lib64:output/acc_links/lib64:output/acc_offload/lib64:$LD_LIBRARY_PATH
export MF_CONFIG_STORE_URL=tcp://127.0.0.1:8570
./build/benchmark/bm/bm_perf_benchmark -bw -ot sdma -t all -s 1048576 -cc 100 -bs 1 -d 0 -ws 2 -lrs 2 -ip tcp://127.0.0.1:8570
```

> **实测日期**: 2026-09-08，CANN 9.1.0，310P3，dev0+dev1 同卡。每个路径输出两个 rank 的独立结果。

| 路径 | 基线 (GB/s) | V1 实测 (GB/s) | 提升 | 实际调用函数 | 说明 |
|------|------------|----------------|------|-------------|------|
| H2D | 4.8-5.2 | 5.7-6.6 | 1.1-1.3x | `CopyLH2GD` H2D async | H2D async 单次延迟固定(~160us/1MB) |
| D2H | 5.5-6.0 | 6.2-6.5 | 1.1x | `CopyGD2LH` D2H async | D2H async，路由修正后使用正确 kind |
| H2RD | 5.6-6.3 | 6.0-6.1 | 1.0x | `CopyLH2GD` H2D async | H2D async 单次延迟固定 |
| H2RH | 2.9-3.0 | 12.7-19.2 | **4-6x** | `CopyLH2GH` H2D async + shm | H2D async + shm 远端写入 |
| D2RD | 10-11 | 10.2-10.9 | 1.0x | `CopyG2G` D2D async | D2D async + stream sync |
| D2RH | 6-7 | 6.2-6.5 | 1.0x | `CopyGD2LH` D2H async | 路由修正后走 D2H kind |
| RH2D | 6-7 | 5.9-6.0 | 1.0x | `CopyLH2GD` H2D async | 路由修正后走 H2D kind |
| **RH2H** | **3.0-3.3** | **15.0-18.8** | **5-6x** | `CopyGH2LH` D2H async + shm | D2H async + shm 远端写入 |
| RD2D | 10-11 | 10.5-10.9 | 1.0x | `CopyG2G` D2D async | D2D async + stream sync |
| RD2H | 6.0-6.2 | 6.2-6.5 | 1.0x | `CopyGD2LH` D2H async | D2H async 单次延迟固定 |

> **实测说明**：以上数据为 2026-09-08 多轮实测的代表性结果（CANN 9.1.0，310P3，dev0+dev1 同卡）。V1 代码包含 commit `19eda1cc` + `7b5525a4` + stream sync 修正 + 路由修正。

#### 3.3.2 路径分类

优化后 10 条路径按实际调用的拷贝函数和 async kind 分为三类：

**device 路径（CopyG2G / D2D async）— 中等带宽**:

| 路径 | 带宽 (GB/s) | 说明 |
|------|------------|------|
| D2RD | 10.2-10.9 | 同卡 HBM→远端 HBM(imported DVA), D2D async |
| RD2D | 10.5-10.9 | 跨 die HBM→HBM, D2D async |

**D2H 方向（CopyGD2LH/CopyGH2LH / D2H async）**:

| 路径 | 带宽 (GB/s) | 说明 |
|------|------------|------|
| D2H | 6.2-6.5 | HBM→本地 DRAM(shm HVA), D2H async |
| D2RH | 6.2-6.5 | HBM→远端 DRAM(shm HVA), D2H async |
| RH2H | 15.0-18.8 | 远端 DRAM(shm)→Host, D2H async + shm |
| RD2H | 6.2-6.5 | 远端 HBM→Host, D2H async |

**H2D 方向（CopyLH2GD/CopyLH2GH / H2D async）**:

| 路径 | 带宽 (GB/s) | 说明 |
|------|------------|------|
| H2D | 5.7-6.6 | Host→HBM, H2D async 单次延迟固定 |
| H2RD | 6.0-6.1 | Host→远端 HBM, H2D async 单次延迟固定 |
| H2RH | 12.7-19.2 | Host→远端 DRAM(shm), H2D async + shm 远端写入 |
| RH2D | 5.9-6.0 | 远端 DRAM(shm HVA)→HBM, H2D async |

> **路径→函数映射**：D2H 路径通过 `HYBM_LOCAL_DEVICE_TO_GLOBAL_HOST` 方向调用 `CopyGD2LH`（D2H async）。D2RH 路径走 `CopyGD2LH`（D2H async）。RH2D 路径走 `CopyLH2GD`（H2D async）。device 间路径（D2RD/RD2D）走 `CopyG2G`（D2D async）。

#### 3.3.3 分析

**async 提升原理**：改为 `AclrtMemcpyAsync` + `AclrtSynchronizeStream` 后，DMA 提交立即返回（不阻塞），`AclrtSynchronizeStream` 仅在等待完成时阻塞。提升效果取决于方向和路径：

- **H2RH 4-6x 提升**：Host→远端 DRAM 路径走 `CopyLH2GH`（H2D async），使 host→device 段 pipeline 化，叠加 shm 远端写入，2.9-3.0→12.7-19.2 GB/s。远端写入比本地 H2D（5.7-6.6）快，因为远端路径走 shm 映射后 DMA 直接写入远端 host 内存，避免了本地 HBM 写入的单次延迟。
- **RH2H 5-6x 提升**：远端 DRAM(shm)→Host 走 `CopyGH2LH`（D2H async），3.0-3.3→15.0-18.8 GB/s。远端 DRAM 经 shm 映射后 DMA 读取走 D2H async 路径。
- **H2D/D2H/H2RD/RD2H 轻微提升**：310P3 `AclrtMemcpyAsync(H2D/D2H)` 单次提交延迟固定约 160-180us/1MB，async 提交后仍立即 `AclrtSynchronizeStream`。DMA 引擎内部 host↔device 写路径是单阶段，无可重叠阶段，无法形成 pipeline。
- **device 路径（D2RD/RD2D）10-11 GB/s**：D2D async 路径在 310P3 上单流带宽约 10-11 GB/s，与 `transfer_perf` 的 18 GB/s（双 rank 聚合）在同一量级。

### 3.4 后续优化方向

#### H2D/D2H async 单阶段限制（已验证）

310P3 上 host↔device 拷贝分两类 DMA 路径，其 pipeline 能力存在本质差异：

**D2D async（可 pipeline）**：device 间拷贝走 `ACL_MEMCPY_DEVICE_TO_DEVICE`，310P3 SVM 下对 HVA 也有效。DMA 引擎内部 D2D 路径包含多个阶段（如地址转换→数据搬运→写回），不同拷贝任务的不同阶段可以重叠执行。批量提交 N 个 async 再统一 sync 时，任务 A 的写回阶段可与任务 B 的搬运阶段重叠，形成 pipeline。实测 D2H bs=1（单次 D2H kind）6.5 GB/s，bs=8（合并为单次 8MB D2H kind）5.4-5.6 GB/s，bs 提升未带来 pipeline 效果。

**H2D/D2H async（单阶段，不可 pipeline）**：host→device 和 device→host 拷贝走 `ACL_MEMCPY_HOST_TO_DEVICE`/`ACL_MEMCPY_DEVICE_TO_HOST`，DMA 引擎内部该路径为**单阶段**操作——每个任务从提交到完成是一个不可分割的阻塞过程，不存在可与其他任务重叠的内部阶段。批量提交 N 个 async 再统一 sync 时，DMA 引擎仍逐个串行执行，无法形成 pipeline。

实测验证（2026-09-08）：对 `BatchCopyLH2GD`/`BatchCopyLH2GH`/`BatchCopyGD2LH`/`BatchCopyGH2LH` 改为批量 async 提交 + 统一 sync（消除每条 async 后的 sync 开销），各 batch size 下带宽如下：

| batch size | H2D V1 (GB/s) | H2D V2 (GB/s) | D2H (D2H kind, GB/s) |
|------------|---------------|---------------|----------------------|
| 1 | 5.7-6.6 | 5.4 | 6.2-6.5 |
| 8 | 5.7-6.6 | 6.1 | 5.4-5.6 |

H2D/D2H 带宽随 batch size 无显著提升，均在 5-7 GB/s 范围。瓶颈在 DMA 硬件路径本身——单阶段无重叠空间，软件层 pipeline 无法解决。该方向已验证为死路，不再追求。

#### 其他方向

**RH2H 15-19 GB/s**：`CopyGH2LH` 的 D2H async 路径 + shm 远端读取。

**transfer_perf（18 GB/s）**：`smem_trans_write` 每次拷贝后 sync，无法利用 DMA pipeline。transfer_perf 走 `CopyG2G`（D2D async）路径，D2D async 本身支持 pipeline，瓶颈在于每轮拷贝后立即 sync。要提升需要 batch async + 统一 sync。

| 优先级 | 方向 | 预期收益 | 改动范围 |
|--------|------|---------|---------|
| P1 | transfer_perf batch async + 统一 sync | 18→60-80 GB/s | `transfer_perf.cpp` 或 `smem_trans_entry.cpp` |
| P2 | `CopyG2G` async fallback 策略优化（减少 ResetThreadAclStream 频率） | 减少不必要的 stream 重建 | `hybm_data_op_sdma.cpp` |


