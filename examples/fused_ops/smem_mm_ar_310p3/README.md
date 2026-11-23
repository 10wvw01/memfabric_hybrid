# smem_mm_ar_310p3：矩阵乘与 AllReduce（Vector 核求和）

本算子在 Ascend 310P3 单卡双 die 上运行。两个 rank 各自计算矩阵乘，再交换结果并求和；每个 rank 都得到完整的和。本文说明独立构建、调用和验证方法。vLLM Ascend 的使用方法见[总览](../README.md)。

## 计算过程

1. Cube 核把本 rank 的矩阵乘结果按行分组写到 MemFabric 对称内存池。
2. 每组结果就绪后，SDMA 用 `COPY` 将本 rank 的完整结果组写入对端的接收区，并通知对端数据已就绪。
3. Vector 核读取本端结果和接收区中的对端结果，逐元素相加后写入输出。最后一组按实际行数处理，不补齐行数。

## 输入输出与调用要求

| 张量 | 形状 | 类型与布局 |
| --- | --- | --- |
| `x` | `[M, 2048]` | 连续 FP16，ND |
| `w_nz` | `[2048, 2048]` | FP16 权重分片，实际存储为 FRACTAL_NZ，可用 `torch_npu.npu_format_cast(w, 29)` 转换 |
| `y` | `[M, 2048]` | 连续 FP16，ND；可由调用方预分配 |

本地计算为 `x @ w.T`，输出为两个 rank 的本地结果之和。`M` 必须在 1024 到 4096 之间，且是 256 的倍数。两个进程使用相同的配置存储地址，在同卡两个 die 上分别以 rank 0、1 创建会话。建池应先于业务张量分配，两个进程的池基址必须相同；每次调用的顺序和行数也必须一致。

直接调用 `torch_npu.npu_smem_mm_ar_310p3` 时，在三个张量之后依次传入 `gva`、`symm`、`statusPtr`、`tracePtr`、`rank`、`epoch`、`rows`。推荐使用 `SmemSession`，由它检查池大小和基址、分配状态与诊断缓冲区，并递增调用编号 `epoch`。池大小下限可从 Python 包的 `POOL_FLOOR_BYTES` 查询。

## 环境与构建

| 项目 | 示例环境 |
| --- | --- |
| 操作系统与架构 | Linux aarch64 |
| 设备 | Ascend 310P3 单卡双 die，两个 rank 各用一个 die |
| CANN | 9.1.0 GA |
| PyTorch / torch_npu | 均为 2.10 |
| MemFabric | `trunk_for_escc_v1.2.0`；构建算子需要设备侧头文件，运行需要 `libmf_smem.so` 和 SDMA 编排内核清单 |

在对应版本的 MemFabric 仓库中执行 `bash script/build_and_pack_run.sh`，构建后确认上表所列文件齐全。

在本算子目录执行，先把 `MF_ROOT` 改为实际的 MemFabric 仓库路径：

```bash
source /usr/local/Ascend/cann-9.1.0/set_env.sh
MF_ROOT=/path/to/memfabric_trunk_v120
export MIXFABRIC_SDK_OUT="$MF_ROOT/output"
export MEMFABRIC_SDK_INC="$MIXFABRIC_SDK_OUT/smem/include/device"
bash build.sh
PKG_ROOT="$PWD/build_out/_CPack_Packages/Linux/External/custom_opp_ubuntu_aarch64.run"
export ASCEND_CUSTOM_OPP_PATH="$PKG_ROOT/packages/vendors/smem_mm_ar_310p3"
(cd xops && bash build_and_install.sh)
```

算子包位于 `build_out/`，上面的命令直接使用构建时展开的 vendor 目录。启动调用进程前保留 `ASCEND_CUSTOM_OPP_PATH` 和 `MIXFABRIC_SDK_OUT`，并设置：

```bash
export MF_SDMA_ORCH_DIR="$MIXFABRIC_SDK_OUT/hybm/aicpu_kernel"
export MEMFABRIC_HYBRID_EXTEND_LIB_PATH="$MIXFABRIC_SDK_OUT/hybm/lib64"
export LD_LIBRARY_PATH="$MIXFABRIC_SDK_OUT/smem/lib64:$MEMFABRIC_HYBRID_EXTEND_LIB_PATH:$LD_LIBRARY_PATH"
export LD_LIBRARY_PATH="$MIXFABRIC_SDK_OUT/acc_links/lib64:$ASCEND_CUSTOM_OPP_PATH/op_api/lib:$LD_LIBRARY_PATH"
```

## Python 调用

以下是每个 rank 进程内的调用顺序；设备编号、rank 和输入张量由调用方提供。先建会话，再分配业务张量。

```python
from xops_ar_310p3 import SmemSession

session = SmemSession(device=device, rank=rank, store_url="tcp://127.0.0.1:8730")
# 在会话建立后准备本 rank 的 x 和 w_nz。
y = session.launch(x, w_nz, rows=x.shape[0])
# 完成对 y 的使用后，两端都在退出前释放会话。
session.teardown()
```

配置存储由 rank 0 启动。独立验证时按功能测试脚本的顺序，两端先发一次预热调用，再检查正式调用的结果。不要在每次调用后添加设备全局同步。

## 验证与源码位置

双 rank 正确性测试和算子耗时对比见 [测试说明](../benchmark/README.md)。vLLM Ascend 对应配置项为 `enable_smem_mm_ar_310p3`；服务选用本算子时，vendor 目录也要对应本算子。

| 路径 | 内容 |
| --- | --- |
| [`op_host/`](op_host/) | 算子定义、形状推导和启动参数校验 |
| [`op_kernel/`](op_kernel/) | Cube 和 Vector 核入口、内存池布局、SDMA 传输及错误码 |
| [`xops/`](xops/) | PyTorch 算子注册与 `SmemSession` 实现 |
