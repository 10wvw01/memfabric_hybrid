# 11_ipc_va_determinism：IPC 导入 VA 对比


## 测试内容

三个进程按相同顺序导入四个 IPC key，逐项比较 VA，并验证跨卡数据读取。

## 本轮条件与结果

2026-11-11，Ascend310P3 / Atlas 300I Duo，aarch64，CANN 9.2.1，驱动 26.1.0。

DEV_LIST=2,4,2：rank0 为导出方，rank1 跨卡，rank2 与导出方同设备；每块 16 MiB。

**功能通过，退出码 0；观测到 VA MISMATCH。**

导入和数据读取成功，但各进程 VA 不全相等。该程序将 VA 是否一致作为观测项，不能将 RESULT: PASS 解读为所有进程同址。设备列表需要保留 rank1 跨卡的测试条件。

## 运行

在仓库根目录执行，设备号以当前进程可见的编号为准：

```bash
source /usr/local/Ascend/cann/set_env.sh
DEV_LIST=2,4,2 bash 310p-capability_matrix/11_ipc_va_determinism/run.sh
```

脚本先编译，传递编译和运行的退出码；超时返回 124，超时后强制结束可能返回 137。
本轮条件、退出码与关键输出收录在[结果记录](../results/2026-11-11.json)。

## 详细测试步骤、API 调用链与历史性能数据

以下保留原文的详细流程、结果和性能数据，环境及设备组合以各段原始记录为准。
本轮结果见上文；涉及不同环境或未复测拓扑时，分别阅读，不合并为一次测试结果。

### 11_ipc_va_determinism — IPC VA 确定性测试

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

**结论**：对称 GVA 池部分可行——importer 间可用同一指针，exporter 直接用 origin VA。

### 2. 带宽

dev1→dev3 = **3.98 GB/s**（同 socket，副→副），与 test 13 完全一致。

### 3. 带宽归因（基于 test 13 的 62 组数据）

| 场景 | dst=die0(主) | dst=die1(副) |
|------|-------------|-------------|
| 卡内 HCCS | ~19 GB/s | ~19 GB/s |
| 同 socket 跨卡 | ~11-12 GB/s | ~4 GB/s |
| 跨 socket | ~11-12 GB/s | ~1.25 GB/s |

**关键发现**：
- 带宽由 **dst die 类型** 决定，src die 无影响
- die0 是 PCIe 主 die，数据先到 die0 再经 HCCS 转发到 die1
- dst=die1 时带宽骤降（HCCS 转发瓶颈）

## 运行

```bash
./run.sh                          # 默认 DEV_LIST="1,3,1"
DEV_LIST="0,2,0" ./run.sh         # 换跨卡对
```
