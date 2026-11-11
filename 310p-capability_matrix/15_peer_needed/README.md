# 15_peer_needed：跨卡 IPC 的 P2P 条件

## 测试内容

每个模式使用独立 MPI 作业，比较 DEFAULT、不同行为方向的显式 enable 和 PEER 导入。

## 本轮条件与结果

2026-11-11，Ascend310P3 / Atlas 300I Duo，aarch64，CANN 9.2.1，驱动 26.1.0。

设备 2→4；五个 MODE：DEFAULT、ENABLE、ENABLE_R0、ENABLE_BOTH、PEER；每次 16 MiB 数据校验。

**修正判定后五种模式均符合各自条件，整体退出码 0。**

DEFAULT、仅导入端 enable、仅导出端 enable 的导入均返回 507899；双向 enable + DEFAULT 和 PEER 导入成功且数据校验通过。负向模式要求返回 507899；enable 模式还要求对应 enable 成功，不能把初始化失败当成对照通过。

## 运行

在仓库根目录执行，设备号以当前进程可见的编号为准：

```bash
source /usr/local/Ascend/cann/set_env.sh
A_DEV=2 B_DEV=4 bash 310p-capability_matrix/15_peer_needed/run.sh
```

脚本先编译，传递编译和运行的退出码；超时返回 124，超时后强制结束可能返回 137。
本轮条件、退出码与关键输出收录在[结果记录](../results/2026-11-11.json)。
