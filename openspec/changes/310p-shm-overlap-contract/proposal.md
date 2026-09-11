# 310P SHM 协同模式行为契约与知识沉淀

> 变更类型：文档/规范（spec 捕获 + 知识归置），不改任何产品代码运行时行为

## Why

xcclpp aicpu-aicore-310p3 协同模式迁移（28 提交，+6199 行）已完成实机验收（八步闭环
mismatch=0、mode14 流水加速比 8.02x），但其行为契约（开关门控、冒烟探测、wire 契约、
回归不变式）只存在于 568 行的《310p-aicore-aicpu-开发进展.md》开发日志中——该文档
混合了平台配方、设计决策、进展记录、踩坑记录、TODO 五种生命周期不同的知识，后续
改动者无法据此判断"哪些行为不能破坏"。同时：PR 清理提交（ec1ee588）在文档中留下
四处编辑残留；长寿命平台知识（SQE 配方、部署规则、F1-F20 实机事实）散落在仓库根
目录的三份过程文档里，新读者难以分辨"该读哪份"；两次 310P 大工作中隐式形成的
工作法没有显式化，PR 清理无 checklist 导致残留顾此失彼。

## What Changes

- 新增 `310p-shm-overlap` 能力 spec：将协同模式**已实机验收的现状**固化为可回归
  验证的规范要求——开关门控与不静默降级、冒烟探测门控、wire 契约布局锁、传输
  准备单例生命周期、overlap host API 参数与错误语义、开关关闭时存量路径零影响
- 修复 ec1ee588 清理提交留下的四处文档残留：《开发进展》0.1-D 表格断裂与重复行、
  08 示例 README 重复引用、编排 kernel 头注释指向不存在的"本目录 README"
- 新建 `docs/310p/` 技术档案：机制配方（SQE/缓存纪律/通知链）、部署链规则、平台
  实机事实三类长寿命知识从过程文档提炼成档案；三份根目录过程文档原地冻结并加
  状态指针头（产品代码注释大量引用其路径，不可移动）
- 新建平台适配开发工作法文档：将"上游方案 → 实机探测 → 开发计划 → 可行性关卡 →
  产品化 → 示例验收 → UT 回归 → 文档同步 → PR 清理"流程显式化，含 PR 清理
  checklist 与产物形态规范（F-表/D-表/判据表）

## Capabilities

### New Capabilities

- `310p-shm-overlap`: 310P SHM API AICore×AICPU 协同模式的行为契约——环境开关门控
  （`MF_310P_SHM_OVERLAP` 默认关闭）、传输准备（CP 流/工作区/编排 kernel 加载/
  冒烟探测）、overlap host API 参数与错误语义、wire 契约与工作区布局不变式、
  开关关闭时存量路径行为零变化

### Modified Capabilities

（无——`openspec/specs/` 当前为空，无既有能力的需求变更）

## Impact

- **文档**：`310p-aicore-aicpu-开发进展.md`（修复残留 + 头部状态指针）、
  `310p-2rank-vmm-开发计划.md`（头部状态指针）、`memfabric-310p-内存池化方案.md`
  （头部状态指针）、`examples/memory_pool/01_basic/08_310p_aicore_aicpu_overlap/README.md`
  （修重复引用）、`docs/310p/`（新建档案）、`docs/平台适配开发工作法.md`（新建）
- **代码**：仅 `src/hybm/ops/hybm_kernel/hybm_overlap_kernel.cc` 头注释一处失效
  引用指向修正，无行为变化
- **回归**：本变更全部为文档/spec 工作，不触发任何构建或 UT 变化
