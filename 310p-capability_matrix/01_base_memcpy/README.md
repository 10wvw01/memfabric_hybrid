# 01_base_memcpy：基础内存与拷贝

## 测试内容

单设备上的分配、释放、H2D、D2H、D2D，以及同步和异步调用的数据正确性。

## 本轮条件与结果

2026-11-11，Ascend310P3 / Atlas 300I Duo，aarch64，CANN 9.2.1，驱动 26.1.0。

设备 2，128 MiB 主缓冲；带宽段 3 次预热、30 次传输；D2D 另测 4/16/64/256 MiB。

**通过，退出码 0。**

Pinned H2D 同步 28.06 GB/s、D2H 同步 25.24 GB/s；同设备 D2D 同步 5.24 GB/s、异步批量提交 88.66 GB/s。异步数值包含末尾的流同步。

## 运行

在仓库根目录执行，设备号以当前进程可见的编号为准：

```bash
source /usr/local/Ascend/cann/set_env.sh
DEV=2 bash 310p-capability_matrix/01_base_memcpy/run.sh
```

脚本先编译，传递编译和运行的退出码；超时返回 124，超时后强制结束可能返回 137。
本轮条件、退出码与关键输出收录在[结果记录](../results/2026-11-11.json)。
