# MM+MemFabric AR 工程内专用开发助手约束（310P）

> **必须先阅读** 自有仓库根目录 `PROJECT_310P_MEMFABRIC.md` 以及本工程 `docs/PROJECT_CONTROL.md`、`docs/BRANCH_GOVERNANCE.md`、`docs/PERFORMANCE_BASELINES.md`、`docs/DECISIONS.md`。优先遵守仓库根的通用`AGENTS.md`，本文件只补充310P定制算子的特有研发规则。
>
> GitHub项目主线固定 `USER/vllm-ascend:dev/mainline`；除用户授权的短期任务分支外不得再创建常驻分叉控制线。外部 `HaonanWu1/vllm-ascend` 只能读，**禁止向其提交/推送/PR**。

## 你需要先回答的五个问题

1. 现在主线HEAD是什么、代码改动与`PROJECT_CONTROL.md`是否一致？若不同，先解释差异。
2. 新需求是大M正式Kernel优化、小M同Kernel固定开销优化、DFlash薄适配还是发布级稳定性？**不得改成一般MoE/GDN/Attention全模型优化**。
3. 本轮性能比较对象是**V2内部**、**当前完整V2 vs原生FP16 Stock MM+HCCL**，还是**Qwen Serving Stock vs融合**？不能混算。
4. 上次实机是否保留原始Rank0/Rank1日志、SO/KVER/设备物理映射、Stream/Graph路由证据？缺证据写UNKNOWN或Blocked，不能宣称PASS。
5. 本轮完成后如何更新主线项目控制台、ADR、性能基线与短分支合入状态？

## 工程安全与门禁

- 除非用户明确批准，**Stock维持默认**；不要擅自打开V1 Decode、尚未验收的小M新核、Graph大M融合。大M D4 Tile2048作为参考实现，但必须测最新**完整算子相对Stock**。
- 310P3 **单卡双Die / TP=2**，K/N=2048、FP16等按验证形状。修改Kernel/Host/协议时至少两轮静态深审，准入必须核对真实装载的Host/Device SO、KVER11和物理设备号修复。
- 不得删除Cache Clean、signal/wait、IB同步、MemFabric credit或MTE3首写防护以创造假性能。跑507014/507899/协议错误/bit-exact失败要停下留双Rank日志。
- Graph成功必须有真实融合Kernel/路由命中和replay证据；若仅输出正确但实际走Stock，不计融合通过。
- 普通模型端到端严格使用input4096/output2048/TP2/batch10/Eager、50请求；投机DFlash需使用相同模式Stock对比，跟踪verify宽度、物理M、接受率、Graph实际模式。
- OpenCode在NPU服务器实机执行前，必须完成G0→CPU合同→短Smoke→正式性能；实验结束后恢复和记录Host/Device SO SHA。不能因为项目主线中存在测试脚本就宣称实机可直接GO。
- 所有新增项目完成记录必须入 `docs/PROJECT_CONTROL.md`（current state）、`docs/PERFORMANCE_BASELINES.md`（性能新证据）、`docs/DECISIONS.md`（新决策）；**旧PLAN.md、历史报告只读，不再是活跃路线图**。

## 代码目录

- `src/op_kernel/mf310p_v2_kernel.asc`：正式算子；
- `src/op_kernel/mf310p_v2_diagnostics.asc`：诊断核；
- `src/op_host/`：Host公共API、诊断ABI；
- `src/torch_adapter/`：PyTorch Op与Serving生命周期；
- `test/`：UT、perf、TP2与真实模型验证；
- `docs/`：此项目控制台、证据索引和各轮报告；不要往`src/`混入仅诊断的代码。

## 交接摘要模板

`主线HEAD / 本轮变更 / 已实测事实 / 版本依赖 / 当前阻塞 / 下轮需要跑的最小实验 / 恢复与回退 / 更新文档Commit`。

不因新聊天丢失这些约束：读库然后执行，不能凭聊天记忆“补全”失去的历史证据。
