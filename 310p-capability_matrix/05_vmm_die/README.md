# 05_vmm_die：同卡跨 die VMM

## 测试内容

比较本地上下文直接导入、切到导出方设备上下文导入和显式 enable 后导入，并校验共享页的读写。

## 本轮条件与结果

2026-11-11，Ascend310P3 / Atlas 300I Duo，aarch64，CANN 9.2.1，驱动 26.1.0。

设备 2→3，两个 MPI 进程；32 MiB 主分配、16 MiB 传输段。

**通过，退出码 0。**

导入时 SetDevice(导出方)→Import(handle, 本地设备号)→SetDevice(本地) 的路径通过；本地上下文直接导入返回 507899，显式 enable 未替代上下文切换。

## 运行

在仓库根目录执行，设备号以当前进程可见的编号为准：

```bash
source /usr/local/Ascend/cann/set_env.sh
A_DEV=2 B_DEV=3 bash 310p-capability_matrix/05_vmm_die/run.sh
```

脚本先编译，传递编译和运行的退出码；超时返回 124，超时后强制结束可能返回 137。
本轮条件、退出码与关键输出收录在[结果记录](../results/2026-11-11.json)。
