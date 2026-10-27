# 19｜MemFabric 协议与 Graph 正确性：把“不会错”证明出来

> 本章位置：主线第四幕收口。目标是把 MM+AR 的同步协议写成可以推理的 happens-before，而不是依赖“实机好像没挂”。

---

## 1. 两类状态先分开

### 数据平面

真正的大块 tensor：

```text
local partial
send arena
recv arena
reduced output
```

### 控制平面

告诉双方“数据处于什么生命周期”：

```text
ready
generation
wave sequence
credit/gate
ack
mail status
```

很多 silent corruption 都来自“数据是对的，但控制状态串轮了”。

---

## 2. 单 batch 的正确性链

设 rank0 的 batch b：

```text
MM0(b)
  happens-before
Clean0(b)
  happens-before
Ready0(b, all cores)
  happens-before
Signal0(b)
  happens-before
Recv1(b) complete
  happens-before
Add1(b)
```

rank1 对称执行。

最终每个 rank 都得到：

```text
Y0[b] + Y1[b]
```

---

## 3. 为什么 signal 里的 batch identity 重要

如果只知道“peer 有一段数据到了”，却不知道是哪一批：

```text
batch1 的数据
被 batch0 的 wait 消费
```

结果 shape/长度甚至可能一致，错误很难发现。

所以 wait 需要检查：

```text
status
dst
length
imm / batch index
```

控制元数据也是 correctness 数据。

---

## 4. wave-level reuse 证明

设 arena slot S 在 wave n 被使用。

要允许 wave n+1 覆盖 S，至少要证明：

```text
① 本地对 S 的 producer 不再写
② 相关 SDMA 不再读/写 S
③ peer 已完成对这一轮数据的消费
④ 双方对 wave 顺序有一致认识
```

当前把这些条件收敛到：

```text
wait/add 完成
 -> quiet
 -> ack/credit
 -> next wave gate
```

这比“sleep 一会儿再复用”可证明得多。

---

## 5. 为什么 `quiet` 放 wave 尾

`quiet` 语义是对异步通信做整体收尾。

如果每 batch 都 quiet：

```text
通信 pipeline 被频繁 drain
```

放 wave 尾则允许：

```text
batch0 SDMA
batch1 SDMA
batch2 SDMA
```

在前面阶段保持 in-flight，只在资源复用前统一确认安全。

所以 quiet 既是 correctness primitive，也是性能调度点。

---

## 6. eager generation 的证明

若 ready cell 初始值可能是旧 generation `G_old`，本 wave 期待 `G_new`：

```text
G_new != G_old
```

在某 core 真正完成并写 `G_new` 前，core0 不会误判。

因此无需把所有 cell 先清 0。

这里隐含前提：generation 在生命周期内不会以危险方式回绕碰撞。

如果 worker 活得极久、generation 位宽有限，就必须分析 wrap-around。

---

## 7. Graph replay 为什么不能直接沿用 eager 自增

capture 时记录的是设备命令，不会在每次 replay 都重新执行一段 host 逻辑去生成新 tag。

如果 Graph 每次都期待同一个 generation，而旧 ready 还保留相同值：

```text
replay 立即误判完成
```

所以当前 Graph 路径需要先把 control region 清到不匹配状态，再执行固定 generation 的 producer。

```text
clear ready
 -> MM/clean
 -> write fixed generation
 -> signal
```

这样每次 replay 都重新建立正确发布关系。

---

## 8. 为什么 eager 和 Graph generation 空间最好分离

如果 eager 与 Graph 共用同一小范围 tag：

```text
capture 前/后切模式
旧 control state
```

容易产生碰撞推理复杂度。

当前 eager 使用高区间 monotonic tag、Graph 使用独立固定值，是在降低跨模式状态别名风险。

---

## 9. Graph capture 前需要建立哪些不变量

进入 capture 前，至少希望：

```text
context 已创建
layout 已协商
arena 地址固定
scratch 已分配
weight slice 已构建
q 已锁定
kernel 首次初始化已完成
credit 初态一致
无未知 in-flight communication
```

Graph 只应该捕获“稳定执行阶段”，而不是“资源建站阶段”。

---

## 10. 双 rank Graph 为什么尤其危险

单卡 Graph 只要本地顺序正确；双 rank Graph 还存在：

```text
rank0 capture/replay 进度
rank1 capture/replay 进度
通信 wave identity
```

如果两边进入不同 epoch，却仍对同一 arena/signal 解释不同，会 hang 或错读。

因此 Graph 生命周期必须和通信协议生命周期一起设计，不能两支团队分别做完再拼。

---

## 11. 异常后的协议为什么不容易恢复

例如 rank0 已 signal batch3，rank1 在 wait batch2 时失败。

此时可能存在：

```text
rank0 已进入更多 in-flight
rank1 没有 ack
arena ownership 不一致
```

单纯 retry 当前 op 无法恢复共同状态。

所以当前 poison + shutdown 是合理保守选择。

真正的 recovery 需要：

```text
全局 epoch reset
取消/quiet in-flight
双方重新 handshake
重新初始化 credit/control
```

这是未来可靠性功能，不是 catch exception 就能实现。

---

## 12. 设计审视：协议下一代怎么演进

### 更深流水

```text
单 wave credit
 -> 多 wave sliding window
```

可提高并发，但需要每 slot sequence/ownership 更严格。

### 更细粒度通信

```text
batch completion
 -> tile/chunk completion
```

能进一步 overlap，但控制消息数量与固定开销会上升。

### 统一 eager/Graph epoch

长期可考虑显式：

```text
execution_epoch = {mode, graph_id, replay_seq, wave_seq}
```

让 tag 空间和资源生命周期更统一。

### 恢复协议

如果业务要求不中断恢复，需要单独设计 protocol reset，而不是削弱 poison。

---

## 13. 什么证据说明值得升级协议

只有当 profiler/稳定性数据显示：

- wave gate 空等明显；
- batch 粒度太粗造成可见通信尾；
- Graph clear/control 占比高；
- 当前错误恢复导致不可接受的服务可用性；

才应承担更复杂协议的工程成本。

---

## 14. 本章最终检查

你应该能解释：

```text
ready 保护 core-to-core 发布
signal/wait 保护 rank-to-rank batch identity
quiet 保护 in-flight completion
ack/credit 保护 arena reuse
generation 保护跨 wave 的 control reuse
Graph clear 保护固定 generation replay
```

能把这六层分清，才算真正理解当前 MM+AR 协议。
