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

## HAL 内存生命周期与超过4GiB的双rank设备池

`hal_vmm.c` 补充验证物理内存分配、共享、映射及释放。ACL负责设备初始化、设备号转换和数据拷贝，
内存生命周期使用HAL。进程设备号经 `aclrtGetLogicDevIdByUserDevId` 转为HAL使用的驱动设备号。

| 命令 | 内容 |
|---|---|
| `hal-probe` | 单rank比较2MiB的HBM、DDR、P2P_HBM、P2P_DDR分配，以及V2的NONE、FABRIC句柄导出；对照项输出OBSERVE，DDR和P2P_DDR分配及所有已分配资源释放必须成功 |
| `hal-pools` | 同卡双rank，三个1GiB窗口，每rank每池2MiB物理内存；完整HAL共享映射、双向全量校验、清理和窗口复用 |
| `hal-context` | 同卡双rank分别以本地上下文、导出方上下文导入；每种模式新建MPI进程，检查当前设备、三个小池的映射和双向数据 |
| `hal-large-pools` | 同卡双rank，默认每rank实际分配8GiB物理内存，共享一个16GiB虚拟窗口；分块覆盖全部8GiB的双向读写、逐字校验及释放复用 |

HAL共享读写流程为：

1. `halMemAddressReserve` 使用 `MEM_RSV_TYPE_REMOTE_MAP` 精确预留窗口，每rank占一半。
2. `halMemCreate` 使用 `MEM_P2P_DDR_TYPE`；仅当返回 `DRV_ERROR_OUT_OF_MEMORY=6` 时，
   改用 `MEM_DDR_TYPE` 重试，日志分别保留两次分配结果。
3. `halMemMap`、`halMemSetAccess` 映射本地物理内存。
4. `halMemExportToShareableHandle` 使用64位NONE句柄、flags=0；
   `halMemShareHandleSetAttribute` 设置与原ACL实现相同的共享属性，MPI交换句柄。
5. 保持本地设备上下文，通过 `halMemImportFromShareableHandle` 导入，
   再使用 `halMemMap`、`halMemSetAccess` 映射对端槽位。`hal-context`另行切换上下文作对照。
6. 写满本地范围，读对端并逐字校验；随后写满对端范围，读本地并逐字校验。
   每个阶段两rank同步；主机缓冲区固定8MiB，循环覆盖整个池，包含4GiB边界及尾部。
7. `halMemUnmap`、`halMemRelease` 释放导入映射，两rank同步后释放本地资源；
   `halMemAddressFree` 释放窗口，再次预留首地址验证复用。

```bash
source /usr/local/Ascend/cann/set_env.sh
cd 310p-capability_matrix/17_va_reservation
A_DEV=4 bash run.sh hal-probe
A_DEV=4 B_DEV=5 bash run.sh hal-pools
A_DEV=2 B_DEV=3 bash run.sh hal-context

# 每rank 8GiB物理内存，总共16GiB；要求两个rank均实际触发回退
POOL_GIB=8 EXPECT_FALLBACK=1 A_DEV=4 B_DEV=5 bash run.sh hal-large-pools

# 可见设备重编号；POOL_GIB支持5～16，按当前空闲内存选择
ASCEND_RT_VISIBLE_DEVICES=4,5 POOL_GIB=8 EXPECT_FALLBACK=1 A_DEV=0 B_DEV=1 \
  bash run.sh hal-large-pools
```

`all`包含地址探测、ACL共享映射及HAL本地上下文小池验证；上下文对照和大池显式运行。
`hal-large-pools` 默认超时300秒，其余命令90秒，可用 `TEST_TIMEOUT` 调整。
`EXPECT_FALLBACK=1` 要求每个rank实际经历P2P分配失败后回退成功，未触发回退也判失败；
未设置时只检查所选容量可用。这里的DDR/P2P_DDR是设备侧内存类型。

### 2026-11-12 实测

环境：10006，Ascend310P3，CANN 9.2.1，npu-smi 26.1.1，同卡设备4、5。

| 检查 | 结果 |
|---|---|
| HAL 2MiB HBM/P2P_HBM分配 | 均返回6 |
| HAL 2MiB DDR/P2P_DDR分配 | 成功 |
| V2 NONE/FABRIC导出 | NONE成功；FABRIC返回8；双rank共享验证使用64位NONE接口 |
| HAL三个小池 | 两rank的每个池双向全量校验通过，窗口复用通过，退出0 |
| 每rank 8GiB大池 | 两rank的P2P_DDR均返回6、DDR回退均成功；各8GiB全范围双向校验通过，窗口复用通过，退出0 |
| 可见设备4、5重编号为0、1，每rank 8GiB | 两rank均触发回退，全范围双向校验和复用通过，退出0 |
| MemCache两rank，每rank 8GiB设备池、DRAM=0 | 两rank均触发HAL回退，跨rank对象写入/恢复数据一致，正常关闭、退出0；实际加载的新库路径已检查 |
| Qwen3.6-35B TP=2，每rank 8GiB设备池、DRAM=0 | 服务启动成功，两rank实际触发HAL回退；0%用例外部命中0，重复输入恢复3072 tokens、本地命中0、首token与种子一致；退出0 |
| 修改后MemFabric库，每rank 8GiB设备池 | 两rank实际触发回退；8MiB范围跨越4GiB边界及池尾部的H2G/G2H、G2G检查均mismatch=0；配套DRAM读写通过，正常销毁，两轮两rank退出均为0 |

