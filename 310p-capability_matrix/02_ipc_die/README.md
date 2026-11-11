# 02_ipc_die — IPC key 链路 die 对(同卡双 die)

Atlas 300I Duo (310P3, 4 卡 × 2 die = 8 dev) / CANN 9.1.0 / aarch64,MPI 双进程:
rank0=exporter(dev A,`A_DEV` 默认 0),rank1=importer(dev B,`B_DEV` 默认 1)。

## 验证能力(矩阵单元格)
- IPC die 对 import(DEFAULT)✓ / PEER flag ✗(对照)
- die 对 async D2D 读+写 ✓ / 同步 D2D ✗(对照)
- host 直达远端 H2D/D2H ✓

## API 链
- export:`aclrtMalloc(ACL_MEM_MALLOC_HUGE_FIRST_P2P)` 32MB → H2D 写 pattern →
  `aclrtIpcMemGetExportKey(ptr, size, key, 65, ACL_RT_IPC_MEM_EXPORT_FLAG_DISABLE_PID_VALIDATION)`
  → key(65B)经 MPI_Send/Recv 传给 importer
- import:`aclrtIpcMemImportByKey(&p, key, ACL_RT_IPC_MEM_IMPORT_FLAG_DEFAULT)`
- 跨 die 拷贝:仅 `aclrtMemcpyAsync(D2D)` + `aclrtSynchronizeStream`(同步 `aclrtMemcpy(D2D)` 507899)
- host 直达:imported VA 上直接 `aclrtMemcpy` D2H/H2D
- 对照A:第二块 16MB 的 key2 用 `ACL_RT_IPC_MEM_IMPORT_FLAG_ENABLE_PEER_ACCESS` import(die 对死路径)
- cleanup:双方各自 `aclrtIpcMemClose(key)`;物理内存只由 exporter `aclrtFree`;
  对照A(PEER_ACCESS)若意外成功(overturn),双方在 ResetDevice 前
  `aclrtDeviceDisablePeerAccess` 拆除 P2P 状态(正常路径不调用)

校验 pattern:`value = (rank<<28)|(tag<<24)|(idx&0xFFFFFF)`,每步不同 tag(0x2/0x3/0x4/0x6)防残留假阳性。

带宽口径:正确性全过后才测;3 次预热 + 30×16MB MemcpyAsync 提交 + 末尾 1 次 StreamSync。
**严禁同步循环测带宽**(同步 D2D 必失败 507899,循环量到的只是失败延迟,非真实带宽)。

## 数据流逻辑关系

### 内存架构

```
  r0/exporter (dev A)                r1/importer (dev B)
  ═════════════════════              ═════════════════════

  ┌── host ──────────┐              ┌── host ──────────┐
  │ hbuf   32MB      │              │ hbuf   32MB      │
  │ vbuf   32MB      │              │ vbuf   32MB      │
  └──────────────────┘              └──────────────────┘
         │    ↑                             │    ↑
    H2D  │    │ D2H                    H2D  │    │ D2H
         ▼    │                             ▼    │
  ┌── device ───────┐  ┌──────────┐  ┌── device ──────────┐
  │                  │  │ 共享物理  │  │                    │
  │ da   32MB ═══════┼══┤ 内存     ├──┼═══ p   32MB(import)│
  │ da2  16MB ═══════┼══┤ da=p    ├──┼═══ p2  16MB(ctrlA) │
  │                  │  │ da2=p2  │  │ loc  32MB(local)   │
  └──────────────────┘  └──────────┘  │ stream(async queue)│
   ↑ r0 aclrtFree物理                 └────────────────────┘
                                        ↑ r1 IpcMemClose(key)

  key1 ──MPI_Send(100)──→ ──Recv(100)──→ DEFAULT import → p    ✓
  key2 ──MPI_Send(101)──→ ──Recv(101)──→ PEER_ACCESS  → p2   ✗507899
```

**da 与 p 映射同一块物理内存**——经 `da` 写入的数据经 `p` 可读,反之亦然。`loc` 是 r1 独立的本地设备内存。

IPC 解决跨进程问题:rank1 是独立进程,无法直接使用 rank0 的 `da` 指针(页表隔离)。IPC import 在 rank1 建立新 VA `p` 指向同一物理内存,使 host CPU 和 NPU DMA 均可访问。

### 写入方 → 校验方关系总表

