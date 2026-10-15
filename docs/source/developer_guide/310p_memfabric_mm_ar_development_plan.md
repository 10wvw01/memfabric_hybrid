# 310P3 TP=2 FP16 o_proj + MemFabric ABI v7 开发与实机验证计划

> 本文记录 `feat/310p-memfabric-public-api` 下一阶段**实际实施顺序、代码 Gate、
> 静态审视要求与实机完成定义**。历史 v6（7 MM worker + 1 coordinator）仅作为
> 对照基线，不再作为目标实现。
>
> 目标设计：[计算通信协作优化方案.md](计算通信协作优化方案.md)
>
> 需求：[310p_memfabric_mm_ar_requirements.md](310p_memfabric_mm_ar_requirements.md)
>
> 使用：[310p_memfabric_mm_ar_usage.md](../user_guide/feature_guide/310p_memfabric_mm_ar_usage.md)

## 0. 当前实施状态（2026-09-22）

本轮按“先计划、后编码”执行：

- ABI v7 编码已完成：8-core cooperative MM、core0 sole signal owner、
  baseM batch、per-batch wait/reduce、wave quiet/credit、tail full-batch pad；
- Python/env/custom-op/test/benchmark 已同步到
  `BATCH_BASEM_COUNT`，旧 `TILE_M` 已退出当前运行路径；
- MemFabric private ring 固定偏移调试探针已删除；
- README / design / requirements / usage 已同步到 ABI v7；
- 静态审视 Round 1（协议/正确性）通过：未发现未修复 P0/P1；
- 静态审视 Round 2（ABI/签名/build/resource/Graph/集成）通过：未发现
  文本级断链或结构错误。

**边界说明**：以上结论是静态代码审视，不替代目标 310P3 的 bisheng/CMake
编译和真实运行。本轮没有把任何 v7 性能数据标记为已实测。下一 Gate 必须从
feature-off/on build 开始，再进入 correctness、profiler、long-run、Qwen
eager/Graph。

当前 correctness-first 实现对 cooperative MM 输出采用保守 per-core
full-batch cache clean；它用于首轮实机正确性 bring-up，待 dav-2002 profiler
确认 core->C ownership 后再缩小 clean 范围。

## 1. 本期目标

把当前 ABI v6：

```text
block0 coordinator
block1..7 single-core MM
tile_m chunk
all producer -> quiet/wait all -> add whole wave
```

迁移为 ABI v7：

```text
8 AI Core cooperative MM
+ core0 兼任唯一 signal owner
+ batch = q * baseM
+ per-batch wait / reduce
+ wave-level fixed credit
```

固定设计约束：

- `baseM/baseN/baseK = 256/256/64`；
- `blockDim = 8`，8 个 AI Core 全部参加 MM；
- `TCubeTiling.usedCoreNum = 8`；
- core0 参加 MM，同时是唯一 `smem_shm_sdma_signal()` producer；
- 通信/规约 batch 只允许为整数个 `baseM` stripe；
- 默认 `BATCH_BASEM_COUNT=2`，即当前 N=2048/FP16 时派生为 2 MiB；
- 2 MiB 不是协议常量；
- batch n signal 后允许 batch n+1 MM 继续，不能等待前一批 SDMA；
- peer batch 到达后立即 reduce 本 batch；
- credit 第一版仍保持 wave 级；
- MemFabric 继续只使用 public `signal/wait/quiet`，不读写私有 ring/SQE/orchestrator。

## 2. 当前已验证基线

ABI v6 已验证能力继续作为 v7 回归底座：

- public API 黑盒边界；
- strict data/credit mail validation；
- fixed-credit wave protocol；
- fail-stop + protocol status；
- real `aclrtGetDevice()` device id；
- peer-space GVA geometry exchange；
- eager single-stream / ACL Graph capture-side-stream 合同；
- warmup fallback；
- `MIN_M=4096` 小 M stock fallback；
- feature on/off build；
- 双 rank correctness、tail、multiple waves；
- minimal ACL Graph capture/replay；
- Qwen eager；
- v6 profiler 已证明 MM/SDMA 可 overlap。

已知外部阻塞项继续保留：

- R6：MemFabric/运行时偶发约 20s engine freeze，未解除前不能判生产可用；
- R7：逻辑 deviceId 授权限制，当前实机仍要求
  `ASCEND_RT_VISIBLE_DEVICES=0,1`。

## 3. 实施阶段

### P0：恢复原生 MM 计算效率

先完成：

```text
ABI v7
8-core cooperative MM
core0 sole signal owner
baseM = 256
batch = q * baseM
arena 与 batch 解耦
tail pad 到完整 batch
```

P0 不以 overlap 深度为第一目标，先保证 MM 形态正确。

代码 Gate：

