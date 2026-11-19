# 310P AICPU 首次自动部署回归

1. 在仓库根目录加载 CANN 环境，并通过 `npu-smi info` 选择同卡两个空闲 die。

   ```bash
   source /usr/local/Ascend/cann-9.2.1/set_env.sh
   ```

2. 构建主库和 AICPU kernel。

   ```bash
   bash script/build_and_pack_run.sh
   ```

3. 运行自动部署测试，`DEVICE_BASE` 指定起始设备，默认使用逻辑设备 2/3。

   ```bash
   DEVICE_BASE=2 bash test/integration/310p_aicpu_deploy/run.sh
   ```

4. 检查两组任务的数据校验、部署和清理日志及脚本退出码。

通过 LOG_DIR 指定日志目录，BYTES 和 ROUNDS 调整双向传输数据量和轮数。

测试使用 SHMEM 公共 API 建池并启动 AICore 同步 put 通信，双方完成后的主机控制栅栏保证接收数据就绪。
连续运行两组独立进程，每组检查目标路径缺失后自动部署、复制校验、全量数据校验、
释放时删除设备文件的日志。每次自动生成加载名称，因此已有设备文件不会绕过部署。
脚本单独编译诊断程序 inspect，在建池后检查设备文件存在，进程退出后检查文件已删除。同卡两个 die 共用设备文件系统，诊断进程保持单一 ACL 上下文。
DEPLOY_INSPECT_SECONDS 控制建池后的暂停时间，默认为 15 秒。
日志和二进制保存在 LOG_DIR，脚本任一阶段失败返回非零。

## 部署失败后重新启动

1. 完成上述环境配置和构建，安装 OpenMPI；`MPI_CXX` 和 `MPI_RUN` 可指定编译器及启动器。
2. 选择同卡两个空闲 die，运行故障注入与重新启动测试。

   ```bash
   DEVICE_BASE=4 bash test/integration/310p_aicpu_deploy/run_retry.sh
   ```

3. 检查各故障任务的预期退出状态及修复后新任务的数据校验。
4. 检查日志目录中的设备文件清理记录和脚本退出码。
分别构造一个 rank 缺少 SO、JSON 无效、KFC 调用失败和复制后的校验失败，检查两端建池失败且任务返回指定非零状态。
脚本修复测试副本后启动新的双 rank 任务，完成 128 KiB、10 轮双向全量数据校验。
初始化错误、超时或崩溃均判定为失败。每个任务有 180 秒上限。
通过 LOG_DIR 指定全新日志目录，STORE_URL 指定独立存储地址。
复制后校验失败场景同时检查目标文件清理日志。