```
┌──────┬──────┬──────┬────────────────────────┬────────────┬────────┐
│ Step │ 写入 │ 校验 │ 数据路径               │ tag        │ 结果   │
├──────┼──────┼──────┼────────────────────────┼────────────┼────────┤
│  1   │  r0  │  —   │ hbuf ─H2D→ da          │ EXP=0x2    │  OK    │
│ 2a   │  r0  │ r1   │ p ─asyncD2D→ loc ─D2H→ │ EXP=0x2    │  ✓     │
│ 2b   │  r1  │  —   │ hbuf ─H2D→ loc ─asyncD2D│ BACK=0x3   │  OK    │
│ 2c   │  r1  │ r0   │ da ─D2H→               │ BACK=0x3   │  ✓     │
│  3   │  —   │ r1   │ PEER import → p2       │ (ctrlA)    │ 507899 │
│ 6a   │  r1  │ r1   │ p ─D2H(host直)→        │ BACK=0x3   │  ✓     │
│ 6b   │  r1  │  —   │ hbuf ─H2D(host直)→ p   │ HOST_W=0x4 │  OK    │
│ 6c   │  r1  │ r0   │ da ─D2H→               │ HOST_W=0x4 │  ✓     │
│  5   │  r1  │ r1   │ p ↔ loc (async ×30)    │ (bandwidth)│  GB/s  │
│  7   │  r0  │ r1   │ p ─syncD2D→ loc ─D2H→  │ SYNC_RD=0x6│ 507899 │
│  4   │  —   │ r1   │ p ─syncD2D→ loc        │ (ctrlB)    │ 507899 │
└──────┴──────┴──────┴────────────────────────┴────────────┴────────┘

执行顺序: 1 → 2a → 2b → 2c → 3 → 6a → 6b → 6c → 5 → 7 → 4
          └── 正向 ──┘    └对照A┘ └── host 直达 ──┘ └带宽┘ └观测┘ └对照B┘
```

## 运行方法
```
./run.sh        # 默认 dev0↔dev1
A_DEV=2 B_DEV=3 mpirun --allow-run-as-root -np 2 ./test   # 换 die 对
```

## 预期结果
DEFAULT import OK;PEER flag 507899;async D2D 双向校验 OK;sync D2D 507899;
host 直达 H2D/D2H 16MB 校验 OK;step7 同步 D2D 读 507899(观测,不影响判定);RESULT: PASS

带宽参考(历史):die 对 async D2D ~17.7-19 GB/s

## ★实测结论

**2026-08-25,默认 die 对 dev0↔dev1,RESULT: PASS,exit=0,无 D 态残留。**

关键输出:
```
[02] step 1 (r0): malloc P2P 32MB + H2D pattern + export key1 + MPI-send     OK (ret=0)
[02] step 2 (r1): import DEFAULT + async D2D read (p->local) + D2H verify    OK (ret=0)
[02] step 2 (r1): async D2D reverse write (local->p) + StreamSync            OK (ret=0)
[02] step 2 (r0): D2H verify reverse-written pattern (r1/TAG3) on own VA     OK (ret=0)
[02] step 3 (r0): malloc P2P 16MB #2 + export key2 + MPI-send (ctrlA setup)  OK (ret=0)
[02] step 3 (r1): ctrlA: import key2 w/ ENABLE_PEER_ACCESS flag              EXPECTED-FAIL ret=507899
[02] step 6 (r1): host direct D2H read remote 16MB (imported VA) + verify    OK (ret=0)
[02] step 6 (r1): host direct H2D write remote 16MB (imported VA)            OK (ret=0)
[02] step 6 (r0): D2H verify host-direct-written 16MB (r1/TAG4) on own VA    OK (ret=0)
[02] bw   D2D-read  (p->local) :   18.88 GB/s (30x16MB async + 1 Sync, total 0.027 s)
[02] bw   D2D-write (local->p) :   17.50 GB/s (30x16MB async + 1 Sync, total 0.029 s)
[02] step 7 (r0): H2D write fresh pattern (TAG_SYNC_RD) for sync D2D read  OK (ret=0)
[02] step 7 (r1): sync D2D read (p->local) + D2H verify                      FAIL (ret=507899)  [与 step4 一致: sync D2D 不可用]
[02] step 4 (r1): ctrlB: sync aclrtMemcpy D2D (p->local) on DEFAULT VA       EXPECTED-FAIL ret=507899
[02] cleanup (r0): IpcMemClose(key1) ret=0
[02] cleanup (r0): IpcMemClose(key2/ctrlA) ret=0
[02] cleanup (r1): IpcMemClose(key1) ret=0
[02] cleanup (r0): physical mem freed by exporter only
[02] RESULT: PASS
```

结论:
- **507899 = `ACL_ERROR_RT_DRV_INTERNAL_ERROR`**(`rt_error_codes.h:148`),驱动层对不支持路径的确定性回复,非随机错误。
- **正向**(step 1/2/6)全 OK:DEFAULT import + async D2D 双向 + host 直达 H2D/D2H 均通过数据校验。
- **死路径**(step 3/4/7)均 507899:PEER_ACCESS import、同步 D2D 读(step7)、同步 D2D(step4)——die 对上同步 D2D 不可用是确定性行为,跨 die 数据搬运只能用 `aclrtMemcpyAsync` + `SynchronizeStream`。
- **带宽**:async D2D 读 18.88 / 写 17.50 GB/s,三轮实测(19.03/19.00/18.88 读,17.34/17.31/17.50 写)稳定在 17-19 GB/s 区间。
- **cleanup**:双方 IpcMemClose ret=0,物理内存仅 exporter free,进程干净退出。DisablePeerAccess 修复就位(正常路径不触发,仅对照A overturn 时防护 D 态)。
