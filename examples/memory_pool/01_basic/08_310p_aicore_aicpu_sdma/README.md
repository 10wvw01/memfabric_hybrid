# 310P 双 rank SDMA 往返示例

rank0 生成 8 个 128 KiB 数据块，批量调用 Put NBI，quiet 成功后发布各块的应用轮次。
rank1 等待轮次、失效数据缓存，将每个 uint32_t 加上 0x100，再同步 Put 回 rank0。
rank0 确认回程轮次后结束 kernel，Host 逐字校验返回数据。
随后执行 8 次单块往返作为串行对照，并演示 FP32 Put ADD：将远端初值 100.0 累加为 101.0。

应用布局在有效共享内存中：数据从段首开始，浮点累加区在 8 MiB，轮次区在 12 MiB。
每个轮次槽占 128 字节，接收标志和发送 scratch 各占一个缓存行。
状态初始化在双方开始传输前完成，轮次跨 kernel 递增。
数据 quiet 成功后发布轮次，回程确认保证下一轮复用数据时消费已完成。
辅助实现见同级 sdma_ready.h。

## 编译和运行

需要 CANN、bisheng、OpenMPI 以及主库构建产物。先加载 CANN 环境：

    source /usr/local/Ascend/cann-9.2.1/set_env.sh
    export ASCEND_HOME_PATH=/usr/local/Ascend/cann-9.2.1
    bash script/build_and_pack_run.sh
    TEST_DEVICE_BASE=0 bash examples/memory_pool/01_basic/08_310p_aicore_aicpu_sdma/run.sh

TEST_DEVICE_BASE 指定起始设备，两个 rank 使用连续设备索引，运行前确认同卡两个 die 空闲。
--build-only 仅编译到 /tmp/ex08，--run-only 运行该产物。
AICPU kernel 在建池时自动部署，脚本使用本仓库构建的 launch json。

全量数据校验、浮点累加校验或设备操作失败时返回非零。
长时间重复启动、设备同步及退出码检查保留在 test/integration/310p_sdma_stability。
