# 05_vmm_die — VMM shareable 链路 die 对(同卡双 die)ctx 技巧

Atlas 300I Duo (310P3, 4 卡 × 2 die = 8 dev) / CANN 9.1.0 / aarch64,MPI 双进程:
rank0=exporter(dev A,`A_DEV` 默认 0),rank1=importer(dev B,`B_DEV` 默认 1)。

## 验证能力(矩阵单元格)
- VMM die 对:ctx 技巧 import ✓ / 本进程 ctx import ✗(对照,预期 507899)
- die 对 async D2D ✓(读+写,双向逐字 pattern 校验)/ 同步 D2D ✗(不测,已知 507899,见 02)
- host 直达 ✓(imported VA 上直接 D2H/H2D 各 16MB + 校验)

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

### Step 3: 对照实验 (预期 507899)

```
┌─ rank0: 第二块独立 16MB ───────────────────────────────────────────────────┐
│  3.1 MallocPhysical(16MB) + Reserve + Map + SetAccess + H2D pattern        │
│  3.2 Export → sh2, MPI_Send(&sh2, tag=101)                                 │
└─────────────────────────────────────────────────────────────────────────────┘

┌─ rank1: 本进程 ctx 直接 import (不做 ctx 技巧) ────────────────────────────┐
│  3.3 MPI_Recv(&sh2, ...)                                                    │
│  3.4 aclrtMemImportFromShareableHandle(sh2, devB, &h2)                      │
│      → 预期 ret=507899 (runtime P2P 前置 para_check 拒绝 die 对)            │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Step 5: Host 直达 (imported VA 上直接 D2H/H2D)

```
┌─ rank1 ────────────────────────────────────────────────────────────────────┐
│  5.1 D2H 读前 16MB:  aclrtMemcpy(vbuf, 16MB, vmmVA, 16MB, D2H)            │
│      → check_pat(rank1, 0xD2)  (step2 反向写的内容)                        │
│                                                                             │
│  5.2 H2D 写前 16MB:  aclrtMemcpy(vmmVA, 16MB, hbuf, 16MB, H2D)            │
│      → fill_pat(rank1, 0xD3)                                               │
└─────────────────────────────────────────────────────────────────────────────┘

┌─ rank0: 复核 ──────────────────────────────────────────────────────────────┐
│  5.3 aclrtMemcpy(vbuf, 16MB, ownVA, 16MB, D2H) → check_pat(rank1, 0xD3)   │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Step 6: 对照实验 — EnablePeerAccess + 不使用 ctx 技巧 (预期失败)

```
┌─ rank0: 第三块独立 16MB ───────────────────────────────────────────────────┐
│  6.1 MallocPhysical(16MB) + Reserve + Map + SetAccess + H2D pattern        │
│  6.2 Export → sh3, MPI_Send(&sh3, tag=102)                                 │
└─────────────────────────────────────────────────────────────────────────────┘

┌─ rank1: EnablePeerAccess + 本进程 ctx 直接 import ─────────────────────────┐
│  6.3 MPI_Recv(&sh3, ...)                                                    │
│  6.4 aclrtDeviceEnablePeerAccess(devA, 0)                                   │
│      → 实测 ret=507899 (EnablePeerAccess 本身就失败)                        │
│  6.5 aclrtMemImportFromShareableHandle(sh3, devB, &h3)                      │
│      → ret=507899 (import 也失败)                                           │
│  6.6 aclrtDeviceDisablePeerAccess(devA)                                     │
│      → ret=507899 (best-effort 清理)                                        │
└─────────────────────────────────────────────────────────────────────────────┘

★结论: EnablePeerAccess 无法替代 ctx 技巧, die 对场景下:
  - aclrtDeviceEnablePeerAccess 本身就返回 507899
  - 必须使用 ctx 技巧 (SetDevice(A)→Import→SetDevice(B)) 才能绕过 P2P 前置
```

### Step 4: 带宽测试 (正确性全过后)

