# 310P 协同模式精简为纯流水提交

> 变更类型：重构（API 收敛 + 死代码清理），实机行为语义保持（流水 chunks=1 等价覆盖原单块闭环）

## Why

协同模式合入后检视发现三类冗余：(1) 单 chunk（mode13）与多 chunk（mode14）两套提交
路径语义重叠——流水模式 chunks=1 即单块闭环，双 API/双 case/双转发层均为此冗余；
(2) fork 自 xcclpp 的 role1 编排方分支、6 个未用 HAL 弱符号与结构、GM doorbell 直写/
SQ 激活/不敲铃/ssid 覆盖等实验钩子在当前 host 栈（恒 role0 + doorbell 固定 HAL）下
为不可达死代码；(3) 契约头 role/roleRsv/arrivalAddr/readyAddr/reserved 字段随之闲置。
冗余显著抬升检视与后续维护成本。

## What Changes

- 编排 kernel 仅保留流水模式（SUBMIT_MODE_PIPELINE），域外 submitMode 拒绝（mainRet=950
  语义不变）；删除 mode13 case、role1 分支、未用弱符号/结构、全部实验钩子
- wire 契约 v2：删除 role/roleRsv/arrivalAddr/readyAddr/reserved，新增 chunks 字段，
  static_assert 布局锁重算；设备侧版本名 MF_OVERLAP_KVER 1→2（CP1 dlopen 缓存纪律）
- host API 收敛为单一提交接口：`hybm_sdma_submit(dst, flag, chunks)` 与
  `smem_shm_sdma_submit(handle, dst, flag, chunks)`（吸收 _pipelined 变体）；
  smem entry 层三方法收敛为一
- 初始化冒烟自证移除（就绪门控回归三步准备：CP 流/工作区/kernel 加载，端到端验证由 08 示例阶段①逐字校验承担）
- 08 示例改两阶段验收：①流水（8×128KB+逐字校验）②串行对照（chunks=1×8 轮+会合），
  掩盖量化口径不变
- UT 对齐新 API（chunks 边界/未就绪拒绝/无效 handle 用例不变）
- 08 示例 AICore kernel 同步合并策略: role2/3(单块)删除, 单块 = role4/5 + chunks=1
- 08 示例双 rank 会合与栅栏改用 MPI（mpirun 拉起 + MPI_Barrier，替代 /tmp 文件轮询；
  编译/启动需同一套 openmpi 安装，串行对照基线口径变化已在 README 注明）

## Capabilities

### Modified

- `smem/310p-shm-overlap`: overlap host API 参数与错误语义（单一提交接口带 chunks）
