# 09_310p_aicore_aicpu_sdma_4rank：310P 双卡四 die 4-rank 设备发起 SDMA 协同（多 AICore AllReduce）

## 场景

2 张 Atlas 300I Duo（dev0/dev1 = 卡0，dev2/dev3 = 卡1）4-rank SHM 对称内存池上，
演示**4-rank 设备发起计算-通信协同**（08 的 2-rank 版本见同目录 08 示例）：

- 4 rank 同时运行 AICore kernel：每 rank 向 3 个对端 signal 投递搬运请求
  （跨 die 走 HCCS，跨卡走 P2P+PCIe）
- 各 rank 的 AICPU epoch kernel 代投 SDMA SQE：邮件/到达戳/完成戳/数据
  **全部经 SDMA SQE 投递**（AICPU 对对端内存零直写——跨卡直写实测不返回，
  设备侧 CPU 核无跨卡 store 通路），邮箱队列**按对端 × AICore block 划分**
  （3 对端 × 16 队列槽位 = 48 队列，对齐 hcomm 规模；block 0..7 在用、8..15 预留，
  多发送方/多 block/多目的地并发互不干扰）
- 接收方 `wait_peer` 显式按对端收件（`m.peer` 标来源）；host 零数据面
  （仅建池/起 kernel/收结果）

## 验证内容（4-rank 协同协议的核心风险点）

- **多 AICore AllReduce**：`<<<8>>>` 起 8 个 AICore block，每 block 独立流水
  （无块间同步）——填本 block 1MB 分片 → 发 3 对端（走独占队列 (对端, 本 block)，
  算完即发不等其它 block）→ 收 3 对端同列分片（block 列对齐）→ 规约（u32 SUM）。
  验证点：
  - 8 block 并发 signal/wait 的队列隔离（48 队列编码双射的正确性）
  - 通信与计算在 block 间重叠（各 block 流水独立，无聚合等待）
  - AllReduce 语义：每 rank 结果 = Σ 四 rank 贡献（host D2H 8MB 逐元素校验，
    pattern 含源 rank 可溯源）；规约 op 为 u32 SUM（fp32 引擎内联 reduce 已由
    08 ⑤ 的 SQE opcode 验证，协议同构）

kernel 侧异常（wait 超时/溢出、路由非法、拒投）经**状态字**（段内 60MB 处，
编码含对端/错误码）显式暴露，host D2H 读回解码打印。

## 使用能力（设备侧自驱）

`smem_shm_create`（SDMA 位，310P 上协同自动启用）→ `smem_shm_sdma_get_workspace`
（就绪判定）→ AICore kernel 内 `smem_shm_sdma_ctx(gva)` 入口一次预计算（自动携带
本 block 号 `GetBlockIdx()`）→ `smem_shm_sdma_signal_at`（dst 自动路由到
(对端, 本 block) 独占队列，单 block 环境自动用 block 0 队列，既有用例零改动）/
`smem_shm_sdma_wait_peer_at`（显式按对端，收 (对端, 本 block) 子环，`m.peer` 标来源）/
`smem_shm_sdma_quiet_at`（逐发送队列末封判定）→ `smem_shm_destroy`。

## 规模建议

- world_size=4 固定（mpirun -np 4 拉起；devId = rank，即 dev0/1+dev2/3）
- 拓扑：2×Duo（每卡 2 rank）或 4 卡各 1 rank——每卡 P2P 槽位预算 4/8、3/8 均通过
  （库内拓扑预检，超限建池显式拒绝 `SM_NO_RESOURCES`）
- 单封长度 ≤ 4GB（SQE length 为 u32 字段，1GB 单封同引擎实测通过）；len 须 4 字节
  对齐；多封分块是接收侧流水粒度的调优参数，非硬件要求
- 邮箱队列按对端×队列槽位划分（每对端 16 槽位共 48 队列，对齐 hcomm MAX_QUE_NUM=48；
  队列长度 64 槽/队列亦对齐 hcomm HCCL_MSG_CNT=64）：每队列请求环深 64（单队列满环
  背压不影响其他队列）、到达子环深 512；最多 8 个 AICore block 并发（`<<<N>>>` N ≤ 8，
  block 0..7 用对应槽位，8..15 预留冗余）
- 每 rank 段末尾 2MB 为邮箱保留区（memfabric 托管，勿存放用户数据）
- 本示例每 rank 物理贡献 64MB（数据区 40MB：贡献 8MB + 接收 24MB + 结果 8MB）

## 必要条件

- 2 张 Atlas 300I Duo（Ascend310P3）+ CANN ≥ 9.1
- **空闲设备选择**：若 dev0/1 被占用，`ASCEND_RT_VISIBLE_DEVICES=2,3,4,5`
  重映射后本示例照常运行（池窗口固定基址 + 物理号授权换算已适配重映射）
- MPI（4 rank 拉起与栅栏；编译与启动必须同一套，混用会段错误：自装
  `/usr/local/openmpi` 全套或发行版 openmpi）
- 协同编排 kernel 三件套（`libmf_sdma_orch.so` + json）——建池时库内自动部署
  （CUST 通道迁移 + CP1 搜索路径就位 + 完整性校验），无需手工部署
- 环境变量（均可选）：`MF_SDMA_ORCH_JSON` 显式指定 launch json（`bash run.sh`
  已自动取版本号最高的一件并注入）

## 运行

```bash
bash run.sh               # 一键: 环境自检 → (必要时)主构建 → 编译 → mpirun -np 4
bash run.sh --build-only  # 仅编译(产物 /tmp/ex09)
bash run.sh --run-only    # 仅运行
# 空闲卡选择(例): ASCEND_RT_VISIBLE_DEVICES=2,3,4,5 bash run.sh --run-only
```

## 验收标准

1. 4 rank 各 `mismatch=0/2097152`（host D2H 8MB 逐元素），8 block 状态字全 PASS
2. 4 rank 全部 `=== EX09 rN PASS ===`，退出码 0（任一 rank 失败即整体 FAIL）
3. 回归：08 示例（2-rank）在新布局下行为不变、全量 PASS

## 已知边界

- 8-rank 全互联不可达（每卡 8 个 P2P 槽位硬约束，全 mesh 8-rank 需 12 槽）——
  建池即被库内拓扑预检拒绝，属预期行为
- 跨卡带宽（~10–14 GB/s）低于跨 die（~17–19 GB/s），AllReduce 全链耗时由跨卡路径
  主导（实测量级 ≈ 62ms——含 AICore 标量填充/规约/校验，非纯通信口径）