```
┌─ rank1 ────────────────────────────────────────────────────────────────────┐
│  读方向: VMM[16MB,32MB) → local[16MB,32MB)                                │
│      3× 预热 (MemcpyAsync + StreamSync)                                    │
│      30× aclrtMemcpyAsync (提交不等待)                                      │
│      1× aclrtSynchronizeStream (末尾统一等待)                                │
│      → 19.00 GB/s                                                          │
│                                                                             │
│  写方向: local[0,16MB) → VMM[0,16MB)                                       │
│      同口径 → 17.94 GB/s                                                    │
│                                                                             │
│  ★终态: 全 32MB 恢复为 TAG_BACK(rank1, 0xD2), 供 exporter 终验             │
└─────────────────────────────────────────────────────────────────────────────┘

┌─ rank0: 终验 ──────────────────────────────────────────────────────────────┐
│  aclrtMemcpy(vbuf, 32MB, ownVA, 32MB, D2H) → check_pat(rank1, 0xD2)       │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Cleanup 顺序 (铁律: importer 先释放, barrier 后 exporter 释放)

```
┌─ rank1 (importer) ─────────────────────────────────────────────────────────┐
│  UnmapMem(vmmVA) → FreePhysical(h_imp) → ReleaseMemAddress(vmmVA)          │
│  Free(local) → DestroyStream(stream)                                       │
└─────────────────────────────────────────────────────────────────────────────┘
                                    ↓ MPI_Barrier
┌─ rank0 (exporter) ─────────────────────────────────────────────────────────┐
│  main: UnmapMem → FreePhysical(h_own) → ReleaseMemAddress                  │
│  ctrl: UnmapMem → FreePhysical(h_ctrl) → ReleaseMemAddress                 │
└─────────────────────────────────────────────────────────────────────────────┘
                                    ↓
┌─ 两 rank 对称 ─────────────────────────────────────────────────────────────┐
│  DisablePeerAccess(peer)  ← best-effort, ret=507899 无害                   │
│  ResetDevice(dev)                                                           │
│  Finalize()                                                                 │
│  rank1 额外: ResetDevice(devA)  ← ctx 技巧开过的 A 侧 context              │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 关键点

- **不调用 EnablePeerAccess**: ctx 技巧跳过 P2P 前置, 无 D 态风险
- **校验 pattern**: `(rank<<28)|(tag<<24)|(idx&0xFFFFFF)`, tag 每步不同防假阳性
- **带宽铁律**: 严禁同步循环, 必须 async 提交 + 末尾单次 Sync

## 与 04_vmm_cross 的差异
| 维度 | 04_vmm_cross(跨卡) | 05_vmm_die(die 对) |
|---|---|---|
| 默认 devB | 2(跨卡) | 1(die 对) |
| 正向 import | 本进程 ctx + EnablePeerAccess | ctx 技巧(SetDevice(A)→Import→SetDevice(B)) |
| 对照 | ctx 技巧(预期 507899) | 本进程 ctx(预期 507899) |
| EnablePeerAccess | 双向(exporter+B, importer+A) | 不调用 |
| D 态风险 | 有(EnablePeerAccess → cleanup 需 DisablePeerAccess) | 理论上无(P2P 前置被跳过) |

## 运行方法
```
./run.sh                    # 默认 dev0↔dev1(die 对)
A_DEV=2 B_DEV=3 ./run.sh    # 换 die 对(卡1 的 2,3)
A_DEV=0 B_DEV=2 ./run.sh    # 跨卡(对照,ctx 技巧预期失败 — 同 04 场景)
```

## 预期结果
正向 ctx 技巧 import(SetDevice(A)→Import→SetDevice(B))OK;async D2D 双向校验 OK;
host 直达 OK;本进程 ctx import:预期 507899(EXPECTED-FAIL,如实记录);
`[05] RESULT: PASS`(正向 1/2/5 全 OK 即 PASS;对照 3 不影响判定)。
实测带宽: die 对 VMM ctx 技巧 async D2D 读 19.00 GB/s / 写 17.94 GB/s。

## 实测结果（2026-08-25 运行）

### 完整输出

