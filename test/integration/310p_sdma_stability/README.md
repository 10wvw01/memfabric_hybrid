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

## 环境、构建和运行

需要 Ascend310P3、CANN、bisheng 和 MPI，编译与运行使用同一套 MPI。

1. 在仓库根目录加载 CANN 环境，并通过 `npu-smi info` 选择同卡两个空闲 die。

   ```bash
   source /usr/local/Ascend/cann-9.2.1/set_env.sh
   export ASCEND_HOME_PATH=/usr/local/Ascend/cann-9.2.1
   ```

2. 构建主库及测试程序。

   ```bash
   bash script/build_and_pack_run.sh
   bash test/integration/310p_sdma_stability/run.sh --build-only
   ```

3. 清除设备可见性映射，显式指定所选设备运行。

   ```bash
   unset ASCEND_RT_VISIBLE_DEVICES
   TEST_DEVICE_BASE=0 STRESS_ROUNDS=30000 \
     bash test/integration/310p_sdma_stability/run.sh --run-only
   ```

4. 按下节检查数据、设备同步和进程退出状态。

STRESS_ROUNDS 使用正整数，默认 30000；两个 rank 分别使用 TEST_DEVICE_BASE 及其下一索引。
AICPU kernel 启动配置使用当前构建目录中的 `libmf_sdma_orch_launch.json`。
测试程序位于 `/tmp/mf_310p_sdma_stability`，应顺序运行，避免并发覆盖产物。

## 判断结果

启动日志中的devId应为0/1。正常完成时，两个rank均输出：

- 收发轮数及总耗时、平均耗时、最大耗时。
- 设备同步完成及其耗时。
- 最终PASS汇总，进程退出码为0。

出现接口调用错误、数据失配或总耗时超限时，结合ERROR日志和非零退出码排查。

## 错误退出回归测试

1. 完成上述编译，在同一 CANN 环境下选择同卡两个空闲 die，例如设备 2/3。
2. 运行退出码测试。

```bash
unset ASCEND_RT_VISIBLE_DEVICES
python3 test/integration/310p_sdma_stability/test_exit_status.py \
  --device-base 2 \
  --binary /tmp/mf_310p_sdma_stability \
  --kernel-json build/src/hybm/ops/hybm_kernel/libmf_sdma_orch_launch.json
```

3. 检查各故障场景的预期退出状态及正常完成对照。
4. 检查脚本退出码和独立日志目录。

脚本执行7轮真实收发，并通过临时预加载库改变host收到的接口返回值或抽样数据。
同步与清理类故障在真实接口完成后注入。检查内容包括接口失败、数据失配、
单个rank失败后的任务退出，以及正常完成对照。

运行需要C编译器，可通过--log-dir指定日志目录。

设备起始索引也可通过TEST_DEVICE_BASE设置，--device-base优先。脚本保留
ASCEND_RT_VISIBLE_DEVICES，设备索引按该变量指定的可见设备列表解释。
默认选择设备0/1；选择当前容器的设备2/3时，先清除不需要的可见性映射。
每个MPI任务限时45秒，超时仍判为失败；指定--log-dir时保存该项输出、退出码及超时标记。

## 相关代码

- examples/memory_pool/01_basic/08_310p_aicore_aicpu_sdma/：AICore与AICPU SDMA功能示例。
- src/hybm/ops/hybm_kernel/hybm_sdma_orch_kernel.cc：AICPU请求处理。
