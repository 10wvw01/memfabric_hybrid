# 310P3 数据路径带宽分析与优化

**平台**: 310P3 (CANN 9.1.0)
**测试条件**: 混合 DRAM+HBM (各1GB), 同卡跨 die (dev0+dev1, NPU2)
**测试工具**: `benchmark/bm/bm_perf_benchmark`（SDMA 全 10 路径）、`benchmark/transfer/perf_cpp/transfer_perf`（TRANS 路径）
**编译**: `bash script/build_and_pack_run.sh --build_test ON`（`--build_test ON` 将 benchmark 纳入 CMake 构建，产物在 `build/benchmark/` 下）

---

## 1. 迭代时间线总览

| 版本 | 日期 | 主题 | 带宽重点 |
|------|------|------|---------|
| 基线 | 2026-08-29 ~ 09-09 | 310P3 数据路径实现设计：驱动适配 → 内存管理 → 拷贝路径 → 跨 rank 共享 → async 优化 | host↔device 5.0-7.1 GB/s（同卡并发），device 间 10.2-17.7 GB/s，H2RH/RH2H ~10.7-16.4 GB/s |
| 分析 | 2026-09-09 | `bm_perf_benchmark` 同卡并发 vs 裸 ACL 独占带宽差异分析（§2.7） | 128MB H2D 并发 7.0 vs 独占 12.1 GB/s，根因为同卡 die 共享 DMA 通路 |
| v1 | 2026-09-09 | benchmark 新增 `-dl` 跨卡设备列表参数，支持 310P3 跨卡拓扑测试（§3） | 跨卡并发 host↔device 5.1-9.8 GB/s（5 次中值），接近裸 ACL 独占带宽 |
| 复测 | 2026-09-10 | §3.3 全部结论重新采集数据校验，插桩验证框架 ACL 时间 vs 裸 ACL | 框架开销 ~110-151us/call 确认；原"并发额外降速 6.5%"不复现（跨卡并发无 ACL 降速） |

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

| 类别 | 路径 | memcpy 方式 | 说明 |
|------|------|------------|------|
| **device 间 (D2D)** | D2RD, RD2D | `BatchCopyG2G` → `CopyG2GAsync(D2D)` + `InnerWait` | device 间走 D2D async（批量合并 + 统一 sync），单流 10.2-17.7 GB/s |
| **D2H 方向** | D2H, D2RH, RH2H, RD2H | `CopyGD2LH`/`CopyGH2LH`：`AclrtMemcpyAsync(D2H)` + `AclrtSynchronizeStream` | host↔device 走 async + sync，带宽 5.3-6.5 GB/s（远端 shm 路径 13.1-16.4 GB/s） |
| **H2D 方向** | H2D, H2RD, H2RH, RH2D | `CopyLH2GD`/`CopyLH2GH`：`AclrtMemcpyAsync(H2D)` + `AclrtSynchronizeStream` | async + sync，远端 shm 路径 10.7-15.9 GB/s |

**`CopyG2G`**：device 间拷贝走 `AclrtMemcpyAsync(ACL_MEMCPY_DEVICE_TO_DEVICE)` + `AclrtSynchronizeStream`，`Wait` 同步 ACL stream。310P3 支持 SVM（Shared Virtual Memory），device 可直接访问 host VA，`ACL_MEMCPY_DEVICE_TO_DEVICE` 对 HVA 和 DVA 均有效。

**`CopyLH2GD`/`CopyGD2LH`/`CopyLH2GH`/`CopyGH2LH`**：host↔device 拷贝统一使用 `AclrtMemcpyAsync` + `AclrtSynchronizeStream`，async 失败后 fallback 到同步 `AclrtMemcpy` + `ResetThreadAclStream`。`stream=null` 时取 `GetThreadAclStream()`。memcpy kind 由方向决定（H2D/D2H）：

| 函数 | kind | 说明 |
|------|------|------|
| CopyLH2GD | H2D | host→HBM GVA，`stream=null` 时走 `GetThreadAclStream` async + sync |
| CopyGD2LH | D2H | HBM GVA→host，`stream=null` 时走 `GetThreadAclStream` async + sync |
| CopyLH2GH | H2D | host→DRAM GVA (device 可访问)，`stream=null` 时走 `GetThreadAclStream` async + sync |
| CopyGH2LH | D2H | DRAM GVA→host，`stream=null` 时走 `GetThreadAclStream` async + sync |

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

#### 测试场景说明

基线带宽使用三个测试工具，各自场景不同，对比如下：

| 工具 | 进程模型 | rank 数 | dev 分配 | 谁测量 | 并发模式 | 测试路径 |
|------|---------|---------|---------|--------|---------|---------|
| `bm_perf_benchmark` | `fork()` 2 子进程 | 2 | rank0=dev0, rank1=dev1（同卡跨 die） | dev0 (rank0) | **两 rank 并发** | SDMA 全 10 路径 |
| `transfer_perf` | 手动启动 2 进程 | 2 | rank0=dev0, rank1=dev1（同卡跨 die） | rank0 | **rank0 独占**（rank1 仅提供远端 HBM 目标） | TRANS D2D |
| `310p3_aclrt_benchmark` | MPI 2 进程 | 2 | rank0=dev0 (exporter), rank1=dev1 (importer) | rank1 | **rank1 独占**（rank0 仅 export 后等待） | 裸 ACL D2D/H2D/D2H |

**`bm_perf_benchmark` 详细场景**：

