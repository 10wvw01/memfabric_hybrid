# 10_vmm_hardcell：VMM 导入上下文对比

## 测试内容

比较本地设备上下文、临时切到导出方设备和创建导出方 context 三种导入方式。

## 本轮条件与结果

2026-11-11，Ascend310P3 / Atlas 300I Duo，aarch64，CANN 9.2.1，驱动 26.1.0。

设备 2→4，两个 MPI 进程，32 MiB 物理分配。

**通过，退出码 0。**

三种方式的导入、映射与数据校验均成功。本轮覆盖 2→4；目录名保留兼容已有命令，不能理解为任意拓扑组合均已验证。

## 运行

在仓库根目录执行，设备号以当前进程可见的编号为准：

```bash
source /usr/local/Ascend/cann/set_env.sh
A_DEV=2 B_DEV=4 bash 310p-capability_matrix/10_vmm_hardcell/run.sh
```

脚本先编译，传递编译和运行的退出码；超时返回 124，超时后强制结束可能返回 137。
本轮条件、退出码与关键输出收录在[结果记录](../results/2026-11-11.json)。
