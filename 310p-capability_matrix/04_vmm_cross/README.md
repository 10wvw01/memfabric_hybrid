# 04_vmm_cross：跨卡 VMM

## 测试内容

验证 ACL 物理页分配、共享句柄导出/导入、VA 预留和映射、访问权限设置，以及双向读写。

## 本轮条件与结果

2026-11-11，Ascend310P3 / Atlas 300I Duo，aarch64，CANN 9.2.1，驱动 26.1.0。

设备 2→4，两个 MPI 进程；32 MiB 物理分配、16 MiB 传输段。

**通过，退出码 0。**

不显式 EnablePeerAccess 的导入成功；切到导出方上下文后导入也成功。异步读 12.43 GB/s、写 13.10 GB/s。普通跨卡导入可以成功，不能据此推出同卡双 die 无需上下文切换。

## 运行

在仓库根目录执行，设备号以当前进程可见的编号为准：

```bash
source /usr/local/Ascend/cann/set_env.sh
A_DEV=2 B_DEV=4 bash 310p-capability_matrix/04_vmm_cross/run.sh
```

脚本先编译，传递编译和运行的退出码；超时返回 124，超时后强制结束可能返回 137。
本轮条件、退出码与关键输出收录在[结果记录](../results/2026-11-11.json)。

独立属性探测 `test_memattr.c` 不在默认 runner 内：

```bash
make -C 310p-capability_matrix/04_vmm_cross test_memattr
310p-capability_matrix/04_vmm_cross/test_memattr 2
```

各属性的分配与粒度返回值分别记录；主用例回退链命中 P2P_HUGE 不代表它是唯一可分配属性。

本轮独立探测 14 种属性，4 MiB 分配成功的有 MEM_NORMAL、MEM_HUGE、MEM_P2P_NORMAL、
MEM_P2P_HUGE、HBM_MEM_HUGE、HBM_MEM_NORMAL，推荐粒度均为 2 MiB。
两个 HUGE1G 属性返回 207000，HBM_P2P 与 DDR 属性返回 100000。
这里只验证粒度查询及分配释放，未将所有成功属性都用于跨设备共享。
