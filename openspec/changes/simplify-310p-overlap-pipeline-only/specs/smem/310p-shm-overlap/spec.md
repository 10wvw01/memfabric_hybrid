# 310P SHM 协同模式能力规范（delta）

## MODIFIED Requirements

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
