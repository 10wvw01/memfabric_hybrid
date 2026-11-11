# 08_host_swap：Host 中转传输

## 测试内容

比较直接共享 D2D 与 remote→host→local 两段异步传输，验证正反向写入的数据。

## 本轮条件与结果

2026-11-11，Ascend310P3 / Atlas 300I Duo，aarch64，CANN 9.2.1，驱动 26.1.0。

设备 2→3，两个 MPI 进程；32 MiB 主块、16 MiB 传输段。

**通过，退出码 0。**

直接 D2D 读 19.03 GB/s；host 中转有效读带宽 3.50 GB/s。中转值按最终交付字节计数，执行两段搬运。

## 运行

在仓库根目录执行，设备号以当前进程可见的编号为准：

```bash
source /usr/local/Ascend/cann/set_env.sh
A_DEV=2 B_DEV=3 bash 310p-capability_matrix/08_host_swap/run.sh
```

脚本先编译，传递编译和运行的退出码；超时返回 124，超时后强制结束可能返回 137。
本轮条件、退出码与关键输出收录在[结果记录](../results/2026-11-11.json)。
