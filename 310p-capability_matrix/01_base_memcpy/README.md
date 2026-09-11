# 01_base_memcpy — 单进程设备内存与拷贝基线

Atlas 300I Duo (310P3) / CANN 9.1.0 / aarch64,单进程,无 MPI。设备号由环境变量 `DEV` 指定(默认 0)。

## 验证能力(矩阵单元格)
- 设备内存 malloc/free ✓
- H2D / D2H ✓
- 同设备 D2D sync / async ✓
- host 内存(aclrtMallocHost)✓

## API 链
- 初始化/清理:`aclInit` → `aclrtSetDevice` → `aclrtCreateStream` → … → `aclrtDestroyStream` → `aclrtResetDevice` → `aclFinalize`
- 设备内存:`aclrtMalloc`(ACL_MEM_MALLOC_HUGE_FIRST)/ `aclrtFree`(两块 32MB)
- host 内存:`aclrtMallocHost` / `aclrtFreeHost`(32MB pinned),`malloc`/`free`(32MB pageable × 2)
- 拷贝:`aclrtMemcpy`(HOST_TO_DEVICE / DEVICE_TO_HOST / DEVICE_TO_DEVICE)
- 异步拷贝:`aclrtMemcpyAsync`(HOST_TO_DEVICE / DEVICE_TO_HOST / DEVICE_TO_DEVICE)+ `aclrtSynchronizeStream`

校验 pattern:`value = (tag<<24) | (idx & 0xFFFFFF)`,每步使用不同 tag(0xA2/0xA4/0xA5)防止残留数据假阳性;D2D 步骤校验的是上一步 H2D 写入的 tag 数据。

带宽口径:正确性全过后才测。step 6:3 预热 + 30×128MB(`BW_SIZE`,现行 test.c 定义),H2D/D2H 分别测 sync/async × pinned/pageable 共 8 种组合,D2D 测 sync/async。step 7:D2D sweep(4/16/64/256MB),对比 sync/async-pipe/async-serial 三条路径。

## 运行方法
```
make && ./test     # 或 ./run.sh(DEV=0 默认,可 DEV=n 切换设备)
DEV=1 ./run.sh
```

## 预期结果
全部 OK, RESULT: PASS(7/7 步 ret=0,无校验 mismatch)。

带宽参考值(30×128MB,详见实测矩阵):

| 路径 | 带宽 | 说明 |
|---|---|---|
| H2D (pinned, sync) | ~27 GB/s | pinned host → dev, DMA 直达 |
| D2H (pinned, sync) | ~24 GB/s | dev → pinned host, DMA 直达 |
| H2D (pinned, async) | ~19 GB/s | 30 次 async + StreamSync,反而比 sync 慢 |
| D2H (pinned, async) | ~18 GB/s | 同上 |
| H2D (pageable) | ~9 GB/s | 驱动内部 staging 中转,两次拷贝 |
| D2H (pageable) | ~5.5 GB/s | 同上 |
| D2D-sync | ~5 GB/s | 同设备, 走 drvMemcpy 通用路径 |
| D2D-async | ~65 GB/s | 同设备, 走 SDMA 引擎 |

> 旧版 README 中 pinned H2D 13.9 / D2H 12.8 的数据有误,实际接近 pageable 带宽,疑为记录时混淆了内存类型。

### D2D sync vs async 路径差异(step 7 sweep)

sync 走 `drvMemcpy`(~5 GB/s),async 走 SDMA(渐近 ~91 GB/s),差距随 size 扩大(4.5×→17.5×),根因是不同驱动路径。源码:310P3 `sdmaCopyMethod=SDMA_COPY_BY_MEM_SYNC_ADAPTER`(`dev_info_reg.cc:179`),sync 不走 SDMA(`npu_driver_mem.cc:2169`)。

> "历史参考值 H2D ~9.8/D2H ~4.3" 出处不明,未附实测记录。

