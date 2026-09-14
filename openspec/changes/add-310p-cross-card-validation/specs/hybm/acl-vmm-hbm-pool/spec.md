## ADDED Requirements

### Requirement: 跨卡导入的 P2P 状态拆除纪律（D 态防挂）

跨卡（非 die 对）导入会隐式建立设备间双向 P2P 状态并消耗 P2P 槽位。当某对端设备的全部跨卡导入句柄被释放（显式 RemoveImported 或 uninit 清理路径）时，系统 SHALL 显式拆除对该对端的 P2P 状态（`aclrtDeviceDisablePeerAccess`）；拆除失败 MUST 记录含 localRank、remoteRank、peerDevId 的 ERROR 日志且不阻断其余清理。进程退出路径 MUST NOT 在跨卡 P2P 状态未拆除的情况下遗留到设备复位（否则 D 态挂死）。die 对（同卡双 die）路径不建立 PCIe P2P，SHALL NOT 调用拆除接口。

#### Scenario: 跨卡全量释放后恰好拆除一次

- **WHEN** 本端从 dev2 跨卡导入 2 个分片后执行 RemoveImported 移除该 rank 全部句柄
- **THEN** 两个句柄均释放后对 peerDevId=2 的 DisablePeerAccess 恰好调用一次，早于最后一个句柄释放前不调用

#### Scenario: die 对导入不拆除

- **WHEN** 本端与 dev1（die 对）互导并执行 RemoveImported
- **THEN** 全程不调用 DisablePeerAccess（die 对无 PCIe P2P 通路，拆除接口对该路径无意义且必失败）

#### Scenario: 部分释放不拆除

- **WHEN** 跨卡导入 2 个分片仅移除其中 1 个
- **THEN** 不调用 DisablePeerAccess（仍有活跃跨卡句柄引用该对端 P2P 状态）

#### Scenario: uninit 兜底拆除

- **WHEN** 跨卡导入后不执行显式 RemoveImported 而直接走 uninit 清理
- **THEN** 清理路径完成全部句柄释放并对每个存在活跃跨卡导入的对端完成拆除，进程干净退出无 D 态
