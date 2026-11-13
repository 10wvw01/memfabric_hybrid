# 14：Host直接访问导入的远端设备内存

## 运行

```bash
source /usr/local/Ascend/cann/set_env.sh
export ASCEND_HOME=/usr/local/Ascend/cann
cd 310p-capability_matrix/14_transfer_remote
A_DEV=2 B_DEV=6 bash run.sh
```

## 2026-11-13测试结果

Ascend310P3，CANN 9.2.1，驱动26.1.1，aarch64。

| 检查项 | 本轮结果 |
|---|---|
| 设备与数据 | 2、6跨卡；32MiB设备块，16MiB校验/带宽块 |
| Host直接读取、写入远端映射 | 校验通过 |
| 导入映射偏移16MiB处读取 | 校验通过 |
| 总结果 | PASS，两个rank退出0 |

远端导入映射可以直接参与Host与设备之间的拷贝。分别比较锁页Host缓冲区与普通Host缓冲区的带宽。


## 本轮带宽

3次预热、30次16MiB；单位GB/s。

| 路径 | GB/s |
|---|---:|
| 远端→普通Host，异步后同步 | 5.55 |
| 远端→普通Host，同步 | 5.56 |
| 远端→锁页Host，同步 | 24.20 |
| 本地设备→锁页Host，同步 | 23.76 |

## 原有实验过程与数据

以下保留CANN 9.1.0下的原有实验条件、步骤、输出及分析。2026-11-13的执行情况见上表。

Atlas 300I Duo (310P3, 4 卡 × 2 die = 8 dev) / CANN 9.1.0 / aarch64,MPI 双进程:
rank0=exporter(dev A,`A_DEV` 默认 0),rank1=importer(dev B,`B_DEV` 默认 1 die 对;或 `A_DEV=0 B_DEV=2` 跨卡)。

## 验证能力
- imported VA 上的**直接 host↔远端 NPU 传输**(不经 host 中转,一步直达)
- 池上的传输能力:host 进程对远端 NPU 内存的直接 H2D/D2H
- 直达 D2H 读远端(sync) + 逐字 pattern 校验
- 直达 H2D 写远端(sync) + exporter 端 D2H 校验
- async D2H 读远端(async + Sync) + 校验
- sync vs async 直达 D2H 远端带宽对比
- **pinned vs pageable host 内存对带宽的影响验证** (step 7)
- **数据路径差异验证**: imported VA vs 本地 dev (step 8)

## API 链
- export:`aclrtMalloc(ACL_MEM_MALLOC_HUGE_FIRST_P2P)` 32MB → H2D 写 pattern(TAG_EXP=0x14) →
  `aclrtIpcMemGetExportKey(..., ACL_RT_IPC_MEM_EXPORT_FLAG_DISABLE_PID_VALIDATION)` → key(65B)经 MPI 发给 importer
- import:`aclrtIpcMemImportByKey(&p, key, flag)`
  - flag = `ACL_RT_IPC_MEM_IMPORT_FLAG_DEFAULT`(die 对,同卡)
  - flag = `ACL_RT_IPC_MEM_IMPORT_FLAG_ENABLE_PEER_ACCESS`(跨卡,内部双向 EnablePeerAccess)
- 直达 D2H 读远端:`aclrtMemcpy(host_buf, 16MB, imported_VA, 16MB, ACL_MEMCPY_DEVICE_TO_HOST)`(同步)
- 直达 H2D 写远端:`aclrtMemcpy(imported_VA, 16MB, host_buf, 16MB, ACL_MEMCPY_HOST_TO_DEVICE)`(同步)
- async D2H 读远端:`aclrtMemcpyAsync(host_buf, 16MB, imported_VA, 16MB, D2H, stream)` + Sync
- cleanup:双方 `aclrtIpcMemClose(key)`;物理内存只由 exporter `aclrtFree`;
  跨卡时 `aclrtDeviceDisablePeerAccess(peer)` before ResetDevice(D 态修复)

校验 pattern:`value = (rank<<28)|(tag<<24)|(idx&0xFFFFFF)`,tag 0x14/0x1E 防残留假阳性。
- step3 sync D2H 读校验第一 16MB(rank0/TAG_EXP)
- step4 H2D 写覆盖第一 16MB(rank1/TAG_REMOTE),exporter 校验
- step5 async D2H 读**第二** 16MB(未被 step4 触及,仍是 rank0/TAG_EXP 原始 pattern)

## 带宽口径
- **async 路(主口径)**:3 预热 + 30×16MB MemcpyAsync 提交 + 末尾 1 次 StreamSync
- **sync 路(对照)**:3 预热 + 30 次同步 `aclrtMemcpy` 调用计时

共享设备映射的D2D与Host端点的D2H分别判断是否支持(历史教训:失败链路同步调用 ~140µs/次会伪造 114 GB/s 假带宽)。
本测试 sync 路径仅作对照,且**仅在 step3 已逐字校验通过(host 直达 D2H 链路真实可用)后才执行** ——
每次同步调用都真实搬运 16MB 数据,非失败返回的延迟,故 sync 计时反映真实搬运带宽。若 host
直达链路本身坏(step3 FAIL 已 fatal),直接跳过,不执行该轮带宽测量。

