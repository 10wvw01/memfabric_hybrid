# VA 管理与 IPC / VMM 观测

IPC key 导入成功不保证进程间 VA 相同。本轮四设备 IPC 导入和写校验通过，但 VA 不一致；
四设备 VMM 槽位窗口的 VA 一致。见[07](README.md)、[11](../11_ipc_va_determinism/README.md)和[16](../16_vmm_matrix/README.md)。

当前 MemFabric 的 VMM 路径使用 HybmVaManager 记录窗口，并从远端管理区查找空位。
310P 使用 HAL 远端预留模式取得窗口，再用 ACL 处理物理页、共享句柄及映射。
第 17 项独立验证了从 0x280040000000 开始的三个 1 GiB 窗口；
该测试中的三处指定地址用于验证接口行为，不替代生产代码的地址管理器。

[地址预留验证](../17_va_reservation/README.md)给出 ACL 与 HAL 标志的区别及逐池对照。
本轮没有在 910B 硬件运行本套件，也没有将 310P 的返回值推广为 910B 的运行结果。