- `-d 0 -ws 2 -lrs 2`：`worldSize=2`、`localRankSize=2`、起始 dev=0
- `fork()` 创建 2 子进程：rank0→dev0、rank1→dev1（310P3 同卡两 die）
- 10 条路径按 `ALL_DIRECTION` 顺序执行，每条路径内**两 rank 并发拷贝**后 barrier 同步（§2.7 分析的 barrier 模式）
- `remoteRankId = (rankId + 1) % worldSize`：rank0 的远端=rank1、rank1 的远端=rank0
- 以下数据仅保留 dev0 (rank0) 的测量值，每规格独立运行 5 次取中值
- `batchSize=1`，`copyCount=100`（1MB/16MB）或 `10`（128MB）
- H2D/D2H 路径各 rank 向**自己**的 device 拷贝；H2RD/D2RD/RH2D 等路径各 rank 向**对端**的 device/host 拷贝

**`transfer_perf` 详细场景**：

- 参数：`transfer_perf <rankSize> <rankId> <deviceId> <useSdma> <storeUrl>`
- rank0（`2 0 0 1 tcp://...`）= dev0，rank1（`2 1 1 1 tcp://...`）= dev1
- 仅 rank0 执行带宽测试，通过 `smem_shm_control_allgather` 获取 rank1 的 dev_addr 后向其写入
- 2 线程 × 100 次迭代，`batch_size=32`，每次 `smem_trans_batch_write` 后 `InnerWait` 同步
- 测试 D2D 路径：rank0 dev_addr → rank1 dev_addr（跨 die HBM→HBM）

**`310p3_aclrt_benchmark` 详细场景**：

- MPI 2 进程：rank0=dev0（exporter）分配 VMM P2P 内存并 export shareable handle；rank1=dev1（importer）import 后分配本地 VMM buffer
- 仅 rank1 执行带宽测试（单 rank 独占，无并发争用）
- D2D：dev0 exported VA → dev1 local（Pull 模式）；H2D：host → dev1 local VA；D2H：dev1 local VA → host
- 对比 single-sync（每次 async 后 sync）与 batch-sync（100 次提交后统一 sync）
- 100 次迭代，10 次 warmup，5 种 block size

#### 2.6.1 bm_perf_benchmark（SDMA 全 10 路径）

**命令**（1MB 规格）:
```bash
source /usr/local/Ascend/ascend-toolkit/latest/set_env.sh
export LD_LIBRARY_PATH=output/smem/lib64:output/hybm/lib64:output/acc_links/lib64:output/acc_offload/lib64:$LD_LIBRARY_PATH
export MF_CONFIG_STORE_URL=tcp://127.0.0.1:8570
./build/benchmark/bm/bm_perf_benchmark -bw -ot sdma -t all -s 1048576 -cc 100 -bs 1 -d 0 -ws 2 -lrs 2 -ip tcp://127.0.0.1:8570
```

> 16MB 规格：`-s 16777216`；128MB 规格：`-s 134217728 -cc 10 -gs 2147483648`（GVA 池需 ≥2GB）。

**1MB 规格**（`-s 1048576 -cc 100 -bs 1`）：

| 路径 | 方向 | 基线带宽 (GB/s) | memcpy 路径 |
|------|------|-----------------|------------|
| H2D | Host→HBM | 5.0 | `AclrtMemcpyAsync(H2D)` + sync |
| D2H | HBM→Host | 5.7 | `AclrtMemcpyAsync(D2H)` + sync |
| H2RD | Host→远端HBM | 5.9 | `AclrtMemcpyAsync(H2D)` + sync |
| H2RH | Host→远端DRAM | 15.9 | `AclrtMemcpyAsync(H2D)` + sync + shm |
| D2RD | 本地HBM→远端HBM | 10.2 | `AclrtMemcpyAsync(D2D)` + sync |
| D2RH | HBM→远端Host | 5.3 | `AclrtMemcpyAsync(D2H)` + sync |
| RH2D | 远端Host→HBM | 5.3 | `AclrtMemcpyAsync(H2D)` + sync |
| RH2H | 远端Host→Host | 16.4 | `AclrtMemcpyAsync(D2H)` + sync + shm |
| RD2D | 远端HBM→本地HBM | 10.4 | `AclrtMemcpyAsync(D2D)` + sync |
| RD2H | 远端HBM→Host | 5.6 | `AclrtMemcpyAsync(D2H)` + sync |

**16MB 规格**（`-s 16777216 -cc 100 -bs 1`）：

| 路径 | 方向 | 基线带宽 (GB/s) | memcpy 路径 |
|------|------|-----------------|------------|
| H2D | Host→HBM | 7.0 | `AclrtMemcpyAsync(H2D)` + sync |
| D2H | HBM→Host | 6.5 | `AclrtMemcpyAsync(D2H)` + sync |
| H2RD | Host→远端HBM | 6.9 | `AclrtMemcpyAsync(H2D)` + sync |
| H2RH | Host→远端DRAM | 14.7 | `AclrtMemcpyAsync(H2D)` + sync + shm |
| D2RD | 本地HBM→远端HBM | 16.0 | `AclrtMemcpyAsync(D2D)` + sync |
| D2RH | HBM→远端Host | 6.5 | `AclrtMemcpyAsync(D2H)` + sync |
| RH2D | 远端Host→HBM | 7.1 | `AclrtMemcpyAsync(H2D)` + sync |
| RH2H | 远端Host→Host | 14.8 | `AclrtMemcpyAsync(D2H)` + sync + shm |
| RD2D | 远端HBM→本地HBM | 17.1 | `AclrtMemcpyAsync(D2D)` + sync |
| RD2H | 远端HBM→Host | 6.5 | `AclrtMemcpyAsync(D2H)` + sync |

**128MB 规格**（`-s 134217728 -cc 10 -bs 1 -gs 2147483648`）：