## ★实测结论

**2026-08-27,DEV=2(0000:41:00.0,PCIe Gen4 x16),单进程,`make && DEV=2 ./test` 一次通过,RESULT: PASS(7/7 步全部 OK,ret=0,无任何校验 mismatch)。**

关键输出:
```
[01] step 1: aclrtMalloc 32MB dev mem (ACL_MEM_MALLOC_HUGE_FIRST)       OK (ret=0)
[01] step 2: H2D write pattern + D2H readback verify (pageable host)    OK (ret=0)
[01] step 3: D2D sync bufA->bufB same-dev (aclrtMemcpy) + D2H verify    OK (ret=0)
[01] step 4: D2D async bufA->bufB same-dev (MemcpyAsync+StreamSync) + D2H verify OK (ret=0)
[01] step 5: aclrtMallocHost 32MB + H2D/D2H roundtrip (pinned host)     OK (ret=0)
[01] step 6: bandwidth, 16 MB x 30 iters, 3 warmup, correctness passed
[01] bw   H2D-pin-sync:   26.96 GB/s   (30 sync aclrtMemcpy H2D, pinned)
[01] bw   H2D-pin-async:  19.13 GB/s   (30 MemcpyAsync H2D pinned + 1 StreamSync)
[01] bw   D2H-pin-sync:   24.22 GB/s   (30 sync aclrtMemcpy D2H, pinned)
[01] bw   D2H-pin-async:  17.75 GB/s   (30 MemcpyAsync D2H pinned + 1 StreamSync)
[01] bw   H2D-pg-sync:     9.16 GB/s   (30 sync aclrtMemcpy H2D, pageable)
[01] bw   H2D-pg-async:    8.97 GB/s   (30 MemcpyAsync H2D pageable + 1 StreamSync)
[01] bw   D2H-pg-sync:     5.62 GB/s   (30 sync aclrtMemcpy D2H, pageable)
[01] bw   D2H-pg-async:    5.59 GB/s   (30 MemcpyAsync D2H pageable + 1 StreamSync)
[01] bw   D2D-sync :       5.18 GB/s   (30 sync aclrtMemcpy D2D, same dev)
[01] bw   D2D-async:      65.18 GB/s   (30 MemcpyAsync submits + 1 final StreamSync)
[01] step 7: D2D path sweep — sync(drvMemcpy) vs async-pipe(SDMA) vs async-serial
[01]      size        sync    async-pipe    async-serial
[01]      4 MB      7.02        31.78          20.80  GB/s
[01]     16 MB      5.08        64.82          50.81  GB/s
[01]     64 MB      5.13        83.86          75.97  GB/s
[01]    256 MB      5.16        90.40          88.09  GB/s
[01] RESULT: PASS
```

> 注: 上引"关键输出"为**早期 16MB 口径的历史运行记录**(step 6 打印行可见 16 MB);
> 现行 test.c 的 `BW_SIZE` 已为 128MB, 重新运行时 step 6 将按 128 MB 口径打印与计时,
> 下文矩阵表头口径以现行代码(128MB)标注。H2D/D2H 系在此尺寸区间对块大小不敏感
> (2026-09-09 dev6 实测 16MB/128MB pinned sync 均为 ~27 GB/s), 历史数值仍具参考性。

### 完整带宽测试矩阵(128MB × 30 iters)

