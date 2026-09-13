## Context

310P（Ascend310P3，Atlas 300I Duo）单卡双 die 2-rank，CANN 9.1.0。实测关键事实（编号沿用测试计划 F1-F3 与 MR !3 评论记录）：

- P2P 物理分区上限 **~4088MB（动态 4083~4091MB，随设备空闲波动）**：`MallocPhysical(P2P_HUGE/P2P_NORMAL)` 超限返 207001（F1）
- `ACL_HBM_MEM_HUGE` 从 AI 主池（~42GB）分配无此限制：4GB 全链路 PASS，带宽与 P2P_HUGE 无差异（读 19.00 / 写 17.93 GB/s，F2；16MB 块两轮对照 D2RD 15.3/15.4、RD2D 16.4/16.4）
- 枚举错位天然实验：旧自编号枚举 `ACL_MEM_P2P_HUGE`=值 4（CANN 真实语义 `ACL_HBM_MEM_HUGE`）——PR !1 全部实机验证事实上跑在 HBM_HUGE 上且全部通过；对齐后 P2P 路径首次真正执行即暴露 4088MB 上限
- die 间共享不依赖 P2P 语义：die 对互联走 HCCS 不占 P2P 槽位；`ImportFromShareableHandle` 内部 `EnableP2PWithOtherDevice` 仅跨卡 import 消耗槽位；import 链路（Export/Import/MapMem）对分配 attr 零依赖——32GB HBM_HUGE 池 die 间互相 import + 十方向校验全 0 实证
- 通用版 `benchmark/bm`：910B GVA 单实体路径，`GVA_SIZE=4GB` 硬编码、无池大小 CLI；310P 双实体双池与 copyType 语义分叉（pinned SVM VA 分类为 LOCAL_DEVICE）决定独立用例而非改造通用版

## Goals / Non-Goals

**Goals:**

- `aclrtMemAttr` 与 CANN 严格对齐，消除"名实不符"的语义漂移
- >4GB HBM 池可建（P2P 分区耗尽自动回退 HBM_HUGE），实测覆盖 8GB/32GB
- 310P 双池十方向带宽基准工具端到端可用，与通用版零耦合（共享头单向复用）

**Non-Goals:**

- 默认分配类型切换为 HBM_HUGE（die 间场景证据充分，但卡间能力与 CANN 未文档化风险待裁决，见 issue #10）
- 卡间（跨卡）HBM_HUGE import 验证（无实测证据）
- 8-rank 池化（受 P2P 槽位预算限制，见前一 change 的 Non-Goals）

## Decisions

| 决策 | 选择 | 依据 |
|------|------|------|
| 回退触发条件 | 仅 ret==207001 精确匹配 | 207001 是 P2P 分区 OOM 的专属语义；100000 等参数错回退无意义且掩盖根因 |
| 回退粒度 | 单次分配（每 slice）判定，smem_bm 整池单次分配天然全池一致 | 避免"前段 P2P 后段 HBM"的混合池（BAR 映射语义不一致） |
| P2P_HUGE 优先而非 HBM_HUGE 默认 | 维持 P2P 优先 + 回退 | `ACL_HBM_MEM_HUGE` 官方文档仅标注 350/A3/A2（310P3 属未文档化行为）；P2P_HUGE 为官方通用类型且是跨卡 BAR 可映射窗口（≤4G 保卡间能力，>4G 退化为 die 间） |
| 枚举修复方式 | 全量 18 项与 CANN acl_rt.h 逐项对齐 + 错位风险注释 | 部分对齐会再次埋雷；注释固化实测教训（曾名 P2P 实发 HBM） |
| 独立用例而非改造通用版 | 新增 `benchmark/bm_310p` 目录 | 双实体路由/copyType 分叉/pinned 语义与 910B 单实体 GVA 语义不兼容，合并会污染稳定工具；通用版零改动 |
| 共享头去重 | include 路径指向 `../bm`，通用版零改动 | `barrier_util.h`（176 行）/`common_def.h`（79 行）逐字重复，bm 侧曾有 sstream 隐性传递包含；不动 bm 是硬约束 |
| pinned host 方向 copyType | localHostPinned_ 状态位自动切换 L2G/G2L | pinned VA 落 SVM 统一窗口（~0x12c0_xxxx_xxxx）被 hybm_va_manager 按范围判为 LOCAL_DEVICE，host 侧类型会被拒 |

## Risks / Trade-offs

- **CANN 升级断供风险**：`ACL_HBM_MEM_HUGE` 在 310P3 上未文档化，升级后可能失效（回退链将整体失效）——issue #10 跟踪，探针矩阵可复测
- **共享机器测试污染**：实测中曾因并发用户作业误判"16GB 边界"（dev2/dev3 被占 ~39.7GB）——大池测试前必须 `npu-smi info` 确认 die 对空闲
- **32GB 上限未压满**：每 die AI 主池可分配 ~40GB，实测到 32GB 通过即止；更大池留待需要时验证
