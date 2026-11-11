# 310P 内存与共享能力验证

本目录包含 01～17 项独立验证程序，覆盖 ACL 基础内存、IPC、VMM、host 共享池、P2P 和虚拟地址预留。
本版用 2026-11-11 的运行结果替换旧版结论；测试条件与返回码见各目录 README 和[结果记录](results/2026-11-11.json)。

## 本轮环境与覆盖

- Ascend310P3 / Atlas 300I Duo，aarch64，CANN 9.2.1，驱动 26.1.0。
- 同卡用设备 2、3；跨卡用 2、4；四设备矩阵用 2、3、6、7，重新编号为 0～3。
- 01～17 均已执行，07、12、16 执行四设备模式。八设备模式完成编译，未运行：当时设备 0、1 告警，设备 5 有其他业务。
- 另一台驱动 26.1.1 机器的初测日志不与本轮主结果混算。该机器 09 曾在初始化阶段超时；主验证机复测通过。

## 测试矩阵

| 项 | 内容 | 本轮结果 |
|---|---|---|
| [01](01_base_memcpy/README.md) | 基础内存与拷贝 | 通过，退出码 0。 |
| [02](02_ipc_die/README.md) | 同卡跨 die IPC | 通过，退出码 0。 |
| [03](03_ipc_cross/README.md) | 跨卡 IPC | 通过，退出码 0。 |
| [04](04_vmm_cross/README.md) | 跨卡 VMM | 通过，退出码 0。 |
| [05](05_vmm_die/README.md) | 同卡跨 die VMM | 通过，退出码 0。 |
| [06](06_host_pool/README.md) | Host 跨进程共享池 | 通过，退出码 0。 |
| [07](07_d2d_matrix/README.md) | 多设备 IPC 池及 VA 一致性 | VA 一致性条件未满足，退出码 1；导入与数据写校验通过。 |
| [08](08_host_swap/README.md) | Host 中转传输 | 通过，退出码 0。 |
| [09](09_vmm_samedev/README.md) | 同设备跨进程 VMM | 通过，退出码 0。 |
| [10](10_vmm_hardcell/README.md) | VMM 导入上下文对比 | 通过，退出码 0。 |
| [11](11_ipc_va_determinism/README.md) | IPC 导入 VA 对比 | 功能通过，退出码 0；观测到 VA MISMATCH。 |
| [12](12_topo_probe/README.md) | 设备可达性与 P2P enable | 通过，退出码 0。 |
| [13](13_bw_compare/README.md) | IPC、VMM 与 host 中转带宽 | 两组均通过，退出码 0。 |
| [14](14_transfer_remote/README.md) | Host 直接访问远端共享区 | 两组均通过，退出码 0。 |
| [15](15_peer_needed/README.md) | 跨卡 IPC 的 P2P 条件 | 修正判定后五种模式均符合各自条件，整体退出码 0。 |
| [16](16_vmm_matrix/README.md) | 多设备 VMM 池 | 通过，退出码 0；join/write 均成功，VA CONSISTENT。 |
| [17](17_va_reservation/README.md) | ACL / HAL 地址预留及三池共享读写 | 正向通过；ACL 低地址逐池对照返回 1 |

## 主要结论

1. 同卡 IPC 的 DEFAULT 导入与异步 D2D 可用。同卡 VMM 导入需要保留本轮验证的导出方上下文切换路径。
2. 跨卡 IPC 的 PEER 导入可用；双向 enable 后 DEFAULT 也可用，单向 enable 的本轮对照被拒绝。
3. IPC 共享成功不等于 VA 一致。07 的四设备导入和写校验通过，但 VA 不一致，保留严格判定后退出 1；11 的功能通过同样不能证明全进程同址。
4. 四设备 VMM 槽位窗口的 join、写校验和 VA 一致性通过。没有本轮八设备运行证据，不沿用旧版“每卡固定八槽位”的定量结论。
5. POSIX shm 可用于跨进程 host 池。V2 PINNED 注册可成功；MAPPED 路径本轮不支持。PINNED 注册和设备可直接寻址是两件事。
6. 310P 可以通过 HAL 的远端预留模式取得 `0x280040000000` 起的窗口，再使用 ACL 分配、共享和映射物理页。第 17 项给出完整步骤与对照。
7. `507899` 是 `ACL_ERROR_RT_DRV_INTERNAL_ERROR`，不能单独解释成地址冲突或功能不支持；需要结合调用参数与底层日志。