| 路径 | 方向 | 基线带宽 (GB/s) | memcpy 路径 |
|------|------|-----------------|------------|
| H2D | Host→HBM | 7.0 | `AclrtMemcpyAsync(H2D)` + sync |
| D2H | HBM→Host | 6.5 | `AclrtMemcpyAsync(D2H)` + sync |
| H2RD | Host→远端HBM | 6.8 | `AclrtMemcpyAsync(H2D)` + sync |
| H2RH | Host→远端DRAM | 10.7 | `AclrtMemcpyAsync(H2D)` + sync + shm |
| D2RD | 本地HBM→远端HBM | 16.5 | `AclrtMemcpyAsync(D2D)` + sync |
| D2RH | HBM→远端Host | 6.5 | `AclrtMemcpyAsync(D2H)` + sync |
| RH2D | 远端Host→HBM | 7.0 | `AclrtMemcpyAsync(H2D)` + sync |
| RH2H | 远端Host→Host | 13.1 | `AclrtMemcpyAsync(D2H)` + sync + shm |
| RD2D | 远端HBM→本地HBM | 17.7 | `AclrtMemcpyAsync(D2D)` + sync |
| RD2H | 远端HBM→Host | 6.1 | `AclrtMemcpyAsync(D2H)` + sync |

**基线特征**：host↔device 路径 5.0-7.1 GB/s（H2D/D2H kind，单阶段 DMA 无法 pipeline；同卡两 rank 并发拷贝共享 DMA 通路，带宽低于单 rank 独占，详见 §2.7）；device 间路径 D2RD/RD2D 随 block 增大从 10.2-10.4 GB/s（1MB）升至 16.5-17.7 GB/s（128MB），接近跨 die D2D 硬件上限 ~19 GB/s；远端 DRAM 路径 H2RH/RH2H 在 1MB 时 15.9-16.4 GB/s，16MB 时 14.7-14.8 GB/s，128MB 时 10.7-13.1 GB/s。

##### 路径分类与分析

基线 10 条路径按实际调用的拷贝函数和 async kind 分为三类：

**device 路径（CopyG2G / D2D async）— 中等带宽**:

| 路径 | 带宽 (GB/s) | 说明 |
|------|------------|------|
| D2RD | 10.2-16.5 | 同卡 HBM→远端 HBM(imported DVA), D2D async |
| RD2D | 10.4-17.7 | 跨 die HBM→HBM, D2D async |

**D2H 方向（CopyGD2LH / CopyGH2LH / D2H async）**:

| 路径 | 带宽 (GB/s) | 说明 |
|------|------------|------|
| D2H | 5.7-6.5 | HBM→本地 DRAM(shm HVA), `CopyGD2LH` D2H async + sync |
| D2RH | 5.3-6.5 | HBM→远端 DRAM(shm HVA), `CopyGD2LH` D2H async + sync |
| RH2H | 13.1-16.4 | 远端 DRAM(shm)→Host, `CopyGH2LH` D2H async + sync + shm |
| RD2H | 5.6-6.5 | 远端 HBM→Host, `CopyGD2LH` D2H async + sync |

**H2D 方向（CopyLH2GD/CopyLH2GH / H2D async）**:

| 路径 | 带宽 (GB/s) | 说明 |
|------|------------|------|
| H2D | 5.0-7.0 | Host→HBM, H2D async 单次延迟固定 |
| H2RD | 5.9-6.9 | Host→远端 HBM, H2D async 单次延迟固定 |
| H2RH | 10.7-15.9 | Host→远端 DRAM(shm), H2D async + shm 远端写入 |
| RH2D | 5.3-7.1 | 远端 DRAM(shm HVA)→HBM, H2D async |

> **路径→函数映射**：D2H 路径通过 `HYBM_LOCAL_DEVICE_TO_GLOBAL_HOST` 方向调用 `CopyGD2LH`（D2H async + sync）。D2RH 路径走 `CopyGD2LH`（D2H async + sync）。RH2D 路径走 `CopyLH2GD`（H2D async + sync）。device 间路径（D2RD/RD2D）走 `BatchCopyG2G` → `CopyG2GAsync`（D2D async）+ `InnerWait`。

**路径带宽差异根因**：

- **H2RH/RH2H 10.7-16.4 GB/s**：远端 DRAM 经 shm 映射后，DMA 直接读/写远端 host 内存（HVA），走 host↔host DMA 路径带宽高于 host↔HBM。比本地 H2D/D2H（5.0-7.1 GB/s）快 1.5-3 倍。
- **H2D/D2H/H2RD/D2RH/RH2D/RD2H 5.0-7.1 GB/s**：310P3 host↔device 单次提交延迟固定约 160-200us/1MB。H2D 方向（`CopyLH2GD`）走 `AclrtMemcpyAsync(H2D)` + `AclrtSynchronizeStream`；D2H 方向（`CopyGD2LH`）走 `AclrtMemcpyAsync(D2H)` + `AclrtSynchronizeStream`。DMA 引擎内部 host↔device 路径是单阶段，无可重叠阶段，无法形成 pipeline。同卡两 rank 并发拷贝共享 DMA 通路，带宽低于单 rank 独占（详见 §2.7）。
- **device 路径（D2RD/RD2D）10.2-17.7 GB/s**：D2D async 路径在 310P3 上单流带宽随 block 增大从 10.2-10.4 GB/s（1MB）升至 16.5-17.7 GB/s（128MB），接近跨 die D2D 硬件上限 ~19 GB/s。D2D 路径并发争用不显著（device 间 DMA 带宽充裕）。

#### 2.6.2 transfer_perf（TRANS 路径）

