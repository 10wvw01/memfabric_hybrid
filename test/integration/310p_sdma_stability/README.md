# 310P SDMA稳定性测试

本测试使用Ascend310P3同卡两个die，检查持续收发以及AICPU kernel运行期间的设备同步。

每轮中，rank0发送128KiB数据，rank1将每个uint32_t加上固定增量后传回，
rank0等待接收通知。两个rank逐轮通过MPI同步进度。

## 检查内容

- 测试stream创建、逐轮同步、销毁，以及结果读取、设备复位和ACL退出的返回值。
- 收发总耗时不超过“轮数×20ms + 30000ms”。
- 末轮返回数据的前4个uint32_t与预期值一致。
- 设备同步接口在90000ms的超时参数下成功返回。

程序输出单轮平均耗时和最大耗时，最大耗时作为统计数据。
数据检查采用上述末轮抽样方式。

任一rank的ACL接口调用失败时，日志输出rank、接口名、返回码和已完成轮数，
随后通过MPI_Abort结束任务。数据或总耗时检查失败时，程序返回1。

## 环境与构建

需要Ascend310P3、CANN、bisheng和MPI。编译与运行使用同一套MPI。
在仓库根目录加载本机CANN环境并设置ASCEND_HOME_PATH，例如：

```bash
source /usr/local/Ascend/cann-9.2.1/set_env.sh
export ASCEND_HOME_PATH=/usr/local/Ascend/cann-9.2.1
```

脚本会编译测试程序，缺少主库静态库时先构建主库。
AICPU kernel启动配置使用当前构建目录中的libmf_sdma_orch_launch.json。
测试程序写入/tmp/mf_310p_sdma_stability，同一容器内应顺序运行，避免并发覆盖产物。

## 运行

使用前通过npu-smi info确认设备0/1可用。以下命令清除设备可见性映射，显式选择设备0/1：

```bash
unset ASCEND_RT_VISIBLE_DEVICES
TEST_DEVICE_BASE=0 STRESS_ROUNDS=30000 \
  bash test/integration/310p_sdma_stability/run.sh
```

可以分开编译和运行：

```bash
bash test/integration/310p_sdma_stability/run.sh --build-only
TEST_DEVICE_BASE=0 STRESS_ROUNDS=30000 \
  bash test/integration/310p_sdma_stability/run.sh --run-only
```

STRESS_ROUNDS使用正整数，默认30000。TEST_DEVICE_BASE默认0，
两个rank分别使用起始索引和下一索引。

## 判断结果

启动日志中的devId应为0/1。正常完成时，两个rank均输出：

- 收发轮数及总耗时、平均耗时、最大耗时。
- 设备同步完成及其耗时。
- 最终PASS汇总，进程退出码为0。

出现接口调用错误、数据失配或总耗时超限时，结合ERROR日志和非零退出码排查。

## 错误退出回归测试

编译后，在同样的CANN环境和设备条件下运行：

```bash
python3 test/integration/310p_sdma_stability/test_exit_status.py \
  --binary /tmp/mf_310p_sdma_stability \
  --kernel-json build/src/hybm/ops/hybm_kernel/libmf_sdma_orch_launch.json
```

脚本执行7轮真实收发，并通过临时预加载库改变host收到的接口返回值或抽样数据。
同步与清理类故障在真实接口完成后注入。检查内容包括接口失败、数据失配、
单个rank失败后的任务退出，以及正常完成对照。

运行需要C编译器，可通过--log-dir指定日志目录。

## 相关代码

- examples/memory_pool/01_basic/08_310p_aicore_aicpu_sdma/：AICore与AICPU SDMA功能示例。
- src/hybm/ops/hybm_kernel/hybm_sdma_orch_kernel.cc：AICPU请求处理。
