# smem/310p-sdma-orch Specification

## Purpose
定义 310P（Atlas 300I Duo）上 SHM API 设备发起语义——AICore×AICPU 协同模式——的行为契约：
环境开关门控与显式降级语义、传输准备与就绪门控、sdma 协同 host API 参数与错误语义、
wire 契约与工作区布局不变式、设备元数据布局兼容，以及开关关闭时存量路径零影响保障。

## Requirements

### Requirement: 平台门控与显式禁用

系统 SHALL 在 310P 平台以 SDMA 传输位创建 SHM 实体时自动启用协同模式（A3 SDMA 配方
在 310P 必败，协同模式是该平台 SDMA 的唯一可用链路，自动启用使用户零配置）。
环境变量 `MF_SDMA_ORCH_DISABLE` 为显式禁用开关（"1"/"on" 禁用）：禁用时 310P 上以
SDMA 传输位创建 SHM 实体的请求 SHALL 返回明确的不支持错误，错误日志 MUST 包含
deviceId 与失败原因；系统 MUST NOT 静默降级到其他链路。禁用状态 MUST NOT 影响非
310P 平台的任何行为，也 MUST NOT 影响 310P 上不携带 SDMA 传输位的既有路径行为。

#### Scenario: 默认自动启用

- **WHEN** 310P 环境下未设置任何协同环境变量，调用方以 SDMA 传输位创建 SHM 实体
- **THEN** 协同模式自动启用，建池成功，无需任何环境变量配置

#### Scenario: 显式禁用时请求 SDMA SHM

- **WHEN** 310P 环境下设置 `MF_SDMA_ORCH_DISABLE=1`，调用方以 SDMA 传输位创建 SHM 实体
- **THEN** 创建失败并返回明确的不支持错误，ERROR 日志含 deviceId 与失败原因，不发生静默降级

#### Scenario: 存量路径零影响

- **WHEN** 任意设置 `MF_SDMA_ORCH_DISABLE`，调用方使用 BM API 或不带 SDMA 传输位的 SHM API
- **THEN** 行为与本能力引入前完全一致（回归不变式）

#### Scenario: 非 310P 平台不受影响

- **WHEN** 在非 310P 平台（如 A3/910B）上设置任意 `MF_SDMA_ORCH_DISABLE` 取值
- **THEN** 平台既有路径（含 SDMA 传输准备）行为与该变量值无关

### Requirement: 传输准备与就绪门控

开关开启后，310P 上首次以 SDMA 传输位创建 SHM 实体时，系统 SHALL 自动完成协同传输
准备（CP 流、协同工作区、编排 kernel 设备侧部署与加载——含 CUST 通道迁移、CP1 搜索路径就位与完整性校验，设备侧已就位时自动跳过），全部准备成功即置就绪；任一步失败
时系统 MUST NOT 置就绪状态、MUST 返回显式错误（日志含 deviceId 与失败步骤），且该
失败 MUST 可重试（后续创建请求重新走准备流程）。端到端传输正确性验证由 08 示例
阶段①的逐字校验承担（初始化内不做自证性提交）。协同传输准备资源 SHALL 在进程级
被多个 SHM 实体共享，以引用计数管理生命周期，计数归零时 MUST 完整释放。

#### Scenario: 准备成功后即就绪

- **WHEN** 开关开启，首次 SDMA SHM 创建且全部准备成功（含设备侧 kernel 自动部署）
- **THEN** 传输准备就绪，sdma 协同 host API 可正常调用

#### Scenario: 设备侧 kernel 缺失时自动部署

- **WHEN** 设备 CP1 搜索路径无编排 kernel（未预部署）且迁移三件套就位于 json 同目录
- **THEN** 初始化自动完成迁移落盘、CP1 搜索路径就位与完整性校验（入口符号+精确尺寸）后加载成功，不要求用户手工部署

#### Scenario: 准备失败显式报错且可重试

- **WHEN** 开关开启但自动部署通道不可用/校验失败（或任一准备步骤失败）
- **THEN** 创建/准备返回显式错误且不置就绪，ERROR 日志含 deviceId 与失败步骤；环境修复后再次创建可重新尝试准备

#### Scenario: 多池共享与引用计数释放

- **WHEN** 同进程先后创建两个 SDMA SHM 实体，随后逐一销毁
- **THEN** 传输准备只初始化一次（第二池复用），最后一个实体销毁时资源完整释放（CP 流/工作区/kernel 卸载）

### Requirement: 设备侧邮箱 API 与 host 就绪判定

