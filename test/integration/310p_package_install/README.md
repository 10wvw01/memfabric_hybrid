# 310P 安装包到 Python SHMEM 通信集成测试

覆盖两条完整使用路径：

1. 执行真实 `.run --install-path=...`，加载生成的 set_env.sh，使用安装器安装的 Python 包。
2. 创建独立环境，执行 `python -m pip install --no-deps --force-reinstall` 安装指定 wheel。

两条路径分别导入 Python 包和扩展、通过 Python SHMEM API 建池、调用 AICore 测试算子，
双 die 通过同步 get 互读 128 KiB，默认执行 100 轮并逐字检查数据，最后释放内存池。
每轮先准备本端源数据，Python SHMEM 栅栏确认双方源就绪，再调用 get；
完成校验后的栅栏保护下一轮源缓冲复用。
测试取消 MF_SDMA_ORCH_JSON，检查自动发现配置和 AICPU 部署、清理日志。
同时校验导入包、扩展位于对应测试虚拟环境，且已安装三份 AICPU 产物。

## 前置条件

310P3 同卡两个空闲 die，已加载 CANN 环境，具备 bisheng、C++ 编译器和 Python venv/pip。
基础 Python 环境已安装与设备匹配的 PyTorch、torch_npu 及 memfabric 的运行依赖。
虚拟环境通过 --system-site-packages 复用这些依赖，本次 memfabric 安装在独立环境内。
提前运行 `bash script/build_and_pack_run.sh` 生成当前 Python ABI 的 `.run` 和 wheel。

## 运行

1. 在仓库根目录加载 CANN 环境，并通过 `npu-smi info` 选择同卡两个空闲 die。

   ```bash
   source /usr/local/Ascend/cann-9.2.1/set_env.sh
   ```

2. 构建当前 Python ABI 对应的安装包和 wheel。

   ```bash
   bash script/build_and_pack_run.sh
   ```

3. 执行两种安装路径的通信测试。

   ```bash
   DEVICE_BASE=2 bash test/integration/310p_package_install/run.sh
   ```

4. 检查安装、导入路径、双 rank 数据校验及部署日志，确认脚本退出码为零。

可通过 RUN_PACKAGE、WHEEL_PACKAGE 指定待测产物绝对路径，通过 LOG_DIR 指定全新日志目录，
ROUNDS 指定正整数轮数，STORE_URL 指定配置存储地址。默认产物来自当前仓库 output 目录。
默认日志、安装目录、虚拟环境和测试算子产物均位于 /workspace/my-logs-dir/package-install 下。
测试使用安装包中的公共头文件编译测试算子，主库和 AICPU kernel 使用安装后的二进制。

## 判定与产物

安装、导入、路径检查、编译、双 rank 运行或部署日志检查失败，脚本返回非零。
每个 rank 有 240 秒运行上限。日志保留安装输出、编译输出和各 rank 的校验及部署记录。
日志目录内保留安装目录和虚拟环境供复查，测试结束后可由使用者统一清理该目录。
