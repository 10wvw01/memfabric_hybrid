# 13：IPC、VMM与Host中转的拷贝对照

## 运行

```bash
source /usr/local/Ascend/cann/set_env.sh
export ASCEND_HOME=/usr/local/Ascend/cann
cd 310p-capability_matrix/13_bw_compare
A_DEV=2 B_DEV=3 bash run.sh
A_DEV=2 B_DEV=6 bash run.sh
```

## 2026-11-13测试结果

Ascend310P3，CANN 9.2.1，驱动26.1.1，aarch64。

| 检查项 | 本轮结果 |
|---|---|
| 同卡2→3 | IPC、VMM和Host中转均校验通过，退出0 |
| 跨卡2→6 | IPC、VMM和Host中转均校验通过，退出0 |
| 测量规模 | 3次预热、30次16MiB；异步提交后同步，Host中转包含D2H和H2D两段 |

同卡IPC使用DEFAULT导入，VMM切换导出方上下文；跨卡IPC使用PEER导入，VMM按用例启用peer。下表仅对应这两组设备和本轮调用方式。

| 设备 | IPC GB/s | VMM GB/s | Host中转GB/s |
|---|---:|---:|---:|
| 2→3，同卡 | 19.01 | 19.05 | 3.32 |
| 2→6，跨卡 | 12.91 | 12.63 | 3.92 |

日志分别为 `13_bw_compare-2-3.log` 和 `13_bw_compare.log`。

## 原有实验过程与数据

以下保留CANN 9.1.0下的原有实验条件、步骤、数据和分析；2026-11-13的执行情况见上表。

本测试程序用于测量 Atlas 300I Duo (310P3) 加速卡在不同拓扑下的跨卡 P2P 带宽，涵盖三种传输后端（IPC、VMM、host-swap），并深入分析 PCIe 事务类型对带宽的影响。

---

## 一、硬件架构

### 1.1 系统拓扑

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                         CPU Socket 0 (NUMA 0,1)                             │
│  ┌─────────────────────┐    ┌─────────────────────┐                         │
│  │ 卡0 (NPU 0,1)       │    │ 卡1 (NPU 2,3)       │                         │
│  │ PCIe 4.0 x8         │    │ PCIe 4.0 x16        │                         │
│  │ ┌─────┐   ┌─────┐   │    │ ┌─────┐   ┌─────┐   │                         │
│  │ │die0 │←→│die1 │   │    │ │die0 │←→│die1 │   │                         │
│  │ │(主) │HCCS│(副) │   │    │ │(主) │HCCS│(副) │   │                         │
│  │ └─────┘~24GB/s└─────┘   │    │ └─────┘~24GB/s└─────┘   │                         │
│  └─────────────────────┘    └─────────────────────┘                         │
└─────────────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────────────┐
│                         CPU Socket 1 (NUMA 2,3)                             │
│  ┌─────────────────────┐    ┌─────────────────────┐                         │
│  │ 卡2 (NPU 4,5)       │    │ 卡3 (NPU 6,7)       │                         │
│  │ PCIe 4.0 x8         │    │ PCIe 4.0 x16        │                         │
│  │ ┌─────┐   ┌─────┐   │    │ ┌─────┐   ┌─────┐   │                         │
│  │ │die0 │←→│die1 │   │    │ │die0 │←→│die1 │   │                         │
│  │ │(主) │HCCS│(副) │   │    │ │(主) │HCCS│(副) │   │                         │
│  │ └─────┘~24GB/s└─────┘   │    │ └─────┘~24GB/s└─────┘   │                         │
│  └─────────────────────┘    └─────────────────────┘                         │
└─────────────────────────────────────────────────────────────────────────────┘
```

**关键特性：**
- 每张卡 2 个 die，通过 HCCS 互联（24Gbps × 8 lanes = 24 GB/s）
- **die0 是 PCIe 主 die**（偶数号设备 0,2,4,6）
- **die1 是副 die**（奇数号设备 1,3,5,7）
- 跨卡 P2P 数据必须先到 dst 的 die0，再经 HCCS 转发到 die1

### 1.2 卡内架构

```
┌─────────────────────────────────────────┐
│              310P3 加速卡                │
│                                         │
│  ┌─────────────┐      ┌─────────────┐  │
│  │   die1      │      │   die0      │  │
│  │   (副die)   │      │   (主die)   │←─┼── PCIe 接口
│  │             │      │             │  │
│  │  ┌───────┐  │      │  ┌───────┐  │  │
│  │  │ HBM   │  │      │  │ HBM   │  │  │
│  │  └───────┘  │      │  └───────┘  │  │
│  └──────┬──────┘      └──────┬──────┘  │
│         │                    │         │
│         └────── HCCS ────────┘         │
│              (8 lanes, 24Gbps)         │
└─────────────────────────────────────────┘
```

**数据路径：**
- 副 die 的数据必须经过 HCCS 到主 die，然后由主 die 的 PCIe DMA 处理
- 没有传统意义上的"PCIe↔HCCS 桥接"

### 1.3 卡间 P2P 数据路径

**同 socket 跨卡：**
```
src NPU → PCIe → CPU PCIe Root Port → CPU 内部 PCIe Fabric → CPU PCIe Root Port → PCIe → dst NPU
                    ↑ 不经过 host 内存，但经过 CPU 内部 fabric
