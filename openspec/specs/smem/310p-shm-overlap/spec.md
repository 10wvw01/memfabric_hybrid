# smem/310p-shm-overlap Specification

## Purpose
定义 310P（Atlas 300I Duo）上 SHM API 设备发起语义——AICore×AICPU 协同模式——的行为契约：
环境开关门控与显式降级语义、传输准备与就绪门控、overlap host API 参数与错误语义、
wire 契约与工作区布局不变式、设备元数据布局兼容，以及开关关闭时存量路径零影响保障。

## Requirements

### Requirement: 开关门控与显式不支持

系统 SHALL 通过环境变量 `MF_310P_SHM_OVERLAP` 门控协同模式，默认关闭。开关关闭时，
310P 上以 SDMA 传输位创建 SHM 实体的请求 SHALL 返回明确的不支持错误，错误日志 MUST
包含 deviceId 与失败原因；系统 MUST NOT 静默降级到其他链路。开关状态 MUST NOT 影响
非 310P 平台的任何行为，也 MUST NOT 影响 310P 上不携带 SDMA 传输位的既有路径行为。

#### Scenario: 开关关闭时请求 SDMA SHM

- **WHEN** 310P 环境下 `MF_310P_SHM_OVERLAP` 未设置（默认关闭），调用方以 SDMA 传输位创建 SHM 实体
- **THEN** 创建失败并返回明确的不支持错误，ERROR 日志含 deviceId 与失败原因，不发生静默降级

#### Scenario: 开关关闭时存量路径零影响

- **WHEN** `MF_310P_SHM_OVERLAP` 未设置，调用方使用 BM API 或不带 SDMA 传输位的 SHM API
- **THEN** 行为与本能力引入前完全一致（回归不变式）

#### Scenario: 非 310P 平台不受开关影响

- **WHEN** 在非 310P 平台（如 A3/910B）上设置任意 `MF_310P_SHM_OVERLAP` 取值
- **THEN** 平台既有路径（含 SDMA 传输准备）行为与开关值无关

### Requirement: 传输准备与就绪门控

开关开启后，310P 上首次以 SDMA 传输位创建 SHM 实体时，系统 SHALL 自动完成协同传输
准备（CP 流、协同工作区、编排 kernel 加载），三步准备全部成功即置就绪；任一步失败
时系统 MUST NOT 置就绪状态、MUST 返回显式错误（日志含 deviceId 与失败步骤），且该
失败 MUST 可重试（后续创建请求重新走准备流程）。端到端传输正确性验证由 08 示例
阶段①的逐字校验承担（初始化内不做自证性提交）。协同传输准备资源 SHALL 在进程级
被多个 SHM 实体共享，以引用计数管理生命周期，计数归零时 MUST 完整释放。

#### Scenario: 准备成功后即就绪

- **WHEN** 开关开启，首次 SDMA SHM 创建且三步准备全部成功
- **THEN** 传输准备就绪，overlap host API 可正常调用

#### Scenario: 准备失败显式报错且可重试

- **WHEN** 开关开启但编排 kernel 未部署（或任一准备步骤失败）
- **THEN** 创建/准备返回显式错误且不置就绪，ERROR 日志含 deviceId 与失败步骤；环境修复后再次创建可重新尝试准备

#### Scenario: 多池共享与引用计数释放

- **WHEN** 同进程先后创建两个 SDMA SHM 实体，随后逐一销毁
- **THEN** 传输准备只初始化一次（第二池复用），最后一个实体销毁时资源完整释放（CP 流/工作区/kernel 卸载）

### Requirement: overlap host API 参数与错误语义

公共 overlap API（`smem_shm_overlap_launch`、`smem_shm_overlap_wait`、
`smem_shm_overlap_get_result`、`smem_shm_overlap_get_workspace`）SHALL 满足：
出参指针为空时拒绝；提交接口的 chunks 参数 MUST 校验在 1~64 范围（邮箱槽容量
约束）；参数有效性校验 MUST 先于就绪状态检查执行；无效 shm handle MUST 拒绝；
传输准备未就绪时所有 overlap API MUST 拒绝并返回明确错误。提交语义为多 chunk
流水编排（chunks=1 即单块闭环语义），系统不再提供独立的单 chunk 提交变体。

#### Scenario: 正常调用

- **WHEN** 就绪状态下以合法 handle 与参数调用 launch/wait/get_result
- **THEN** 调用被接受，编排 kernel 按多 chunk 流水语义执行（chunks=1 即单块语义）

#### Scenario: chunks 越界先于就绪检查报错

- **WHEN** 以 chunks=0 或 chunks=65 调用提交接口（即使传输准备未就绪）
- **THEN** 返回参数错误（参数校验先于就绪检查的语义可观测）

#### Scenario: 未就绪与无效 handle 均拒绝

- **WHEN** 传输准备未就绪或传入无效 shm handle 时调用任一 overlap API
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

协同模式向设备元数据结构写入扩展字段（工作区地址、就绪标志）时，MUST NOT 改变该
结构的既有布局与尺寸（128B 不变）；不使用协同能力的平台读取设备元数据的行为 MUST
保持不变。

#### Scenario: 元数据布局不变

- **WHEN** 编译含协同扩展字段的设备元数据结构
- **THEN** 结构尺寸与字段偏移与扩展前完全一致（布局断言可验证）

#### Scenario: 非 310P 平台元数据读取回归

- **WHEN** A3/910B 等平台按既有方式读取设备元数据
- **THEN** 读取行为与协同模式引入前一致（扩展字段位于原保留区，不参与既有解析）

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