## 运行方法
```
./run.sh                    # 默认 die 对 dev0→dev1(DEFAULT import)
A_DEV=0 B_DEV=2 ./run.sh    # 跨卡(PEER import,启用 D 态修复)
A_DEV=1 B_DEV=4 ./run.sh    # 原 ✗ 单元格(10 实测 CANN 9.1.0 下 VMM 已可达)
```

## 预期结果
- die 对:DEFAULT import OK;直达 D2H 读 / H2D 写 / async D2H 读 校验全 OK;RESULT: PASS
- 跨卡:PEER import OK;直达校验全 OK;cleanup 无 D 态残留;RESULT: PASS
- 带宽:sync D2H 远端 vs async D2H 远端对比(async 通常 ≥ sync,因 async 可重叠提交)

## 实测结论
**测试环境**: Atlas 300I Duo (310P3) / CANN 9.1.0 / 默认 die 对 (A_DEV=0, B_DEV=1)

**测试结果**: **PASS**

| Step | 测试项 | 结果 |
|------|--------|------|
| 1 | malloc P2P 32MB + H2D pattern + export key1 + MPI-send | OK |
| 2 | import DEFAULT -> imported VA | OK |
| 3 | host direct D2H read remote 16MB + verify | OK |
| 4 | host direct H2D write remote 16MB + exporter verify | OK |
| 5 | async D2H read remote 16MB #2 + verify | OK |
| 6 | 带宽对比 (pageable) | 见下 |
| 7 | pinned host 内存 D2H 带宽 | 见下 |
| 8 | 本地 dev memory D2H 带宽 | 见下 |

**带宽对比** (16MB × 30 iters + 3 warmup):
| Step | 模式 | Host 内存 | 数据源 | 带宽 | 耗时 |
|------|------|-----------|--------|------|------|
| 6 | async D2H (主口径) | pageable | imported VA (远端) | **4.46 GB/s** | 0.113 s |
| 6 | sync D2H (对照) | pageable | imported VA (远端) | **4.47 GB/s** | 0.113 s |
| **7** | sync D2H | **pinned** | imported VA (远端) | **12.78 GB/s** | 0.039 s |
| **8** | sync D2H | **pinned** | 本地 dev | **12.84 GB/s** | 0.039 s |

**与 01-06 测试 D2H 带宽横向对比**:
| 测试 | 场景 | Host 内存 | D2H 带宽 |
|------|------|-----------|----------|
| 01_base_memcpy | 本地 dev → host (基线) | pinned | **12.8 GB/s** |
| 06_host_pool (卡0, x8) | 本地 dev → host (共享池) | pinned | **12.6 GB/s** |
| 06_host_pool (卡1, x16) | 本地 dev → host (共享池) | pinned | **24.5 GB/s** |
| **14 (step 6)** | 远端 die → host (IPC imported) | **普通 malloc** | **4.47 GB/s** |
| **14 (step 7)** | 远端 die → host (IPC imported) | **pinned** | **12.78 GB/s** |
| **14 (step 8)** | 本地 dev → host | **pinned** | **12.84 GB/s** |

**结论**:
1. 远端 slab 直达 H2D/D2H 传输功能正常，pattern 校验全部通过
2. sync 与 async 带宽基本持平（~4.47 GB/s），说明该链路下 async 无明显重叠优势
3. cleanup 正常，无 D 态残留

**带宽影响因素对照** (step7/8)：

| 对比 | 带宽变化 | 结论 |
|------|----------|------|
| Step 6 → Step 7 (pageable → pinned) | 4.47 → 12.78 GB/s (**2.86x**) | 使用pinned后的带宽提高2.86倍 |
| Step 7 → Step 8 (远端 → 本地) | 12.78 → 12.84 GB/s (**相同**) | 该次两种数据源的带宽接近 |

**详细说明**:

1. **Pageable与Pinned内存**（实测2.86倍差异）:
   - **Step 6**: `malloc()` pageable → 驱动内部 bounce buffer 中转 (2 次拷贝) → **4.47 GB/s**
   - **Step 7**: `aclrtMallocHost()` pinned → DMA 直达 (1 次拷贝) → **12.78 GB/s**
   - 代码对比:
     ```c
     // Step 6 (pageable, 4.47 GB/s)
     vbuf = malloc(DEV_MEM_SIZE);  // pageable
     aclrtMemcpy(vbuf, BW_SIZE, p, BW_SIZE, D2H);  // bounce buffer 中转

     // Step 7 (pinned, 12.78 GB/s)
     aclrtMallocHost(&pinned_buf, DEV_MEM_SIZE);  // pinned
     aclrtMemcpy(pinned_buf, BW_SIZE, p, BW_SIZE, D2H);  // DMA 直达
     ```

2. 该次两种数据源的带宽接近 (实测确认):
   - **Step 7**: imported VA (远端 die0) → pinned host = **12.78 GB/s**
   - **Step 8**: 本地 dev (die1) → pinned host = **12.84 GB/s**
   - 两者测得12.78和12.84GB/s；原分析以**die0 → PCIe x8 → Host**作为路径解释
   - 硬件拓扑：die0 是 PCIe 主 die，所有 PCIe DMA 都通过 die0

3. **优化效果**:
   - 改用 pinned 内存后，带宽从 4.47 GB/s 提升至 **12.78 GB/s**
   - 与 Test 01 基线 (12.8 GB/s) 完全一致
