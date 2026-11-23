# 310P 批量传输完成与 epoch 重启测试

本目录沿用原诊断测试的路径和退出码检查。
每轮两个 rank 批量提交 put NBI，quiet 成功后在应用数据区发布每块的递增轮次。
接收端确认轮次并检查数据块首字和完整轮次，消费完成后回传确认。
下一轮复用同一数据块前等待该确认，支持单个 kernel 中执行多轮。
AICPU epoch 可配置为较短时间，持续传输跨越 kernel 重启。

保留的覆盖范围包括批量进度、跨轮复用、epoch 重启、轮询预算、超时诊断、
中断、Host 接口失败和退出码传播。基础尺寸及分块测试继续由 PTO sdma_direct 承担。

## 构建和运行

1. 在仓库根目录加载 CANN 环境，并通过 `npu-smi info` 确认同卡两个 die 空闲。

   ```bash
   source /usr/local/Ascend/cann-9.2.1/set_env.sh
   ```

2. 构建主库与 AICPU kernel。

   ```bash
   bash script/build_and_pack_run.sh
   ```

3. 选择起始设备、轮数及数据块数运行测试。

   ```bash
   EX09_DEVICE_BASE=4 EX09_TOTAL_GENS=10000 EX09_CHUNKS=4 \
     bash test/integration/310p_mail_loss_repro/run.sh
   ```

4. 检查各 rank 的终态轮数、收发数及脚本退出码，再按下节执行退出码检查。

--build-only 编译到 /tmp/mf_mail_loss_repro，--run-only 运行该产物。
编译和启动使用同一套 MPI；两个 rank 使用起始设备及其下一索引。
运行前确认两个设备空闲。

| 变量 | 默认值 | 含义 |
|---|---:|---|
| EX09_DEVICE_BASE | 4 | 起始设备 |
| EX09_TOTAL_GENS | 10000000 | 总轮数 |
| EX09_CHUNKS | 4 | 每轮数据块数，1–128 |
| EX09_CHUNK_BYTES | 131072 | 每块字节数，至少 8、4 字节对齐，总数据不超过 16 MiB |
| EX09_BATCH_GENS | 1 | 每次 kernel 的轮数，大于 1 时在设备内协调复用 |
| EX09_GEN_DELAY_US | 0 | kernel 启动之间的等待微秒数 |
| MF_SDMA_ORCH_MODE | 1 | 本脚本默认每轮1秒；resident为常驻，正整数为每轮秒数 |
| EX09_WAIT_BUDGET | 10000000 | 单个应用状态等待的轮询预算 |
| EX09_STALL_S | 60 | Host 检查进度停滞的超时秒数 |

有效数据区：发送区 0–16 MiB，接收区 16–32 MiB，就绪槽从 48 MiB 开始，
消费确认槽从 52 MiB 开始。每槽 128 字节，输入状态和输出 scratch 分占缓存行。
就绪及确认状态在首轮启动前由双方清零并同步。

state=0 为运行中，1 为应用状态等待超时，2 为请求提交失败，4 为 quiet 失败，
5 为完成，6 为数据校验失败。历史状态结构保留原字段及偏移，以支持已有故障注入工具。
sent/recv 统计数据块数量，应用状态传输独立计入 AICPU 的请求调试记录。
终态轮数和收发数必须与配置一致，任一 rank 异常通过 MPI_Abort 结束任务。
Host 诊断可读取请求游标和 AICPU 日志；设备传输使用公开 put/quiet。

## 退出码检查

1. 完成上述编译，并选择同卡两个空闲 die。
2. 执行错误退出检查，指定独立日志目录。

   ```bash
   python3 test/integration/310p_mail_loss_repro/test_exit_status.py \
     --device-base 4 --binary /tmp/mf_mail_loss_repro \
     --kernel-dir build/src/hybm/ops/hybm_kernel \
     --log-dir /workspace/my-logs-dir/mail-exit
   ```

3. 检查各注入场景的退出状态与正常完成对照。
4. 检查日志目录及脚本退出码，超时视为失败。

该脚本检查 stream 错误、状态读取失败、进度停滞、中断、计数错误和单 rank 失败，
并包含正常完成及零轮对照。
