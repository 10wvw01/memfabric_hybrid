# 15｜深度推导：为什么选 o_proj 做 MM+AR，而不是别的节点

> 本章位置：主线第三幕深挖。06 给了方法，这一章真正做一次“设计评审”，重点学习怎样证明一个融合点值得做，又怎样证明它可能不再值得做。

---

## 1. 候选点先全部摆上桌

当前模型里可能想到的候选：

```text
A. QKV projection 周边
B. Attention 内部
C. o_proj/out_proj + TP SUM
D. residual / norm 周边
E. MoE dispatch/combine
F. 更大的 Transformer block
```

不要先带着答案选 C。

---

## 2. 第一关：数学等价性

C 的数学最干净：

```text
Y = X0W0 + X1W1
```

而且按 M 分块：

```text
Y[b] = X0[b]W0 + X1[b]W1
```

所以 partial batch 可以独立生产、传输、规约。

这是非常强的性质：

```text
可分块
可流水
无跨 batch 数学依赖
```

QKV/Attention/MoE 的语义都更复杂。

---

## 3. 第二关：数据邻接性

当前 stock 路径概念上：

```text
local MM
 -> partial tensor
 -> TP SUM
```

两步天然紧邻。

如果融合的是 QKV + Attention，之间还会经过：

```text
reshape
RoPE
KV write
mask
attention algorithm
```

融合边界大得多。

---

## 4. 第三关：频率与关键路径

`o_proj/out_proj`：

```text
每相关 layer 一次
每个 Prefill/Decode forward 都会走
TP SUM 在后续依赖链上
```

因此即使单次收益不夸张，累计也可能显著。

但这仍然必须通过 profiler 证明它真在 critical path，而不是只看调用频率。

---

## 5. 第四关：shape 与工程可控性

当前目标非常规整：

```text
local K=2048
N=2048
FP16
no bias
TP=2
M 动态
```

这让我们能把大部分复杂度集中在 M 维和通信协议上。

如果一开始就做几十种 K/N/dtype，性能原因会被 shape 泛化问题淹没。

---

## 6. 第五关：接口稳定性

对上层来说，融合前后都返回：

```text
[M,2048]
```

只是“谁负责 SUM”变了。

因此模型 forward 不需要理解：

```text
arena
MemFabric
ready
generation
```

这是很好的 abstraction boundary。

---

## 7. 为什么不直接在 HCCL 层优化

一个候选思路是：

```text
MM 仍然普通执行
只把 AllReduce 换成更快实现
```

优点：侵入小。

局限：

```text
通信仍要等完整 MM 结束
无法利用 MM 逐批产生数据的时序
```

MM+AR 融合的核心价值就是打破这个完整 tensor 边界。

但如果 profiler 证明 AR 固定开销才是主要问题、而 overlap 空间很小，那么“只优化 collective”反而可能是更简单的好方案。

---

## 8. 为什么不把 Add 之后也一起融合

例如：

```text
MM + peer SUM + residual
```

数学上可能可做。

但当前先停在 reduced output，是为了保持：

```text
融合前后 Linear contract 一致
```

这让 correctness/reference/testing 都更简单。

如果未来 Add/Residual 明显成为关键路径，再扩边界更合理。

---

## 9. 一个决策矩阵

| 候选 | 数学清晰 | 通信紧邻 | shape 稳定 | 接口侵入 | 当前风险 |
|---|---|---|---|---|---|
| o_proj + SUM | 高 | 高 | 高 | 低 | 中 |
| QKV + Attention | 中 | 低 | 中 | 高 | 高 |
| 整个 Attention | 中 | 中 | 低/中 | 高 | 很高 |
| MoE | 另一类语义 | 高 | 动态 | 高 | 很高 |
| 只换 AllReduce | 高 | 高 | 高 | 很低 | 低，但 overlap 空间小 |

这张表不是永久排名，只是当前约束下的工程判断。

---

## 10. “这是最好的吗？”应该怎么回答

不能回答“是”。

正确说法：

> 当前 `o_proj/out_proj` 是一个**收益机制清楚、数学边界稳定、实现风险可控**的融合入口，适合验证 310P 上计算通信协同；但它是否是全模型最佳优化点，需要和真实 profiler 中其它热点持续比较。

---

## 11. 什么时候应该推翻这个融合边界

出现这些情况要重新评审：

```text
o_proj MM+AR 已经只占极小比例
后续 residual/layout 成为主要 launch 开销
MoE 通信远大于 TP reduction
新模型不再有稳定 K/N
TP 扩到 4/8 后 collective 复杂度主导
硬件提供原生 fused collective primitive
```

那时“继续深挖当前 o_proj”可能已经不是最佳投入方向。

---

## 12. 下一代融合边界的三条路线

### 向下做深

```text
更细 chunk
更好的 kernel/SDMA overlap
```

### 向上做大

```text
MM+AR+后续 elementwise
Attention tail fusion
```

### 向系统层做宽

```text
Scheduler/Graph/shape policy 主动配合 fused op
```

真正成熟的优化通常不是永远向一个方向做，而是 profiler 决定三条路线中哪条更值。
