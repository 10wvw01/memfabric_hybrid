## ADDED Requirements

### Requirement: 跨卡 2-rank 部署场景

协同模式 SHALL 支持跨 PCIe 卡 2-rank 部署（对端 devId 与本端不在同一卡，即 devId 整除 2 的商不同）：本端 die 号 SHALL 仍由 devId % 2 推导（本地 die 编址与对端所在卡无关）；对端池分片经跨卡导入（P2P policy 分配 + 本进程上下文直接导入）后，其 GVA SHALL 可作为 SDMA SQE 的 src/dst（SMMU 可译），八步闭环与多 chunk 流水行为 SHALL 与 die 对场景一致（逐字校验 mismatch=0、双侧 mainRet=0、head 前进）。跨卡部署时两张卡 SHALL 各自完成编排 kernel 部署（deployer 互斥作用域 = 卡，跨卡设备 OS 文件系统独立）。die 对场景的全部既有契约与默认行为 SHALL 保持不变。

#### Scenario: 跨卡建池与传输准备

- **WHEN** rank0=dev0、rank1=dev2（跨卡对）分别初始化并以 SDMA 位建 SHM 池，开关 `MF_310P_SHM_OVERLAP=1`
- **THEN** 跨卡分片导入成功，双侧传输准备（CP 流 + 工作区 + 编排 kernel 加载）完成且冒烟探测通过

#### Scenario: 跨卡 mode13 八步闭环

- **WHEN** 跨卡 2-rank 上执行单 chunk（1MB）八步闭环
- **THEN** r0 终态数据逐字校验 mismatch=0，双侧 kernel mainRet=0，headAfter 前进

#### Scenario: 跨卡 mode14 流水掩盖

- **WHEN** 跨卡 2-rank 上执行 8×128KB 多 chunk 流水
- **THEN** 全量（1MB）逐字校验 mismatch=0，且流水总时延低于同规模串行对照（通信被计算掩盖）

#### Scenario: 跨卡干净退出

- **WHEN** 跨卡 2-rank 场景执行 destroy 与 uninit
- **THEN** 进程干净退出，无 D 态挂死（依赖池层 P2P 状态拆除纪律）
