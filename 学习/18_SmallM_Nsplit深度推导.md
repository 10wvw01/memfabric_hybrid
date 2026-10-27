# 18｜Small-M N-split 深度推导：为什么小 M 会推翻大 M 的最优切法

> 本章位置：主线第四幕末端。这里最值得学的不是 N-split 本身，而是：**同一个算子在不同 shape 区间，瓶颈模型会变，原本正确的优化方向可能彻底失效。**

---

## 1. 大 M 下 M-split 为什么自然

大 M 时：

```text
A[M,2048] @ B[2048,2048]
```

让 8 核按 M 分行：

```text
core0 算 rows 0...
core1 算下一段...
...
```

优点：

```text
输出天然是普通 [M,N] layout
各核写不同 rows
分工简单
```

当每核 M 足够大时，完整 B 的读取成本可以被较多计算摊薄。

---

## 2. 小 M 时为什么问题变了

假设 M=16，8 核 M-split：

```text
每核平均只算 2 行
```

但每核为了这 2 行仍然要面对完整：

```text
B [2048,2048]
```

于是时间不再主要由 FLOPs 决定，而更像：

```text
T ≈ weight stream 固定成本 + launch/tiling + 少量 compute
```

这就是为什么“矩阵变小 16 倍，时间也变小 16 倍”通常不成立。

---

## 3. N-split 的核心思想

既然 M 太小，不值得继续按行切，那就按输出列切：

```text
N = 2048
8 core
每核 256 columns
```

每个 core：

```text
A [T,2048]
B_slice [2048,256]
C_slice [T,256]
```

等价地从 weight ownership 看：

```text
每核只需要约 1/8 的 B
```

这显著降低 small-M 下每核的 weight working set。

---

## 4. 为什么 A 反而要给所有 core

N-split 时 8 核都需要同一批输入 rows：

```text
A[T,2048]
```

但每核用不同 B slice 生成不同列。

也就是说：

```text
M-split：A 分片，B 近似全量
N-split：A 共享，B 分片
```

优化本质是选择“复制/共享哪一边更便宜”。

小 M 时 A 很小，所以共享 A 通常更划算。

---

## 5. 为什么用 T stair

实际 M 可以是：

```text
1,7,13,29,57,...
```

如果每个 M 都做一套动态 kernel/Graph shape，复杂度很高。

当前把它映射到：

```text
T ∈ {16,32,64,128,256}
```

例如：

```text
M=13 -> T=16
M=29 -> T=32
```

多出来的 rows 作为 padding，只在最终输出时消费 valid M。

这用少量“多算一点”换取：

```text
更稳定的 kernel shape
更少的 Graph/tiling 变体
```

---

## 6. 为什么输出先是 blocked layout

8 核按列各自产生：

```text
core0: [T,256]
core1: [T,256]
...
core7: [T,256]
```

自然写法是：

```text
[8][T][256]
```

但上层最终需要：

```text
[M,2048]
```

所以 small path 的 add kernel 不只是：

```text
local + peer
```

还需要：

```text
blocked add + unblock
```

这是一种典型 tradeoff：前面的 MM 更高效，后面多承担一点 layout 还原工作。

---

## 7. weight slice cache 为什么关键

如果每次 Decode 都临时：

```text
完整 B -> 切 8 份 -> 转 NZ
```

small-M 省下来的时间可能全被准备成本吃掉。

所以 runtime 预构建并缓存每核 weight slice。

当前每个 slice 对应约：

```text
[256,2048] FP16
```

8 份合计仍然约等于完整 8 MiB weight 的量级，只是物理组织更适合每核直接消费。

---

## 8. 为什么不是“少用几个 core”就够了

另一个候选方案：

```text
M=16 时只用 1~2 个 core
```

这样减少多核协调，但使用中的 core 仍可能读取完整 B。

它可能有效，也可能不如 N-split。

所以需要比较：

```text
1-core M-split
2-core M-split
4-core M-split
8-core M-split
8-core N-split
```

真正瓶颈如果是 weight stream，N-split 往往更有针对性；如果瓶颈是多核启动/同步，少核方案可能更简单。

---

## 9. 为什么历史实测不能当永久真理

当前源码注释里有某次目标环境测量，例如：

```text
full-B M-split 存在约百微秒级 weight-stream floor
1MiB N-slice MM 明显更低
某些 alternate tiling 反而更差
```

这些数据非常有价值，因为它解释了设计来源。

但必须标注：

```text
机器/CANN/代码/频率/workload 都可能变化
```

所以它们是“当时做决策的证据”，不是规格常数。

---

## 10. 设计审视：N-split 是 small-M 最终答案吗？

不是。

### 候选演进 1：自适应 core count

```text
M<=16  -> 1/2 core
M<=64  -> 4 core
更大   -> 8 core
```

### 候选演进 2：二维 M×N split

既不纯切行，也不纯切列，让 core tile 同时拥有局部 M/N。

### 候选演进 3：persistent weight locality

如果硬件/runtime 能让常用 weight 更长时间驻留更近的层次，small-M 的 weight-stream floor 可能再次改变。

### 候选演进 4：输出直接 normal layout

如果能让多个 core 高效地直接写最终 `[M,2048]` 布局，可省 unblock；但要评估写地址模式是否反而更差。

---

## 11. threshold 应该怎样产生

不要写：

```text
M<256 就 small path，因为 256 看起来整齐
```

应该测交点：

```text
T_normal(M)
T_small(M)
```

然后考虑真实概率：

```text
Expected cost = Σ P(M) * T_path(M)
```

DFlash 开关、K、并发变化都会改变 `P(M)`。

所以更成熟的实现应该允许 threshold 通过 profiling/autotune 产生，而不是永久硬编码。

---

## 12. 本章最重要的优化思维

> **shape 变化会改变瓶颈性质。**

大 M 时代价主要是算；小 M 时代价可能主要是 weight/launch/同步。

当性能模型变了，就应该允许算法结构也变，而不是强迫一个 kernel 覆盖所有 M。