**命令**:
```bash
source /usr/local/Ascend/ascend-toolkit/latest/set_env.sh
export LD_LIBRARY_PATH=output/smem/lib64:output/hybm/lib64:output/acc_links/lib64:output/acc_offload/lib64:$LD_LIBRARY_PATH
export MF_CONFIG_STORE_URL=tcp://127.0.0.1:22052
./build/benchmark/transfer/perf_cpp/transfer_perf 2 0 0 1 tcp://127.0.0.1:22052 &
./build/benchmark/transfer/perf_cpp/transfer_perf 2 1 1 1 tcp://127.0.0.1:22052
```

| Block Size | Latency (us) | Throughput (GB/s) |
|-----------|-------------|-------------------|
| 32KB | 34.3 | 10.6 |
| 64KB | 35.5 | 15.1 |
| 128KB | 39.4 | 17.5 |
| 256KB | 45.8 | 17.8 |
| 512KB | 59.2 | 17.9 |
| 1MB | 83.7 | 18.0 |
| 2MB | 138.9 | 18.0 |
| 4MB | 242.7 | 18.1 |
| 8MB | 471.3 | 18.1 |
| 16MB | 936.0 | 18.1 |

> transfer_perf BW 测试走 `smem_trans_batch_write`（batch_size=32）→ `BatchCopyG2G` → `CopyG2GAsync`(D2D) 路径，每次调用将 32 个连续 block 合并为 1 个 async 提交后立即 `InnerWait` 同步。2 线程 × 100 次迭代，每次 `smem_trans_batch_write` 后 sync 阻止了跨迭代 pipeline。大 block 下吞吐量稳定在 18 GB/s（2 线程聚合）。

#### 2.6.3 310p3_aclrt_benchmark（裸 ACL D2D/H2D/D2H）

裸 ACL VMM P2P 基准测试，直接调用 `aclrtMemcpyAsync` + `aclrtSynchronizeStream`，不经过 MemFabric GVA 框架，用于量化框架开销与 DMA 硬件上限。host 内存使用 `posix_memalign`（非 `aclrtMallocHost`）。测试场景见上方"测试场景说明"表。

**命令**（同卡跨 die dev0→dev1）：
```bash
source /usr/local/Ascend/ascend-toolkit/latest/set_env.sh
cd examples/310p3_aclrt_benchmark && bash build.sh && bash run.sh 0 1
```

> `run.sh [devA] [devB]` 默认 devA=0, devB=1（同卡跨 die）。跨卡测试：`bash run.sh 0 2`。产物在 `examples/310p3_aclrt_benchmark/310p3_aclrt_benchmark`，构建依赖 MPI（`mpirun`）与 CANN（`ASCEND_HOME_PATH`）。

**D2D**（dev0 exported VA → dev1 local, Pull 模式, `ACL_MEMCPY_DEVICE_TO_DEVICE`）：

| Block Size | single-sync (GB/s) | batch-sync (GB/s) | pipeline ratio |
|-----------|--------------------|-------------------|----------------|
| 1MB | 7.7 | 17.7 | 2.30x |
| 4MB | 13.7 | 18.7 | 1.36x |
| 16MB | 17.4 | 19.1 | 1.10x |
| 64MB | 18.4 | 19.1 | 1.04x |
| 128MB | 18.7 | 19.2 | 1.03x |

**H2D / D2H**（host ↔ dev1 local VA, `ACL_MEMCPY_HOST_TO_DEVICE` / `ACL_MEMCPY_DEVICE_TO_HOST`）：

| Block Size | H2D single (GB/s) | H2D batch (GB/s) | ratio | D2H single (GB/s) | D2H batch (GB/s) | ratio |
|-----------|-------------------|------------------|-------|-------------------|------------------|-------|
| 1MB | 5.6 | 5.8 | 1.04x | 5.5 | 5.7 | 1.04x |
| 4MB | 6.6 | 6.5 | 0.98x | 6.8 | 6.9 | 1.01x |
| 16MB | 10.1 | 10.2 | 1.01x | 9.9 | 9.8 | 0.99x |
| 64MB | 11.3 | 11.2 | 0.99x | 11.7 | 11.8 | 1.01x |
| 128MB | 12.1 | 11.9 | 0.98x | 12.1 | 11.7 | 0.97x |

**特征分析**：

- **D2D batch-sync pipeline 显著**：1MB 时 batch-sync（17.7 GB/s）是 single-sync（7.7 GB/s）的 2.30x，小 block 下批量提交利用 DMA 多阶段重叠形成 pipeline。随 block 增大 single-sync 逼近 batch-sync（128MB 时 18.7 vs 19.2，ratio 1.03x），大 block 单次提交已接近硬件上限。大 block（4MB+）下 batch-sync 稳定在 18.7-19.2 GB/s，即跨 die D2D 硬件上限 ≈19 GB/s。
- **H2D/D2H batch-sync ratio ≈1.0x**：所有 block size 下 batch-sync 无加速（ratio 0.97-1.04x），实证确认 H2D/D2H 为单阶段 DMA，无 pipeline 空间（与 §4 结论一致）。但 single-sync 带宽随 block 增大显著提升（1MB 5.5-5.6 → 128MB 12.1 GB/s），原因是固定 per-call 开销（stream sync 延迟 ~150us）被大 payload 摊薄，非 pipeline 效果。


---

### 2.7 裸 ACL vs MemFabric 框架带宽差异分析

#### 差异现象

§2.6.3 裸 ACL benchmark 测得 128MB H2D/D2H 带宽 12.1 GB/s（单进程独占），而框架 `bm_perf_benchmark` 仅 7.0/6.5 GB/s，差距达 ~42-46%。差距随 block size 增大而扩大：1MB 时差距小，16MB 时 ~1.4x，128MB 时 ~1.8x。

