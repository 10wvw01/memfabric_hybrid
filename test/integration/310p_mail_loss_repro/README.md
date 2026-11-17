# 310P3 接收通知丢失与等待超时复现测试

该测试用于复现双 rank 并发收发过程中，接收通知未按预期就绪而导致等待超时的问题。
历史故障表现为 vllm-ascend 中的 AICore 超时，错误码为507014。
测试直接调用 MemFabric SHMEM SDMA 接口，无需启动 vLLM。

每轮中，两个 rank 先连续提交搬运请求，再依次等待接收通知、校验通知字段和数据块首字，
最后通过 quiet 等待本端发送完成。默认每轮处理4个数据块，每次 AICore kernel 执行一轮，
两个 rank 通过 MPI 同步进度。

## 环境与构建

需要 Ascend310P3、CANN、bisheng 和 MPI。两个 rank 应使用同卡的两个 die，
设备索引由 `EX09_DEVICE_BASE` 和 `EX09_DEVICE_BASE + 1` 指定。
运行前通过 `npu-smi info` 确认目标设备及其占用情况。

在仓库根目录加载本机的 CANN 环境，并设置 `ASCEND_HOME_PATH`。
例如，使用已安装的 CANN 9.2.1：

```bash
source /usr/local/Ascend/cann-9.2.1/set_env.sh
export ASCEND_HOME_PATH=/usr/local/Ascend/cann-9.2.1
```

`ASCEND_RT_VISIBLE_DEVICES` 会限制进程可见的设备并重新编号。以下示例先清除该变量，
再使用当前容器中的设备索引0/1：

```bash
unset ASCEND_RT_VISIBLE_DEVICES
EX09_DEVICE_BASE=0 EX09_TOTAL_GENS=10000 EX09_CHUNKS=4 \
  bash test/integration/310p_mail_loss_repro/run.sh
```

脚本会编译测试程序；缺少主库静态库时先构建主库。测试程序写入
`/tmp/mf_mail_loss_repro`，同一容器内应顺序执行该测试，避免并发覆盖编译产物。
AICPU kernel 启动配置从当前仓库的构建目录中选择版本号最大的
`libmf_sdma_orch_launch.json`。

也可以分开编译和运行：

```bash
bash test/integration/310p_mail_loss_repro/run.sh --build-only
EX09_DEVICE_BASE=0 EX09_TOTAL_GENS=10000 EX09_CHUNKS=4 \
  bash test/integration/310p_mail_loss_repro/run.sh --run-only
```

## 运行参数

| 环境变量 | 默认值 | 含义 |
|---|---:|---|
| `EX09_DEVICE_BASE` | 4 | rank 0的起始设备索引，rank 1使用下一索引；上述示例显式指定0 |
| `EX09_TOTAL_GENS` | 10000000 | 每个 rank 执行的总轮数 |
| `EX09_CHUNKS` | 4 | 每轮发送的数据块数 |
| `EX09_CHUNK_BYTES` | 131072 | 每个数据块的字节数 |
| `EX09_BATCH_GENS` | 1 | 每次 AICore kernel 执行的轮数；值为1时逐轮进行 MPI 同步 |
| `EX09_GEN_DELAY_US` | 0 | 两次 AICore kernel 执行之间的等待时间，单位为微秒 |
| `EX09_EPOCH_BUDGET_S` | 1 | AICPU kernel 单次运行时间，单位为秒；缩短时间会增加重启频率 |
| `EX09_WAIT_BUDGET` | 10000000 | 等待操作使用的轮询预算，实际耗时取决于设备 |
| `EX09_STALL_S` | 60 | host 检查进度无变化的超时时间，单位为秒 |

## 如何判断结果

首先检查启动日志中的 `devId`，确认两个 rank 使用预期设备。
程序正常完成时，应满足以下条件：

- 进程退出码为0。
- 两个 rank 的终态 `gensDone` 均等于配置的总轮数。
- `sent` 和 `recv` 均等于总轮数乘以每轮数据块数。
- 最终汇总行显示两个 rank 正常，且“本次运行未检测到异常”。

`state=0` 表示尚未记录设备侧异常，`state=5` 表示完成。
程序会自动检查是否收到中断信号、设备状态、完成轮数及收发数量。
stream创建、同步、销毁或运行状态读取失败，以及进度超时，均返回失败。
任一rank失败时通过MPI_Abort结束任务，避免其他rank继续等待同步。

### AICPU 请求调试记录

程序结束时输出 AICPU 调试信息，包括 doorbell 更新硬件提交位置时使用的值、
返回码及累计请求记录数。随后输出最近的请求记录，每条包含请求序号、
源地址、目标地址、长度、标签、接收通知地址和硬件提交位置变化。

记录保存在60槽的环形缓冲区中。累计记录数为 `N` 时，应输出
`min(N, 60)` 条；序号从 `max(0, N-60)` 到 `N-1` 连续递增。
两个 rank 的输出可以交错，应分别检查各 rank 的序号。

日志中的位置字段含义如下：

| 字段 | 含义 |
|---|---|
| `reqTail` | 请求生产位置，AICore 下一条请求对应的累计序号 |
| `reqHead` | 请求消费位置，AICPU 下一条待处理请求的累计序号 |
| `pending` | 生产位置减消费位置，表示尚未消费的请求数量 |
| `arrHead` | 接收通知消费位置，下一条待处理接收通知的累计序号 |
| `notifyAddr` | 对端接收通知的写入地址 |
| `sqTail` | SQE 硬件提交队列的提交位置，日志显示处理请求前后的值 |

### 异常状态

| state | 含义 |
|---:|---|
| 1 | 等待接收通知超出轮询预算 |
| 2 | 软件请求环形队列持续满，或提交请求失败 |
| 3 | 接收通知的序号、目标地址、长度或标签不匹配 |
| 4 | 等待发送完成超出轮询预算 |
| 6 | 接收数据块首字与预期校验值不符 |
| 7 | 接收就绪序号超过期望值 |

异常日志还会输出软件请求队列的生产和消费位置、接收就绪序号数组、
发送完成序号数组及失败时的接收通知。序号复查结果只报告当时读取到的状态；
具体原因需结合请求记录和收发计数分析。

异常退出路径使用 `MPI_Abort`，避免设备异常导致资源清理过程阻塞。

## 退出码回归测试

先按上面的步骤编译测试程序，在已加载CANN环境且设备0/1可用时运行：

```bash
python3 test/integration/310p_mail_loss_repro/test_exit_status.py \
  --binary /tmp/mf_mail_loss_repro \
  --kernel-json build/src/hybm/ops/hybm_kernel/libmf_sdma_orch_launch.json
```

脚本会执行真实收发，并通过临时预加载库改变host收到的接口返回值或状态快照。
检查内容包括stream错误、状态读取失败、进度超时、中断、完成轮数和计数不符，
以及单个rank失败时能否结束MPI任务。正常完成和零轮运行作为对照。
运行需要C编译器，日志可通过 --log-dir 指定目录保存。

## 相关代码

- `src/smem/include/device/smem_shm_sdma_layout.h`：软件请求队列、接收通知和序号数组布局。
- `src/hybm/ops/hybm_kernel/hybm_sdma_orch_kernel.cc`：AICPU 请求处理及调试记录写入。
- `src/hybm/ops/hybm_kernel/hybm_sdma_contract.h`：AICPU 工作区和调试记录共享布局。
- `test/integration/310p_sdma_stability/`：SDMA 稳定性测试。
