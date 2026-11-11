# 14_transfer_remote：Host 直接访问远端共享区

## 测试内容

对 IPC 导入地址直接进行 H2D/D2H，比较 pageable、pinned 和本地内存对照，并校验数据。

## 本轮条件与结果

2026-11-11，Ascend310P3 / Atlas 300I Duo，aarch64，CANN 9.2.1，驱动 26.1.0。

同卡 2→3、跨卡 2→4；两个 MPI 进程；32 MiB 主块、16 MiB 传输段。

**两组均通过，退出码 0。**

导入的远端 VA 可直接作为本轮 H2D/D2H 的设备地址参数。跨卡 pinned 同步 D2H 为 21.87 GB/s，pageable 同步为 5.02 GB/s；不据此推断其他拓扑的带宽。

## 运行

在仓库根目录执行，设备号以当前进程可见的编号为准：

```bash
source /usr/local/Ascend/cann/set_env.sh
A_DEV=2 B_DEV=3 bash 310p-capability_matrix/14_transfer_remote/run.sh
```

脚本先编译，传递编译和运行的退出码；超时返回 124，超时后强制结束可能返回 137。
本轮条件、退出码与关键输出收录在[结果记录](../results/2026-11-11.json)。
