# 11：不同进程导入同一IPC对象后的地址

## 运行

```bash
source /usr/local/Ascend/cann/set_env.sh
export ASCEND_HOME=/usr/local/Ascend/cann
cd 310p-capability_matrix/11_ipc_va_determinism
DEV_LIST=2,6,2 bash run.sh
```

## 2026-11-13测试结果

Ascend310P3，CANN 9.2.1，驱动26.1.1，aarch64。

| 检查项 | 本轮结果 |
|---|---|
| 进程与设备 | rank0导出方设备2，rank1跨卡导入方设备6，rank2同设备导入方设备2 |
| 对象 | 4个16MiB设备块 |
| 四对象导入 | 三个进程均成功 |
| 首对象地址 | r0自导入0x12c180000000；r1、r2导入均为0x12c140000000 |
| 地址一致性 | 四对象均存在进程间地址差异 |
| 数据校验 | rank1对四个导入地址的异步D2D及D2H校验均通过 |
| 总结果 | PASS，三个rank退出0 |

四个对象均成功导入，跨进程地址存在差异。rank1完成四对象数据校验，rank2参加导入与地址比较。

## 原有实验过程与数据

以下保留CANN 9.1.0下的原有实验条件、步骤、输出及分析。2026-11-13的执行情况见上表。

Atlas 300I Duo (310P3) / CANN 9.1.0 / MPI 三进程
rank0=dev1(exporter), rank1=dev3(跨卡 importer), rank2=dev1(同设备 importer)

## 核心结论

**RESULT: PASS**

### 1. VA 一致性

| 观察 | 结论 |
|------|------|
| r1 == r2 | importer 间 VA 完全一致（跨卡 PEER + 同设备 DEFAULT） |
| r0 ≠ r1/r2 | exporter 自 import 得到不同 VA |
| r1/r2 VA = origin VA | importer 的 VA 等于 exporter 的 malloc 地址 |

**该次观察**：r1、r2导入VA相同，r0自导入VA不同；应用访问使用各进程实际地址和对象内偏移。

### 2. 带宽

dev1→dev3 = **3.98 GB/s**（同 socket，副→副），与 test 13 完全一致。

### 3. 历史带宽比较（test13的56个读取方向及6组写入对照）

| 场景 | dst=die0(主) | dst=die1(副) |
|------|-------------|-------------|
| 卡内 HCCS | ~19 GB/s | ~19 GB/s |
| 同 socket 跨卡 | ~11-12 GB/s | ~4 GB/s |
| 跨 socket | ~11-12 GB/s | ~1.25 GB/s |

**关键发现**：
- 表中dst设备类型与带宽存在明显关联
- 当时以主die及HCCS转发作为解释模型，带宽测量未直接跟踪内部事务
- 所列跨卡读取数据中，dst=die1的带宽较低

## 运行

```bash
./run.sh                          # 默认 DEV_LIST="1,3,1"
DEV_LIST="0,2,0" ./run.sh         # 换跨卡对
```
