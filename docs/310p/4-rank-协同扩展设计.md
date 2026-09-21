# 310P 4-rank 协同扩展设计（每卡 2 die × 2 卡）

> **文档定位**：设计文档（需求 + 可行性 + 实施计划），设备侧自驱协同层（signal/wait/quiet
> 邮箱 + epoch kernel）的 4-rank 扩展。
> **与《memfabric-310p-内存池化方案.md》互补**：方案文档负责池层（BM create/join/copy 语义、
> rank 支持矩阵、P2P 槽位与 VMM 槽位窗口），本文档只负责协同层；池层结论本文直接引用，不重复。
> **读者**：4-rank 协同扩展的实施与评审人。
> **工作方式**：开发分支 `wgm-dev-310p-4rank`（基于 `wgm-dev-310p`）；`wgm-dev-310p` 处于
> 合入流程中，冻结不再改动。

## 1. 需求

| 项 | 内容 |
|---|---|
| 拓扑 | 2 张 Atlas 300I Duo × 2 die = 4 rank（dev0/1 = 卡0，dev2/3 = 卡1），HBM 池（SDMA+VMM） |
| 传输 | 设备侧自驱：AICore 内 `signal/wait/quiet`，AICPU epoch 代投 SDMA SQE，host 零数据面 |
| 兼容 | 2-rank 用例（08 示例、既有 UT）不改行为全量回归通过 |
| 交付形态 | 库内自动部署照旧（每卡 CP1 独立落盘），用户仅改 worldSize/rank |

### 非目标

1. 8-rank 全互联——每卡 8 个 P2P 槽位硬约束，方案文档 §16 已实测否定；
2. ring/doubling 等稀疏拓扑路由层；
3. 跨节点（RDMA/URMA/TCP 后端）；
4. TRANS/SHM/Acc Offload 等其他模块适配。

## 2. 可行性评估（证据链）

| 层 | 判定 | 证据 |
|---|---|---|
| 池层（4 段对称 GVA 空间） | ✅ 可行 | 见下方池层证据链 |
| SDMA SQE 跨卡通路 | ✅ 已实证 | 见下方 SQE 证据链 |
| 邮箱协议层 | ❌ 现设计不支持，需重设计 | 见下方邮箱分析 |
| 部署/资源层 | ✅ 免改 | 每卡独立设备 OS，自动部署按本 rank devId 落各卡；meta 区每进程一份 |

**池层证据链**：方案文档 rank 矩阵明确"两卡四 die 4 rank 全互联"为一期目标；
槽位预算每卡 2 rank × 2 对端 = **4/8**（16_vmm_matrix 实证每卡 8 槽上限，die 对
import 免费）。跨卡使能按路径分：IPC 路径需**双向** `rtEnableP2P`（15_peer_needed
ENABLE_BOTH 实证单向必败 507899）；本池 VMM shareable handle 路径实测（04_vmm_cross）
**免 EPA** 即可跨卡 import + D2D 读写，配额仍隐式消耗。

**SQE 证据链**（三层）：① xcclpp 原型 `aicpu-aicore-310p3` 跨卡实测——人工验证手册
§3.2 明文 dev0↔dev2 跨卡 runbook，机制结论"跨卡导出内存必须 P2P policy、跨卡通知
用 SDMA flag-SQE 对"即出自这些运行；② 平台事实 S 表（E0b）：SMMU 可译 AclVmm 池
GVA（含导入映射），数据 SQE 的 src/dst 直接用池地址成立；③ hcomm 同配方
（`AddOneMemcpySqeV2`）跨卡 AllReduceRing 生产验证。

**邮箱分析**：到达环为单生产者 FIFO——对端 epoch 用**其自身请求序号**戳我的到达环
`seq%1024` 槽，我的 wait 按本端消费游标对齐；多对端时序号互踩、跨对端无全局 FIFO。
quiet 环为本端 epoch 自写（多对端安全）、请求环为本端单生产者（安全），均免改；
监督线程/epoch 续命模型不变。

## 3. 设计

### 3.1 xcclpp 跨卡配方参照（已实证，照抄）

原型 `overlap_demo_mpi` 的跨卡四件套（`overlap_demo_mpi.asc` + 人工验证手册）：

| # | 做法 | 出处 |
|---|---|---|
| 1 | 跨卡 SDMA 写目标内存一律 P2P policy 分配（IPC 路径；普通分配跨卡 import 507899） | asc:481-484 |
| 2 | `aclrtGetDevicesTopo` 探测分派 importFlag：HCCS（die 对）DEFAULT / PCIe（跨卡）ENABLE_PEER_ACCESS | asc:635-641 |
| 3 | host 传 `dieId = myDev % 2` 进 kernel；streamid 按 die 分支（die0→0x7F45 / die1→0xBF45） | asc:654-672 |
| 4 | 跨卡通知 = SDMA flag-SQE 对（数据+8B 标志一次 doorbell，FIFO 保序）；NOTIFY_RECORD 跨卡弃用 | README 通知链定型 |

