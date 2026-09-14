# Design: add-310p-cross-card-validation

## Context

上游 xcclpp 协同模式的跨卡验证基于 demo 自分配内存（P2P policy + 跨卡 import 在 demo 脚本内闭环）；本仓把内存面换成 AclVmm 池 GVA 后，"跨卡导入 VA 是否可作 SQE src/dst"是上游没替我们验过的最大未知点（E0b 跨卡变体存在的意义）。同时跨卡导入隐式建立的 P2P 状态在产品清理路径中无拆除动作（D 态挂死风险，探针 03/04/10/14 四次实证）。

## Goals / Non-Goals

- Goals：D 态拆除进产品代码；跨卡验收矩阵推进到 08 端到端；两份 spec 消除超卖。
- Non-Goals：8-rank 跨卡（P2P 槽位预算另行立项）；跨节点（双机）场景；Reduce SQE / CQE / host 纯搬运（既有 TODO-0/4/5 不动）；OPP 产品化部署（TODO-1 不动）。

## Decisions

| 编号 | 决策 | 理由 |
|---|---|---|
| D1 | D 态拆除挂在**段内**：`HybmAclVmmSegment` 维护跨卡 peer 计数，`UnmapOne` 成功路径递减、归零即拆；`UnReserve` 兜底扫描剩余 | P2P 状态生命周期与导入句柄严格一一对应，段是唯一知情者；全局 uninit 钩子拿不到 rank→devId 映射 |
| D2 | 拆除走 `DlAclApi::AclrtDeviceDisablePeerAccess`（镜像既有 Enable 符号封装：typedef + DL_LOAD_SYM + inline wrapper + unload 置空） | 不引入 rt 内部符号，dlopen 失败安全降级（符号缺失返回 BM_UNDER_API_UNLOAD 并记 ERROR） |
| D3 | 07/08 的 rank→devId 映射用环境变量参数化（`RANK_DEV_LIST`），缺省值保持 dev0/dev1 die 对 | 向后兼容：既有 README 命令与验收记录零变化；跨卡是显式 opt-in |
| D4 | E0b 跨卡变体从 git 历史 017802a0 恢复 `probe_e0b.cc`，仅参数化 dev 映射，产物留在未跟踪备份目录 | 探针是过程产物不进产品 PR（既有纪律）；恢复而非重写保证与 E0b 验收记录可对照 |
| D5 | 验收顺序 topo → 07 → E0b → 08 | 先排除机器级 P2P 劣化（上游机器 socket1 曾劣化 80×，报障平台侧），再逐层上收：池建链 → SMMU 可译性 → 端到端 |
| D6 | 跨卡运行前两卡各自跑 deployer（作用域=卡，X9 实测跨卡文件系统独立） | 08 协同 kernel 部署粒度是卡；同卡单次部署的结论不覆盖跨卡 |

## Risks / Trade-offs

- [本机跨卡 P2P 链路劣化] → topo 探针复核门（步骤 0），带宽异常即止并按上游先例报障，不在坏链路上排障。
- [P2P 槽位预算] → 跨卡 import 隐式消耗槽位；2-rank 每对端 1 槽远低于上限（8-rank 受限是既有结论，本变更不碰）。
- [F6 跨卡导出必须 P2P policy] → 已满足：`FillMemProp` 固定 `ACL_MEM_P2P_HUGE`（产品代码现状，无需改动）。
- [DisablePeerAccess 失败阻断清理] → 拆除失败仅记 ERROR 不返回错误（清理路径必须推进，不能因拆不掉卡死其余 unmap）。

## Revision Impact

- `dl_acl_api.{h,cpp}`：+1 符号（typedef/成员/加载/封装/置空五处镜像 Enable）。
- `hybm_acl_vmm_segment.{h,cpp}`：+2 个私有容器（slotAddr→peerDevId、peerDevId→活跃计数），ImportOne 跨卡分支登记、UnmapOne 成功递减、UnReserve 兜底。
- UT：stub so 增加 DisablePeerAccess 计数符号，新增跨卡拆除用例（全量拆/die对不拆/部分不拆/uninit兜底）。
- 示例 07/08：dev 映射读环境变量 + README 跨卡段。
