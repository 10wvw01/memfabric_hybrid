# 310P3 单卡双 Die MM + MemFabric AllReduce 融合算子｜项目唯一入口

> **权威研发主线：`dev/mainline`。** 本文件及本分支下的 `PROJECT_CONTROL.md` 是当前状态的唯一权威入口；其他 `feat/`、`perf/`、`diag/`、`docs/` 分支为历史/取证/待评审资产，不得按名称推断最新状态。更新日：2026-10-27（Asia/Taipei）。
>
> **请新 ChatGPT 会话、OpenCode 及新接手开发人员先读这里，再读下列链接，不要仅凭聊天记忆或旧版 PLAN.md 制定计划。**

## 从这里恢复上下文（推荐阅读顺序）

1. [项目控制台：当前全项目状态、技术合同和决策](csrc/_310P/memfabric_mm_ar_v2/docs/PROJECT_CONTROL.md)——全项目权威状态。
2. [大M Prefill TTFT实验总账：目标、流程、任务清单、状态、证据和进度日志](csrc/_310P/memfabric_mm_ar_v2/docs/LARGE_M_PREFILL_TTFT_EXPERIMENT_LEDGER.md)——**本轮大M实验唯一进度追踪入口**；新会话和OpenCode开始A1/L3之前必须读取。
3. [工作分支与合入治理](csrc/_310P/memfabric_mm_ar_v2/docs/BRANCH_GOVERNANCE.md)——唯一主线、实验分支归档、代码来源与禁止事项。
4. [统一性能基线与证据索引](csrc/_310P/memfabric_mm_ar_v2/docs/PERFORMANCE_BASELINES.md)——严格区分单算子 V2 内部收益、完整 V2 vs Stock、模型实际传导。
5. [项目决策记录 ADR](csrc/_310P/memfabric_mm_ar_v2/docs/DECISIONS.md)——ADR-013 废止重建 Runner；ADR-015 锁定TTFT；ADR-016 主线管方向。
6. [正式 V2 融合工程及薄接入](csrc/_310P/memfabric_mm_ar_v2/docs/README.md)——只改 MM+MemFabric 算子、选层与必要的 Stock 回退。
7. [DFlash K7 Stock/V2四象限历史实机准入](csrc/_310P/memfabric_mm_ar_v2/docs/DFLASH_STOCK_V2_R1_GO_GATE_20261025.md)——历史Stock/V2/DFlash取证，不代替当前大M实验总账。
8. [工程准入与310P3实机交接门禁](csrc/_310P/memfabric_mm_ar_v2/docs/ENGINEERING_GATES.md)——区分纯CPU、Kernel、发布级验证；锁定源码/二进制/装载三重身份。
9. [310P 融合算子工程入口](csrc/_310P/memfabric_mm_ar_v2/docs/README.md) 与 [目录内自动执行约束](csrc/_310P/memfabric_mm_ar_v2/AGENTS.md)。

## 不可偏离的当前目标