与本池（VMM 路径）的对应关系：①由 `ACL_MEM_P2P_HUGE` 物理页天然满足；②仅 IPC
import 需要，VMM 免 EPA（见 §2 池层证据链）；③④已被 2-rank 实现继承，4-rank
修正点见 §3.5。

### 3.2 编址

4 段对称 GVA：`gva + symSize * rank`，rank ∈ {0..3}；symSize 2 的幂约束不变（掩码算术依赖）。

### 3.3 池层补强（引用方案文档，仅列协同层相关点）

- 4 段对称映射 + 槽位预检：4-rank 拓扑下每卡 4/8，富余；不可行拓扑确定性失败
  （方案文档目标 4 语义）；
- 跨卡 import 按 §3.1 配方分派：本池 VMM 路径免 EPA（04_vmm_cross 实测），
  IPC 参照路径才需双向 enable。

### 3.4 邮箱协议 4-rank 化（核心改造）

- **保留区 48KB → 128KB**：内含 N-1 = 3 个**按对端划分的到达环**（每环 1024×8B 戳 +
  1024×32B 邮件，SPSC 语义原样复用 2-rank 已验证原语）+ 本端请求环/quiet 环不变；
- **epoch 路由**：`dst` 段基 → `peerIdx = (dstSeg - poolBaseGva) / symSize` → 写对端对应环；
  `HybmSdmaOrchParam` 增 `poolBaseGva`/`rank` 字段（wire 变更，KVER 同步 bump）；
- **wait 语义**：推荐双提供——`wait(gva, peer)`（显式按序消费）与 `wait_any(gva)`
  （轮询归并，保 RDMADemo 同构的无参消费习惯）；signal 免改（dst 已含路由信息）、
  quiet 免改（本端自写）；
- **布局单一事实源**：`smem_shm_sdma_layout.h` 三端（设备头/契约/UT）同步，UT 布局锁更新。

### 3.5 streamid 推导（xcclpp 原样配方，定案）

kernel 内 `(devId == 0) ? 0x7F45 : 0xBF45` 的分支依据是 **die 号**（每卡 CP1 内
die0/die1 各取一值）。当前 `epochParam_.devId` 误传全量设备号——2-rank 时
dev0/dev1 恰与 die 号重合而掩盖，4-rank 下 dev2（卡1 die0）将错拿 die1 值。
修正：host 传 `dieId_`（orchestrator 已有 `dieId_ = devId % 2` 字段），kernel 按
dieId 分支；kernel 内 HAL 调用（doorbell/查询）本就以 die 号为语义（A.3），
传 dieId 同时修正其取值。

### 3.6 版本纪律

wire/布局变更 → `MF_SDMA_ORCH_KVER` bump；已部署旧版本 kernel 与新库不混载
（CP1 按文件名 dlopen 缓存，版本号即缓存键）。

## 4. 实施计划（每阶段独立验收，走 openspec change）

| 阶段 | 内容 | 验收标准 |
|---|---|---|
| **P1 池层 4-rank** | 4 段映射 + 跨卡 import（VMM 免 EPA）+ 槽位预检 | 4 进程建池 + D2D 全互连 + 槽位不超限 + 复跑 |
| **P2 邮箱多 peer 化** | 布局 128KB + KVER bump + epoch 路由 + dieId 修正 | UT 全绿 + 2-rank 回归 + 跨卡邮件用例 |
| **P3 示例与端到端** | 08 示例扩 4-rank（或新 09 示例） | 4-rank mismatch=0 + 干净退出 + 流水加速比记录 + 2-rank 回归 |

## 5. 风险

| # | 风险 | 应对 |
|---|---|---|
| R1 | epochParam 误传全量 devId（2-rank 恰好掩盖，4-rank 错路由） | P2 传 dieId 修正（§3.5），跨卡邮件用例回归 |
| R2 | 跨卡带宽 ~10–14 GB/s（die 对 17–19）低于 die 对，加速比受其主导 | P3 如实记录，不承诺与 2-rank 持平 |
| R3 | 多进程建池启动时序敏感（2-rank 已证） | 16_vmm_matrix 8 rank 并发建池实测无失败，利好；P1 验收含复跑 |
| R4 | 保留区扩容改变段尾布局，旧 kernel 混载风险 | KVER bump 纪律（同版本号不混载，§3.6） |

## 6. 开放问题（评审定）

1. wait API 形态：`wait(peer)` + `wait_any` 双提供（本文推荐）vs 仅 `wait_any`；
2. `HybmSdmaOrchParam` 扩字段的 wire 布局（尾部追加 vs 重排对齐）；
3. 08 示例直接扩展 vs 新开 09 示例。
