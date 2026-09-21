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
| 传输 | 设备侧自驱：AICore kernel 内 `smem_shm_sdma_signal/wait/quiet`，AICPU epoch kernel 代投 SDMA SQE，host 零数据面 |
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
| 池层（4 段对称 GVA 空间） | ✅ 可行，需补跨卡使能 | 方案文档 rank 矩阵明确"两卡四 die 4 rank 全互联"为一期目标；槽位预算：每卡 2 rank × 2 对端 = **4/8**（16_vmm_matrix 实证每卡 8 槽上限，die 对 import 免费）；缺口：跨卡 import 前需**双向** `rtEnableP2P`（15_peer_needed ENABLE_BOTH 实证单向必败 507899；hcomm 同款：PXI 拓扑 → enable → DEFAULT import） |
| SDMA SQE 跨卡通路 | ⚠ 高可信，差一个探针 | SQE 配方出自 hcomm `AddOneMemcpySqeV2`（机制档案配方节），hcomm 在 310P 跨卡跑 AllReduceRing 大数据——同配方跨卡已被生产验证；但本库未自发过跨卡 SQE，streamid 跨卡取值未定案 → P0 探针一次定案 |
| 邮箱协议层 | ❌ 现设计不支持，需重设计 | 到达环为单生产者 FIFO：对端 epoch 用**其自身请求序号**戳我的到达环 `seq%1024` 槽，我的 wait 按本端消费游标对齐——多对端时序号互踩、跨对端无全局 FIFO。quiet 环为本端 epoch 自写（天然多对端安全）、请求环为本端单生产者（安全），均免改 |
| 部署/资源层 | ✅ 免改 | 每卡独立设备 OS，自动部署按本 rank devId 落各自卡；meta 区每进程一份；监督线程/epoch 续命模型不变 |

## 3. 设计

### 3.1 编址

4 段对称 GVA：`gva + symSize * rank`，rank ∈ {0..3}；symSize 2 的幂约束不变（掩码算术依赖）。

### 3.2 池层补强（引用方案文档，仅列协同层相关点）

- 建池时按 `aclrtGetDevicesTopo` 分派：die 对（HCCS）跳过 enable；跨卡（PXI）双向
  `rtEnableP2P` 后 DEFAULT import——hcomm 模式照搬；
- 槽位预检：4-rank 拓扑下每卡 4/8，富余；不可行拓扑确定性失败（方案文档目标 4 语义）。

### 3.3 邮箱协议 4-rank 化（核心改造）

- **保留区 48KB → 128KB**：内含 N-1 = 3 个**按对端划分的到达环**（每环 1024×8B 戳 +
  1024×32B 邮件，SPSC 语义原样复用 2-rank 已验证原语）+ 本端请求环/quiet 环不变；
- **epoch 路由**：`dst` 段基 → `peerIdx = (dstSeg - poolBaseGva) / symSize` → 写对端对应环；
  `HybmSdmaOrchParam` 增 `poolBaseGva`/`rank` 字段（wire 变更，KVER 同步 bump）；
- **wait 语义**：推荐双提供——`wait(gva, peer)`（显式按序消费）与 `wait_any(gva)`
  （轮询归并，保 RDMADemo 同构的无参消费习惯）；signal 免改（dst 已含路由信息）、
  quiet 免改（本端自写）；
- **布局单一事实源**：`smem_shm_sdma_layout.h` 三端（设备头/契约/UT）同步，UT 布局锁更新。

### 3.4 streamid 推导

`(devId == 0) ? 0x7F45 : 0xBF45` → 按 `dieId = devId % 2` 映射（每卡 CP1 内 die0/die1
各取一值）；跨卡取值由 P0 探针定案后固化。

### 3.5 版本纪律

wire/布局变更 → `MF_SDMA_ORCH_KVER` bump；已部署旧版本 kernel 与新库不混载
（CP1 按文件名 dlopen 缓存，版本号即缓存键）。

## 4. 实施计划（每阶段独立验收，走 openspec change）

| 阶段 | 内容 | 验收标准 |
|---|---|---|
| **P0 跨卡 SQE 探针** | 4 进程池（复用 P1 前置的临时建池脚本亦可）+ dev0→dev2 单封邮件探针，同时定案 streamid 跨卡取值 | 跨卡邮件数据落地 + 戳序正确；**不通过则协同层 4-rank 止步于此**，降级 host 搬运（池层成果保留） |
| **P1 池层 4-rank** | topo 探测分派 + 双向 rtEnableP2P + 4 段映射 | 4 进程建池成功 + host async D2D 全互连校验通过 + 槽位不超限 |
| **P2 邮箱多 peer 化** | 布局 128KB + KVER bump + epoch 路由 + wait API + UT 布局锁 | UT 全绿 + 2-rank e2e 回归通过（mismatch=0） |
| **P3 示例与端到端** | 08 示例扩 4-rank（或新 09 示例） | 4-rank mismatch=0 + 干净退出 + 流水加速比记录 + 2-rank 回归 |

## 5. 风险

| # | 风险 | 应对 |
|---|---|---|
| R1 | 跨卡 SQE 未自证（hcomm 证据是间接的） | P0 探针先行，一票否决制 |
| R2 | streamid 跨卡取值未知 | P0 一并定案 |
| R3 | 跨卡带宽 ~10–14 GB/s（die 对 17–19，03/16 实测）低于 die 对，流水加速比上限受跨卡 hop 主导 | P3 如实记录加速比，不承诺与 2-rank 持平 |
| R4 | 多进程建池启动时序敏感（2-rank 已证） | 16_vmm_matrix 8 rank 并发建池实测无失败，利好；P1 验收含复跑稳定性 |
| R5 | 保留区扩容改变段尾布局，旧 kernel 混载风险 | KVER bump 纪律（同版本号不混载，3.5） |

## 6. 开放问题（评审定）

1. wait API 形态：`wait(peer)` + `wait_any` 双提供（本文推荐）vs 仅 `wait_any`；
2. `HybmSdmaOrchParam` 扩字段的 wire 布局（尾部追加 vs 重排对齐）；
3. 08 示例直接扩展 vs 新开 09 示例。