```

**跨 socket 跨卡：**
```
src NPU → PCIe → CPU0 → Kunpeng 片间互联 → CPU1 → PCIe → dst NPU
```

### 1.4 互联带宽对比

| 互联类型 | 理论单向带宽 | 实测带宽 | 效率 |
|---------|-------------|---------|------|
| HCCS (8 lanes × 24Gbps) | ~23.6 GB/s | ~19 GB/s | 80% |
| PCIe 4.0 x16 | ~31.5 GB/s | ~11-12 GB/s | 35-38% |
| PCIe 4.0 x8 | ~15.75 GB/s | ~11-12 GB/s | 70-76% |

**关键发现：**
- HCCS 是专用互联，效率高（80%）
- PCIe P2P 经过 CPU fabric，有额外开销
- PCIe x8 和 x16 的卡间带宽相同（~11-12 GB/s），说明瓶颈不在 lane 数量

---

## 二、测试方法

### 2.1 三种传输后端

| 后端 | 说明 | API |
|------|------|-----|
| **IPC** | 进程间内存共享 | `aclrtIpcMemGetExportKey` / `aclrtIpcMemImportByKey` |
| **VMM** | 虚拟内存管理 | `aclrtMallocPhysical` / `aclrtReserveMemAddress` / `aclrtMapMem` |
| **host-swap** | 主机内存中转 | D2H + H2D 两步 |

### 2.2 带宽测量口径

```c
// 3 次预热 + 30 次 16MB MemcpyAsync + 1 次 StreamSync
for (int i = 0; i < BW_WARMUP; i++) {
    aclrtMemcpyAsync(local, BLOCK_SIZE, ipc_va, BLOCK_SIZE, ACL_MEMCPY_DEVICE_TO_DEVICE, stream);
    aclrtSynchronizeStream(stream);
}
double t0 = now_sec();
for (int i = 0; i < BW_ITERS; i++)
    aclrtMemcpyAsync(local, BLOCK_SIZE, ipc_va, BLOCK_SIZE, ACL_MEMCPY_DEVICE_TO_DEVICE, stream);