```
[run] A_DEV=0 B_DEV=1 (die pair, timeout 60s)
[05] === 05_vmm_die: VMM shareable handle link die-pair ctx trick, rank0 r0/exporter dev0 ===
[05] === 05_vmm_die: VMM shareable handle link die-pair ctx trick, rank1 r1/importer dev1 ===
[05] init: r0/exporter aclInit/SetDevice(0) OK
[05] init: r1/importer aclInit/SetDevice(1)/CreateStream OK
[05]   [main attr 1/7 HBM_P2P_HUGE  ] gran ret=100000 gran=0; MallocPhysical(32MB) ret=100000
[05]   [main attr 2/7 HBM_P2P_NORMAL] gran ret=100000 gran=0; MallocPhysical(32MB) ret=100000
[05]   [main attr 3/7 DDR_P2P_HUGE  ] gran ret=100000 gran=0; MallocPhysical(32MB) ret=100000
[05]   [main attr 4/7 DDR_P2P_NORMAL] gran ret=100000 gran=0; MallocPhysical(32MB) ret=100000
[05]   [main attr 5/7 MEM_P2P_HUGE  ] gran ret=0 gran=2097152; MallocPhysical(32MB) ret=0
[05]   [main] chosen MEM_P2P_HUGE  , 32MB = 16 x gran 2097152B
[05] step 1 (r0): SetAccess(RW, A_dev) on own VA                             [OBS] OK (ret=0)
[05] step 1 (r0): sh1=0x8 sent
[05] step 1 (r0): MallocPhysical(7attr)+Reserve+Map+H2D pat+Export sh1+send  OK (ret=0)
[05] step 2 (r1): ctx trick: SetDevice(A_dev) before import (bypass P2P prereq) [OBS] OK (ret=0)
[05] step 2 (r1): SetDevice(B_dev) restore after import                      [OBS] OK (ret=0)
[05] step 2 (r1): SetAccess(RW, B_dev) on imported VA                        [OBS] OK (ret=0)
[05] step 2 (r1): ctx-trick import(A->Imp sh1,B->SetDevice B)+Reserve+Map+async D2D read+verify OK (ret=0)
[05] step 2 (r1): async D2D reverse write (local->VMM) + StreamSync          OK (ret=0)
[05] step 2 (r0): D2H verify reverse-written pattern (r1/TAG_D2) on own VA   OK (ret=0)
[05]   [ctrl attr 1/7 HBM_P2P_HUGE  ] gran ret=100000 gran=0; MallocPhysical(16MB) ret=100000
[05]   [ctrl attr 2/7 HBM_P2P_NORMAL] gran ret=100000 gran=0; MallocPhysical(16MB) ret=100000
[05]   [ctrl attr 3/7 DDR_P2P_HUGE  ] gran ret=100000 gran=0; MallocPhysical(16MB) ret=100000
[05]   [ctrl attr 4/7 DDR_P2P_NORMAL] gran ret=100000 gran=0; MallocPhysical(16MB) ret=100000
[05]   [ctrl attr 5/7 MEM_P2P_HUGE  ] gran ret=0 gran=2097152; MallocPhysical(16MB) ret=0
[05]   [ctrl] chosen MEM_P2P_HUGE  , 16MB = 8 x gran 2097152B
[05] step 3 (r0): SetAccess(RW, A_dev) on ctrl VA                            [OBS] OK (ret=0)
[05] step 3 (r0): sh2=0xb sent
[05] step 3 (r0): MallocPhysical 16MB #2 + H2D pattern + Export sh2 + MPI-send OK (ret=0)
[05] step 3 (r1): ctrl: own-ctx Import(sh2,B) — expect 507899              EXPECTED-FAIL ret=507899
[05]   [epa attr 1/7 HBM_P2P_HUGE  ] gran ret=100000 gran=0; MallocPhysical(16MB) ret=100000
[05]   [epa attr 2/7 HBM_P2P_NORMAL] gran ret=100000 gran=0; MallocPhysical(16MB) ret=100000
[05]   [epa attr 3/7 DDR_P2P_HUGE  ] gran ret=100000 gran=0; MallocPhysical(16MB) ret=100000
[05]   [epa attr 4/7 DDR_P2P_NORMAL] gran ret=100000 gran=0; MallocPhysical(16MB) ret=100000
[05]   [epa attr 5/7 MEM_P2P_HUGE  ] gran ret=0 gran=2097152; MallocPhysical(16MB) ret=0
[05]   [epa] chosen MEM_P2P_HUGE  , 16MB = 8 x gran 2097152B
[05] step 6 (r0): SetAccess(RW, A_dev) on epa VA                             [OBS] OK (ret=0)
[05] step 6 (r0): sh3=0xc sent
[05] step 6 (r0): MallocPhysical 16MB #3 + H2D pattern + Export sh3 + MPI-send OK (ret=0)
[05] step 6 (r1): aclrtDeviceEnablePeerAccess(devA=0) ret=507899
[05] step 6 (r1): epa: EnablePeerAccess+own-ctx Import(sh3,B)                FAIL ret=507899 (EnablePeerAccess 无法替代 ctx 技巧, die 对仍被拒绝)
[05] step 6 (r1): DisablePeerAccess(devA=0) ret=507899
[05] step 5 (r1): host direct D2H read remote 16MB (imported VA) + verify    OK (ret=0)
[05] step 5 (r1): host direct H2D write remote 16MB (imported VA)            OK (ret=0)
[05] step 5 (r0): D2H verify host-direct-written 16MB (r1/TAG_D3) on own VA  OK (ret=0)
[05] step 4 (r1): bandwidth on imported VMM VA (ctx trick), 16 MB x 30 iters + 3 warmup, async submits + 1 final Sync ONLY (no sync loop!)
[05] bw   D2D-read  (VMM->local) :   19.00 GB/s (30x16MB async + 1 Sync, total 0.026 s)
[05] bw   D2D-write (local->VMM) :   17.94 GB/s (30x16MB async + 1 Sync, total 0.028 s)
[05] step 2 (r0): final D2H verify reverse pattern (full 32MB) after r1 done OK (ret=0)
[05] cleanup (r1): UnmapMem(imported VA) ret=0
[05] cleanup (r1): FreePhysical(imported) ret=0
[05] cleanup (r1): ReleaseMemAddress(imported VA) ret=0
[05] cleanup (r0): UnmapMem(main VA) ret=0
[05] cleanup (r0): FreePhysical(main) ret=0
[05] cleanup (r0): ReleaseMemAddress(main VA) ret=0
[05] cleanup (r0): UnmapMem(ctrl VA) ret=0
[05] cleanup (r0): FreePhysical(ctrl) ret=0
[05] cleanup (r0): ReleaseMemAddress(ctrl VA) ret=0
[05] cleanup (r0): UnmapMem(epa VA) ret=0
[05] cleanup (r0): FreePhysical(epa) ret=0
[05] cleanup (r0): ReleaseMemAddress(epa VA) ret=0
[05] cleanup (r1): DisablePeerAccess(0) ret=507899 (best-effort)
[05] cleanup (r0): DisablePeerAccess(1) ret=507899 (best-effort)
[05] RESULT: PASS  ★step3: 本进程 ctx import EXPECTED-FAIL ret=507899(符合预期)  ★step6: EnablePeerAccess+own-ctx import FAIL ret=507899(EnablePeerAccess 无法替代 ctx 技巧)
[05] RESULT: PASS  ★step3: 本进程 ctx import EXPECTED-FAIL ret=507899(符合预期)  ★step6: EnablePeerAccess+own-ctx import FAIL ret=507899(EnablePeerAccess 无法替代 ctx 技巧)
[run] exit=0 (124=timeout/hang)
```

