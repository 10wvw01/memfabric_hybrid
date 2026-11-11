# 17_va_reservation — 地址窗口预留与 ACL 共享映射

验证 Ascend310P3 上的设备虚拟地址预留，比较 ACL 普通预留和 HAL 远端预留，并验证 HAL 窗口与 ACL 物理内存、共享句柄和映射的组合。

## 测试内容

| 命令 | 条件 | 检查内容 |
|---|---|---|
| `probe` | 1 个 MPI rank，使用 `A_DEV` | ACL 自动选址及三个指定地址，HAL `flag=0/512`，窗口大小，重复预留，释放后复用 |
| `pools` | 同一张 Duo 卡的两个 die，各一个 MPI rank | 三个 1 GiB 远端窗口，ACL 分配及导入映射，双向读写，资源释放和首窗口复用 |
| `acl-pools` | 同卡两个 die，从 `0x12c200000000` 逐池预留并映射 | 记录 ACL 普通窗口与 runtime 内部地址占用的实际结果 |

`run.sh` 默认依次执行 `probe` 和 `pools`。`acl-pools` 作为独立对照命令运行，预留或读写失败时返回非零退出码。

### 预留模式与返回值

```c
halMemAddressReserve(&base, size, 0, expected, MEM_RSV_TYPE_REMOTE_MAP);
```

`MEM_RSV_TYPE_REMOTE_MAP = 1u << 9 = 512`。驱动头文件描述为用于映射远端地址、不创建 double page table，要求指定地址预留。测试从 `0x280040000000` 开始逐池预留窗口。

`probe` 记录以下组合的返回值。

- ACL `flags=0`，地址为 `NULL`、`0x12c200000000`、`0x12da80000000`、`0x280040000000`，大小分别为 32 MiB、1 GiB、4 GiB。
- HAL `flags=0`，地址 `0x280040000000`，大小 1 GiB。
- HAL `flags=512`，地址 `0x280040000000`，大小分别为 2 MiB、32 MiB、64 MiB、1 GiB。

这些组合输出 `OBSERVE` 行，记录当前驱动的行为。能力检查要求 HAL 的 1 GiB 远端窗口精确命中指定地址、重复预留失败、释放后重新预留成功。任何一次已取得窗口的释放失败都会导致测试失败。

### 三个池的共享读写

两个 rank 按相同顺序，在以下地址分别预留 1 GiB 窗口。

```text
pool 0  0x280040000000
pool 1  0x280080000000
pool 2  0x2800c0000000
```

每个窗口分成两个 512 MiB 槽位，每 rank 实际分配 2 MiB 物理内存。1 GiB 是虚拟地址预留大小。

每个池依次执行以下操作。

1. HAL 预留窗口，校验返回地址和两 rank 基址。
2. `aclrtMallocPhysical(ACL_MEM_P2P_HUGE)` 分配本地物理内存，`aclrtMapMem` 映射到本 rank 槽位。
3. 使用进程内设备号调用 `aclrtMemSetAccess`。
4. 导出共享句柄并通过 MPI 交换。导入方切到导出方设备上下文，调用 `aclrtMemImportFromShareableHandle` 后恢复本地上下文，再映射到对端槽位。
5. 两边写本地 pattern，逐字检查对端 2 MiB。随后写入对端修改后的 pattern，再逐字检查本地回写结果。
6. 所有池完成后，先释放导入映射和句柄，MPI 同步后释放本地映射、物理内存及窗口。重新预留首地址以验证复用。

共享句柄只通过 MPI 交换。日志输出 API、地址、大小、flags、返回码和数据校验结果。

## 运行

依赖 MPI、CANN 头文件与 `libascendcl`、驱动 `libascend_hal.so`。使用对应 CANN 环境脚本设置环境。

```bash
source /usr/local/Ascend/cann/set_env.sh
cd 310p-capability_matrix/17_va_reservation

# 完整正向验证，A_DEV/B_DEV 为当前进程可见的设备号
A_DEV=2 B_DEV=3 bash run.sh

# 仅预留对照或仅共享读写
A_DEV=2 bash run.sh probe
A_DEV=2 B_DEV=3 bash run.sh pools

# 重编号后的同卡双 die
ASCEND_RT_VISIBLE_DEVICES=2,3 A_DEV=0 B_DEV=1 bash run.sh pools

# ACL 低地址逐池预留对照，命令保留实际失败退出码
A_DEV=2 B_DEV=3 bash run.sh acl-pools
```

`ASCEND_HOME` 默认取环境变量 `ASCEND_HOME_PATH`，未设置时使用 `/usr/local/Ascend/cann`。
可通过 `HAL_LIBRARY` 指定驱动库完整路径。
`TEST_TIMEOUT` 设置每个 MPI 命令的超时秒数，默认 90 秒；超时后最多再等待 10 秒强制结束。
运行脚本传递构建、MPI 和测试程序的失败退出码，超时返回 124。



### 查看 ACL 到 HAL 的调用记录

```bash
ASCEND_GLOBAL_LOG_LEVEL=0 ASCEND_SLOG_PRINT_TO_STDOUT=1 A_DEV=2 \
  bash run.sh probe > /tmp/va-reservation-runtime.log 2>&1
grep -E 'aclrtReserveMemAddressImpl|halMemAddressReserve|drvRetCode' \
  /tmp/va-reservation-runtime.log
```

日志中的进程号、请求地址、大小、flags 和驱动返回码可用于关联同一次调用。

## 2026-11-11 实测结果

环境为 Atlas 300I Duo / Ascend310P3，CANN 9.2.1，驱动 26.1.0，aarch64。使用同卡设备 2、3，并验证了可见设备重新编号为 0、1 的配置。

| 项目 | 实测 |
|---|---|
| ACL 自动选址，32 MiB / 1 GiB / 4 GiB | 均成功 |
| ACL 指定 `0x12c200000000`、`0x12da80000000`，三种大小 | 本轮 probe 均成功 |
| ACL 指定 `0x280040000000`，三种大小，flags=0 | 均返回 507899 |
| HAL 指定 `0x280040000000`，1 GiB，flags=0 | 返回 13 |
| HAL 指定 `0x280040000000`，2 / 32 / 64 MiB，flags=512 | 均返回 3 |
| HAL 指定 `0x280040000000`，1 GiB，flags=512 | 成功 |
| 重复预留同一远端窗口 | 返回 6，释放后可再次预留 |
| 三个 HAL 远端池 + ACL 共享映射 | 两 rank 的每个池双向 2 MiB 全量校验通过，首窗口复用通过 |
| `ASCEND_RT_VISIBLE_DEVICES=2,3`，A_DEV=0、B_DEV=1 | 通过 |
| ACL 低地址逐池预留并映射 | 第一个池数据校验通过，第二个地址 `0x12c240000000` 返回 507899，进程清理后退出 1 |

`507899` 为 `ACL_ERROR_RT_DRV_INTERNAL_ERROR`。
runtime 日志能进一步显示底层调用及驱动返回码。
例如本轮 ACL 对 `0x280040000000` 的 `flags=0` 请求下发到 `halMemAddressReserve`，驱动返回 13，ACL 对外返回 507899。
其他失败原因需要结合对应调用的底层日志定位。


本目录验证预留接口和共享映射能力。MemFabric 的地址管理器及正常 BM/SHM 入口另有单元测试和集成测试；本目录可以独立构建运行。

本轮执行记录见[结果记录](../results/2026-11-11.json)。
