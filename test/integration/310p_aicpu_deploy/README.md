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