aclrtSynchronizeStream(stream);
double t1 = now_sec();
bandwidth = (BW_ITERS * BLOCK_SIZE) / (t1 - t0);
```

**严禁同步循环测带宽**（历史教训：失败链路上同步调用只量到失败返回延迟，伪造假带宽）

### 2.3 运行方法

```bash
./run.sh                        # 默认 die 对 A_DEV=0 B_DEV=1
A_DEV=0 B_DEV=2 ./run.sh        # 跨卡 (dev0→dev2)
A_DEV=2 B_DEV=3 ./run.sh        # 换 die 对
./run_reverse.sh                # Push 模式测试
```

---

## 三、测试结果

### 3.1 测试覆盖

共 **56 组** Pull 模式测试，全部 PASS：
- 8 组 die 对（卡内 die0↔die1 双向）
- 16 组同 socket 跨卡（socket 0 全部 8 方向 + socket 1 全部 8 方向）
- 32 组跨 socket（card0↔card2/3, card1↔card2/3 全部方向）
- 每种拓扑覆盖 src die0/die1 × dst die0/die1 全部方向

另补充 **Push 模式**测试（使用 `test_reverse.c`），覆盖关键场景。

### 3.2 die 间通信（卡内 HCCS 直传）

| 卡 | src→dst | src die | dst die | IPC | VMM | host-swap |
|----|---------|---------|---------|-----|-----|-----------|
| 卡0 | 0→1 | die0 (主) | die1 (副) | 19.03 | 19.03 | 3.11 |
| 卡0 | 1→0 | die1 (副) | die0 (主) | 19.01 | 19.02 | 3.30 |
| 卡1 | 2→3 | die0 (主) | die1 (副) | 19.03 | 19.03 | 3.46 |
| 卡1 | 3→2 | die1 (副) | die0 (主) | 19.05 | 19.05 | 2.98 |
| 卡2 | 4→5 | die0 (主) | die1 (副) | 19.00 | 19.02 | 2.07 |
| 卡2 | 5→4 | die1 (副) | die0 (主) | 19.02 | 19.01 | 2.33 |
| 卡3 | 6→7 | die0 (主) | die1 (副) | 19.03 | 19.04 | 2.07 |
| 卡3 | 7→6 | die1 (副) | die0 (主) | 19.02 | 19.03 | 2.38 |

**结论：** 卡内 die 间双向带宽对称，均 ~19 GB/s，方向无影响

### 3.3 同 socket 跨卡（PCIe P2P）

#### Socket 0: card0 ↔ card1 (x8 ↔ x16)

| src→dst | src die | dst die | IPC | VMM | host-swap |
|---------|---------|---------|-----|-----|-----------|
| 0→2 | die0 (主) | die0 (主) | 11.09 | 11.70 | 3.49 |
| 0→3 | die0 (主) | die1 (副) | 4.17 | 4.18 | 3.02 |
| 1→2 | die1 (副) | die0 (主) | 11.66 | 11.47 | 3.61 |
| 1→3 | die1 (副) | die1 (副) | 3.98 | 3.98 | 3.02 |
| 2→0 | die0 (主) | die0 (主) | 12.36 | 12.37 | 3.87 |
| 2→1 | die0 (主) | die1 (副) | 4.37 | 4.37 | 3.24 |
| 3→0 | die1 (副) | die0 (主) | 12.29 | 12.30 | 3.51 |
| 3→1 | die1 (副) | die1 (副) | 4.00 | 4.00 | 3.30 |

#### Socket 1: card2 ↔ card3 (x8 ↔ x16)

| src→dst | src die | dst die | IPC | VMM | host-swap |
|---------|---------|---------|-----|-----|-----------|
| 4→6 | die0 (主) | die0 (主) | 11.14 | 11.75 | 1.96 |
| 4→7 | die0 (主) | die1 (副) | 4.15 | 4.15 | 1.91 |
| 5→6 | die1 (副) | die0 (主) | 11.42 | 11.34 | 2.28 |
| 5→7 | die1 (副) | die1 (副) | 3.94 | 3.46 | 2.30 |
| 6→4 | die0 (主) | die0 (主) | 12.36 | 12.38 | 2.07 |
| 6→5 | die0 (主) | die1 (副) | 4.38 | 4.37 | 2.36 |
| 7→4 | die1 (副) | die0 (主) | 12.30 | 12.31 | 2.07 |
| 7→5 | die1 (副) | die1 (副) | 4.00 | 4.00 | 1.84 |

**结论：** 同 socket 跨卡，dst=die0 时 ~11-12 GB/s，dst=die1 时 ~4 GB/s

### 3.4 跨 socket 通信（PCIe P2P + Kunpeng 片间互联）

> **术语说明：** 本文档使用 "Kunpeng 片间互联" 而非 Intel 的 "QPI/UPI"。
> 本测试平台使用华为 Kunpeng 920 ARM 处理器，其 socket 间采用华为自研的片间互联协议。

#### card0 ↔ card2 (x8 ↔ x8)

| src→dst | src die | dst die | IPC | VMM | host-swap |
|---------|---------|---------|-----|-----|-----------|
| 0→4 | die0 (主) | die0 (主) | 11.60 | 11.62 | 2.68 |
| 0→5 | die0 (主) | die1 (副) | 1.29 | 1.29 | 2.34 |
| 1→4 | die1 (副) | die0 (主) | 11.38 | 11.61 | 2.71 |
| 1→5 | die1 (副) | die1 (副) | 1.29 | 1.29 | 2.65 |
| 4→0 | die0 (主) | die0 (主) | 11.07 | 11.48 | 2.32 |
| 4→1 | die0 (主) | die1 (副) | 1.29 | 1.29 | 2.22 |
| 5→0 | die1 (副) | die0 (主) | 11.70 | 11.55 | 2.49 |
| 5→1 | die1 (副) | die1 (副) | 1.29 | 1.29 | 1.82 |

#### card1 ↔ card3 (x16 ↔ x16)

| src→dst | src die | dst die | IPC | VMM | host-swap |
|---------|---------|---------|-----|-----|-----------|
| 2→6 | die0 (主) | die0 (主) | 12.27 | 12.11 | 2.75 |
| 2→7 | die0 (主) | die1 (副) | 1.24 | 1.24 | 3.14 |
| 3→6 | die1 (副) | die0 (主) | 11.19 | 11.21 | 2.84 |
| 3→7 | die1 (副) | die1 (副) | 1.23 | 1.23 | 2.68 |
| 6→2 | die0 (主) | die0 (主) | 12.26 | 12.30 | 2.55 |
| 6→3 | die0 (主) | die1 (副) | 1.23 | 1.24 | 2.14 |
| 7→2 | die1 (副) | die0 (主) | 11.08 | 11.10 | 2.46 |
| 7→3 | die1 (副) | die1 (副) | 1.23 | 1.23 | 2.36 |

#### card0 ↔ card3 (x8 ↔ x16)

| src→dst | src die | dst die | IPC | VMM | host-swap |
|---------|---------|---------|-----|-----|-----------|
| 0→6 | die0 (主) | die0 (主) | 11.69 | 11.54 | 2.81 |
| 0→7 | die0 (主) | die1 (副) | 1.25 | 1.25 | 2.60 |
| 1→6 | die1 (副) | die0 (主) | 11.14 | 11.48 | 2.97 |
| 1→7 | die1 (副) | die1 (副) | 1.25 | 1.25 | 3.07 |
| 6→0 | die0 (主) | die0 (主) | 11.96 | 11.97 | 2.56 |
| 6→1 | die0 (主) | die1 (副) | 1.28 | 1.28 | 2.11 |
| 7→0 | die1 (副) | die0 (主) | 11.21 | 11.13 | 2.64 |
| 7→1 | die1 (副) | die1 (副) | 1.28 | 1.28 | 2.12 |

#### card1 ↔ card2 (x16 ↔ x8)

| src→dst | src die | dst die | IPC | VMM | host-swap |
|---------|---------|---------|-----|-----|-----------|
| 2→4 | die0 (主) | die0 (主) | 11.97 | 11.98 | 2.76 |
| 2→5 | die0 (主) | die1 (副) | 1.28 | 1.28 | 2.53 |
| 3→4 | die1 (副) | die0 (主) | 11.18 | 11.18 | 3.05 |
| 3→5 | die1 (副) | die1 (副) | 1.28 | 1.28 | 2.40 |
| 4→2 | die0 (主) | die0 (主) | 11.25 | 11.27 | 2.34 |
| 4→3 | die0 (主) | die1 (副) | 1.25 | 1.25 | 2.28 |
| 5→2 | die1 (副) | die0 (主) | 10.90 | 11.14 | 2.34 |
| 5→3 | die1 (副) | die1 (副) | 1.25 | 1.25 | 2.08 |

**结论：** 跨 socket，dst=die0 时 ~11-12 GB/s，dst=die1 时 ~1.25 GB/s

### 3.5 汇总矩阵

| 场景 | dst=die0 (主) | dst=die1 (副) | 差异倍数 |
|------|---------------|---------------|---------|
| die 间 (卡内 HCCS) | ~19 GB/s | ~19 GB/s | 1x (无差异) |
| 同 socket 跨卡 | ~11-12 GB/s | ~4 GB/s | 3x |
| 跨 socket | ~11-12 GB/s | ~1.25 GB/s | 9x |

---

## 四、带宽差异分析

4.1和4.2为实测对照；4.3～4.7保留当时依据PCIe事务与卡内转发建立的解释模型。图中的事务顺序用于解释假设，Completion时延和DMA并发度尚未直接测量。

### 4.1 Push 模式测试数据

为验证 Pull/Push 模式差异，使用 `test_reverse.c` 补充测试：

**同 socket 跨卡 (card0↔card1, Socket 0)**

| src→dst | src die | dst die | Pull IPC | Push IPC | 差异 |
|---------|---------|---------|----------|----------|------|
| 0→2 | die0 (主) | die0 (主) | 11.09 | 12.90 | +1.81 |
| 0→3 | die0 (主) | die1 (副) | 4.17 | **12.95** | **+8.78 (3.1x)** |
| 2→0 | die0 (主) | die0 (主) | 12.36 | 12.88 | +0.52 |
| 2→1 | die0 (主) | die1 (副) | 4.37 | **12.92** | **+8.55 (2.9x)** |

**跨 socket (card0→card2)**

| src→dst | src die | dst die | Pull IPC | Push IPC | 差异 |
|---------|---------|---------|----------|----------|------|
| 0→4 | die0 (主) | die0 (主) | 11.60 | 13.11 | +1.51 |
| 0→5 | die0 (主) | die1 (副) | 1.29 | **13.08** | **+11.79 (10.1x)** |

**关键发现：**
- Push 模式下，dst=die0 和 dst=die1 带宽一致（~13 GB/s）
- Pull 模式下，dst=die1 带宽骤降（同 socket ~4 GB/s，跨 socket ~1.25 GB/s）
- Push 模式可完全绕过 dst=die1 的带宽瓶颈

### 4.2 四种带宽数据汇总

| 模式 | dst=die0 | dst=die1 | 说明 |
|------|----------|----------|------|
| **Pull (读)** | ~11-12 GB/s | ~4 GB/s (同 socket) / ~1.25 GB/s (跨 socket) | 3x-10x 差异 |
| **Push (写)** | ~13 GB/s | ~13 GB/s | 基本一致 |

### 4.3 PCIe 事务类型

**Posted 事务（Memory Write）：**
```
Master                    Slave
  |--- Write TLP -------->|
  |    (数据)              |
  |                        |--> 写入完成
  |    (无需返回)          |