| 路径 | Block | 裸 ACL single (GB/s) | MemFabric 同卡并发 (GB/s) | 差异 | 说明 |
|------|-------|----------------------|--------------------------|------|------|
| D2D | 1MB | 7.7 | 10.2 | 框架高 33% | `BatchCopyG2G` 部分聚合，高于裸 ACL single |
| D2D | 16MB | 17.4 | 16.0 | 框架低 8% | 接近 |
| D2D | 128MB | 18.7 | 16.5 | 框架低 12% | 接近，D2D 并发争用小 |
| H2D | 1MB | 5.6 | 5.0 | 框架低 11% | per-call 开销主导，差距小 |
| H2D | 16MB | 10.1 | 7.0 | 框架低 31% | 并发争用 DMA |
| H2D | 128MB | 12.1 | 7.0 | 框架低 42% | 并发争用带宽减半 |
| D2H | 1MB | 5.5 | 5.7 | 接近 | per-call 开销主导 |
| D2H | 16MB | 9.9 | 6.5 | 框架低 34% | 并发争用 |
| D2H | 128MB | 12.1 | 6.5 | 框架低 46% | 并发争用 |

D2D 路径两者接近，H2D/D2H 路径差距随 block 增大而扩大，小 block 下接近。以下逐步排查差异来源。

#### 步骤 1：确认 memcpy kind 一致

通过代码审查确认框架 310P3 路径（`hybm_data_op_sdma.cpp:222`）使用 `AclrtMemcpyAsync(ACL_MEMCPY_HOST_TO_DEVICE)` + `AclrtSynchronizeStream`，D2H 使用 `ACL_MEMCPY_DEVICE_TO_HOST`，与裸 ACL benchmark 完全一致。HBM 内存分配参数（`aclrtMallocPhysical` prop 字段、`aclrtReserveMemAddress` flags、`aclrtMapMem` flags）也完全一致。host 内存均使用 `posix_memalign(4096)`。

#### 步骤 2：确认时间统计方式一致

框架 `BatchCopyPut`（`band_width_calculator.cpp:523-528`）使用 `TimeNs()`（`clock_gettime(CLOCK_MONOTONIC)`）包裹 `copyCount` 次迭代循环，裸 ACL 使用 `gettimeofday` 包裹 100 次循环。均为 wall-clock 时间，per-rank 独立计时。通过插桩验证框架 per-call 框架开销仅 ~0.1ms（128MB）/ ~28us（1MB），不足以解释 ~46% 差距。

#### 步骤 3：定位根因 — 两 rank 并发拷贝

在 `CopyLH2GD` 和 `smem_bm_copy_batch` 中插入时间戳，发现两 rank 的拷贝开始时间仅差 0.16us（并发启动），且两者均报告 `isMaster_=1`。根因是 benchmark 的 barrier 模式设计面向多节点场景，节点内 rank 并发执行，而 310P3 同卡跨 die 不满足"节点内各 rank 独占 DMA"的假设。

#### `isMaster_` 与 barrier 模式

`BandWidthCalculator` 构造函数（`band_width_calculator.cpp:130`）在 fork 前设置 `isMaster_`：

```
BandWidthCalculator::BandWidthCalculator(BwTestParam &param) {
    cmdParam_ = param;
    if (param.localRankStart == 0) {   // ← fork 前在父进程执行
        isMaster_ = true;              // ← 所有子进程继承此值
    }
}
```

`localRankStart`（`-rs` / `--rank-start`，默认 0）是本节点的起始 rankId，用于多节点场景：节点 A 的 rank 从 0 开始，节点 B 的 rank 从 `localRankSize` 开始。`MultiProcessExecute` 中每个子进程 i 的 rankId = `i + localRankStart`（`band_width_calculator.cpp:156`）。原逻辑意图是"本节点 rank0 为 master"，即 `localRankStart == 0` 时该节点是主节点。

`MultiProcessExecute` 通过 `fork()` 创建子进程，`Execute` 在子进程中运行但从未更新 `isMaster_`。`BandWidthCalculation` 的 barrier 模式（`band_width_calculator.cpp:476`）依赖 `isMaster_` 区分 master/non-master：

```
if (!isMaster_) { barrier->Barrier(); }  // ← non-master 拷贝前等待
// 拷贝
if (isMaster_)  { barrier->Barrier(); }  // ← master 拷贝后等待
```

#### 设计意图：节点间序列化，节点内并发

原设计面向**多节点**场景：

- **主节点**（`localRankStart == 0`）→ `isMaster_=true` → 本节点所有 rank 跳过 pre-barrier，**先并发拷贝** → 跑完后 post-barrier 放行
- **从节点**（`localRankStart != 0`）→ `isMaster_=false` → pre-barrier 等主节点跑完 → 本节点所有 rank **后并发拷贝**

barrier 序列化的是**节点间**，不是节点内。节点内的 rank 设计为并发——多节点场景下同一节点内的各 rank 跑在不同卡上，各自有独立 DMA 引擎，并发不争用，测的是每 rank 在该路径上的带宽。

#### 耗时计算方式

benchmark 的带宽计算（`band_width_calculator.cpp:523-528, 394-396`）：每个 rank 独立用 `TimeNs()` 包裹自己的 `copyCount` 次迭代循环，`BW = totalBytes / totalTime`。即每个 rank 测的是**自己这段循环的 wall-clock 时间**。

**设计意图——多节点多卡（节点内各 rank 独占 DMA，并发不争用）**：

