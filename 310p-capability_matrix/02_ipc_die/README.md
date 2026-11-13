# 02：同卡双die的IPC共享

导出方分配32MiB设备内存并导出IPC名称，导入方以DEFAULT方式映射；用异步D2D及Host直接读写验证双向可见性。另用16MiB内存检查PEER导入和同步D2D的返回值。

## 测试步骤、API和数据关系

rank0是导出方A，rank1是导入方B。A的设备指针`da`与B导入所得指针`p`访问同一块物理内存；`loc`是B另外分配的本地设备内存。

| 阶段 | 写入与校验顺序 | 主要API |
|---|---|---|
| 建立共享 | A分配32MiB并写入数据，导出IPC名称，经MPI发给B | `aclrtMalloc`、`aclrtIpcMemGetExportKey`、`MPI_Send/Recv` |
| 正向读取 | B以DEFAULT导入；`p → loc → Host`，由B校验A的数据 | `aclrtIpcMemImportByKey`、`aclrtMemcpyAsync(D2D)`、stream同步、D2H |
| 反向写入 | B写本地数据后`loc → p`，A从`da`读回校验 | H2D、异步D2D、stream同步、D2H |
| PEER对照 | 使用另一块16MiB的IPC名称，记录PEER_ACCESS导入结果 | `aclrtIpcMemImportByKey` |
| Host直接访问 | B直接从`p`做D2H，再H2D写入`p`；A从`da`复核 | `aclrtMemcpy(D2H/H2D)` |
| 带宽与同步对照 | 测异步读写；使用新数据另行尝试共享映射的同步D2D | `aclrtMemcpyAsync`与`aclrtMemcpy`分别记录 |

校验数据包含rank、tag和元素索引；各步骤更换tag。异步传输等待stream后才读回。带宽先通过数据校验，再作3次预热及30次16MiB提交，末尾等待一次stream。


## 运行

```bash
source /usr/local/Ascend/cann/set_env.sh
export ASCEND_HOME=/usr/local/Ascend/cann
cd 310p-capability_matrix/02_ipc_die
A_DEV=2 B_DEV=3 bash run.sh
```

## 2026-11-13测试结果

Ascend310P3，CANN 9.2.1，驱动26.1.1，aarch64。设备及数据规模见下表。

| 检查项 | 本轮结果 |
|---|---|
| 设备 | 2、3，同卡两个MPI rank |
| DEFAULT导入、异步D2D双向读写 | 通过 |
| Host直接H2D/D2H访问导入映射 | 16MiB校验通过 |
| PEER_ACCESS导入 | 返回507899 |
| 同步D2D读取共享映射 | 返回507899 |
| 带宽 | 16MiB，3次预热、30次异步提交后同步：读18.99、写17.34 GB/s |
| 总结果 | PASS，两个rank退出0 |


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
