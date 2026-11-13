# 05：同卡双die的VMM共享

比较同卡两个进程导入VMM共享句柄时的设备上下文，并验证导入后的异步D2D和Host读写。

## 测试步骤与API调用顺序

导出方A和导入方B是同一卡的两个die，各运行一个MPI进程。

1. A查询分配粒度，`aclrtMallocPhysical`分配32MiB；`aclrtReserveMemAddress`预留地址，`aclrtMapMem`映射，`aclrtMemSetAccess`设置访问权限。写入数据后`aclrtMemExportToShareableHandle`，经MPI传给B。
2. B执行下述导入顺序。传给导入函数的目标设备号一直是B，改变的是调用时的当前设备上下文：

   ```text
   aclrtSetDevice(A)
   aclrtMemImportFromShareableHandle(sh, B, &imported_handle)
   aclrtSetDevice(B)
   aclrtReserveMemAddress → aclrtMapMem → aclrtMemSetAccess
   ```

3. B从映射地址异步D2D读至本地设备缓冲区，等待stream后D2H校验；B再写入新数据，A从自己的映射读回校验。
4. A另行导出16MiB对照块，B保持自身上下文直接导入，记录结果。再用另一独立块比较“先EnablePeerAccess，再在自身上下文导入”。独立句柄避免重复导入同一对象干扰对照。
5. B对主映射的前16MiB直接D2H/H2D，A复核；随后测高16MiB的读带宽和低16MiB的写带宽。结束前恢复完整主块的数据，A作最终校验。
6. B先解除映射、释放导入句柄和预留地址；MPI同步后A释放物理内存及自己的映射。最后销毁stream、重置使用过的设备并结束ACL。


### 数据关系

```text
A的物理内存 ← A本进程映射ownVA
            ← B导入句柄后的映射vmmVA
B本地缓冲区local ↔ 异步D2D ↔ vmmVA
A经ownVA读回，确认B写入的是共享物理内存
```

Host直接访问与D2D使用同一主映射，但源、目的端不同；两者分别校验。每步数据带不同tag，避免残留数据掩盖拷贝失败。

## 运行

```bash
source /usr/local/Ascend/cann/set_env.sh
export ASCEND_HOME=/usr/local/Ascend/cann
cd 310p-capability_matrix/05_vmm_die
A_DEV=2 B_DEV=3 bash run.sh
```

## 2026-11-13测试结果

Ascend310P3，CANN 9.2.1，驱动26.1.1，aarch64。

| 检查项 | 本轮结果 |
|---|---|
| 设备与数据 | 2、3；主要物理块32MiB，导入/Host对照块16MiB |
| 切到导出方设备后导入，再切回本地设备 | 导入、映射、异步D2D双向和Host读写校验均通过 |
| 保持导入方设备上下文直接导入 | 返回507899 |
| 先EnablePeerAccess再保持导入方上下文导入 | EnablePeerAccess和导入均返回507899 |
| 带宽 | 16MiB，3次预热、30次异步提交后同步：读19.04、写17.94 GB/s |
| 总结果 | PASS，两个rank退出0 |

当前同卡双die可以使用切换导出方设备上下文的导入流程。EnablePeerAccess未能使本地上下文的导入对照成功。共享物理内存由导出方释放，导入方解除映射并释放导入句柄。


## API 链（按执行顺序）

### Step 1: Exporter 分配 + 导出

