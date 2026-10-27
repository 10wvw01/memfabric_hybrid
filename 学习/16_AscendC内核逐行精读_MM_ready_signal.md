# 16｜AscendC 内核逐行精读：MM、ready、cache clean、signal 为什么这样排

> 本章位置：主线第四幕深挖。目标是理解 `memfabric310p_device.asc` 中 direct producer 的正确性链，以及哪些地方是性能策略、哪些地方是硬约束。

---

## 1. 先看大图，再看代码

一个 producer batch 的语义：

```mermaid
flowchart LR
  A[8核分工 MM] --> B[各核写自己的 C rows]
  B --> C[各核 cache clean 自己区域]
  C --> D[各核写 ready=generation]
  D --> E[core0 等 8 个 ready]
  E --> F[core0 signal]
```

读源码时，每一段都对应这张图中的一条边。

---

## 2. 为什么 Classic AscendC Matmul 需要显式多核切 M

当前实现里单个 Matmul 对象的语义更接近“当前 core 计算自己给定的矩阵区域”，不会自动帮你把一个 `[batch_m,2048]` MM 横跨 8 核协作完成。

因此显式计算：

```text
row_begin = block_idx * batch_m / 8
row_end   = (block_idx + 1) * batch_m / 8
```

每核得到自己的 A/C view。

### 当前 tiling

已知主路径采用类似：

```text
base N = 256
base K = 64
CONFIG_NORM
```

选择不是教科书常数，而是针对当前 per-core M 与 310P on-chip buffer 约束做的实现选择。

历史上某些更激进配置在当前小 per-core M 下会触发 MTE buffer 问题，因此不能只因为配置名“更强”就使用。

---

## 3. 为什么 8 核都算，而不是 7 核算 + 1 核通信

如果拿一个 core 专门做 signal：

```text
MM compute capacity 少 12.5%
```

而 signal 并不是全程持续占核。

当前原则：

```text
8 核全部算
core0 算完自己的 partition 后再负责聚合 ready 与 signal
```

这样通信职责复用一个参与计算的 core。

### 这是不是永远最好？

不一定。如果未来 signal/协议工作非常重、或者通信核能执行完全不同的异步 engine 逻辑，专核可能重新有价值。

判断依据仍是 core0 是否因 signal/polling 明显拉长整批完成时间。

---

## 4. ready cell 的地址应该怎么理解

概念上：

```text
ready[batch_id][core_id]
```

每个 core 只写自己的 cell：

```text
core3 只写 ready[b][3]
```

core0 读 8 个 cell。

这样避免多个 core 抢写同一个普通标志。

---

## 5. 为什么 ready 写的是 generation，不是 1

如果第 n wave 结束后：

```text
ready[b][3] = 1
```

下一 wave 还没计算，core0 一看仍是 1，就可能误认为 core3 已完成。

generation：

```text
wave n   -> 0x40000010
wave n+1 -> 0x40000011
```

core0 只接受当前期望值。

这相当于给“完成”加上身份证，而不只是布尔值。

---

## 6. 为什么 cache clean 一定在 ready 前

假设错误顺序：

```text
ready
 -> cache clean
```

core0 看到 ready 后可以立刻 signal，SDMA 可能先于 clean 读 C。

正确：

```text
C write
 -> clean 让数据对通信侧可见
 -> ready 宣布“可读了”
```

因此 ready 的真正语义不是“我算完”，而是：

> **我负责的 C 区域已经完成，并且满足后续通信读取的可见性条件。**

---

## 7. 为什么每核只 clean 自己的行

整块 C 都 clean 会增加额外 cache 操作。

M-split 已经明确每核 ownership：

```text
core b 只写自己的 rows
```

因此每核只 clean 自己写的 cache lines，职责清晰，也避免重复工作。

---

## 8. core0 等 ready 的本质是一个 barrier

不是通用 barrier primitive，而是数据依赖 barrier：

```text
while any ready[b][c] != generation:
    wait/poll
```

只有所有 C partition 对 SDMA 可见，才能 signal 整个 batch。

### 为什么不是每核各 signal 自己的 fragment？

当前协议把一个 batch 当成一个 communication payload，单 producer signal 也更符合当前 MemFabric 使用方式。

如果未来 public API 支持低开销多 fragment/tile completion，就可以重新评估。

---

## 9. wait kernel 在检查什么

peer mail 到达后不能只看“有消息”。

当前语义会严格核对类似：

```text
status
dst
len
imm / batch index
```

原因是通信 bug 最危险的不是没有数据，而是“收到别的 batch 的数据却当成当前 batch”。

---

## 10. Add kernel 为什么自己写

动态小 shape 用通用 `at::add_out` 可能引入不希望的图编译/固定开销。

自定义 Add 的目标非常朴素：

```text
for valid elements:
    out = local + peer
```

利用 AscendC TQue / double buffer 做向量加。

重点是低固定成本和可控 shape，不是复杂数学。

---

## 11. 设计审视：这套 producer 内核是不是终态？

### 可以演进的点

```text
M-split -> 自适应核数/二维切分
per-core ready -> counter/bitmap/设备 event
batch signal -> tile signal
cache clean -> 更精细/硬件一致性原语
core0 polling -> 更低开销 barrier
```

### 不能凭感觉动的点

```text
数据产生
可见性建立
完成发布
通信启动
```

顺序必须被某种机制继续保证。

### 什么 profiler 证据值得改

- core0 polling 时间明显；
- clean 占 producer 时间显著；
- 8 核负载不均；
- 通信只能等整批结束导致长尾明显；
- small M 下很多 core 实际利用率极低。

没有这些证据时，先别为了“代码更漂亮”改协议核心。

---

## 12. 本章最重要的一句话

`ready` 不是性能技巧，而是**发布协议**的一部分；只有在找到等价或更强的发布机制后才能替换。