```
- 本测试提交异步写入后，仍调用`aclrtSynchronizeStream`等待完成并读回校验
- 单向数据流
- 延迟低，带宽高

**Non-Posted 事务（Memory Read）：**
```
Master                    Slave
  |--- Read TLP --------->|
  |    (地址)              |
  |                        |--> 读取数据
  |<-- Completion TLP -----|
  |    (数据)              |
  |--> 接收完成            |
```
- 发送方必须等待 Completion TLP
- 双向数据流
- 延迟高，带宽受限

### 4.4 四种情况的数据路径示意

#### 情况1：Pull dst=die0（~11-12 GB/s）

```
rank0 (die0) 读 rank1 (die0)：

数据流：
1. rank0 DMA 发起 Read TLP
2. Read TLP → PCIe → rank1 die0
3. rank1 die0 直接访问本地 HBM
4. Completion TLP → PCIe → rank0

路径：rank0 → PCIe → rank1 die0 → HBM → PCIe → rank0

延迟分析：
- Read TLP 传输：低延迟
- HBM 访问：低延迟（本地）
- Completion TLP 返回：低延迟（无需 HCCS）

DMA 流水线：
Read#1 → Read#2 → Read#3 → ...
  ↓        ↓        ↓
Comp#1   Comp#2   Comp#3

