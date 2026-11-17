# 310P AICPU 首次自动部署回归

先在仓库根目录构建主库，再运行 `bash test/integration/310p_aicpu_deploy/run.sh`。
通过 DEVICE_BASE 选择空闲的同卡两个 die，默认使用逻辑设备 2/3。
通过 LOG_DIR 指定日志目录，BYTES 和 ROUNDS 调整双向传输数据量和轮数。

测试使用 SHMEM 公共 API 建池并启动 AICore signal/wait/quiet 通信。
连续运行两组独立进程，每组检查目标路径缺失后自动部署、复制校验、全量数据校验、
释放时删除设备文件的日志。每次自动生成加载名称，因此已有设备文件不会绕过部署。
脚本单独编译诊断程序 inspect，在建池后检查设备文件存在，进程退出后检查文件已删除。同卡两个 die 共用设备文件系统，诊断进程保持单一 ACL 上下文。
DEPLOY_INSPECT_SECONDS 控制建池后的暂停时间，默认为 15 秒。
日志和二进制保存在 LOG_DIR，脚本任一阶段失败返回非零。

## 部署失败后重新启动

运行 `DEVICE_BASE=4 bash test/integration/310p_aicpu_deploy/run_retry.sh`。
需安装 OpenMPI，MPI_CXX 和 MPI_RUN 可指定编译器及启动器。
分别构造一个 rank 缺少 SO、JSON 无效、KFC 调用失败和复制后的校验失败，检查两端建池失败且任务返回指定非零状态。
脚本修复测试副本后启动新的双 rank 任务，完成 128 KiB、10 轮双向全量数据校验。
初始化错误、超时或崩溃均判定为失败。每个任务有 180 秒上限。
通过 LOG_DIR 指定全新日志目录，STORE_URL 指定独立存储地址。
复制后校验失败场景同时检查目标文件清理日志。
