# 310P SHM SDMA 协同模式能力规范（delta）

## MODIFIED Requirements

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
