# 06_host_pool — host DRAM 跨进程共享池(POSIX shm)

Atlas 300I Duo(310P3,4 卡 × 2 die = 8 dev)/ CANN 9.1.0 / aarch64,MPI 双进程:
rank0=host-owner(`A_DEV` 默认 0),rank1=consumer(`B_DEV` 默认 0)—— 默认同设备跨进程。
(host DRAM 池焦点:设备仅作 H2D/D2H 目标,无跨卡 P2P,无 D 态风险)

## 验证能力(矩阵单元格 06)
- host pinned 内存(`aclrtMallocHost`)✓ —— H2D/D2H 往返逐字校验 + 带宽(H2D/D2H 各 3 预热 + 30×16MB sync 调用计时,host pinned 同步可用)
- POSIX shm 跨进程 host DRAM 共享 ✓ —— `shm_open` + `ftruncate` + `mmap(MAP_SHARED)`,两进程映射同物理页,host 侧直接可见(无需 ACL)
- 共享 host 内存 + H2D 链路 ✓ —— rank1 从共享池 `aclrtMemcpy(H2D)` 写到本 rank dev + D2H 读回校验;rank0 改共享区后 rank1 再 H2D 读新数据,验证跨进程更新可见 + H2D 链路
- host register 系列边界探测:
  - `aclrtHostMemMapCapabilities` 查询所有 10 种 HAC 类型 **均返回 NOT_SUPPORTED**
  - `aclrtHostRegister` V1/V2 MAPPED → 均 207000(ACL_ERROR_RT_FEATURE_NOT_SUPPORT)
  - `aclrtHostRegisterV2` PINNED → ret=0 但**实际什么都没做**(仅 RT 层记录区间,无驱动调用)
  - `aclrtHostGetDevicePointer` → ret=107000(PINNED 无映射,查不到设备指针)
  - `halHostRegister` HAL 层 flag=0 → ret=8(DRV_ERROR_PARA_ERROR)
  - `halHostRegister` HAL 层 flag=3 → ret=65534(DRV_ERROR_NOT_SUPPORT)
  - **结论**: 310P3 上所有 host register 路径均不可用,无法实现设备直接访问 host 内存

## API 链(按执行流程)

```
[初始化]
  aclInit(NULL) → aclrtSetDevice(dev)

[rank0 分配]
  aclrtMallocHost(&hp, 32MB)          ← pinned host 基线块
  aclrtMalloc(&da0, 32MB, HUGE_FIRST) ← 设备内存,H2D 目标
  malloc(32MB)                        ← 非锁页 host 内存,用于 V2 PINNED 测试

[rank1 分配]
  aclrtMalloc(&da1, 32MB, HUGE_FIRST) ← 设备内存,H2D 目标

[step1: pinned host H2D/D2H 基线]
  fill_pat(hp) → aclrtMemcpy(da0←hp, H2D) → aclrtMemcpy(hp←da0, D2H) → check_pat(hp)
  带宽: 3 warmup + 30×16MB sync aclrtMemcpy 计时(H2D / D2H 各一轮)

[step2: POSIX shm 跨进程共享]
  rank0: shm_open(CREAT|RDWR) → ftruncate(32MB) → mmap(MAP_SHARED) → fill_pat(shm)
  ── MPI_Barrier ──
  rank1: shm_open(RDWR) → mmap(MAP_SHARED) → check_pat(shm)    ← host 侧直接读,无需 ACL

[step3: 共享 host 内存 + H2D 链路]
  rank1: aclrtMemcpy(da1←shm, H2D) → aclrtMemcpy(vbuf←da1, D2H) → check_pat(vbuf, TAG_SHM)
  ── MPI_Barrier ──
  rank0: fill_pat(shm, TAG_SHM2)    ← 改共享区
  ── MPI_Barrier ──
  rank1: aclrtMemcpy(da1←shm, H2D) → aclrtMemcpy(vbuf←da1, D2H) → check_pat(vbuf, TAG_SHM2)

[step4: host register 系列边界探测]
  rank0:
    4a: aclrtHostMemMapCapabilities 查询全部 10 种 HAC 类型 ← 均 NOT_SUPPORTED
    4b: aclrtHostRegister(shm, 32MB, MAPPED, &devPtr)   ← 207000
    4c: aclrtHostRegisterV2(shm, 32MB, MAPPED)          ← 207000
    4d: aclrtHostRegisterV2(shm, 32MB, PINNED)          ← ret=0(仅 RT 层记录,无驱动调用)
    4e: aclrtHostRegisterV2(shm, 32MB, MAPPED|PINNED)   ← 207000
    4f: aclrtHostRegister(hp, 32MB, MAPPED, &devPtr)    ← 207000 (aclrtMallocHost pinned)
    4g: aclrtHostRegisterV2(malloc, 32MB, PINNED)       ← ret=0(仅 RT 层记录,无驱动调用)
    4h: aclrtHostRegisterV2(shm, 32MB, MAPPED)          ← 207000, skip GetDevicePointer
    4i: aclrtHostRegisterV2(shm, 32MB, PINNED)          ← ret=0
        aclrtHostGetDevicePointer(shm, &devPtr, 0)      ← 107000(无映射)
    4j: aclrtHostRegister(shm, 32MB, MAPPED, &devPtr)   ← 207000, skip GetDevicePointer
    4k: halHostRegister(aclrtMallocHost, 32MB, flag=0)  ← ret=8 (DRV_ERROR_PARA_ERROR)
    4l: halHostRegister(aclrtMallocHost, 32MB, flag=3)  ← ret=65534 (DRV_ERROR_NOT_SUPPORT)
    若 4d/4g 意外成功 → aclrtHostUnregister(ptr)

[cleanup]
  双方: munmap → close(fd)
  ── MPI_Barrier ──
  rank0: shm_unlink(摘名)
  rank0: aclrtFreeHost(hp) + aclrtFree(da0) + free(malloc_ptr)
  rank1: aclrtFree(da1)
  双方: aclrtResetDevice → aclFinalize
```