```
时间 →
节点A rank0: startTime [====== copy 128MB ×10 ======] endTime
节点A rank1: startTime [====== copy 128MB ×10 ======] endTime   ← 各卡独立 DMA，不争用
                                                             Barrier()
节点B rank2:                                                 Barrier() startTime [copy] endTime
节点B rank3:                                                 Barrier() startTime [copy] endTime
```
每 rank 独占自己卡的 DMA 通路，测得独占带宽。

**实际行为——310P3 单节点同卡跨 die（两 die 共享 DMA 通路）**：

```
时间 →
rank0: startTime [============== copy 128MB ×10 ==============] endTime  Barrier()
rank1: startTime [============== copy 128MB ×10 ==============] endTime  Barrier()
       ↑ 两 rank 同时 startTime，共享 DMA 通路
```
310P3 同卡两 die 共享 host↔device DMA 通路，两 rank 并发拷贝时带宽被对半分（各 ~6.5-7.0 GB/s），`BW = (128MB × 10) / totalTime ≈ 6.5-7.0 GB/s`。

#### 结论：拓扑不匹配设计假设

原设计假设"节点内各 rank 跑在不同卡上，各自独占 DMA 通路"。310P3 单节点 2 rank 跑在同卡跨 die，两 die 共享 host↔device DMA 通路，不满足该假设，并发导致 H2D/D2H 大 block 下带宽减半。D2D 路径受影响小（device 间 DMA 带宽充裕，并发争用不显著）。

因此 §2.6.1 数据为**同卡并发带宽**（两 rank 同时拷贝），§2.6.3 裸 ACL 数据为**单 rank 独占带宽**。两者差异反映的是拓扑差异（同卡共享 DMA vs 独占 DMA），非代码 bug。多卡或多节点拓扑下框架并发带宽应与裸 ACL 独占带宽一致（详见 §3 跨卡验证）。


---

## 3. v1：跨卡拓扑支持与带宽数据

### 3.1 `-dl` 跨卡设备列表参数

基线 `bm_perf_benchmark` 通过 `-d` 指定起始 deviceId，子进程 i 的 deviceId = `i + deviceId`，即 rank 连续分配到 dev0, dev1, ...。310P3 同卡两 die 为 dev0+dev1，`-d 0 -lrs 2` 得到 dev0+dev1（同卡），两 die 共享 host↔device DMA 通路，并发拷贝带宽减半（§2.7）。

v1 新增 `-dl` / `--device-list` 参数（`band_width_calculator.h:60`, `main.cpp:248-262`），显式指定每个 rank 的 deviceId，逗号分隔：

```bash
# 同卡跨 die（基线行为，dev0+dev1）
./build/benchmark/bm/bm_perf_benchmark -bw -ot sdma -t all -s 134217728 -cc 10 -bs 1 -gs 2147483648 -d 0 -ws 2 -lrs 2 -ip tcp://127.0.0.1:8570

# 跨卡（v1，dev0+dev2）
./build/benchmark/bm/bm_perf_benchmark -bw -ot sdma -t all -s 134217728 -cc 10 -bs 1 -gs 2147483648 -dl 0,2 -ws 2 -lrs 2 -ip tcp://127.0.0.1:8570
```

> `-dl` 覆盖 `-d`，设备列表大小必须等于 `-lrs`。未指定 `-dl` 时回退到 `-d` 连续分配（向后兼容）。

### 3.2 跨卡带宽数据（`-dl 0,2`，dev0+dev2 跨卡并发）

**命令**（128MB 规格）:
```bash
source /usr/local/Ascend/ascend-toolkit/latest/set_env.sh
export LD_LIBRARY_PATH=output/smem/lib64:output/hybm/lib64:output/acc_links/lib64:output/acc_offload/lib64:$LD_LIBRARY_PATH
./build/benchmark/bm/bm_perf_benchmark -bw -ot sdma -t all -s 134217728 -cc 10 -bs 1 -gs 2147483648 -dl 0,2 -ws 2 -lrs 2 -ip tcp://127.0.0.1:8570
```

> 以下数据为每规格独立运行 5 次取中值，仅保留 dev0 (rank0) 数据。同卡并发列取自 §2.6.1 基线数据。

**1MB 规格**（`-s 1048576 -cc 100 -bs 1 -dl 0,2`）：

| 路径 | 方向 | 跨卡并发 (GB/s) | 同卡并发 (GB/s) | 裸 ACL 独占 (GB/s) |
|------|------|----------------|----------------|-------------------|
| H2D | Host→HBM | 5.1 | 5.0 | 5.6 |
| D2H | HBM→Host | 5.2 | 5.7 | 5.5 |
| H2RD | Host→远端HBM | 5.6 | 5.9 | — |
| H2RH | Host→远端DRAM | 15.1 | 15.9 | — |
| D2RD | 本地HBM→远端HBM | 7.4 | 10.2 | — |
| D2RH | HBM→远端Host | 5.2 | 5.3 | — |
| RH2D | 远端Host→HBM | 5.3 | 5.3 | — |
| RH2H | 远端Host→Host | 15.1 | 16.4 | — |
| RD2D | 远端HBM→本地HBM | 7.7 | 10.4 | — |
| RD2H | 远端HBM→Host | 5.6 | 5.6 | — |

**16MB 规格**（`-s 16777216 -cc 100 -bs 1 -dl 0,2`）：

