# 02_ipc_die：同卡跨 die IPC

## 测试内容

导出 IPC key 后，在另一个 die 导入并进行异步 D2D 读写，以及 host 对导入地址的 H2D/D2H 校验。

## 本轮条件与结果

2026-11-11，Ascend310P3 / Atlas 300I Duo，aarch64，CANN 9.2.1，驱动 26.1.0。

设备 2→3，两个 MPI 进程；主分配 32 MiB，传输段 16 MiB。

**通过，退出码 0。**

DEFAULT 导入及异步数据校验成功；PEER 标志导入和同步共享 D2D 对照返回 507899。对照被拒绝与正向功能通过分别记录。

## 运行

在仓库根目录执行，设备号以当前进程可见的编号为准：

```bash
source /usr/local/Ascend/cann/set_env.sh
A_DEV=2 B_DEV=3 bash 310p-capability_matrix/02_ipc_die/run.sh
```

脚本先编译，传递编译和运行的退出码；超时返回 124，超时后强制结束可能返回 137。
本轮条件、退出码与关键输出收录在[结果记录](../results/2026-11-11.json)。
