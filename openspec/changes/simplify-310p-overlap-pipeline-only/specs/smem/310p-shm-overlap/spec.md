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
