# 06｜为什么选择 o_proj 做 MM+AR 融合：先学会选点，再学会写算子

> 本章位置：主线第三幕。真正值得学的不是“答案是 o_proj”，而是以后面对任何模型都能复用的融合点评审方法。

---

## 1. 先从数学合法性开始

TP=2 RowParallel：

```text
Y = X0@W0 + X1@W1
```

记：

```text
Y0 = X0@W0
Y1 = X1@W1
```

则：

```text
Y = Y0 + Y1
```

按 M 行切 batch：

```text
Y[b] = Y0[b] + Y1[b]
```

所以每个 batch 可以独立完成：

```text
local MM -> exchange -> local SUM
```

这是 MM+AR 能流水化的数学基础。

---

## 2. 选融合点的六层筛选法

以后看到候选节点，按顺序问：

### ① 数学能不能安全重排？

是否存在严格等价变换，而不是“数值差不多”。

### ② 数据是不是天然紧邻？

如果中间还有复杂算子/布局变换，融合代价会暴涨。

### ③ 是否高频出现在关键路径？

一次省 20μs 但一天只跑一次，没有价值。

### ④ shape 是否足够稳定？

过于动态会让定制 kernel/Graph/缓存难以维护。

### ⑤ 上下层接口能否保持稳定？

好的融合边界最好让上层仍看到同样的 `[M,N]` tensor。

### ⑥ 有没有可靠 reference path？

必须能与 stock 路径做 correctness 和 performance A/B。

`o_proj/out_proj` 在当前模型上同时满足得比较好。

---

## 3. 为什么不是 qkv_proj

QKV projection 后面通常会进入：

```text
reshape / RoPE / attention / cache write
```

它后面不是一个简单、立即发生的 TP SUM 边界。

如果硬做更大融合，会把：

```text
projection
KV cache
attention metadata
layout
Graph
```

一次性绑在一起，测试空间和维护成本大很多。

不是说永远不能做，而是第一刀风险/收益比不如 o_proj 干净。

---

## 4. 为什么不是直接融合整个 Attention block

大融合潜在收益更大：减少中间 tensor 和 launch。

但代价是：

```text
full attention 与 GDN 两套路径
Prefill/Decode 差异
KV cache
mask/position
DFlash
Graph
```

都会被纳入一个巨大合同。

当前项目的目标是先验证“MM 与 TP reduction 的协作”本身，所以把边界收窄更有利于定位问题。

---

## 5. 为什么不是优先做 MoE

MoE 的通信也很重，但它通常涉及：

```text
routing
top-k
dispatch
expert parallel
combine
负载不均
```

它是另一类通信问题，不只是“两个 rank 的同 shape tensor SUM”。

所以 MoE 值得优化，但不适合作为验证 MM+AR 协同机制的第一落点。

---

## 6. 当前融合真正省的是什么

不是把数学乘加次数变少。

理论 FLOPs 基本还在，真正试图减少的是：

```text
通用 collective 启动固定开销
完整 MM 与完整 AR 的串行等待
不必要的中间同步
部分通信暴露时间
```

目标时间线：

```text
Stock:
[------ MM whole ------][---- AR whole ----]

Fused:
[MM0][MM1][MM2][MM3]
     [TX0][TX1][TX2]...
          [A0][A1]...
```

收益来自 overlap，而不是“算得少”。

---

## 7. 什么时候融合反而可能输

如果 M 很小：

```text
custom runtime fixed cost
ready/signal/wait
weight streaming
```

可能比 stock collective 更贵。

如果 M 很大但 SDMA 本来就不是瓶颈，融合也未必有明显端到端收益。

所以 eligibility 最终可能不仅是 shape 合法，还应该演进成“shape + performance policy”。

---

## 8. 设计审视：o_proj 是当前最好选择吗？

更严谨的结论：

> 它是当前阶段**较好的第一个融合边界**，不是全模型全局最优证明。

### 可能的下一代边界

```text
更小：只优化 collective launch
当前：MM + AR
更大：MM + AR + residual/后续 elementwise
更大：attention tail 整体融合
系统级：Scheduler 主动塑造适合融合的 shape
```

### 怎样决定是否扩大边界

只有当 profiler 明确显示：

```text
MM+AR 已经不是主要暴露时间
但紧邻的 layout/add/residual/launch 成为新 critical path
```

才值得扩融合边界。

否则大融合只是在增加复杂度。

---

## 9. 下一章

选点只是开始。下一步要真正设计协议：

```text
怎么切 M？
谁 signal？
通信 buffer 怎么排？
怎么避免覆盖？
怎么让 SDMA 与下一 batch MM 重叠？
Graph replay 怎么保持正确？
```
