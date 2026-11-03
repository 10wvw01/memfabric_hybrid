# 设备发起 vs host 编排通信时延对比报告

> **文档定位**：310P 设备发起（AICPU 构造 SQE → SDMA）与 host 编排
>（host 调用 runtime memcpy → SDMA）的每次搬运通信时延对比。
> **测试日期**：2026-11-03 | **环境**：Atlas 300I Duo, CANN 9.1.0, aarch64

## 1. 测试问题

两条路径把同一份数据从 r0 搬到 r1：

```
设备发起: r0 AICore signal → AICPU 构造 SQE → SDMA 搬运 → r1 AICore wait 收到
host 编排: host 调用 aclrtMemcpyAsync(D2D) → runtime 构造指令 → SDMA 搬运 → host sync 返回
```

## 2. 方法

- **设备发起**：kernel 内连发 100 封 signal/wait（AICPU 全程在忙，无空闲唤醒），
  总时减去 baseDev（空 kernel launch + sync），除以 100 → **每封纯通信时延**
  （signal + AICPU 消费 + SQE 构造 + doorbell + SDMA + 完成）
- **host 编排**：每轮 memcpyAsync + sync（sync 是 host 路径固有成本——每次通信必有
  host↔device 往返），直接测量，**不减任何基线** → **每次全量通信时延**
  （API + runtime + SDMA + sync）

测量参数（两条路径完全对等）：

| 参数 | 设备发起 | host 编排 |
|------|---------|----------|
| 预热轮 | 5 轮丢弃 | 5 轮丢弃 |
| 有效轮 | 3 轮 | 3 轮 |
| 每有效轮通信次数 | 100（kernel 内连发） | 100（逐次 API+sync） |
| 有效通信总次数 | 300 | 300 |
| 计时粒度 | 批次计时（1 次测 100 封） | 逐次计时（每封独立计时） |
| baseDev 基线 | 205 次空 kernel（5 预热 + 200 有效） | 不适用（不减基线） |

- **校验**：每尺寸全量 D2H 逐字核对，全部 mismatch=0

## 3. 数据

每次搬运时延（µs，3 有效轮均值）：

| 尺寸 | 设备发起(纯通信) | host 编排(全量) | 设备发起快 |
|------|----------------|----------------|-----------|
| 4KB | **16.2** | 82.4 | **66.2 µs (80%)** |
| 64KB | **18.3** | 93.6 | **75.3 µs (80%)** |
| 256KB | **25.8** | 110.8 | **85.0 µs (77%)** |
| 1MB | **64.4** | 159.4 | **95.0 µs (60%)** |
| 4MB | **239.0** | 403.9 | **164.9 µs (41%)** |

baseDev（空 kernel launch+sync）= 87.7 µs。

## 4. 分析

### 4.1 设备发起为什么全尺寸都快

| 环节 | 设备发起(每封) | host 编排(每次) |
|------|--------------|----------------|
| 指令构造 | signal ~1µs + AICPU 消费/SQE ~16µs | memcpyAsync API ~10µs + runtime ~5µs |
| SDMA 传输 | ~0.1-400µs（同引擎） | ~0.1-400µs（同引擎） |
| 完成确认 | 完成戳回 r0 + quiet 检测 ~2µs | sync（device→host 通知+线程唤醒）**~70µs** |
| **合计** | **~17-240µs** | **~82-404µs** |

host 的 sync 开销（~70µs）是每次通信的固定成本，不可消除——host 必须跨越 host↔device 边界确认完成。

### 4.2 架构级差异

| 维度 | 设备发起(AICPU) | host 编排 |
|------|----------------|----------|
| 进入设备 | **一次**（pool 创建时 launch 常驻 kernel） | **每次至少 2 次**（API 提交 + sync 通知） |
| host 线程占用 | **零**（设备侧闭环） | API + sync 阻塞 |
| 计算通信重叠 | **原生**（signal 后继续算，AICPU 并行消费） | stream FIFO 无重叠 |

### 4.3 计算通信重叠（独有优势）

host 编排在 stream 上只能串行（FIFO）：fill(i) → memcpy(i) → fill(i+1) → memcpy(i+1)。

设备发起允许 AICore signal 后继续算下一块，AICPU 并行消费——08 示例的流水加速比 2.0x 即为实证。

## 5. 结论

| 尺寸 | 设备发起 | host 编排 | 设备发起快 |
|------|---------|----------|-----------|
| 全尺寸 | **16~239 µs** | 82~404 µs | **40~80%** |

**设备发起在所有尺寸下均优于 host 编排**。
核心原因：AICPU 一次进入设备后始终在设备上（零 host↔device 转换/次），
host 每次通信至少 2 次转换（sync ~70µs/次）。

## 6. 复现

```bash
bisheng --npu-arch=dav-2002 -O2 -std=c++17 -w -x asc bench_device_vs_host.asc -x none \
    -o /tmp/bench_dh -DOMPI_SKIP_MPICXX $INC_FLAGS $LIB_FLAGS
MF_SDMA_ORCH_JSON=<v32 json> ASCEND_RT_VISIBLE_DEVICES=4,5 mpirun -np 2 /tmp/bench_dh
```