```
┌─ rank0 (exporter, devA=0) ─────────────────────────────────────────────────┐
│                                                                             │
│  1.1 分配物理内存 (32MB, 七级 memAttr 回退)                                  │
│      aclrtMemGetAllocationGranularity(prop, RECOMMENDED, &gran)             │
│      → 检查粒度 2MB, 32MB = 16 × 2MB ✓                                      │
│      aclrtMallocPhysical(&h_own, 32MB, prop, 0)                             │
│      → 实测仅 ACL_MEM_P2P_HUGE 成功 (前 4 项 ret=100000)                    │
│                                                                             │
│  1.2 保留虚拟地址 + 映射                                                     │
│      aclrtReserveMemAddress(&ownVA, 32MB, 0, NULL, 0)                       │
│      aclrtMapMem(ownVA, 32MB, 0, h_own, 0)                                  │
│                                                                             │
│  1.3 设置访问权限                                                            │
│      aclrtMemSetAccess(ownVA, 32MB, {RW, devA}, 1)                          │
│                                                                             │
│  1.4 Host→Device 写入 pattern                                               │
│      aclrtMemcpy(ownVA, 32MB, hbuf, 32MB, H2D)                              │
│      → pattern: (rank0<<28)|(0xD1<<24)|(idx&0xFFFFFF)                       │
│                                                                             │
│  1.5 导出为 shareable handle                                                │
│      aclrtMemExportToShareableHandle(h_own, NONE, DISABLE_PID_VALIDATION, &sh1) │
│                                                                             │
│  1.6 MPI_Send(&sh1, 1, UINT64, rank1, tag=100)                              │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Step 2: Importer 正向 ctx 技巧 import + D2D 双向校验

```
┌─ rank1 (importer, devB=1) ─────────────────────────────────────────────────┐
│                                                                             │
│  2.1 MPI_Recv(&sh1, ...) 接收 handle                                        │
│                                                                             │
│  2.2 ★ctx 技巧★ (绕过 runtime P2P 前置 para_check)                          │
│      aclrtSetDevice(devA)           ← 临时切到导出方 die 上下文               │
│      aclrtMemImportFromShareableHandle(sh1, devB, &h_imp)  ← devId=B 本 die │
│      aclrtSetDevice(devB)           ← 切回本进程上下文                       │
│                                                                             │
│  2.3 保留虚拟地址 + 映射                                                     │
│      aclrtReserveMemAddress(&vmmVA, 32MB, 0, NULL, 0)                       │
│      aclrtMapMem(vmmVA, 32MB, 0, h_imp, 0)                                  │
│                                                                             │
│  2.4 设置访问权限                                                            │
│      aclrtMemSetAccess(vmmVA, 32MB, {RW, devB}, 1)                          │
│                                                                             │
│  2.5 分配本地 buffer + async D2D 读 (VMM→local)                             │
│      aclrtMalloc(&local, 32MB, HUGE_FIRST)                                  │
│      aclrtMemcpyAsync(local, 32MB, vmmVA, 32MB, D2D, stream)                │
│      aclrtSynchronizeStream(stream)                                         │
│      aclrtMemcpy(vbuf, 32MB, local, 32MB, D2H)  → check_pat(rank0, 0xD1)   │
│                                                                             │
│  2.6 async D2D 反向写 (local→VMM)                                           │
│      aclrtMemcpy(local, 32MB, hbuf, 32MB, H2D)  ← fill_pat(rank1, 0xD2)    │
│      aclrtMemcpyAsync(vmmVA, 32MB, local, 32MB, D2D, stream)                │
│      aclrtSynchronizeStream(stream)                                         │
└─────────────────────────────────────────────────────────────────────────────┘