校验 pattern:`value = (rank<<28)|(tag<<24)|(idx&0xFFFFFF)`,每步不同 tag(0x1/0x2/0x3)防残留假阳性;step3 校验的是 rank0 写入共享区的 tag 数据。

带宽口径:正确性全过后才测;host pinned 同步可用,H2D/D2H 各 3 次预热 + 30×16MB(共 480MB)sync `aclrtMemcpy` 调用计时(与 D2D async-only 口径不同,host pinned 无失败延迟伪造带宽风险)。

## 运行方法
```
./run.sh                  # 默认 A_DEV=0 B_DEV=0(同设备跨进程,host 池焦点)
A_DEV=0 B_DEV=1 ./run.sh  # die 对(rank1 dev1)
A_DEV=0 B_DEV=2 ./run.sh   # 跨卡(rank1 dev2)
```

## 预期结果
- step1(rank0):`aclrtMallocHost` 32MB + H2D/D2H 往返校验 OK;H2D/D2H 带宽参考 `01_base_memcpy` pinned 口径(H2D ~13.9 / D2H ~12.8 GB/s)
- step2:rank0 建 shm 池写 pattern OK;rank1 mmap 后 host 侧直接读校验 OK(MAP_SHARED 同物理页)
- step3:rank1 从共享池 H2D + D2H 校验 OK;rank0 改共享区后 rank1 再 H2D 读新 pattern 校验 OK(跨进程更新可见)
- step4:所有 host register 调用均失败或空操作(详见下方实测结论)
- RESULT: PASS(step1/2/3 全 OK 即 PASS;step4 为文档化边界)

## ★实测结论(四种设备配置)

### 配置 1: A_DEV=0 B_DEV=0(卡0, PCIe 4.0 x8)

| 步骤 | 结果 | 说明 |
|------|------|------|
| step1 pinned host 基线 | OK | aclrtMallocHost 32MB + H2D/D2H 往返逐字校验通过 |
| step1 H2D 带宽 | **13.93 GB/s** | 30×16MB sync aclrtMemcpy,pinned host→device |
| step1 D2H 带宽 | **12.57 GB/s** | 30×16MB sync aclrtMemcpy,device→pinned host |
| step2 rank0 建池 | OK | shm_open+ftruncate+mmap 32MB + 写 pattern(MAP_SHARED) |
| step2 rank1 读池 | OK | shm_open+mmap 后 host 侧直接读 pattern 校验通过(同物理页,无需 ACL) |
| step3a rank1 H2D/D2H | OK | 从共享池 H2D→dev0→D2H 读回校验通过(TAG_SHM) |
| step3b 跨进程更新 | OK | rank0 改共享区(TAG_SHM2)后 rank1 再 H2D 读新 pattern 校验通过 |
| step4a HostMemMapCapabilities | 全部 NOT_SUPPORTED | 10 种 HAC 类型均不支持 |
| step4b/c/e/f MAPPED 系列 | 均 207000 | ACL_ERROR_RT_FEATURE_NOT_SUPPORT |
| step4d/g PINNED | ret=0 但空操作 | 仅 RT 层 InsertPinnedMemory 记录区间,无驱动调用 |
| step4i GetDevicePointer | ret=107000 | PINNED 无映射,查不到设备指针 |
| step4k halHostRegister flag=0 | ret=8 | DRV_ERROR_PARA_ERROR |
| step4l halHostRegister flag=3 | ret=65534 | DRV_ERROR_NOT_SUPPORT |
| **RESULT** | **PASS** | step1/2/3 全 OK |

