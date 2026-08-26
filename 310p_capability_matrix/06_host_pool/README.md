# 06_host_pool — host DRAM 跨进程共享池(POSIX shm)

Atlas 300I Duo(310P3,4 卡 × 2 die = 8 dev)/ CANN 9.1.0 / aarch64,MPI 双进程:
rank0=host-owner(`A_DEV` 默认 0),rank1=consumer(`B_DEV` 默认 0)—— 默认同设备跨进程。
(host DRAM 池焦点:设备仅作 H2D/D2H 目标,无跨卡 P2P,无 D 态风险)

## 验证能力(矩阵单元格 06)
- host pinned 内存(`aclrtMallocHost`)✓ —— H2D/D2H 往返逐字校验 + 带宽(H2D/D2H 各 3 预热 + 30×16MB sync 调用计时,host pinned 同步可用)
- POSIX shm 跨进程 host DRAM 共享 ✓ —— `shm_open` + `ftruncate` + `mmap(MAP_SHARED)`,两进程映射同物理页,host 侧直接可见(无需 ACL)
- 共享 host 内存 + H2D 链路 ✓ —— rank1 从共享池 `aclrtMemcpy(H2D)` 写到本 rank dev + D2H 读回校验;rank0 改共享区后 rank1 再 H2D 读新数据,验证跨进程更新可见 + H2D 链路
- `aclrtHostRegister` 边界(预期 207000)—— 310P3 不支持对任意 mmap 内存做 host register(pinned host 必须来自 `aclrtMallocHost`),文档化此边界

## API 链(按执行流程)

```
[初始化]
  aclInit(NULL) → aclrtSetDevice(dev)

[rank0 分配]
  aclrtMallocHost(&hp, 32MB)          ← pinned host 基线块
  aclrtMalloc(&da0, 32MB, HUGE_FIRST) ← 设备内存,H2D 目标

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

[step4: aclrtHostRegister 边界探测]
  rank0: aclrtHostRegister(shm, 32MB, MAPPED, &devPtr)   ← 预期 207000
         若意外成功 → aclrtHostUnregister(shm)

[cleanup]
  双方: munmap → close(fd)
  ── MPI_Barrier ──
  rank0: shm_unlink(摘名)
  rank0: aclrtFreeHost(hp) + aclrtFree(da0)
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
- step4:`aclrtHostRegister` 预期 207000(310P3 不支持);若意外成功如实记录(不影响判定)
- RESULT: PASS(step1/2/3 全 OK 即 PASS;step4 为文档化边界)

## ★实测结论(四种设备配置)

### 配置 1: A_DEV=0 B_DEV=0(卡0, PCIe 4.0 x8)

| 步骤 | 结果 | 说明 |
|------|------|------|
| step1 pinned host 基线 | OK | aclrtMallocHost 32MB + H2D/D2H 往返逐字校验通过 |
| step1 H2D 带宽 | **13.81 GB/s** | 30×16MB sync aclrtMemcpy,pinned host→device |
| step1 D2H 带宽 | **12.61 GB/s** | 30×16MB sync aclrtMemcpy,device→pinned host |
| step2 rank0 建池 | OK | shm_open+ftruncate+mmap 32MB + 写 pattern(MAP_SHARED) |
| step2 rank1 读池 | OK | shm_open+mmap 后 host 侧直接读 pattern 校验通过(同物理页,无需 ACL) |
| step3a rank1 H2D/D2H | OK | 从共享池 H2D→dev0→D2H 读回校验通过(TAG_SHM) |
| step3b 跨进程更新 | OK | rank0 改共享区(TAG_SHM2)后 rank1 再 H2D 读新 pattern 校验通过 |
| step4 HostRegister | ret=207000 | 预期失败:ACL_ERROR_RT_FEATURE_NOT_SUPPORT |
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

**核心结论:**
1. **带宽完全由 PCIe lane 数量决定**,x16 卡带宽是 x8 卡的 ~1.9 倍
2. host pinned 内存(aclrtMallocHost)同步可用,所有卡 H2D/D2H 往返校验通过
3. POSIX shm(MAP_SHARED)可实现跨进程 host DRAM 共享,两进程映射同物理页,host 侧直接可见
4. 共享 host 内存可直接作为 aclrtMemcpy(H2D) 源,**无论使用哪张卡,H2D 链路均正常工作**
5. 跨进程写入共享区后,对端 H2D 读立即生效(同物理页,无需显式同步)
6. 310P3 不支持 aclrtHostRegister 对任意 mmap 内存做 page-lock,pinned host 必须来自 aclrtMallocHost
7. **host DRAM 池焦点验证完成:设备仅作 H2D/D2H 目标,无跨卡 P2P,无 D 态风险**
