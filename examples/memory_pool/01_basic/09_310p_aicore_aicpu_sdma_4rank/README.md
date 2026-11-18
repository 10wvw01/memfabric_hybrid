# 310P 四 rank、八 block get AllReduce

1. 每个 rank 由 8 个 AICore block 分别准备 1 MiB 的 uint32_t 分片并维护源缓存。
2. Host 同步准备 kernel，再用 MPI 栅栏确认所有源数据就绪。
3. 各 block 独立向三个对端发起 get NBI，quiet 完成后失效目标缓存，再与本端数据逐元素求和。
4. Host 检查每个 block 的完成状态及完整的 8 MiB 输出。

结果保持 uint32_t SUM 语义：
每个源元素为 rank * 4096 + (i & 0xFFF)，结果为 6 * 4096 + 4 * (i & 0xFFF)。

有效数据区布局：本端分片 16–24 MiB，接收分片 24–48 MiB，结果 48–56 MiB，
状态字从 60 MiB 开始。接收槽按应用定义的三个来源和八个 block 紧凑排列。

## 编译和运行

需要两张 Atlas 300I Duo 或满足四 rank P2P 映射要求的设备，以及 CANN、bisheng、OpenMPI。

1. 在仓库根目录加载 CANN 环境。

   ```bash
   source /usr/local/Ascend/cann-9.2.1/set_env.sh
   export ASCEND_HOME_PATH=/usr/local/Ascend/cann-9.2.1
   ```

2. 构建主库与 AICPU kernel。

   ```bash
   bash script/build_and_pack_run.sh
   ```

3. 通过 `npu-smi info` 确认 4 个连续设备索引空闲，并选择满足拓扑要求的设备运行。

   ```bash
   TEST_DEVICE_BASE=0 bash examples/memory_pool/01_basic/09_310p_aicore_aicpu_sdma_4rank/run.sh
   ```

4. 检查各 rank 的数据校验输出和进程退出码；任一校验失败均返回非零。

TEST_DEVICE_BASE 指定 rank0 设备，其他 rank 依次递增。
--build-only 仅编译到 /tmp/ex09，--run-only 运行该产物。
建池执行拓扑检查并自动部署 AICPU kernel，传输或全量校验失败时返回非零。

该示例覆盖四 rank 的应用规约；基础 put/get 尺寸、分块及地址边界由 PTO sdma_direct 测试覆盖。
