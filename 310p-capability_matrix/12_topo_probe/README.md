# 12_topo_probe：设备可达性与 P2P enable

## 测试内容

查询 CanAccessPeer，并逐对 enable；成功后立即 disable，记录每条有向关系的返回码。

## 本轮条件与结果

2026-11-11，Ascend310P3 / Atlas 300I Duo，aarch64，CANN 9.2.1，驱动 26.1.0。

物理设备 2、3、6、7 映射为进程内 0～3；单进程 4×4 矩阵。

**通过，退出码 0。**

本轮仅覆盖两个 Duo 卡上的四个 die；同卡两个有向设备对均 can=0、enable=507899；跨卡四个设备对均双向 can=1，八次有向 enable 均返回 0。CanAccessPeer 和 EnablePeerAccess 是不同观测，不能仅凭前者判断后者或完整数据链路可用。

## 运行

在仓库根目录执行，设备号以当前进程可见的编号为准：

```bash
source /usr/local/Ascend/cann/set_env.sh
ASCEND_RT_VISIBLE_DEVICES=2,3,6,7 MATRIX_N=4 bash 310p-capability_matrix/12_topo_probe/run.sh
```

脚本先编译，传递编译和运行的退出码；超时返回 124，超时后强制结束可能返回 137。
本轮条件、退出码与关键输出收录在[结果记录](../results/2026-11-11.json)。

`MATRIX_N` 支持 4 或 8，默认 8；切换规模时脚本强制重新编译。
可见设备列表必须按完整 Duo 卡的两个 die 相邻排列，使逻辑 (0,1)、(2,3) 等仍对应同卡。
本轮执行了四设备模式，八设备模式仅编译，未运行。
