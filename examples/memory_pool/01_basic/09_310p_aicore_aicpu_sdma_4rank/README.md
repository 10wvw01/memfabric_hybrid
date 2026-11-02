# 09_310p_aicore_aicpu_sdma_4rank：310P 双卡四 die 4-rank 设备发起 SDMA 协同（全对全）

## 场景

2 张 Atlas 300I Duo（dev0/dev1 = 卡0，dev2/dev3 = 卡1）4-rank SHM 对称内存池上，
演示**4-rank 全对全的设备发起计算-通信协同**（08 的 2-rank 版本见同目录 08 示例）：

- 4 rank 同时运行 AICore kernel：每 rank 向 3 个对端 signal 投递搬运请求
  （跨 die 走 HCCS，跨卡走 P2P+PCIe）
- 各 rank 的 AICPU epoch kernel 代投 SDMA SQE：邮件像/到达戳/完成戳/数据
  **全部经 SDMA SQE 投递**（AICPU 对对端内存零直写——跨卡直写实测 stall，
  设备侧 CPU 核无跨卡 store 通路），请求环与到达环均**按对端划分**
  （多发送方/多目的地并发互不踩踏、序号无空洞）
- 接收方 `wait_any`（归并收件，`m.peer` 标来源）与 `wait_peer`（显式按对端）
  双形态齐用；host 零数据面（仅建池/起 kernel/收结果）

## 验证内容（4-rank 协同协议的核心风险点）

- **① 全对全搬运**：12 有向对各 1 块（128KB），接收方读改写 INC 后回件
  （imm 置回件位），统一收件循环收齐 3 入件 + 3 回件。验证点：
  - 多发送方并发同一接收方的**到达环槽位隔离**
  - 对回件早到鲁棒（按 imm 类别位计数，非按调用次数——回件可能早于
    部分跨卡入件到达）
  - host D2H 逐字校验终态（pattern+INC，pattern 含源/目的 rank 可溯源）
- **② 多目的地交错序号连续性**：每 rank 向 3 对端**交错**发 32 封（4KB），
  接收方 `wait_peer` 显式按对端逐封收齐。验证点：
  - **`m.seq` 每发送方环内连续无空洞**（旧单环协议在交错多目的地下序号断档，
    本阶段即其真机回归用例）
  - `m.imm` 轮次对账；首封基址+连续增量校验（对前序流量不敏感）

kernel 侧异常（wait 超时/溢出、路由非法、序号断档、拒投）经**判定词**（段内
12MB 处，编码含阶段/对端/错误码）显式暴露，host D2H 读回解码打印。

## 状态观测线程

host 侧后台线程每 5 秒从设备内存读回三组数据：常驻 kernel 结果区
（mainRet/consumed/exitReason——判断 kernel 在运行、已退出还是错误循环）、
kernel 判定词、3 个到达环的游标快照（`N=2E` 表示对端 N 的环已消费 2 封且在
等下一封，`R` 表示有就绪邮件未消费）。发现就绪未消费时，自动读出最近邮件内容
（sender/dest/类别位）供定位投递错误。全程小于 5 秒的运行不会有任何输出。

## 使用能力（设备侧自驱）

`smem_shm_create`（SDMA 位，310P 上协同自动启用）→ `smem_shm_sdma_get_workspace`
（就绪判定）→ AICore kernel 内 `smem_shm_sdma_ctx(gva)` 入口一次预计算 →
`smem_shm_sdma_signal_at`（dst 自动路由到对端请求环）/ `smem_shm_sdma_wait_any_at`
（归并，`m.peer` 标来源）/ `smem_shm_sdma_wait_peer_at`（显式按对端）/
`smem_shm_sdma_quiet_at`（逐环末封判定）→ `smem_shm_destroy`。

## 规模建议

- world_size=4 固定（mpirun -np 4 拉起；devId = rank，即 dev0/1+dev2/3）
- 拓扑：2×Duo（每卡 2 rank）或 4 卡各 1 rank——每卡 P2P 槽位预算 4/8、3/8 均通过
  （库内拓扑预检，超限建池显式拒绝 `SM_NO_RESOURCES`）
- 单次搬运 ≤ 1MB（AICPU SQE 上限）；len 须 4 字节对齐
- 每对端请求环深 64（单环满环背压不影响其他对端）；到达环深 1024/对端
- 每 rank 段末尾 192KB 为邮箱保留区（memfabric 托管，勿存放用户数据）

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

1. ①：4 rank 各自 `mismatch=0/98304` PASS（终态 = pattern+INC，pattern 含源/目的
   rank 可溯源），判定词 PASS
2. ②：4 rank 判定词 PASS（`m.peer` 路由/`m.seq` 环内连续/`m.imm` 对账全过）
3. 4 rank 全部 `=== EX09 rN PASS ===`，退出码 0（任一 rank 失败即整体 FAIL）
4. 回归：08 示例（2-rank）在新布局下行为不变、全量 PASS

## 已知边界

- 8-rank 全互联不可达（每卡 8 个 P2P 槽位硬约束，全 mesh 8-rank 需 12 槽）——
  建池即被库内拓扑预检拒绝，属预期行为
- 跨卡带宽（~10–14 GB/s）低于跨 die（~17–19 GB/s），①全对全耗时由跨卡路径主导
  （实测量级：① 12 有向对×128KB ≈ 4ms，② 交错 96 封/rank < 1ms）