Completion 延迟低，流水线保持满载
带宽：~11-12 GB/s
```

#### 情况2：Pull dst=die1（~4 GB/s）

```
rank0 (die0) 读 rank1 (die1)：

数据流：
1. rank0 DMA 发起 Read TLP
2. Read TLP → PCIe → rank1 die0
3. rank1 die0 通过 HCCS 向 die1 请求数据
4. die1 HBM 返回数据
5. 数据：die1 → HCCS → die0
6. die0 返回 Completion TLP → PCIe → rank0

路径：rank0 → PCIe → rank1 die0 → HCCS → die1 → HBM → HCCS → die0 → PCIe → rank0

延迟分析：
- Read TLP 传输：低延迟
- HCCS 转发到 die1：额外延迟
- HBM 访问：低延迟
- 数据通过 HCCS 返回 die0：额外延迟
- Completion TLP 返回：低延迟

关键：Completion 需要等待 die1 的数据通过 HCCS 传到 die0

DMA 流水线：
Read#1 → Read#2 → Read#3 → ...
  ↓        ↓        ↓
  ↓        ↓     Comp#3 (延迟高)
  ↓     Comp#2
Comp#1 (延迟更高)

Completion 延迟高（需要额外 HCCS 往返）
DMA 引擎等待 Completion，无法发送新请求
流水线出现气泡，效率下降
带宽：~4 GB/s
```

#### 情况3：Push dst=die0（~11-12 GB/s）

```
rank0 (die0) 写 rank1 (die0)：