V5 邮箱化后数据面完全设备侧自驱。设备侧 API（AICore kernel 内，`smem_shm_sdma_signal/
wait/quiet`，均以池 gva 为首参）SHALL 满足：signal 投递 {src,dst,len,imm} 请求邮件（满环
自旋背压，预算耗尽返回 false）；wait 按序消费到达邮件并返回 {dst,len,imm,seq,status}
（邮件自描述落点，接收方免预约定布局；超时/溢出由 status 表达，戳值 1 基防 seq0 幻影）；
quiet 等待本端全部投递落地（源缓冲复用前）。host 侧仅保留 `smem_shm_sdma_get_workspace`
（就绪判定：非空=可用；无效 handle 返回 NULL）；host 侧 submit/wait/get_result MUST NOT
再提供。到达环深度 1024，溢出 MUST 以错误暴露而非静默覆写。

#### Scenario: 设备侧自驱闭环

- **WHEN** 双 rank 建池后，AICore kernel 内 signal N 封、对端 wait N 封、回程 signal/wait
- **THEN** 全程无 host 参与搬运，数据逐字校验通过（08 示例阶段①语义）

#### Scenario: 溢出与超时显式暴露

- **WHEN** 到达环被越界覆写（消费落后超 1024 封）或轮询预算耗尽
- **THEN** wait 返回 status 非 0 的邮件（OVERRUN/TIMEOUT），不静默丢失

#### Scenario: 未就绪与无效 handle 均拒绝

- **WHEN** 传输准备未就绪或传入无效 shm handle 时调用任一 sdma 协同 API
- **THEN** 返回明确错误，不产生部分副作用

### Requirement: wire 契约与工作区布局不变式

host 与设备编排 kernel 共享的参数/结果 POD 契约 SHALL 以编译期断言（static_assert）
锁死字段布局，且该契约头文件 MUST 可被 host 编译器与设备 kernel 工具链共同编译。
协同工作区（邮箱环/哨兵/结果/溢出区）SHALL 位于可被 AICore 直接 GM 访问且可被
SDMA 引用的设备内存。编排 kernel MUST NOT 直接解引用池 GVA——池地址仅允许出现在
SQE 字段中，由 SDMA 经 SMMU 翻译访问。

#### Scenario: 三工具链契约编译一致

- **WHEN** 同一份契约头文件分别被 host 编译器（g++）与设备 kernel 工具链（hcc/bisheng）编译
- **THEN** 布局断言全部通过，两侧二进制对契约字段偏移的理解一致

#### Scenario: 工作区可被双方访问

- **WHEN** 传输准备完成，AICore kernel 与编排 AICPU kernel 分别访问工作区（邮箱写/轮询）
- **THEN** 双方读写可见（含相应 cache 维护动作后），数据搬运仅经 SDMA 在池 GVA 间流动

### Requirement: 设备元数据布局兼容

协同模式向设备元数据写入工作区地址时 MUST 复用既有的 `sdmaWorkSpace` 槽（与 A3
血统的"设备发起 SDMA 工作区"为同一概念，两机制平台互斥不同时写入），MUST NOT 改变
该结构的既有布局与尺寸（128B 不变）；就绪判定以"工作区非空"承载，不新增能力位
字段；不使用协同能力的平台读取设备元数据的行为 MUST 保持不变。

#### Scenario: 元数据布局不变

- **WHEN** 编译复用 sdmaWorkSpace 槽的设备元数据结构
- **THEN** 结构尺寸与字段偏移与协同模式引入前完全一致（布局断言可验证）

#### Scenario: 非 310P 平台元数据读取回归

- **WHEN** A3/910B 等平台按既有方式读取设备元数据
- **THEN** 读取行为与协同模式引入前一致（协同仅在 310P 写入同一槽位，平台互斥）

### Requirement: 编排 kernel 版本纪律

设备侧编排 kernel 以 so 文件名缓存（CP1 按文件名 dlopen），其版本名 SHALL 独立于
主 kernel 版本管理；编排 kernel 内容发生变更时 MUST 提升其版本名，以避免设备侧
加载旧缓存导致新旧行为不一致。

#### Scenario: 版本名提升后加载新 kernel

- **WHEN** 编排 kernel 逻辑变更并提升版本名（文件名随版本变化）后重新部署
- **THEN** 设备侧按新文件名加载新版本，不命中旧缓存

#### Scenario: 未变更时版本名稳定

- **WHEN** 编排 kernel 内容未变更，主 kernel（其他设备 kernel 产物）单独演进
- **THEN** 编排 kernel 版本名不受主 kernel 版本影响，保持稳定