- 不再存在 `block1..7 + usedCoreNum=1`；
- producer launch blockDim 固定 8；
- `usedCoreNum=8`；
- static tiling 与目标 `256/256/64` 对齐；
- 每个 batch 只有一次 data signal；
- core0 在 signal 前必须确认 8 core 完成且数据 cache 可见；
- tail 不允许退回 16/32/64/128-row 单核 MM；
- send/recv arena 容量不随 batch bytes 成比例放大。

实机 Gate：

- M=4096/6144/8192 的纯 MM 时间与 stock `F.linear/aclnnMm` 比较；
- 目标：median 不劣于 stock 约 5%；
- 若不满足，停止进入 P1，先修 MM tiling/ownership/cache clean。

### P1：per-batch wait + reduce 流水

runtime enqueue 顺序改为：

```text
gate(wave)
P0
P1
W0
A0
P2
W1
A1
...
drain W/A(last)
quiet
ack
```

其中：

- `Wb` 只消费 batch b 的一封 data mail；
- mail 严格校验 `status/dst/len/imm`；
- `Ab` 只 reduce batch b，并写入对应 output offset；
- `quiet()` 不得重新塞回每个 batch 热路径；
- wave drain 后只执行一次 quiet，再 ack wave credit；
- batch 之间禁止 host synchronize。

Profiler 必须证明：

```text
SDMA(batch n) overlaps MM(batch n+1)
SDMA(batch n+1) overlaps Reduce(batch n)
```

### P2：实机调优

只允许调：

- `BATCH_BASEM_COUNT = 1/2/4`；
- lookahead = 1/2；
- `arena_rows`；
- per-core clean 范围。

不允许重新通过缩小 MM tile 或恢复专职 communication core 制造 overlap。

## 4. ABI v7 迁移计划

`memfabric310p_adapter_api.h`：

- ABI version 6 -> 7；
- chunk 语义替换为 batch 语义；
- layout 输出 `batch_bytes / arena_rows / max_batches`；
- producer API 输入 `batch_index / valid_rows / generation`；
- waiter API 改为 wait-one-batch；
- add API 增加 send/recv/output batch offset 与 valid rows；
- 保留 public quiet / ack / gate。

`memfabric310p_adapter.cpp`：

- local pool 只按 `arena_rows` 定容；
- ready control 改为每 batch 8 个独立 64B core-ready cell；
- launch 参数只传 public protocol 所需 GVA；
- 保持 geometry exchange、deviceId、生命周期和错误传播。

兼容策略：

- v7 为内部定制 ABI，不维持 v6 二进制兼容；
- Python/C++/device 必须同 commit 升级；
- 旧 `VLLM_ASCEND_310P_MEMFABRIC_MM_AR_TILE_M` 退出核心设计；
- 新增
  `VLLM_ASCEND_310P_MEMFABRIC_MM_AR_BATCH_BASEM_COUNT`，
  默认 2，合法值 1/2/4。

## 5. Device kernel 计划

### 5.1 cooperative producer

一个 producer launch 只处理一个 batch：

```text
A [BATCH_M, 2048]
B [2048, 2048]
8-core cooperative MM
C [BATCH_M, 2048] -> send_arena[batch]
```

每个 core：

```text
MM fragment complete
-> clean 自己写出的 C 区域
-> ready[batch][core] = generation
```

core0：

```text
完成自身 MM fragment
-> ready[batch][0]
-> 等待 ready[batch][0..7] == generation
-> signal(send_batch, peer_recv_batch, batch_bytes, batch_id)
```

bring-up 阶段如果暂时无法可靠取得 Matmul core->C tile ownership，可使用保守
cache flush correctness fallback；静态代码必须显式标注其为待 profiler 收敛项，
不能把未证明的 ownership 当作事实。

### 5.2 wait-one

每次只 wait 一封 mail：

```text
mail.status == OK
mail.dst == expected_recv_base + batch_offset
mail.len == batch_bytes
mail.imm == batch_id
```

wait-one 不执行 quiet。

### 5.3 batch add

保持现有多 block FP16 add 结构，但参数化：

- send batch offset；
- recv batch offset；
- output row offset；
- valid rows。

对 tail：transport/reduce 可以覆盖 pad 后完整 batch，output 只能写 valid rows。

## 6. Runtime / tail / arena 计划

固定：

```text
BASE_M = 256
batch_m = BASE_M * q
row_bytes = 2048 * sizeof(FP16)
batch_bytes = batch_m * row_bytes
```

arena：

```text
arena_rows = floor(configured_arena_bytes / row_bytes)
arena_rows = floor(arena_rows / batch_m) * batch_m
max_batches = arena_rows / batch_m
```

要求：

