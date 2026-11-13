# 03：跨卡IPC共享

通过两个rank比较IPC跨卡导入，并验证导入映射的异步D2D及Host直接读写。

## 测试步骤与API

1. 导出方A以`aclrtMalloc(ACL_MEM_MALLOC_HUGE_FIRST_P2P)`分配32MiB，H2D写入带tag的数据。
2. `aclrtIpcMemGetExportKey`导出IPC名称，经MPI传给导入方B。
3. B调用`aclrtIpcMemImportByKey(..., ACL_RT_IPC_MEM_IMPORT_FLAG_ENABLE_PEER_ACCESS)`取得本进程地址。
4. B异步D2D读取共享内存至本地设备缓冲区，等待stream，D2H逐字校验；再反向写入，由A读回校验。
5. B直接对导入地址执行16MiB D2H/H2D，A复核更新。这里Host是传输源或目的端，不是D2D的中转区。
6. 校验通过后分别测异步读取与写入带宽，3次预热、30次16MiB提交，末尾等待一次stream。
7. 双方`aclrtIpcMemClose`，A释放物理内存；在设备重置前尝试`aclrtDeviceDisablePeerAccess`，销毁stream并结束ACL。


## 运行

```bash
source /usr/local/Ascend/cann/set_env.sh
export ASCEND_HOME=/usr/local/Ascend/cann
cd 310p-capability_matrix/03_ipc_cross
A_DEV=2 B_DEV=6 bash run.sh
```

## 2026-11-13测试结果

Ascend310P3，CANN 9.2.1，驱动26.1.1，aarch64。设备及数据规模见下表。

| 检查项 | 本轮结果 |
|---|---|
| 设备 | 2、6，跨卡两个MPI rank |
| PEER_ACCESS导入 | 成功 |
| 异步D2D双向与Host直接读写 | 校验通过 |
| 带宽 | 16MiB，3次预热、30次异步提交后同步：读12.42、写23.11 GB/s |
| 清理 | 导入映射关闭和物理内存释放完成；r1 DisablePeerAccess返回0，r0返回507899 |
| 总结果 | PASS，两个rank退出0 |

本轮跨卡IPC共享读写可用。清理中一侧DisablePeerAccess的507899保留为实际观察，程序仍正常退出；不将该返回值写成全部清理API均成功。

## 历史设备组合与方向对照

之前的CANN 9.1.0记录如下：

| 拓扑 | RESULT | bw 读 (p→local) | bw 写 (local→p) |
|---|---|---|---|
| 0→2 跨卡(默认) | PASS | ~11 GB/s | ~14 GB/s |
| 1→4 跨卡 | PASS | 11.25 GB/s | 13.14 GB/s |
| 4→1 跨卡反向 | PASS | **1.29 GB/s** | 13.00 GB/s |
| 0→0 同设备参考 | FAIL | — | — |
