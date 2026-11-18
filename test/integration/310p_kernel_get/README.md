# 310P kernel 内 ready 通知与 Get 测试

两个 rank 各启动一次通信 kernel，在 kernel 内交替充当生产方和消费方，完成所有轮次。
消费方先确认本轮 ready 尚未到达，再用 MemFabric Put 请求对端生产数据。
生产方等待请求、延迟计算、写入源数据并处理缓存，再用 Put 发布本轮 ready。
消费方在 kernel 内等待 ready，调用同步 Get，处理目标缓存并逐字校验完整数据，
最后用 Put 通知生产方读取完成。生产方收到通知后才允许复用源缓冲。
每轮使用递增序号，数据包含 rank、轮次和元素下标，目标缓冲预填错误值。

ready、请求和读取完成标志均为测试实现的单写者协议，使用 MemFabric COPY Put 搬运通知。
这些辅助函数位于测试文件内；测试无需整数原子加或新增公共 API。
Host 负责建池、启动前控制区初始化及同步、启动 kernel、读取最终状态和释放资源。
数据生产、ready 判断、Get 和跨轮缓冲复用均在通信 kernel 内完成。
启动前和释放前各有一次 Host barrier；轮次之间没有 Host barrier 或 Host 数据拷贝。

## 运行

加载 CANN 环境，并运行 `bash script/build_and_pack_run.sh` 生成安装包后，在仓库根目录执行：

```bash
bash test/integration/310p_kernel_get/run.sh
ROUNDS=1000 BYTES=1048576 bash test/integration/310p_kernel_get/run.sh
```

需要同一张 310P 卡的两个空闲 die，双 rank 建池会校验该拓扑要求。
默认使用设备 2、3，执行 100 轮，每次 Get 128 KiB。
通过 DEVICE0、DEVICE1 指定空闲设备，通过 STORE_URL 指定配置存储地址，
RUN_PACKAGE 指定安装包，LOG_DIR 指定独立日志目录。
轮数范围为 1..10000；数据大小为 64..4194304 字节，要求 64 字节对齐。
脚本将安装包部署到独立目录和 Python 虚拟环境，使用安装后的公共头文件编译。
编译、每个 rank 的运行均记录日志，rank 超时或任何协议、数据检查失败均返回非零。
失败日志包含 rank、轮次、协议阶段、错误位置及实际数据。