MemFabric补查使用07双die示例的临时副本，设备池改为8GiB，分别从4GiB−4MiB及8GiB−16MiB处验证；
端口和工作目录隔离，先启动rank1再启动rank0。大池容量为每个rank的物理分配量。
Qwen TP=2补查使用Qwen3.6-35B-A3B-w8a8，同卡4、5，输入4096、输出1 token，
并发1、block_size与max_num_batched_tokens均1024、gpu_memory_utilization=0.65、
expandable_segments=True；0%和100%共享输入各一个正式请求。此为大池功能检查，提交前UT结果见下节。

MemCache补查采用隔离Python包目录加载新构建的三份MemFabric库，逐进程检查实际加载路径；
首次使用原Python环境的记录走了旧ACL库，已单独保留为对照，未计入HAL路径通过结果。
MemCache对象检查为每rank 256KiB，池容量为8GiB；超过4GiB部分的全范围访问由前述HAL测试覆盖，
实际MemFabric访问另检查4GiB边界及尾部。测试中未修改系统安装的Python包。

MemCache两rank对象测试退出0，但关闭过程中出现 `thread pool has stopped` 日志，旧ACL库对照也出现同样日志。
本次没有修改该关闭流程。vLLM测试的HAL回退由服务日志确认；进程退出后才检查maps，未取得该轮worker的maps快照。

### 提交前UT与检查

完整ASAN UT：2070项通过、12项环境跳过、无失败，脚本退出0；行覆盖率70.3%，分支覆盖率46.5%。
定向的两个VMM测试套件共23项通过。本次新增12项回归，覆盖分配、回退、共享格式和策略、
设备号、导入失败、映射失败、转换失败清理及缺失符号。非310P的HBM分配行为也有回归覆盖。
转换失败时的导入映射清理已由UT验证，实际成功路径沿用上述硬件记录。
另完成能力验证程序编译、run.sh语法检查及git diff --check。

### 同卡HAL导入的上下文对照

`hal-context`依次以新建双MPI进程运行两种模式，其他分配、导出、映射和校验流程相同：

| 模式 | 导入顺序 |
|---|---|
| 本地上下文 | 初始化本地设备 → 查询当前设备 → HAL导入 → 映射 |
| 导出方上下文 | 初始化本地设备 → 切到导出方 → 查询当前设备 → HAL导入 → 恢复本地 → 映射 |

两种模式都检查`aclrtGetDevice`返回的当前设备，导入目标参数始终是本地驱动设备号。每rank每池2MiB，三个1GiB虚拟窗口；双向全范围读写逐字校验后释放资源，验证首窗口可再次预留。普通`hal-pools`及`hal-large-pools`均保持本地上下文。

#### 2026-11-13测试结果

Ascend310P3，CANN 9.2.1，驱动26.1.1。下列上下文对照每种模式均检查当前设备、HAL导入、三个池的映射和双向全量数据校验，以及释放后窗口复用。

| 检查 | 结果 |
|---|---|
| 设备2、3，保持本地上下文 | 通过，退出0 |
| 设备2、3，切到导出方上下文 | 通过，退出0 |
| 设备4、5，保持本地上下文 | 通过，退出0 |
| 设备4、5，切到导出方上下文 | 通过，退出0 |
| 可见设备2、3重编号为0、1，两种模式 | 均通过，退出0 |
| 每rank 8GiB大池，保持本地上下文 | 两rank均触发P2P_DDR不足后回退DDR；各8GiB全范围双向读写及窗口复用通过 |
| 新运行库双rank读写，设备2、3 | 每rank设备池32MiB、DRAM池32MiB；跨die H2G/G2H、G2G及DRAM三个8MiB校验均mismatch=0，两rank退出0 |

HAL的64位NONE共享句柄在本地上下文可直接导入；当前运行库已去掉同卡导入前后的设备切换，监听线程仍初始化自己的本地ACL上下文。

本次完整ASAN UT共2082项，2070通过、12跳过、0失败，脚本退出0；行覆盖率70.3%，分支覆盖率46.5%。导入成功与失败用例均检查未发生上下文切换。