- `arena_rows >= batch_m`；
- batch 变化不得改变 local pool 的目标内存预算；
- 默认保持与旧 8192-row arena 量级相当；
- `VLLM_ASCEND_MF310P_MAX_CHUNKS=64` 不再承担计算切块语义。

tail：

```text
valid_rows
-> pad 到完整 batch_m
-> cooperative MM
-> signal full batch_bytes
-> reduce full batch payload
-> output copy/write valid_rows
```

Graph capture 内禁止 malloc/create/barrier/host sync/lazy warmup。

## 7. Python / benchmark 计划

`vllm_ascend/_310p/ops/memfabric_mm_ar.py`：

- Plan 改为 `base_m / batch_basem_count / batch_m / batch_bytes / arena_rows`；
- 删除 tile/chunk 核心语义；
- 调用 custom op 时传 `batch_basem_count`；
- 保留模型、TP、dtype、full-attention、MIN_M、warmup fallback 约束。

`envs.py`：

- 新增 `VLLM_ASCEND_310P_MEMFABRIC_MM_AR_BATCH_BASEM_COUNT=2`；
- TILE_M 标为 deprecated/不再参与 v7 路径，后续清理。

benchmark：

- correctness 增加 255/256/257、511/512/513；
- 重点性能 M=4096/6144/8192；
- overlap analyzer 以 batch 为 token，不再以 chunk 为 token；
- A/B/C：stock、ABI v6、ABI v7。

## 8. 静态代码审视 Gate

编码后至少执行两轮独立静态审视。

### Round 1：协议与正确性

逐项检查：

- producer 单 signal owner；
- 8-core rendezvous 无越界/复用 race；
- generation 不会被旧 wave stale ready 误命中；
- peer GVA 校验方向正确；
- mail id/batch offset/len 一致；
- tail scratch/pad/output 边界；
- arena 多波复用只发生在 credit 之后；
- quiet/ack 顺序；
- error 后 poison/fail-stop；
- eager/Graph 生命周期；
- 所有 size/offset 乘法的 64-bit 溢出与合法性校验。

发现 P0/P1 缺陷必须编码修复后才能进入 Round 2。

### Round 2：可编译性、资源与集成

逐项检查：

- C/C++ 声明与 device launcher 签名完全一致；
- ABI version/layout/API 全链路一致；
- AscendC static tiling 与 runtime shape 参数一致；
- `usedCoreNum=8` 与 launch blockDim=8；
- UB/L1/workspace 使用没有显见超限；
- Python custom op schema/调用参数一致；
- env 默认值和合法值检查；
- feature-off build 不引用 MemFabric；
- CMake/public-package 边界无回退；
- source regression 测试同步更新；
- benchmark/文档没有残留误导性的 v6 chunk 调优指令。

Round 2 结束后再复查 diff，保证没有 debug 临时代码、旧 ABI 死分支、历史产物。

## 9. 实机验证矩阵

### Build

- feature-off；
- feature-on CMake；
- bisheng dav-2002 编译 `.asc`；
- link/load；
- custom op 注册；
- public `libmf_smem.so` 解析。

### Correctness

至少：

```text
M = 255, 256, 257,
    511, 512, 513,
    1024, 2048, 4096, 6144, 8192
```

覆盖：

- q=1/2/4；
- tail；
- multiple waves；
- repeated calls；
- diverge-va；
- 两 rank bit-exact；
- feature-off stock baseline。

### Stability

- representative M repeat >=1000；
- mixed-M；
- >60s；
- 10min；
- destroy/recreate；
- HCCL 共存；
- protocol status 全程 OK。

### Graph

- eager warmup；
- minimal capture/replay；
- 多 replay；
- 不同 bucket；
- Qwen ACL Graph；
- repeated requests。

### Performance

比较：

```text
A: stock F.linear + HCCL
B: ABI v6 7+1 chunk pipeline
C: ABI v7 8-core batch pipeline
```

记录：

- MM duration；
- signal-to-arrival 暴露时间；
- batch reduce；
- fused total；
- stock total；
- eager TTFT；
- prefill tokens/s；
- profiler timeline。

## 10. 本期完成定义

代码进入“可上实机测验”必须满足：

- ABI v7 全链路一致；
- 8-core cooperative MM 代码形态完成；
- batch wait/reduce pipeline 完成；
- baseM/batch/arena/tail 语义统一；
- source regression 更新；
- 两轮静态审视完成且 P0/P1 问题已修；
- feature-off 路径无回归；
- 未声称任何未经 310P 实机验证的性能结论。

项目进入“可生产化”仍需额外满足：

- feature-on 310P build；
- correctness matrix；
- >=1000 repeat + 10min；
- Qwen eager + ACL Graph；
- profiler overlap；
- MM 效率恢复；
- 相对 stock 可重复正收益；
- R6/R7 外部阻塞项解除或有明确、可接受的部署约束。
