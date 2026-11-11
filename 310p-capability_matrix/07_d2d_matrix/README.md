# 07_d2d_matrix：多设备 IPC 池及 VA 一致性

## 测试内容

各 rank 导出自己的 slab，串行导入其他 rank 的 IPC key，再比较各进程看到的 VA，并进行环形写入和接收端校验。

## 本轮条件与结果

2026-11-11，Ascend310P3 / Atlas 300I Duo，aarch64，CANN 9.2.1，驱动 26.1.0。

4 个 MPI rank，物理设备 2、3、6、7，进程内编号 0～3；每 rank 16 MiB slab，带宽段为 4 MiB。

**VA 一致性条件未满足，退出码 1；导入与数据写校验通过。**

12 条跨 rank 导入全部成功，四个 rank 的写入和接收校验通过，但 VA 矩阵 INCONSISTENT。程序保留原有严格判定：join、write、VA 一致性均满足才返回 0。该结果说明这组 IPC 导入不能直接构成跨进程同址池。

## 运行

在仓库根目录执行，设备号以当前进程可见的编号为准：

```bash
source /usr/local/Ascend/cann/set_env.sh
ASCEND_RT_VISIBLE_DEVICES=2,3,6,7 MATRIX_N=4 bash 310p-capability_matrix/07_d2d_matrix/run.sh
```

脚本先编译，传递编译和运行的退出码；超时返回 124，超时后强制结束可能返回 137。
本轮条件、退出码与关键输出收录在[结果记录](../results/2026-11-11.json)。

`MATRIX_N` 支持 4 或 8，默认 8；切换规模时脚本强制重新编译。
可见设备列表必须按完整 Duo 卡的两个 die 相邻排列，使逻辑 (0,1)、(2,3) 等仍对应同卡。
本轮执行了四设备模式，八设备模式仅编译，未运行。
