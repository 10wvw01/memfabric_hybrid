## MODIFIED Requirements

### Requirement: 拷贝方向支持矩阵

310P HBM 池上的拷贝算子 SHALL 支持以下方向：设备到设备（GD2GD/LD2GD/GD2LD，含跨 die 与跨 rank）经异步 D2D 接口提交后同步等待流完成；主机到设备（LH2GD/GH2GD 等 H2D 方向）经异步 `AclrtMemcpyAsync` 提交后同步等待流完成；设备到主机（GD2LH/GH2LH 等 D2H 方向）经异步 `AclrtMemcpyAsync` 提交后同步等待流完成。对不支持的方向，SHALL 返回参数错误码并记录含 rankId 与 direction 的 ERROR 日志。池内 VMM VA 为设备地址，主机侧不可直接解引用，主机访问 SHALL 一律经 H2D/D2H 拷贝。方向判定 SHALL 按 `IsD2DDirection`/`IsH2DDirection`/`IsD2HDirection` 三类静态方法分派，覆盖全部 12 个 `hybm_data_copy_direction` 枚举值。

#### Scenario: 跨 die D2D 拷贝

- **WHEN** 用户在 rank0 与 rank1 的 HBM 池地址间发起设备到设备拷贝
- **THEN** 拷贝经异步 D2D 接口提交并等待流同步完成后返回，目标端数据与源端逐字一致

#### Scenario: 主机往返拷贝

- **WHEN** 用户发起 H2D 拷贝写入对端 HBM 池地址，随后 D2H 读回
- **THEN** 异步拷贝提交后同步等待完成，读回数据与写入数据逐字一致，全程无需主机直接解引用 VMM VA

#### Scenario: 不支持的方向

- **WHEN** 用户请求算子不支持的方向参数
- **THEN** 返回参数错误码，并记录包含 rankId 与 direction 的 ERROR 日志

### Requirement: 同步语义与等待契约

310P 拷贝算子 SHALL 支持真实异步语义：当 `ExtOptions` 携带外部 stream 时，`DataCopyAsync` SHALL 将 `AclrtMemcpyAsync` 提交到外部 stream 并由调用方负责同步；当 `ExtOptions` 携带 `ASYNC_COPY_FLAG` 时，`DataCopyAsync` SHALL 提交到内部 stream 并立即返回（调用方通过 `Wait` 同步）。不携带异步标志时 SHALL 等效同步：异步提交后立即调用 `InnerWait` 等待完成。`Wait` SHALL 调用 `AclrtSynchronizeStream(stream_)` 同步内部流，不再为 no-op。异步提交失败时（`SyncStream` 返回非零）SHALL 回退到同步 `AclrtMemcpy` 重试一次，回退失败 SHALL 记录含 rankId、src、dst、size、ret 的 ERROR 日志。`BatchCopyD2D` 在批量同步失败时 SHALL 逐项回退到同步 `AclrtMemcpy`。算子占 compose 的 SDMA 槽位，`hybm_wait` 对 310P HBM 池 SHALL 正常返回。

#### Scenario: hybm_wait 对 310P HBM 池可用

- **WHEN** 用户在 310P HBM 池上执行拷贝后调用 `hybm_wait`
- **THEN** 等待返回成功（算子占 SDMA 槽位，`Wait` 同步内部流）

#### Scenario: 外部 stream 异步拷贝

- **WHEN** 用户以外部 stream 发起 `DataCopyAsync`
- **THEN** 拷贝提交到外部 stream，不阻塞调用方，由调用方负责 stream 同步

#### Scenario: ASYNC_COPY_FLAG 异步拷贝

- **WHEN** 用户以 `ASYNC_COPY_FLAG` 发起 `DataCopyAsync`
- **THEN** 拷贝提交到内部 stream 并立即返回，调用方后续调用 `Wait` 同步

#### Scenario: 异步请求降级为同步

- **WHEN** 用户不携带异步标志发起 `DataCopy` 或 `DataCopyAsync`
- **THEN** 拷贝异步提交后立即调用 `InnerWait` 等待流同步完成并返回正确结果，数据一致性不受影响

#### Scenario: 异步失败同步回退

- **WHEN** D2D 异步提交后 `SyncStream` 返回失败
- **THEN** 系统回退到同步 `AclrtMemcpy(DEVICE_TO_DEVICE)` 重试一次，回退结果作为最终返回

### Requirement: 地址变换为恒等

310P HBM 池拷贝算子的地址变换 SHALL 经 `HybmVaManager::TransformVa` 将 GVA 翻译为对应的 DVA 或 HVA：源地址按方向对应的源内存类型（HOST→HVA，DEVICE→DVA）翻译，目标地址按方向对应的目标内存类型翻译。翻译结果为 0 时（地址不在池内映射表中）SHALL 保持原地址不变。方向与内存类型的对应关系 SHALL 由 `DirectionSrcMemType`/`DirectionDestMemType` 静态查表确定，覆盖全部 12 个方向枚举值。

#### Scenario: 池内 GVA 翻译为 DVA

- **WHEN** 用户以池内 GVA 地址发起 D2D 方向拷贝
- **THEN** 源与目标地址经 `TransformVa(GVA→DVA)` 翻译后用于设备拷贝接口，结果正确

#### Scenario: 非池地址保持原值

- **WHEN** 用户传入的地址不在池内映射表中（`TransformVa` 返回 0）
- **THEN** 地址保持原值直接用于拷贝接口

#### Scenario: 池内地址直接可用

- **WHEN** 用户以池内登记地址发起任一支持方向的拷贝
- **THEN** 地址经 `TransformVa` 翻译为对应的 DVA 或 HVA 后用于设备拷贝接口，结果正确

### Requirement: 初始化不触碰宿主注册路径

算子初始化 SHALL 仅惰性创建内部 stream，不得触碰 HAL 宿主注册（HalHostRegister）或参数空间等既有路径；初始化失败（如 stream 创建失败、可选符号未加载）SHALL 返回错误并记录含关键参数的 ERROR 日志。算子 SHALL 提供 `CleanUp` 接口（310P AclMemcpy 为 no-op，不持有 HybmStreamManager 资源）。

#### Scenario: 初始化成功

- **WHEN** 在 310P 上初始化拷贝算子
- **THEN** 仅创建内部 stream，既有 HAL 注册路径不受影响，初始化返回成功
