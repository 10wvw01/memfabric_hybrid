# 16_vmm_matrix：多设备 VMM 池

## 测试内容

各 rank 预留 VA 窗口，将本地页和导入页映射到固定编号的槽位，再校验 join、环形写入和窗口地址一致性。

## 本轮条件与结果

2026-11-11，Ascend310P3 / Atlas 300I Duo，aarch64，CANN 9.2.1，驱动 26.1.0。

4 个 MPI rank；物理设备 2、3、6、7 映射为 0～3；每 rank 16 MiB 物理 slab，每个窗口 64 MiB，带宽段为 4 MiB。

**通过，退出码 0；join/write 均成功，VA CONSISTENT。**

本轮没有显式调用 EnablePeerAccess；同卡导入使用导出方上下文路径。四设备 VMM 池成立，不能从中推出八设备全连接成立或不存在 P2P 资源限制。

## 运行

在仓库根目录执行，设备号以当前进程可见的编号为准：

```bash
source /usr/local/Ascend/cann/set_env.sh
ASCEND_RT_VISIBLE_DEVICES=2,3,6,7 MATRIX_N=4 bash 310p-capability_matrix/16_vmm_matrix/run.sh
```

脚本先编译，传递编译和运行的退出码；超时返回 124，超时后强制结束可能返回 137。
本轮条件、退出码与关键输出收录在[结果记录](../results/2026-11-11.json)。

`MATRIX_N` 支持 4 或 8，默认 8；切换规模时脚本强制重新编译。
可见设备列表必须按完整 Duo 卡的两个 die 相邻排列，使逻辑 (0,1)、(2,3) 等仍对应同卡。
本轮执行了四设备模式，八设备模式仅编译，未运行。
