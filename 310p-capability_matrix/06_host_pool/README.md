# 06_host_pool：Host 跨进程共享池

## 测试内容

验证 POSIX shm 的跨进程写入可见性、共享区到设备的 H2D/D2H，以及各个 host registration 模式。

## 本轮条件与结果

2026-11-11，Ascend310P3 / Atlas 300I Duo，aarch64，CANN 9.2.1，驱动 26.1.0。

两个 MPI 进程都使用设备 2；共享区和正确性传输段为 32 MiB，带宽段为 16 MiB。

**通过，退出码 0。**

共享池写入、更新后再次 H2D 的校验通过。V2 PINNED 对 mmap shm 和 malloc 内存返回 0，注销成功；V1、V2 MAPPED、V2 MAPPED|PINNED 返回 207000。PINNED 注册后 GetDevicePointer 返回 107000，因此注册成功不能视为设备地址映射成功。

## 运行

在仓库根目录执行，设备号以当前进程可见的编号为准：

```bash
source /usr/local/Ascend/cann/set_env.sh
A_DEV=2 B_DEV=2 bash 310p-capability_matrix/06_host_pool/run.sh
```

脚本先编译，传递编译和运行的退出码；超时返回 124，超时后强制结束可能返回 137。
本轮条件、退出码与关键输出收录在[结果记录](../results/2026-11-11.json)。
