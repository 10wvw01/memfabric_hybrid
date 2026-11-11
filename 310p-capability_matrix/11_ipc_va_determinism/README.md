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