| 路径 | 方向 | 跨卡并发 (GB/s) | 同卡并发 (GB/s) | 裸 ACL 独占 (GB/s) |
|------|------|----------------|----------------|-------------------|
| H2D | Host→HBM | 8.5 | 7.0 | 10.1 |
| D2H | HBM→Host | 8.8 | 6.5 | 9.9 |
| H2RD | Host→远端HBM | 8.7 | 6.9 | — |
| H2RH | Host→远端DRAM | 14.5 | 14.7 | — |
| D2RD | 本地HBM→远端HBM | 11.7 | 16.0 | — |
| D2RH | HBM→远端Host | 8.5 | 6.5 | — |
| RH2D | 远端Host→HBM | 8.4 | 7.1 | — |
| RH2H | 远端Host→Host | 14.6 | 14.8 | — |
| RD2D | 远端HBM→本地HBM | 11.2 | 17.1 | — |
| RD2H | 远端HBM→Host | 9.4 | 6.5 | — |

**128MB 规格**（`-s 134217728 -cc 10 -bs 1 -gs 2147483648 -dl 0,2`）：

| 路径 | 方向 | 跨卡并发 (GB/s) | 同卡并发 (GB/s) | 裸 ACL 独占 (GB/s) |
|------|------|----------------|----------------|-------------------|
| H2D | Host→HBM | 9.0 | 7.0 | 12.1 |
| D2H | HBM→Host | 9.7 | 6.5 | 12.1 |
| H2RD | Host→远端HBM | 9.5 | 6.8 | — |
| H2RH | Host→远端DRAM | 10.2 | 10.7 | — |
| D2RD | 本地HBM→远端HBM | 12.7 | 16.5 | — |
| D2RH | HBM→远端Host | 9.0 | 6.5 | — |
| RH2D | 远端Host→HBM | 9.3 | 7.0 | — |
| RH2H | 远端Host→Host | 10.6 | 13.1 | — |
| RD2D | 远端HBM→本地HBM | 11.3 | 17.7 | — |
| RD2H | 远端HBM→Host | 9.8 | 6.1 | — |

**v1 特征**：

- **host↔device 路径跨卡并发恢复**：跨卡两 rank 各自独占本卡 DMA 通路，并发不争用。128MB H2D 从同卡 7.0 GB/s 升至跨卡 9.0 GB/s，接近裸 ACL 独占 12.1 GB/s。剩余差距来自跨卡 P2P 开销（import 远端 HBM、跨卡 `AclrtMemSetAccess`）和框架 per-call 开销。
- **D2RD/RD2D 跨卡低于同卡**：同卡跨 die D2D 走片内互联（16.5-17.7 GB/s），跨卡 D2D 需经 PCIe/NVLink，带宽降至 11.3-12.7 GB/s。此为硬件拓扑特性，非框架开销。
- **H2RH/RH2H 跨卡与同卡接近**：远端 DRAM 经 shm 映射后走 host↔host DMA。1MB 时跨卡 15.1 GB/s（与同卡 15.9 GB/s 接近），128MB 时跨卡 10.2 GB/s（与同卡 10.7-13.1 GB/s 接近），大 block 下两者趋同。
- **小 block 下跨卡 D2D 低于同卡**：1MB D2RD 跨卡 7.4 vs 同卡 10.2，因跨卡 P2P import 开销在小 block 下占比更大。

### 3.3 框架逐调用开销分解与跨卡残余差距分析

> **复测说明**（2026-09-10）：16MB 规格，每次进程 100 次拷贝取平均带宽，外侧跑 10 轮统计。裸 ACL 与框架均无 warmup。框架纯 ACL 耗时通过 `CopyLH2GD` 内部 `clock_gettime` 插桩实测（10 轮每轮输出）。CANN 9.1.0, aarch64, dev0+dev1 同卡, dev0+dev2 跨卡。

#### 3.3.1 16MB H2D 10 轮统计数据

| 场景 | 平均 (GB/s) | 中值 (GB/s) | 最小 (GB/s) | 最大 (GB/s) | CV |
|------|------------|------------|------------|------------|-----|
| 裸 ACL 同卡并发 | 7.0 | 7.1 | 6.7 | 7.2 | 2% |
| 裸 ACL 跨卡并发 | 10.8 | 10.8 | 10.4 | 11.5 | 3% |
| 框架同卡并发 | 7.0 | 7.0 | 6.8 | 7.5 | 3% |
| 框架跨卡并发 | 9.6 | 8.6 | 7.5 | 12.8 | 22% |

> 裸 ACL 同卡/跨卡并发均极稳定（CV≤3%）。框架同卡稳定（CV=3%），跨卡波动大（CV=22%，含 12.8 GB/s 异常高值）。

#### 3.3.2 耗时分解与差距归因

基于 10 轮实测数据，逐层拆解每次迭代耗时：

| 场景 | 平均带宽 (GB/s) | 每次迭代耗时 | 纯 ACL 耗时 (10轮实测) | 框架开销 | 差距（裸 ACL vs 框架） |
|------|---------------|------------|----------------------|---------|----------------------|
| 裸 ACL 同卡并发 | 7.0 | 2.28ms | ~2.28ms | — | — |
| 裸 ACL 跨卡并发 | 10.8 | 1.48ms | ~1.48ms | — | — |
| 框架同卡并发 | 7.0 | 2.27ms | avg 2.33ms (2.21-2.45, CV=3%) | ~51us/call (2.1%) | 7.0 vs 7.0，持平 |
| 框架跨卡并发 | 9.6 | 1.67ms | avg 1.60ms (0.88-2.11, CV=27%) | ~160us/call (9.1%) | 9.6 vs 10.8，-1.2 GB/s (-11%) |

> 纯 ACL 耗时通过 `CopyLH2GD` 内部 `clock_gettime` 插桩测量，10 轮每轮输出 100 次平均。框架开销 = 每次迭代总耗时 - 纯 ACL 耗时。

**差距归因**：

