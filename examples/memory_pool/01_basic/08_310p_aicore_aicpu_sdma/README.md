# 310P 双 rank SDMA 往返示例

1. rank0 生成 8 个 128 KiB 数据块，批量调用 put NBI，quiet 成功后发布各块的应用轮次。
2. rank1 等待轮次、失效数据缓存，将每个 uint32_t 加上 0x100，再同步 put 回 rank0。
3. rank0 确认回程轮次后结束 kernel，Host 逐字校验返回数据。
4. 执行 8 次单块往返作为串行对照，并演示 FP32 put ADD：将远端初值 100.0 累加为 101.0。

应用布局在有效共享内存中：数据从段首开始，浮点累加区在 8 MiB，轮次区在 12 MiB。
每个轮次槽占 128 字节，接收标志和发送 scratch 各占一个缓存行。
状态初始化在双方开始传输前完成，轮次跨 kernel 递增。
数据 quiet 成功后发布轮次，回程确认保证下一轮复用数据时消费已完成。
辅助实现见上一级目录的 `sdma_ready.h`。

## 编译和运行

需要 CANN、bisheng、OpenMPI 以及主库构建产物。

1. 在仓库根目录加载 CANN 环境。

   ```bash
   source /usr/local/Ascend/cann-9.2.1/set_env.sh
   export ASCEND_HOME_PATH=/usr/local/Ascend/cann-9.2.1
   ```

2. 构建主库与 AICPU kernel。

   ```bash
   bash script/build_and_pack_run.sh
   ```

3. 通过 `npu-smi info` 确认 2 个连续设备索引空闲，并选择满足拓扑要求的设备运行。

   ```bash
   TEST_DEVICE_BASE=0 bash examples/memory_pool/01_basic/08_310p_aicore_aicpu_sdma/run.sh
   ```

4. 检查各 rank 的数据校验输出和进程退出码；任一校验失败均返回非零。

TEST_DEVICE_BASE 指定起始设备，两个 rank 使用连续设备索引，运行前确认同卡两个 die 空闲。
--build-only 仅编译到 /tmp/ex08，--run-only 运行该产物。
AICPU kernel 在建池时自动部署，脚本使用本仓库构建的 launch json。

全量数据校验、浮点累加校验或设备操作失败时返回非零。
长时间重复启动、设备同步及退出码检查保留在 test/integration/310p_sdma_stability。