数据流：
1. rank0 DMA 发起 Write TLP
2. Write TLP → PCIe → rank1 die0
3. rank1 die0 直接写入本地 HBM
4. 无需返回

路径：rank0 → PCIe → rank1 die0 → HBM

延迟分析：
- Write TLP 传输：低延迟
- HBM 写入：低延迟（本地）
- 无需等待 Completion

DMA 流水线：
Write#1 → Write#2 → Write#3 → ...
（无需等待，连续发送）

DMA 引擎持续发送 Write
流水线满载
带宽：~11-12 GB/s
```

#### 情况4：Push dst=die1（~13 GB/s）

```
rank0 (die0) 写 rank1 (die1)：

数据流：
1. rank0 DMA 发起 Write TLP
2. Write TLP → PCIe → rank1 die0
3. rank1 die0 通过 HCCS 把数据写到 die1 HBM
4. 无需返回

路径：rank0 → PCIe → rank1 die0 → HCCS → die1 → HBM

延迟分析：
- Write TLP 传输：低延迟
- HCCS 转发到 die1：额外延迟
- HBM 写入：低延迟
- 无需等待 Completion

关键：虽然需要 HCCS，但 Write 是 Posted，不需要等待

DMA 流水线：
Write#1 → Write#2 → Write#3 → ...
（无需等待，连续发送）

