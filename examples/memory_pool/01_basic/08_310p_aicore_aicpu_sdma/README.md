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

### 新机器前置条件

- 310P3 设备，同卡两个 die 空闲（`npu-smi info` 确认）
- CANN 自带 `libaicpu_extend_kernels.so`（CP1 启动时预加载，搬运 kernel 由它加载；
  CANN 9.0.0 + 2026-11-18 驱动已验证）
- bisheng（`--npu-arch=dav-2002`）与 OpenMPI（编译和运行必须同一套）
- 设备侧无需任何手工部署：建池时库自动把 kernel 以 memfd 方式加载进 CP1

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

## 新机器验证判据

1. 双 rank 打印 `=== EX08 r0 PASS ===` / `=== EX08 r1 PASS ===`，退出码 0。
2. 构建产物 `build/src/hybm/ops/hybm_kernel/libmf_sdma_orch.so` 里能看到
   `nm -D | grep TileFwk` 的三个导出符号。
3. 设备日志 `/root/ascend/log/run/device-<die>/` 出现
   `Get api DynTileFwkKernelServerInit from so libaicpu_extend_kernels.so success`，
   且进程名为 `aicpu_scheduler`（CP1）。若看到 `aicpu_custom_scheduler`，说明走错链路。
4. 部署失败时先查 `MF_SDMA_ORCH_DIR` 是否指向构建产物目录
   （如 `build/src/hybm/ops/hybm_kernel`，须恰好一套三件套），再查目标机 CANN 是否带 extend lib。
   完整链路说明见 `docs/310p/部署档案.md`。
