# 11_ipc_va_determinism — IPC VA 确定性测试

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
