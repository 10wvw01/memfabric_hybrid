# 03_ipc_cross：跨卡 IPC

## 测试内容

验证不同卡间的 IPC PEER 导入、异步双向 D2D 和 host 直达导入地址。

## 本轮条件与结果

2026-11-11，Ascend310P3 / Atlas 300I Duo，aarch64，CANN 9.2.1，驱动 26.1.0。

设备 2→4，两个 MPI 进程；32 MiB 主分配、16 MiB 传输段；3 次预热、30 次异步提交后同步。

**通过，退出码 0。**

异步读 11.94 GB/s、写 13.11 GB/s，数据校验成功。DEFAULT 与显式 enable 的独立对照见第 15 项。

## 运行

在仓库根目录执行，设备号以当前进程可见的编号为准：

```bash
source /usr/local/Ascend/cann/set_env.sh
A_DEV=2 B_DEV=4 bash 310p-capability_matrix/03_ipc_cross/run.sh
```

脚本先编译，传递编译和运行的退出码；超时返回 124，超时后强制结束可能返回 137。
本轮条件、退出码与关键输出收录在[结果记录](../results/2026-11-11.json)。
