## Purpose

定义 310P HBM 内存池上数据拷贝算子（ACL memcpy 路径）的行为契约：拷贝方向支持矩阵、同步语义、compose 候选链接入与 hybm_wait 契约。

## Requirements

### Requirement: 拷贝方向支持矩阵

310P HBM 池上的拷贝算子 SHALL 支持以下方向：设备到设备（GD2GD/LD2GD/GD2LD，含跨 die 与跨 rank）经异步 D2D 接口提交后同步等待流完成；主机到设备/设备到主机（LH2GD/GD2LH）经同步 H2D/D2H 接口完成。对不支持的方向，SHALL 返回参数错误码并记录含 rankId 与 direction 的 ERROR 日志。池内 VMM VA 为设备地址，主机侧不可直接解引用，主机访问 SHALL 一律经 H2D/D2H 拷贝。

#### Scenario: 跨 die D2D 拷贝

- **WHEN** 用户在 rank0 与 rank1 的 HBM 池地址间发起设备到设备拷贝
- **THEN** 拷贝经异步 D2D 接口提交并等待流同步完成后返回，目标端数据与源端逐字一致

#### Scenario: 主机往返拷贝

- **WHEN** 用户发起 H2D 拷贝写入对端 HBM 池地址，随后 D2H 读回
- **THEN** 同步拷贝接口完成，读回数据与写入数据逐字一致，全程无需主机直接解引用 VMM VA

#### Scenario: 不支持的方向

- **WHEN** 用户请求算子不支持的方向参数
- **THEN** 返回参数错误码，并记录包含 rankId 与 direction 的 ERROR 日志

### Requirement: 同步语义与等待契约

310P 拷贝算子在 V1 中所有方向 SHALL 等效同步：异步提交后立即等待完成，显式异步请求与外部 stream 参数降级为阻塞行为（登记为 V2 改进）。算子的等待接口 SHALL 为 no-op 并返回成功。由于算子接入 compose 候选链的 SDMA 槽位，`hybm_wait`（smem_bm_wait）对 310P HBM 池 SHALL 正常返回成功，不得恒失败。

#### Scenario: hybm_wait 对 310P HBM 池可用

- **WHEN** 用户在 310P HBM 池上执行拷贝后调用 `hybm_wait`
- **THEN** 等待返回成功（算子占 SDMA 槽位，compose 的 Wait 链路可达）

#### Scenario: 异步请求降级为同步

- **WHEN** 用户以异步标志或外部 stream 发起拷贝
- **THEN** 拷贝以阻塞方式完成并返回正确结果，数据一致性不受影响

### Requirement: 地址变换为恒等

310P HBM 池上 GVA==DVA==HVA（同一 VMM VA），拷贝算子的地址变换 SHALL 为恒等变换：用户传入的池内地址直接作为设备地址使用，不经过 VA 管理器窗口二次分配或映射。

#### Scenario: 池内地址直接可用

- **WHEN** 用户以池内登记地址发起任一支持方向的拷贝
- **THEN** 地址不经变换直接用于设备拷贝接口，结果正确

### Requirement: 初始化不触碰宿主注册路径

算子初始化 SHALL 仅惰性创建内部 stream，不得触碰 HAL 宿主注册（HalHostRegister）或参数空间等既有路径；初始化失败（如 stream 创建失败、可选符号未加载）SHALL 返回错误并记录含关键参数的 ERROR 日志。

#### Scenario: 初始化成功

- **WHEN** 在 310P 上初始化拷贝算子
- **THEN** 仅创建内部 stream，既有 HAL 注册路径不受影响，初始化返回成功