## 执行方法

```bash
source /usr/local/Ascend/cann/set_env.sh
DEV=2 bash 310p-capability_matrix/01_base_memcpy/run.sh
A_DEV=2 B_DEV=3 bash 310p-capability_matrix/05_vmm_die/run.sh
A_DEV=2 B_DEV=4 bash 310p-capability_matrix/03_ipc_cross/run.sh
ASCEND_RT_VISIBLE_DEVICES=2,3,6,7 MATRIX_N=4 bash 310p-capability_matrix/16_vmm_matrix/run.sh
A_DEV=2 B_DEV=3 bash 310p-capability_matrix/17_va_reservation/run.sh
```

依赖 `mpicc` / MPI、CANN 头文件、`libascendcl`，第 17 项还加载驱动 `libascend_hal.so`。
`ASCEND_HOME` 默认取 `ASCEND_HOME_PATH`，未设置时为 `/usr/local/Ascend/cann`；支持显式覆盖。
每个 runner 显示编译输出，传递真实退出码。MPI 不读取调用方标准输入，避免从批量脚本中吞掉后续命令。
运行超时后最多再等待 10 秒强制结束；124 表示超时，137 可能表示后续强制终止。

07、12、16 的 `MATRIX_N` 支持 4/8，默认 8；设备列表须按完整 Duo 卡的两个 die 相邻排列。
并发业务和已有 P2P 状态可能影响结果；测试使用获准的设备，释放自身映射和 P2P 关系，不复位其他业务。
`precheck.sh` 输出当前 CANN、设备和容器内 D 状态进程，移除了旧版自行设定的资源比例判定及设备复位建议。

## 带宽口径

通常为 3 次预热、30 次异步提交后一次流同步，按传输字节数除以整体耗时；每项具体大小见对应 README。
01 同时测同步、异步批量和逐次同步；08/13 的 host 中转按最终交付字节计数，两段拷贝的成本都计入时间。
数值来自本轮指定设备组合。移除旧版跨环境带宽拼表、未复测的 56 链路数据及据此得出的硬件瓶颈推断。
没有额外性能门限。

## 文档与代码关系

各目录默认入口是 `run.sh` 和 `test.c`。目录内保留的其他探针和历史实验源码不属于默认执行范围。
README 只将结果记录中实际执行的程序列为本轮验证；没有把保留源码的存在当作测试通过。
MemFabric 生产路径的地址管理器及 BM/SHM 入口另有单元测试和集成测试，第 17 项可独立运行。

## 本次测试程序修改

- 01～16 的 runner 传递编译和运行的失败退出码，MPI 不读取批量脚本的标准输入，超时后有明确退出时间。
- 02、03、13、14、15 在导出失败时也完成对应 MPI 消息；11 在广播前同步失败状态，避免接收方一直等待。
  六个用例的正常运行均返回 0；注入导出失败后均返回 1，02 的第二次导出失败也已验证。
- 06 按 API 与 flag 分别输出 host 注册返回值；04 的属性探针删除硬编码硬件支持结论，并检查成功分配后的释放。
- 15 修正单向 enable 的负向判定，检查 enable 是否实际成功，并拒绝未知 MODE。
- 07、12、16 支持四设备与八设备规模；新增 17 地址预留与共享映射验证及本轮结果记录。

本次修改位于能力验证目录，未改动 MemFabric 的生产内存分配实现。