### 逐步结果解读

| Step | 内容 | 结果 | 说明 |
|------|------|------|------|
| **init** | aclInit + SetDevice + CreateStream | OK | r0 绑定 dev0, r1 绑定 dev1, r1 额外创建 async stream |
| **1.1** | MallocPhysical 七级回退 | OK | 前 4 项 (HBM/DDR P2P 变体) ret=100000 不支持; 第 5 项 `ACL_MEM_P2P_HUGE` 成功, 粒度 2MB, 32MB=16×2MB ✓ |
| **1.2-1.3** | Reserve + Map + SetAccess | OK | 虚拟地址保留 + 物理内存映射 + 设置 RW 权限 (devA) |
| **1.4-1.6** | H2D 写 pattern + Export + MPI 发送 | OK | sh1=0x8 成功发送 |
| **2.2** | ★ctx 技巧★ SetDevice(A)→Import→SetDevice(B) | OK | 临时切到 dev0 上下文做 import (devId=dev1), 绕过 P2P 前置 |
| **2.3-2.4** | Reserve + Map + SetAccess | OK | importer 侧建立虚拟地址窗口, 设置 RW 权限 (devB) |
| **2.5** | async D2D 读 VMM→local + 校验 | OK | 读到 pattern (rank0, 0xD1), 逐字匹配 |
| **2.6** | async D2D 反向写 local→VMM | OK | 写入 pattern (rank1, 0xD2) |
| **2.7** | exporter D2H 校验反向写 | OK | 读到 pattern (rank1, 0xD2), 证明 D2D 写成功到达远端 die |
| **3.1-3.2** | 第二块 16MB 物理内存 + Export sh2 | OK | 独立块, 避免同 sh 二次 import 干扰 |
| **3.4** | 本进程 ctx 直接 Import (对照) | **EXPECTED-FAIL ret=507899** | 不做 ctx 技巧 → runtime P2P 前置 para_check 拒绝 die 对 |
| **6.1-6.2** | 第三块 16MB 物理内存 + Export sh3 | OK | 独立块, 用于 EnablePeerAccess 对照测试 |
| **6.4** | aclrtDeviceEnablePeerAccess(devA) | **ret=507899** | EnablePeerAccess 本身就失败, die 对不被识别为合法 P2P 对 |
| **6.5** | EnablePeerAccess + Import (对照) | **FAIL ret=507899** | EnablePeerAccess 无法替代 ctx 技巧 |
| **5.1** | host 直达 D2H 读 16MB | OK | importer 直接 `aclrtMemcpy(D2H)` 读 imported VA, pattern (rank1, 0xD2) 匹配 |
| **5.2** | host 直达 H2D 写 16MB | OK | importer 直接 `aclrtMemcpy(H2D)` 写 imported VA, pattern (rank1, 0xD3) |
| **5.3** | exporter 复核 host 直达写 | OK | 读到 pattern (rank1, 0xD3), 证明 host→远端 die 写成功 |
| **4** | 带宽测试 (async 口径) | OK | 读 19.00 GB/s, 写 17.94 GB/s |
| **终验** | exporter 全 32MB 校验 | OK | 带宽写方向恢复后, 全块 pattern (rank1, 0xD2) 匹配 |
| **cleanup** | Unmap + Free + Release | OK | importer 先释放, barrier 后 exporter 释放, 全部 ret=0 |
| **DisablePeerAccess** | best-effort | ret=507899 | 未 enable 过则 disable 报错, 无害 |
| **RESULT** | 最终判定 | **PASS** | 正向 1/2/5 全 OK, 对照 3 符合预期 |

### 关键结论

1. **die 对必须使用 ctx 技巧**: 本进程 ctx 直接 import 返回 507899, ctx 技巧绕过 para_check 成功
2. **EnablePeerAccess 无法替代 ctx 技巧**: `aclrtDeviceEnablePeerAccess(devA)` 本身就返回 507899, die 对不被 runtime 识别为合法 P2P 对
3. **memAttr 选择**: 310P3 仅 `ACL_MEM_P2P_HUGE` 成功, HBM/DDR 前缀变体均 ret=100000
4. **双向 D2D 可用**: async D2D 读 (VMM→local) + 写 (local→VMM) 均校验通过
5. **host 直达可用**: imported VA 上直接 D2H/H2D, 无需额外配置
6. **带宽**: 读 19.01 GB/s / 写 17.95 GB/s (30×16MB async + 1 Sync 口径)
7. **无 D 态风险**: 未调用 EnablePeerAccess, cleanup 正常退出 exit=0

---

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