DMA 引擎持续发送 Write
流水线满载
带宽：~13 GB/s
```

### 4.5 DMA流水线解释模型

| 模式 | Outstanding 请求 | Completion 延迟 | 流水线效率 | 带宽 |
|------|------------------|-----------------|-----------|------|
| Pull dst=die0 | 未测量（原估计32） | 低 | 高 | ~11-12 GB/s |
| Pull dst=die1 | 未测量（原估计32） | 高（需要 HCCS 往返） | 低（出现气泡） | ~4 GB/s |
| Push dst=die0 | 未测量 | 无 | 高 | ~11-12 GB/s |
| Push dst=die1 | 未测量 | 无 | 高 | ~13 GB/s |

### 4.6 PCIe 4.0 带宽验证

| 指标 | 值 |
|------|-----|
| PCIe 4.0 x8 理论带宽 | 16 GT/s × 8 lanes × (128/130) / 8 = **15.75 GB/s** |
| Pull dst=die0 实测 | 11.5 GB/s，效率 73% |
| Push dst=die1 实测 | 13 GB/s，效率 83% |

**观察**：该组结果达到x8理论编码带宽的73%～83%；具体带宽损失需要结合事务、并发度和拓扑测量。

### 4.7 原因假设汇总

| 因素 | 当时判断（待测量） | 解释 |
|------|---------|------|
| PCIe 链路带宽 | 否 | 73-83% 效率合理 |
| HCCS 链路带宽 | 否 | Push 模式可达 13 GB/s |
| **Completion 延迟** | **是** | dst=die1 时额外 HCCS 往返导致延迟增加 |
| **DMA 流水线效率** | **是** | 高延迟导致流水线气泡，带宽下降 |

**对应解释模型：**
1. PCIe Read 是 Non-Posted 事务，需要等待 Completion TLP
2. dst=die1 时，Completion 路径多一次 HCCS 往返，延迟更高
3. 高延迟导致 DMA 流水线效率下降，带宽降低
4. PCIe Write 是 Posted 事务，不需要等待 Completion，该模型下对读Completion往返的依赖较小

### 4.8 传输完成与数据校验

原分析进一步讨论了PCIe链路应答与最终设备数据可见性的区别。这个问题需要沿调用顺序检查：

```text
应用提交aclrtMemcpyAsync
    ↓
运行时及设备执行传输
    ↓
aclrtSynchronizeStream返回
    ↓
接收方D2H读回
    ↓
逐字比较预期pattern
```

上述实验保留了读回校验；下列故障场景尚未注入测试：

```text
待验证的异常场景：
发送端提交 → 对端PCIe入口 → 卡内转发 → 目标内存
                         ↑         ↑           ↑
                   各阶段故障如何上报给API与stream？
```

| 检查内容 | Pull（读） | Push（写） | 当前记录 |
|---|---|---|---|
| 提交调用是否成功 | 检查API返回值 | 检查API返回值 | 属于程序正确性检查 |
| stream是否完成 | 等待stream同步 | 等待stream同步 | 再进行数据校验 |
| 实际接收数据 | 从本地目标缓冲区D2H读回 | 从被写入的共享物理内存D2H读回 | 用带rank/tag/index的pattern比较 |
| 目标设备异常或转发故障 | 待测试 | 待测试 | 未做故障注入 |
| 超时、设备错误的上报 | 待测试 | 待测试 | 需要记录API及stream返回值 |

原文根据Posted/Non-Posted事务推断“Push可能静默丢失、Pull没有该风险”，还给出了未测量的错误率。这里修正为待验证问题，保留完成通知与最终数据校验的分析过程。实际应用按传输API、stream完成结果及数据校验判断本次传输。

---

## 五、结论与建议

### 5.1 核心结论

1. **die0 是 PCIe 主 die**：跨卡 P2P 数据先到 dst 的 die0，再经 HCCS 转发到 die1
2. **历史矩阵中的方向差异**：所列跨卡读取结果与dst die类型存在明显关联
3. **dst=die1 带宽骤降（Pull 模式）**：
   - 同 socket：~4 GB/s（Pull 模式的 3x 差异）
   - 跨 socket：~1.25 GB/s（Pull 模式的 10x 差异）
4. **Push 模式可绕过 dst=die1 瓶颈**：跨卡 dst=die1 时，Push 模式带宽 ~13 GB/s，与 dst=die0 一致
5. **IPC 与 VMM 性能等价**：全部测试中两种后端带宽一致（个别异常点除外，如 Socket 1 的 5→7 VMM=3.46 vs IPC=3.94）

### 5.2 性能建议

1. **跨卡通信优先使用 dst die0**（偶数号设备）作为接收端
2. **如必须使用 dst die1，优先采用 Push 模式**（src 主动写）
3. **避免跨 socket + dst=die1 + Pull 模式**（最差场景，仅 ~1.25 GB/s）
4. **数据完整性检查**：读取与写入方式均等待传输完成，并检查接收数据