### 配置 2: A_DEV=2 B_DEV=2(卡1, PCIe 4.0 x16)

| 步骤 | 结果 | 说明 |
|------|------|------|
| step1 pinned host 基线 | OK | 同上 |
| step1 H2D 带宽 | **26.92 GB/s** | 30×16MB sync aclrtMemcpy,pinned host→device |
| step1 D2H 带宽 | **24.47 GB/s** | 30×16MB sync aclrtMemcpy,device→pinned host |
| step2-4 | OK | 同配置 1 |
| **RESULT** | **PASS** | step1/2/3 全 OK |

### 配置 3: A_DEV=4 B_DEV=4(卡2, PCIe 4.0 x8)

| 步骤 | 结果 | 说明 |
|------|------|------|
| step1 pinned host 基线 | OK | 同上 |
| step1 H2D 带宽 | **13.78 GB/s** | 30×16MB sync aclrtMemcpy,pinned host→device |
| step1 D2H 带宽 | **12.73 GB/s** | 30×16MB sync aclrtMemcpy,device→pinned host |
| step2-4 | OK | 同配置 1 |
| **RESULT** | **PASS** | step1/2/3 全 OK |

### 配置 4: A_DEV=6 B_DEV=6(卡3, PCIe 4.0 x16)

| 步骤 | 结果 | 说明 |
|------|------|------|
| step1 pinned host 基线 | OK | 同上 |
| step1 H2D 带宽 | **24.45 GB/s** | 30×16MB sync aclrtMemcpy,pinned host→device |
| step1 D2H 带宽 | **23.80 GB/s** | 30×16MB sync aclrtMemcpy,device→pinned host |
| step2-4 | OK | 同配置 1 |
| **RESULT** | **PASS** | step1/2/3 全 OK |

### 带宽对比总结

| 卡 | NPU ID | PCIe 链路 | H2D 带宽 | D2H 带宽 | 效率 |
|----|--------|-----------|----------|----------|------|
| 卡0 | 0,1 | PCIe 4.0 x8 | 13.81 GB/s | 12.61 GB/s | ~88% |
| 卡1 | 2,3 | PCIe 4.0 x16 | **26.92 GB/s** | **24.47 GB/s** | ~85% |
| 卡2 | 4,5 | PCIe 4.0 x8 | 13.78 GB/s | 12.73 GB/s | ~88% |
| 卡3 | 6,7 | PCIe 4.0 x16 | **24.45 GB/s** | **23.80 GB/s** | ~77% |

**理论带宽对比:**
- PCIe 4.0 x8: 16 GT/s × 8 lanes × 128/130 编码 ≈ **15.75 GB/s**
- PCIe 4.0 x16: 16 GT/s × 16 lanes × 128/130 编码 ≈ **31.51 GB/s**

## 核心结论

1. **带宽完全由 PCIe lane 数量决定**,x16 卡带宽是 x8 卡的 ~1.9 倍
2. host pinned 内存(aclrtMallocHost)同步可用,所有卡 H2D/D2H 往返校验通过
3. POSIX shm(MAP_SHARED)可实现跨进程 host DRAM 共享,两进程映射同物理页,host 侧直接可见
4. 共享 host 内存可直接作为 aclrtMemcpy(H2D) 源,**无论使用哪张卡,H2D 链路均正常工作**
5. 跨进程写入共享区后,对端 H2D 读立即生效(同物理页,无需显式同步)
6. **host DRAM 池焦点验证完成:设备仅作 H2D/D2H 目标,无跨卡 P2P,无 D 态风险**

