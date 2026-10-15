# 310P3 Qwen3.6 FP16 o_proj + MemFabric 融合需求基线

> 本文定义当前 ABI v7 最终需求和验收合同。实现细节以
> [计算通信协作优化方案.md](计算通信协作优化方案.md) 为准。

## 1. 最终目标

在 Ascend 310P3 单物理卡双 die、TP=2 上，为
`Eco-Tech/Qwen3.6-35B-A3B-w8a8` full-attention `self_attn.o_proj`
提供：

```text
FP16 local o_proj MM
+ TP=2 peer exchange
+ local reduction
```

要求恢复原生多核 MM 形态，并实现 baseM 对齐的 batch 级计算通信 overlap。

## 2. 固定适用范围

- Ascend 310P3 / dav-2002
- TP=2
- model type `qwen3_5_moe_text`
- 仅 full-attention `*.self_attn.o_proj`
- global K=4096 / local K=2048 / N=2048
- unquantized FLOAT target layer
- FP16 input/weight/exchange/output

命中后必须 `reduce_results=False`；融合 op 已返回 TP=2 reduction 结果。

## 3. 固定 MM / batch 合同

```text
baseM/baseN/baseK = 256/256/64
blockDim = 8
usedCoreNum = 8
batch_m = baseM * q
q in {1,2,4}, default 2
```

硬约束：

- 8 个 AI Core 全部参加 cooperative MM；
- core0 也参加 MM，完成后兼任唯一 data `signal()` producer；
- 不允许恢复专职 communication core；
- batch 必须是整数个 baseM stripe；
- 2 MiB 只能是当前默认形状推导结果，不能硬编码成协议粒度；
- tail 必须 pad 到完整 batch 后继续 cooperative MM。

## 4. MemFabric 边界

MemFabric 为外部黑盒：

- 只消费 public host/device headers 和 `libmf_smem.so`；
- host 只使用 public lifecycle/readiness/control API；
- device 只使用 public `signal/wait/quiet`；
- 不读取或复制 ring/head/tail/reserved/orchestrator/SQE 等私有布局。

## 5. 计算通信 overlap

每 batch：

```text
8-core MM(batch n)
-> cache visibility
-> ready[n][0..7]
-> core0 signal(n)
-> SDMA(n)
```

runtime 必须允许：

```text
SDMA(n)   || MM(n+1)
SDMA(n+1) || reduce(n)
```

禁止 batch 间 host synchronize/barrier。overlap 由 CANN profiler 证明。

## 6. Wait / reduce

data mail 必须严格验证 `status/dst/len/imm`。

wait 以单 batch 为单位，不在每 batch 执行 quiet。wave drain 后执行一次
`quiet()`。

每个到达 batch 尽早 reduce 到普通 output Tensor；最后 padded batch 只写
valid rows。

## 7. Arena / credit

send/recv arena 使用固定内存预算，并按 rows 定容：

```text
arena_bytes = arena_rows * 2048 * 2
```

改变 q 不得直接把 arena 扩大 q 倍。

recv arena 跨 wave 复用由应用级 fixed credit 保护：

```text
prepare -> gate -> batch pipeline -> quiet -> ack
```

第一版仍是 wave-level credit，不要求 batch-level credit。

## 8. Graph / repeated

要求：

- eager 单 execution stream；
- capture 前完成 context/scratch/protocol/kernel warmup；
- capture 内无 create/malloc/host barrier/lazy warmup；
- repeated eager >=1000；
- mixed-M、>60s、10min；
- minimal Graph capture/replay；
- Qwen Graph repeated requests。

## 9. 错误模型

READY_TIMEOUT、SIGNAL_FAILED、QUIET_FAILED、WAIT_FAILED、MAIL_MISMATCH、
CREDIT_MISMATCH、ACK_FAILED 均不得静默继续。device fail-stop，失败 runtime
不得继续作为健康 context 复用。

## 10. Build

feature 默认关闭。

feature-off 不依赖 MemFabric；feature-on 仅支持 `ascend310p*`，必须从
`MEMFABRIC_HYBRID_HOME_PATH` 找到 public headers/`libmf_smem.so`，并从
当前 `.asc` 以 dav-2002 重新编译 device library。

## 11. 实机验收矩阵

Correctness 至少：

```text
M = 255, 256, 257,
    511, 512, 513,
    1024, 2048, 4096, 6144, 8192
q = 1, 2, 4
```

验证两 rank bit-exact、tail、multiple waves、diverge-va、repeated calls。

性能同时比较：

```text
A stock F.linear + HCCL
B ABI v6 baseline
C ABI v7 cooperative batch pipeline
```

记录 local MM、producer/wait/add/quiet/ack、fused total、TTFT、prefill、
TPOT/ITL、tokens/s 和 CANN timeline。

进入生产化前还必须解决或明确接受开发计划中 R6/R7 外部依赖约束。