┌─ rank0 (exporter) 校验反向写 ──────────────────────────────────────────────┐
│  2.7 aclrtMemcpy(vbuf, 32MB, ownVA, 32MB, D2H) → check_pat(rank1, 0xD2)   │
└─────────────────────────────────────────────────────────────────────────────┘
```

## 附录: ctx 技巧图解

### 图 1: 两个逻辑设备与内存布局

```
┌─────────────────────────────────────────────────────────────────────────────────┐
│                              同一张卡 (Atlas 300I Duo)                           │
│                                                                                 │
│   ┌─────────────────────────────────┐     ┌─────────────────────────────────┐   │
│   │        devA (die 0)             │     │        devB (die 1)             │   │
│   │      rank0 / exporter           │     │      rank1 / importer           │   │
│   │                                 │     │                                 │   │
│   │  ┌───────────────────────────┐  │     │  ┌───────────────────────────┐  │   │
│   │  │  物理内存 h_own (32MB)    │  │     │  │  物理内存 (imported)      │  │   │
│   │  │  MallocPhysical           │  │     │  │  ImportFromShareableHandle│  │   │
│   │  │  ACL_MEM_P2P_HUGE         │  │     │  │  h_imp → 指向同一物理页  │  │   │
│   │  └──────────┬────────────────┘  │     │  └──────────┬────────────────┘  │   │
│   │             │                   │     │             │                   │   │
│   │             ▼                   │     │             ▼                   │   │
│   │  ┌───────────────────────────┐  │     │  ┌───────────────────────────┐  │   │
│   │  │  虚拟地址 ownVA           │  │     │  │  虚拟地址 vmmVA           │  │   │
│   │  │  Reserve + Map            │  │     │  │  Reserve + Map            │  │   │
│   │  │  SetAccess(RW, devA)      │  │     │  │  SetAccess(RW, devB)      │  │   │
│   │  └──────────┬────────────────┘  │     │  └──────────┬────────────────┘  │   │
│   │             │                   │     │             │                   │   │
│   │             │    同一物理页, 两个不同的虚拟地址窗口                       │   │
│   │             └───────────────────┴─────────────────────┘                 │   │
│   │                                                                         │   │
│   │  ┌───────────────────────────┐  │     │  ┌───────────────────────────┐  │   │
│   │  │  本地 buffer local        │  │     │  │  本地 buffer local        │  │   │
│   │  │  (rank0 无)               │  │     │  │  aclrtMalloc(32MB)        │  │   │
│   │  └───────────────────────────┘  │     │  └───────────────────────────┘  │   │
│   │                                 │     │                                 │   │
│   └─────────────────────────────────┘     └─────────────────────────────────┘   │
│                                                                                 │
└─────────────────────────────────────────────────────────────────────────────────┘
```

### 图 2: ctx 技巧关键流程 (Step 2)

```
时间线 ─────────────────────────────────────────────────────────────────────────►

  rank0 (devA ctx)                    rank1 (devB ctx)
  ════════════════                    ════════════════

  [Step 1]                            [等待]
  MallocPhysical → h_own
  Reserve → ownVA
  Map(ownVA, h_own)
  SetAccess(RW, devA)
  H2D 写 pattern(0xD1)
  Export → sh1
  MPI_Send(sh1) ──────────────────►   MPI_Recv(sh1)

                                      [Step 2 - ctx 技巧]
                                      ┌─────────────────────────────────────┐
                                      │ ★ SetDevice(devA)                   │ ← 临时切到 devA 上下文
                                      │   (绕过 runtime P2P 前置检查)        │
                                      │                                     │
                                      │ ImportFromShareableHandle(sh1,      │
                                      │                    devB, &h_imp)    │ ← devId=devB, 本 die
                                      │                                     │
                                      │ SetDevice(devB)                     │ ← 切回本进程上下文
                                      └─────────────────────────────────────┘

                                      Reserve → vmmVA
                                      Map(vmmVA, h_imp)
                                      SetAccess(RW, devB)

                                      [D2D 读: devA 物理页 → devB local]
                                      ┌─────────────────────────────────────┐
                                      │ ownVA ──(D2D async)──► local       │
                                      │ (devA 物理页)          (devB 本地)  │
                                      └─────────────────────────────────────┘
                                      D2H 读 local → vbuf
                                      check_pat(rank0, 0xD1) ✓

                                      [D2D 写: devB local → devA 物理页]
                                      ┌─────────────────────────────────────┐
                                      │ local ──(D2D async)──► vmmVA       │
                                      │ (devB 本地)           (devA 物理页) │
                                      └─────────────────────────────────────┘

  [Step 2 终验]
  D2H 读 ownVA → vbuf
  check_pat(rank1, 0xD2) ✓