---

## host register 系列接口分析

### aclrtHostMemMapCapabilities 全类型查询结果

| HAC Type | 名称 | 结果 |
|----------|------|------|
| 0 | STARS | NOT_SUPPORTED |
| 1 | AICPU | NOT_SUPPORTED |
| 2 | AIC | NOT_SUPPORTED |
| 3 | AIV | NOT_SUPPORTED |
| 4 | PCIEDMA | NOT_SUPPORTED |
| 5 | RDMA | NOT_SUPPORTED |
| 6 | SDMA | NOT_SUPPORTED |
| 7 | DVPP | NOT_SUPPORTED |
| 8 | UDMA | NOT_SUPPORTED |
| 9 | CCU | NOT_SUPPORTED |

### host register 能力汇总

| 接口 | 参数/内存类型 | 310P3 结果 | 说明 |
|------|-------------|-----------|------|
| `aclrtHostRegister` V1 MAPPED | mmap shm / aclrtMallocHost | 207000 | RT 层拦截 |
| `aclrtHostRegisterV2` MAPPED | mmap shm | 207000 | RT 层拦截 |
| `aclrtHostRegisterV2` PINNED | mmap shm / malloc | ret=0 | 空操作,仅 RT 层记录 |
| `aclrtHostRegisterV2` MAPPED\|PINNED | mmap shm | 207000 | RT 层拦截 |
| `aclrtHostGetDevicePointer` | PINNED 注册后 | 107000 | 无映射,查不到 |
| `halHostRegister` (HAL) flag=0 | aclrtMallocHost | 8 | DRV_ERROR_PARA_ERROR |
| `halHostRegister` (HAL) flag=3 | aclrtMallocHost | 65534 | DRV_ERROR_NOT_SUPPORT |

### V2 PINNED 源码分析

`aclrtHostRegisterV2` PINNED 返回 0 但**实际什么都没做**(`api_impl.cc:2568-2598`):

```cpp
rtError_t ApiImpl::HostRegisterV2(void* ptr, uint64_t size, uint32_t flag) {
    // MAPPED 位: 调用 Driver->HostRegister() → 310P3 返回 NOT_SUPPORT(207000)
    if ((flag & (MAPPED | IOMEMORY | READONLY)) != 0U) {
        error = dev->Driver_()->HostRegister(ptr, size, typeMask, pDevice, deviceId);
    }
    // PINNED 位: 只调用 InsertPinnedMemory, 无驱动调用
    if ((flag & RT_MEM_HOST_REGISTER_PINNED) != 0U) {
        (void)InsertPinnedMemory(ptr, size);  // ← 仅 RT 层记录区间
    }
}
```

`InsertPinnedMemory` 不做 `mlock()`,不调用 `halHostRegister`,不 pin 物理页,不创建 IO 映射。

### Runtime 层拦截逻辑

`NpuDriver::HostRegister`(`npu_driver_mem.cc:199-237`):
```cpp
if (!IsSupportFeature(RT_FEATURE_MEM_HOST_REGISTER)) {
    return RT_ERROR_FEATURE_NOT_SUPPORT;  // 310P3 在这里被拦截(207000)
}
```

各芯片支持情况:

| 芯片 | RT_FEATURE_MEM_HOST_REGISTER | RT_FEATURE_MEM_HOST_REGISTER_PCIE_THROUGH |
|------|-----|-----|
| 910B | ✅ | ✅ |
| 350 | ✅ | ✅ |
| 950 | ✅ | ✅ |
| 310P3 | ❌ | - |

### HAL 层 flag 类型分析

memfabric_hybrid 的 `HOST_MEM_MAP_DEV=3`(`dl_hal_api_def.h:30`),对应驱动的 `HOST_MEM_MAP_DEV_PCIE_TH`:

```c
// driver/pkg_inc/ascend_hal_define.h
HOST_MEM_MAP_DEV = 0,              // 旧版
HOST_MEM_MAP_DEV_PCIE_TH = 3,      // PCIe Through(910B 实际使用)
```

v3 HAL(`svm_register_only.c`)的 switch 分支:

| map_type | 值 | v3 HAL 行为 | 310P3 实测 |
|----------|---|-------------|-----------|
| HOST_MEM_MAP_DEV | 0 | NOT_SUPPORT (todo) | ret=8 (PARA_ERROR) |
| HOST_SVM_MAP_DEV | 1 | NOT_SUPPORT (todo) | - |
| DEV_SVM_MAP_HOST | 2 | `svm_register_to_peer` | - |
| HOST_MEM_MAP_DEV_V2 | 3 | `svm_register_pcie_th` | ret=65534 (NOT_SUPPORT) |
| DEV_MEM_MAP_HOST | 4 | NOT_SUPPORT | - |
| HOST_MEM_MAP_DMA | 5 | NOT_SUPPORT | - |
| HOST_IO_MAP_DEV | 6 | `svm_register_pcie_th(IO)` | - |

---

## SDMA Operator 影响分析

**事实**: `HostDataOpSDMA::Initialize()` 调用 `HalHostRegister(flag=3)` 返回 65534,导致 `inited_=false`。

**未确定项**:
1. 310P3 有 SDMA 引擎,但 host 构造 SQE 下发(`RT_STARS_SQE_TYPE_SDMA`)是否支持未验证
2. `DataCopy`/`DataCopyAsync` 是 SHMEM 场景下 kernel 侧调用的接口,当前场景是 host 构造 SQE 下发,两者不同
3. "跳过 paramSpace 走 Stream Task" 的假设未验证——Stream Task 也是构造 SQE 下发

**已有规划**(0824-plan.md):
```
| HostDataOpSDMA | **重写** | STARS SQE → 统一 aclrtMemcpyAsync |
│  data_operation【重写 SDMA】HostDataOpMemcpy:统一 aclrtMemcpyAsync │
```

### 批量优化 (BatchCopyExtend) 详解

**设计目标**: 减少大量小拷贝任务的 SQE 构造开销。将 N 个任务的参数打包到 host 内存(paramSpace),SDMA 引擎直接读取执行。

**内存布局 (64MB paramSpace)**:
```
paramSpace_ (64MB):
┌─────────────────────────────────────────────────────────────┐
│  槽位 0 (384KB)  │  槽位 1 (384KB)  │ ... │ 槽位 169 (384KB) │
│  最多 16K 任务    │  最多 16K 任务    │     │  最多 16K 任务    │
├─────────────────────────────────────────────────────────────┤
│  META 区域 (256KB)                                          │
│  每个槽位 64B (8×uint64), 用于 CAS 同步                     │
│  mask[i*8] = 32 → 空闲, 0 → 占用, 完成后设备写回 32          │
└─────────────────────────────────────────────────────────────┘
```

**工作流程**:
```
Host 侧                                    设备侧 (SDMA 引擎)
────────                                   ──────────────────
① TryGetOneParamSpace()
   CAS(mask[slot], 32→0) 分配槽位
② 填充参数到槽位数据区
   [src0, dst0, len0, src1, dst1, len1, ...]
③ 计算 remoteAddr = HVA + paramOffset_
   (paramOffset_ = DVA - HVA)
④ HybmBatchCopyExtend(remoteAddr, count, mask_dva, stream)
                                         ⑤ 读 remoteAddr 获取任务参数
                                         ⑥ 执行 DMA 拷贝 (最多 16K 个)
                                         ⑦ 写 mask_dva = 32 (释放槽位)
⑧ 下次 TryGetOneParamSpace() 可复用该槽位
```

**核心依赖**: paramSpace 是 host 内存,需要 SDMA 引擎通过 DVA 直接访问(设备读 host 内存)。310P3 上 `halHostRegister` 全部失败,这个依赖无法满足。

**关键参数**:
| 常量 | 值 | 说明 |
|------|---|------|
| `HYBM_SINGLE_PARAM_SIZE` | 384KB | 每个槽位大小 |
| `HYBM_SINGLE_PARAM_NUM` | 16K | 每槽位最多任务数 (每任务 3×8B=24B) |
| `HYBM_PARAM_SPACE_CAP` | 170 | 槽位数量 |
| `HYBM_PARAM_SPACE_SIZE` | 64MB | 总大小 |
| `HYBM_EXTEND_CONCURRENT` | 32 | 并发度/空闲标记 |
| `paramOffset_` | DVA-HVA | 地址转换偏移 |

### 未验证假设

1. 310P3 TS 是否支持 `RT_STARS_SQE_TYPE_SDMA` 类型 SQE
2. 上层调用方在 310P3 上是否默认设置 `COPY_EXTEND_FLAG`