> **最新权威研发方向（2026-10-27，ADR-015/ADR-016）：主线管方向，工作分支管实施。** 大M以真实Qwen3.6 **Prefill TTFT**为最终指标，L1唯一完整基线是原生FP16 Stock MM+HCCL AllReduce，优化聚焦每Rank实际命中的40个`o_proj/out_proj`目标站点双Rank设备关键路径。阶段顺序为 **L1/L2归因与调优 → L3真实无Profiler Prefill Step可重复稳定正向收益 → L4原始4096/2048/TP2/并发10/50请求客户端TTFT**；L3未达到稳定正收益则继续优化，不启动L4长输出验收。L3设备Step尚缺可审计无Profiler采集与校准，不能以4096/1客户端时延代替。正式方向、[L1～L4历史证据复用及实验合同](csrc/_310P/memfabric_mm_ar_v2/docs/L1_L4_HISTORICAL_REUSE_AND_TTFT_MINIMAL_CAMPAIGN_20261027.md)、[ADR](csrc/_310P/memfabric_mm_ar_v2/docs/DECISIONS.md)保存在本主线；[A1 PR #9]([removed-link]) Draft只负责尚未验收的实验实现。当前没有因此产生新设备测量/合入Kernel，Stock默认及全部G0/安全停止条件不变。

- **Ascend 310P3 单卡双 Die / TP=2**；同 Kernel 完成 FP16 MM + MemFabric SDMA AllReduce + Add，Stock MM+HCCL 是唯一有效的完整算子最终性能对照。
- **大 M 必须持续向硬件可证实的极限优化**，不再每轮讨论“值不值得做”；每次输出完整算子 vs Stock，以及 Qwen 集成真实收益及差距原因。
- **小 M 专攻单 Kernel 带来的固定开销消减**，覆盖普通 Decode 和 DFlash 投机验证的实际 M；旧 T1 M=10 已经严格 NO-GO，不能复活其错误的 +20% 结论。
- **与 Qwen3.6 / DFlash 投机推理集成同步演进**。指定外部只读参照：`HaonanWu1/vllm-ascend:dev_v24/feat-310p-dflash`；**绝不向该仓库提交代码、推送分支或发 PR**；全部工作只落在我们的 `USER/vllm-ascend`。
- 生产/Serving **Stock 默认不变**。任何未完成实机正确性、路由真实性、Graph/生命周期与稳定性验证的融合候选不能启用为默认。

## 项目状态速览

| 事项 | 状态 |
|---|---|
| D4 Tile2048 正式大 M Kernel | V2 内部相对前版本 +6.26%（不是相对 Stock），保留基线 |
| 4096 输入/2048 输出/TP2/batch10/Eager 模型 | SEL-01：V2 vs Stock 输出速率中位 +1.12%，方向不一致、未达目标 |
| 小 M（M=10） | F1-C：T1 425.2µs vs 原生 Stock 308.2µs，T1 等效速率约 −27.5%；NO-GO |
| F2 Chunk 缩小 | 256、512 均不优于1024；保留 D4 Chunk1024 / Overlap1 |
| MemFabric 长稳与 Graph | 507014 Gate E 尚未根治；Graph 不能仅依据输出正确就宣称真正命中融合 |
| DFlash/Stock 集成 | 保留 Stock/DFlash 基线与 R1-B 历史证据（当时指定组合失败）；**重建 V2 Runner/DFlash 兼容体系正式废止**。融合只做现有模型路径的 LinearMethod 薄接入与 Stock 回退，实际可运行环境和融合命中须分别验证 |
| U1 统一诊断工具 | 已纳入 GitHub 纯CPU源码/合成数据合同回归；**U1真实设备运行仍NOT_RUN**，不是研发主目标 |
| 统一主线 | **本分支**；已有[专用CI]([removed-link])：治理6/6 + V2源码合同54/54 PASS；正式Kernel未因治理改变 |

## 每次新会话请这样开始

> 阅读 `USER/vllm-ascend` 的 `dev/mainline:PROJECT_310P_MEMFABRIC.md` 和其链接的 `PROJECT_CONTROL.md`、`LARGE_M_PREFILL_TTFT_EXPERIMENT_LEDGER.md`、`DECISIONS.md`、`PERFORMANCE_BASELINES.md`、`BRANCH_GOVERNANCE.md`；核查远程主线 HEAD 和未合入变更。**先报告当前有效结论和下一步动作，再继续我本次的需求。禁止把历史实验报告当作新决策，禁止改动 HaonanWu1 仓库。** 如文档与代码冲突，用源码和新证据定位冲突，并同步更新控制台，不能自行静默改变方向。

## 变更纪律

- 每个有实质意义的工作：在本主线或由其派生的短命任务分支完成，审查后回到本主线；不要把新的实验分支当长期主线。
- 每次完成实机、性能决策、接口变更或外部 DFlash 基线变化，**必须同一批提交更新** `PROJECT_CONTROL.md`，必要时补 ADR/性能报告/分支表。未更新项目控制台的任务视为**未完成归档**。
- 不把文档的“预计收益”“静态通过”“本地测试通过”写成实机 PASS；环境 SO、KVER11 和运行时真正命中的 Kernel 都须有证据。

> **2026-10-26 纠偏硬规则：** `work/310p-v2-r2-implementation-20261025`、PR #7、R2-00～21、T01～T31/46项通用Runner兼容矩阵全部废止，不得从中合入 Runner、DFlash、Hybrid ModelState、GDN/采样/多KV重建代码。唯一目标：保持可运行的Stock+DFlash框架不变，仅替换合格RowParallelLinear的 MM+HCCL 为 V2 MM+MemFabric，真实设备测试按G0/安全门禁执行。历史R1-B失败只属于当时的冻结配置，不得断言其它已可运行Stock环境也失败。

### 2026-10-26｜新增四卡动态择优实机原则（ADR-014）

310P3服务器共有4张卡，实机应优先选**健康且独占的一张双Die卡**；卡级Alarm/占用则记录并换卡重新G0，运行中507014/507899/IB/KFC/错算则STOP保全现场，不自动轮测四卡。换卡需核查`ASCEND_RT_VISIBLE_DEVICES`逻辑/物理映射、双Host SO的SetAccess物理DeviceID修复和KVER11；**KFC由MemFabric在获准首次建池时自动按需部署，G0只读阶段不预部署**。同一Stock/V2 ABBA不得跨卡混合。详见[310P3四卡动态选卡与KFC标准](csrc/_310P/memfabric_mm_ar_v2/docs/310P3_四卡动态选卡与KFC自动部署准入规范_20261026.md)。未进行新实机测试。
