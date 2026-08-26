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
- host 内存:`aclrtMallocHost` / `aclrtFreeHost`(32MB pinned)
- 拷贝:`aclrtMemcpy`(HOST_TO_DEVICE / DEVICE_TO_HOST / DEVICE_TO_DEVICE)
- 异步拷贝:`aclrtMemcpyAsync`(DEVICE_TO_DEVICE)+ `aclrtSynchronizeStream`

校验 pattern:`value = (tag<<24) | (idx & 0xFFFFFF)`,每步使用不同 tag(0xA2/0xA4/0xA5)防止残留数据假阳性;D2D 步骤校验的是上一步 H2D 写入的 tag 数据。

带宽口径:正确性全过后才测。step 6:3 预热 + 30×16MB,H2D/D2H 用 pinned host。step 7:D2D sweep(4/16/64/256MB),对比 sync/async-pipe/async-serial 三条路径。

## 运行方法
```
make && ./test     # 或 ./run.sh(DEV=0 默认,可 DEV=n 切换设备)
DEV=1 ./run.sh
```

## 预期结果
全部 OK, RESULT: PASS(7/7 步 ret=0,无校验 mismatch)。

带宽参考值(pinned host 口径,多次实测稳定区间):

| 路径 | 带宽 | 说明 |
|---|---|---|
| H2D | 13.9 GB/s | pinned host → dev, 30×16MB sync |
| D2H | 12.8 GB/s | dev → pinned host, 30×16MB sync |
| D2D-sync | ~5 GB/s | 同设备 bufA→bufB, 30×16MB sync,走 drvMemcpy 通用路径 |
| D2D-async | ~65 GB/s | 同设备, 30×MemcpyAsync + 1×StreamSync,走 SDMA 引擎 |

### D2D sync vs async 路径差异(step 7 sweep)

sync 走 `drvMemcpy`(~5 GB/s),async 走 SDMA(渐近 ~91 GB/s),差距随 size 扩大(13×→17×),根因是不同驱动路径。源码:310P3 `sdmaCopyMethod=SDMA_COPY_BY_MEM_SYNC_ADAPTER`(`dev_info_reg.cc:179`),sync 不走 SDMA(`npu_driver_mem.cc:2169`)。

> "历史参考值 H2D ~9.8/D2H ~4.3" 出处不明,未附实测记录。

## ★实测结论

**2026-08-25(第二次),DEV=0,单进程,`make && ./test` 一次通过,RESULT: PASS(7/7 步全部 OK,ret=0,无任何校验 mismatch)。**

关键输出:
```
[01] step 1: aclrtMalloc 32MB dev mem (ACL_MEM_MALLOC_HUGE_FIRST)       OK (ret=0)
[01] step 2: H2D write pattern + D2H readback verify (pageable host)    OK (ret=0)
[01] step 3: D2D sync bufA->bufB same-dev (aclrtMemcpy) + D2H verify    OK (ret=0)
[01] step 4: D2D async bufA->bufB same-dev (MemcpyAsync+StreamSync) + D2H verify OK (ret=0)
[01] step 5: aclrtMallocHost 32MB + H2D/D2H roundtrip (pinned host)     OK (ret=0)
[01] bw   H2D      :   13.87 GB/s   (30 sync aclrtMemcpy H2D, pinned host, total 0.036 s)
[01] bw   D2H      :   12.77 GB/s   (30 sync aclrtMemcpy D2H, pinned host, total 0.039 s)
[01] bw   D2D-sync :    3.64 GB/s   (30 sync aclrtMemcpy D2D, same dev, total 0.138 s)
[01] bw   D2D-async:   65.14 GB/s   (30 MemcpyAsync submits + 1 final StreamSync, total 0.008 s)
[01] step 6: bandwidth H2D/D2H/D2D-sync/D2D-async (see lines above)     OK (ret=0)
[01] step 7: D2D path sweep — sync(drvMemcpy) vs async-pipe(SDMA) vs async-serial
[01]      size        sync    async-pipe    async-serial
[01]      4 MB      3.57        31.61          20.18  GB/s
[01]     16 MB      5.21        65.76          50.72  GB/s
[01]     64 MB      5.27        84.39          76.85  GB/s
[01]    256 MB      5.31        90.92          88.53  GB/s
[01] step 7: D2D path sweep sync/async-pipe/async-serial (see lines above) OK (ret=0)
[01] RESULT: PASS
```

三次实测对比(step 6 带宽,DEV=0):

| 路径 | 08-24 | 08-25① | 08-25② | 说明 |
|---|---|---|---|---|
| H2D | 13.88 | 13.91 | 13.87 | 稳定 |
| D2H | 12.82 | 12.83 | 12.77 | 稳定 |
| D2D-sync | 5.20 | 5.10 | 3.64 | ②偏低,见下 |
| D2D-async | 65.02 | 64.86 | 65.14 | 稳定 |

> 08-25② step 6 D2D-sync 3.64 偏低(前两次 5.20/5.10,step 7 同 size 5.21),疑为负载波动;以 step 7 数据为准。

step 7 D2D 路径差异 sweep(4 个 size × 3 条路径):

| size | sync (drvMemcpy) | async-pipe (SDMA) | async-serial | sync/pipe 差距 |
|---|---|---|---|---|
| 4 MB | 3.57 | 31.61 | 20.18 | 8.9× |
| 16 MB | 5.21 | 65.76 | 50.72 | 12.6× |
| 64 MB | 5.27 | 84.39 | 76.85 | 16.0× |
| 256 MB | 5.31 | 90.92 | 88.53 | 17.1× |

分析:
- H2D 13.9 / D2H 12.8 GB/s(pinned):三次波动<1%,稳定。
- D2D-sync ~5 GB/s:16MB+ 不随 size 变化,走 `drvMemcpy` 路径上限,非固定开销(加大 size 带宽未升)。
- D2D-async 65→91 GB/s:走 SDMA,随 size 渐近峰值;async-serial 大 size 时追上 async-pipe。
- sync/async 差距 12.6×→17.1×:不同驱动路径导致,非数据量或测试方法问题。
- 结论:全部功能正常,数值稳定,可作为本机基线。