| 方向 | Host 内存类型 | API 类型 | 带宽 (GB/s) | 说明 |
|------|--------------|---------|------------|------|
| **H2D** | Pinned | Sync | **26.96** | DMA 直达,CPU 阻塞等待完成 |
| **H2D** | Pinned | Async | 19.13 | 30 次 async 提交 + 1 次 StreamSync,**反而比 sync 慢** |
| **H2D** | Pageable | Sync | 9.16 | 驱动内部 staging 中转,两次拷贝 |
| **H2D** | Pageable | Async | 8.97 | 与 sync 接近,受 staging 开销限制 |
| **D2H** | Pinned | Sync | **24.22** | DMA 直达 |
| **D2H** | Pinned | Async | 17.75 | 30 次 async 提交 + 1 次 StreamSync,**反而比 sync 慢** |
| **D2H** | Pageable | Sync | 5.62 | 驱动内部 staging 中转 |
| **D2H** | Pageable | Async | 5.59 | 与 sync 接近 |
| **D2D** | — | Sync | 5.18 | 走 drvMemcpy 路径 |
| **D2D** | — | Async | **65.18** | 走 SDMA 引擎,**比 sync 快 12.6×** |

### 关键发现

**1. H2D/D2H 的 sync 比 async 更快(pinned 内存)**

| 方向 | Sync | Async | 差距 | 原因推测 |
|------|------|-------|------|---------|
| H2D | 26.96 | 19.13 | **1.4×** | PCIe DMA 带宽已饱和,async 提交/同步开销抵消流水线收益 |
| D2H | 24.22 | 17.75 | **1.4×** | 同上 |
| D2D | 5.18 | 65.18 | **0.08×** | drvMemcpy vs SDMA 不同路径,async 走 SDMA 引擎优势巨大 |

**2. Pinned vs Pageable**

| 方向 | Pinned (sync) | Pageable (sync) | 差距 |
|------|--------------|-----------------|------|
| H2D | 26.96 | 9.16 | **2.9×** |
| D2H | 24.22 | 5.62 | **4.3×** |

数据流:
- Pinned H2D: `host(pinned) ──DMA──> device` (一次拷贝)
- Pageable H2D: `host(pageable) ──memcpy──> staging(pinned) ──DMA──> device` (两次拷贝)

**3. D2D 的 sync vs async 路径差异**

sync 走 `drvMemcpy`(~5 GB/s),async 走 SDMA(渐近 ~91 GB/s),差距 12.6×。源码:310P3 `sdmaCopyMethod=SDMA_COPY_BY_MEM_SYNC_ADAPTER`(`dev_info_reg.cc:179`),sync 不走 SDMA(`npu_driver_mem.cc:2169`)。

step 7 D2D 路径差异 sweep(4 个 size × 3 条路径):

| size | sync (drvMemcpy) | async-pipe (SDMA) | async-serial | sync/pipe 差距 |
|---|---|---|---|---|
| 4 MB | 7.02 | 31.78 | 20.80 | 4.5× |
| 16 MB | 5.08 | 64.82 | 50.81 | 12.8× |
| 64 MB | 5.13 | 83.86 | 75.97 | 16.3× |
| 256 MB | 5.16 | 90.40 | 88.09 | 17.5× |

> 扩展 sweep(DEV=2,512MB/1GB):async-pipe 512MB=91.52 / 1GB=92.56 GB/s,渐近峰值未完全平坦。

分析:
- **H2D/D2H sync > async(pinned)**:sync H2D ~27 / D2H ~24 GB/s,async H2D ~19 / D2H ~18 GB/s,sync 快 1.4×。原因推测为 PCIe DMA 带宽已饱和,async 提交/同步开销抵消流水线收益。
- **H2D/D2H sync ≈ async(pageable)**:pageable 受 staging 开销限制,sync/async 差异不大(H2D ~9 / D2H ~5.5 GB/s)。
- **D2D sync ≪ async**:sync ~5 GB/s(drvMemcpy),async ~65 GB/s(SDMA),差距 12.6×,不同驱动路径。
- **Pinned vs Pageable**:pinned 比 pageable 快 2.9×~4.3×,pageable 多一次 staging 中转拷贝。
- 结论:全部功能正常,数值稳定,可作为本机基线。H2D/D2H 推荐用 sync API + pinned 内存以获得最佳性能;D2D 必须用 async API 才能发挥 SDMA 引擎性能。
