# Proposal: add-310p-cross-card-validation

## Why

上游 xcclpp `aicpu-aicore-310p3` 协同模式的主场景就是**跨卡**（README 首行："AICPU 编排 SDMA 跨卡搬运"；mode13/mode14/带宽表均在跨卡上验收），而本仓迁移后的全部实机验收锚定在 dev0↔dev1 同卡 die 对。当前状态：

1. **池层跨卡路径已写但从未实机验证**：`hybm_acl_vmm_segment.cpp` 的 `ImportOne` 已按 `IsDiePair` 分派跨卡直连导入分支，但同事的 VMM 池验收与 07/08 示例全部为 die 对。
2. **D 态拆除缺失（产品代码零防护）**：跨卡导入隐式建立的双向 P2P 状态若不在设备复位前显式拆除（`aclrtDeviceDisablePeerAccess`），进程退出挂 D 态——探针 03/04/10/14 四次实证；`src/` 下无任何 DisablePeerAccess 调用。die 对不走 P2P 故此前未爆。
3. **规格超卖**：`hybm/acl-vmm-hbm-pool` spec 已声明"跨卡场景 SHALL 在本进程上下文直接导入"但无实测场景支撑；`smem/310p-shm-overlap` spec 无范围声明（未区分 die 对已验收/跨卡未验收）。

## What Changes

- **产品代码（唯一功能性改动，几行量级）**：`dl_acl_api` 新增 `aclrtDeviceDisablePeerAccess` 符号封装；`HybmAclVmmSegment` 增加跨卡 peer 跟踪，句柄全量释放时显式拆除 P2P 状态，uninit 路径兜底扫描——消除 D 态挂死。
- **示例跨卡跑法**：07/08 的 rank→devId 映射参数化（环境变量，默认值保持 die 对不变）+ README 补跨卡运行说明。
- **E0b 跨卡变体**：从 git 历史（017802a0）恢复 `probe_e0b.cc`，dev 映射参数化后实机验证"SMMU 可译跨卡导入的池 GVA 作 SQE src/dst"——上游 demo 用自分配内存验的跨卡，换成池 GVA 后的可达性上游没验过。
- **实机验收矩阵推进**：topo 探针复核（排除机器级 P2P 劣化，上游机器 socket1 曾劣化 80×）→ 07 跨卡建池 → E0b 跨卡 → 08 跨卡三阶段（含两卡各自部署编排 kernel）。
- **规格与文档对齐**：`hybm/acl-vmm-hbm-pool` 增补"P2P 状态拆除纪律"需求；`smem/310p-shm-overlap` 增补"跨卡 2-rank 部署场景"需求；docs/310p 三档案补跨卡实测结论。

## Capabilities

### Modified

- `hybm/acl-vmm-hbm-pool`: 新增"跨卡导入的 P2P 状态拆除纪律（D 态防挂）"需求——句柄全量释放时显式拆除、失败记 ERROR、die 对不拆除、uninit 兜底。
- `smem/310p-shm-overlap`: 新增"跨卡 2-rank 部署场景"需求——dieId=devId%2 本地编址跨卡成立、跨卡导入池 GVA 可作 SQE src/dst、八步闭环/流水行为与 die 对一致、两卡各自部署 kernel。

## Impact

- 代码：`src/hybm/csrc/under_api/dl_acl_api.{h,cpp}`（新增符号）、`src/hybm/csrc/mm/hybm_acl_vmm_segment.{h,cpp}`（跟踪与拆除）、`test/ut/testcase/hybm/mm/hybm_acl_vmm_segment_test.cpp`（新用例）。
- 示例：`examples/memory_pool/01_basic/07_310p_die_pair_pool/`、`examples/memory_pool/01_basic/08_310p_aicore_aicpu_overlap/`（映射参数化 + README）。
- 探针（不进产品 PR，留未跟踪备份目录）：`310p-capability_matrix/17_aicore_aicpu_overlap/probe_e0b.cc` 跨卡变体。
- 文档：`docs/310p/平台事实.md`、`docs/310p/部署档案.md`、`docs/310p/机制档案.md`、`310p-aicore-aicpu-开发进展.md`（跨卡结论与 TODO-2 去向标注）。
- 不改 wire 契约、不改元数据布局、不改默认行为（die 对路径与关闭开关路径零变化）。