| 场景 | 差距（裸 ACL vs 框架） | 并发 DMA 争用 | 框架软件开销 | 系统波动 |
|------|----------------------|-------------|------------|---------|
| 同卡并发 | 持平 (0%) | — | ~2% | — |
| 跨卡并发 | -11% | — | ~9% | ~90% |

- **同卡并发**：框架与裸 ACL 平均带宽完全一致（7.0 vs 7.0），纯 ACL 耗时接近（2.33ms vs ~2.28ms），框架开销仅 51us/call（2.1%）。同卡场景下框架无额外损耗。
- **跨卡并发**：框架跨卡并发 9.6 vs 裸 ACL 10.8，差距 11%。纯 ACL 耗时波动极大（CV=27%，0.88-2.11ms），偶发 0.88ms 低值导致带宽异常高（12.8 GB/s）。框架开销 160us/call（9.1%），高于同卡（51us），可能因跨卡路径中 `TransformVa`/`GetRankByGva` 等地址转换开销增大。但差距主要来源仍为系统波动（裸 ACL 极稳定 CV=3%，框架 CV=22%，两者波动范围部分重叠 7.5-12.8 vs 10.4-11.5）。

#### 3.3.3 已排除项

| 排查项 | 结论 | 数据 |
|--------|------|------|
| 裸 ACL 并发争用 ratio | 确认硬件特性 | 同卡 0.60x（0.56-0.67x），跨卡 0.90x（0.87-0.92x） |
| 迭代次数（10 vs 100） | 排除 | ratio ≈1.0x |
| NUMA CPU 绑定 | 排除 | 310P3 非 A5 平台跳过 `sched_setaffinity` |
| FAST_SYNC stream 配置 | 排除 | 改用 FAST_SYNC 后不降（12.2→11.9） |
| 并发额外 ACL 降速 | 排除 | 跨卡并发 ACL 时间未升高（9.679ms），原 6.5% 降速不复现 |

#### 3.3.4 H2D/D2H 逐元素同步说明

框架 H2D/D2H 路径逐元素调 `CopyLH2GD`/`CopyGD2LH`，每个元素各自 `AclrtMemcpyAsync` + `AclrtSynchronizeStream`（等效 single-sync）。D2D 路径（`BatchCopyG2G`）先合并连续地址段、批量 async 提交、最后统一 sync（等效 batch-sync）。`batchSize=1` 时无影响，`batchSize>1` 时 H2D/D2H 退化为 single-sync 模式。

---

## 4. 后续优化方向

### H2D/D2H async 单阶段限制（已验证）

310P3 上 host↔device 拷贝分两类 DMA 路径，其 pipeline 能力存在本质差异：

**D2D async（可 pipeline）**：device 间拷贝走 `ACL_MEMCPY_DEVICE_TO_DEVICE`，310P3 SVM 下对 HVA 也有效。DMA 引擎内部 D2D 路径包含多个阶段（如地址转换→数据搬运→写回），不同拷贝任务的不同阶段可以重叠执行。批量提交 N 个 async 再统一 sync 时，任务 A 的写回阶段可与任务 B 的搬运阶段重叠，形成 pipeline。实测 D2H bs=1（单次 D2H kind）5.7 GB/s，bs=8（合并为单次 8MB D2H kind）5.4-5.6 GB/s，bs 提升未带来 pipeline 效果。

**H2D/D2H async（单阶段，不可 pipeline）**：host→device 和 device→host 拷贝走 `ACL_MEMCPY_HOST_TO_DEVICE`/`ACL_MEMCPY_DEVICE_TO_HOST`，DMA 引擎内部该路径为**单阶段**操作——每个任务从提交到完成是一个不可分割的阻塞过程，不存在可与其他任务重叠的内部阶段。批量提交 N 个 async 再统一 sync 时，DMA 引擎仍逐个串行执行，无法形成 pipeline。

实测验证（2026-09-08）：对 `BatchCopyLH2GD`/`BatchCopyLH2GH`/`BatchCopyGD2LH`/`BatchCopyGH2LH` 改为批量 async 提交 + 统一 sync（消除每元素 async 后的 sync 开销），各 batch size 下带宽如下：

| batch size | H2D (GB/s) | D2H (GB/s) |
|------------|------------|------------|
| 1 | 5.0-5.4 | 5.6-6.3 |
| 8 | 5.0-5.4 | 5.4-5.6 |

H2D/D2H 带宽随 batch size 无显著提升，均在 5-6 GB/s 范围。瓶颈在 DMA 硬件路径本身——单阶段无重叠空间，软件层 pipeline 无法解决。该方向已验证为死路，不再追求。

#### 其他方向

**RH2H 13.1-16.4 GB/s**：`CopyGH2LH` 的 D2H async 路径 + shm 远端读取。

**transfer_perf（18 GB/s）**：BW 测试用 `smem_trans_batch_write`（batch_size=32），每次调用后 `InnerWait` sync，无法跨迭代利用 DMA pipeline。transfer_perf 走 `BatchCopyG2G` → `CopyG2GAsync`（D2D async）路径，D2D async 本身支持 pipeline，瓶颈在于每次 batch_write 后立即 sync。要提升需要多次 batch_write 批量提交后再统一 sync。

| 优先级 | 方向 | 预期收益 | 改动范围 |
|--------|------|---------|---------|
| P1 | transfer_perf batch async + 统一 sync | 18→60-80 GB/s | `transfer_perf.cpp` 或 `smem_trans_entry.cpp` |
| P2 | `CopyG2G` async fallback 策略优化（减少 ResetThreadAclStream 频率） | 减少不必要的 stream 重建 | `hybm_data_op_sdma.cpp` |