```

### 图 3: 对照实验对比

```
┌───────────────────────────────────────────────────────────────────────────────┐
│  正向 (ctx 技巧)                              对照 (本进程 ctx)               │
│  ═══════════════                              ═══════════════                 │
│                                                                               │
│  rank1 ctx = devB                         rank1 ctx = devB                   │
│       │                                       │                              │
│       ▼                                       ▼                              │
│  SetDevice(devA)  ◄── 临时切换              (保持 devB)                      │
│       │                                       │                              │
│       ▼                                       ▼                              │
│  Import(sh1, devB)                      Import(sh2, devB)                    │
│       │                                       │                              │
│       │  runtime 检查:                        │  runtime 检查:                │
│       │  "当前 ctx 是 devA,                   │  "当前 ctx 是 devB,           │
│       │   import 目标是 devB"                 │   import 目标是 devB"         │
│       │   → devA 和 devB 同卡,                │   → devB 和 devB 同设备?      │
│       │     但 runtime 不认为                  │   → 但需要跨 die P2P,         │
│       │     这是合法 P2P 对...                 │     runtime 触发 para_check   │
│       │   → 等等, 为什么成功了?                │   → para_check 拒绝!          │
│       │   → 因为 ctx 技巧让                    │                              │
│       │     runtime 以为在                     │  ret = 507899 ✗              │
│       │     "本地" import                     │                              │
│       │                                       │                              │
│       ▼                                       ▼                              │
│  SetDevice(devB)  ◄── 切回                  (无需切换)                       │
│       │                                                                      │
│       ▼                                                                      │
│  Reserve + Map + SetAccess                                                   │
│       │                                                                      │
│       ▼                                                                      │
│  D2D 读写成功 ✓                                                              │
│                                                                               │
│  ★ 关键: ctx 技巧让 runtime 认为 import 是"本地"操作,                         │
│          绕过了 P2P 前置检查                                                  │
└───────────────────────────────────────────────────────────────────────────────┘
```

### 图 4: 数据流向总览

```
                    Host Memory (rank1)
                    ┌─────────────────┐
                    │  hbuf (32MB)    │
                    │  vbuf (32MB)    │
                    └────────┬────────┘
                             │ H2D / D2H
                             ▼
┌────────────────────────────────────────────────────────────────────────────────┐
│                           devB (rank1 / importer)                              │
│                                                                                │
│   ┌─────────────────┐    async D2D    ┌─────────────────────────────────────┐ │
│   │  local (32MB)   │ ◄────────────── │  vmmVA (imported VA, 32MB)          │ │
│   │  aclrtMalloc    │ ──────────────► │  Map(h_imp)                         │ │
│   └─────────────────┘                 │  SetAccess(RW, devB)                │ │
│                                       └──────────────────┬──────────────────┘ │
│                                                          │                    │
└──────────────────────────────────────────────────────────┼────────────────────┘
                                                           │
                                          同一物理页 (跨 die 映射)
                                                           │
┌──────────────────────────────────────────────────────────┼────────────────────┐
│                           devA (rank0 / exporter)        │                    │
│                                                          │                    │
│   ┌──────────────────────────────────────────────────────▼─────────────────┐ │
│   │  ownVA (32MB)                                                          │ │
│   │  Map(h_own)                                                            │ │
│   │  SetAccess(RW, devA)                                                   │ │
│   └────────────────────────────────────────────────────────────────────────┘ │
│                                                                                │
└────────────────────────────────────────────────────────────────────────────────┘

数据传输路径:
  Step 2 读:  ownVA ──(D2D async)──► local ──(D2H)──► vbuf
  Step 2 写:  hbuf ──(H2D)──► local ──(D2D async)──► vmmVA
  Step 5 读:  vmmVA ──(D2H)──► vbuf  (host 直达, 无需 local)
  Step 5 写:  hbuf ──(H2D)──► vmmVA  (host 直达, 无需 local)
```

### ctx 技巧本质总结

```
┌───────────────────────────────────────────────────────────────────────────────┐
│                                                                               │
│  问题: die 对 (同卡双 die) 的 VMM shareable handle import 被 runtime 拒绝     │
│                                                                               │
│  原因: aclrtMemImportFromShareableHandle 执行时, runtime 做 P2P 前置检查:     │
│        "当前进程 ctx 所在设备" 与 "目标设备" 必须是合法 P2P 对                 │
│        die 对不被 runtime 识别为合法 P2P 对 → para_check 拒绝 → 507899        │
│                                                                               │
│  解决: ctx 技巧                                                               │
│        1. SetDevice(devA)      ← 临时切到导出方 die 上下文                    │
│        2. Import(sh, devB)     ← runtime 以为在"本地" import                  │
│        3. SetDevice(devB)      ← 切回本进程上下文                            │
│                                                                               │
│  本质: 欺骗 runtime 的 P2P 前置检查, 让 import 操作在导出方 ctx 下执行,       │
│        绕过 "当前 ctx 设备 vs 目标设备" 的 P2P 合法性校验                     │
│                                                                               │
│  对比: EnablePeerAccess 无法解决, 因为 aclrtDeviceEnablePeerAccess 本身就     │
│        返回 507899 — runtime 根本不认为 die 对是合法 P2P 对                   │
│                                                                               │
└───────────────────────────────────────────────────────────────────────────────┘
```
