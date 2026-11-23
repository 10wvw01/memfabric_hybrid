# Ascend 310P3 矩阵乘与 AllReduce 融合示例

本目录演示如何在 Ascend 310P3 单卡双 die 上，将 vLLM Ascend 推理中部分线性层的矩阵乘和 AllReduce 合并为一次算子调用。目录中提供两种 Ascend C 实现、vLLM Ascend 接入代码，以及功能和性能测试脚本。

## 背景与现状

在两路张量并行（`TP=2`）下，每个 rank 只计算线性层的一部分结果。原生实现先分别完成矩阵乘，再调用 HCCL AllReduce，把两份结果相加并返回给两个 rank。本示例用 MemFabric 的 SDMA 在矩阵乘过程中传输已经算完的结果块，目的是让计算和通信部分重叠，缩短 prefill 阶段这类线性层的执行时间。

下图用两组结果说明执行顺序。横向位置只表示先后或重叠关系，不表示实测耗时。

```text
时间 →

原生实现
  矩阵乘       [计算第 1 组][计算第 2 组]
  HCCL                                   [对所有结果执行 AllReduce]

融合实现
  矩阵乘       [计算第 1 组][计算第 2 组]
  传输与求和                [处理第 1 组][处理第 2 组]
```

两种实现的输入输出相同，区别在于由谁执行加法：

| 算子 | 如何求和 |
| --- | --- |
| [`smem_mm_ar_310p3`](smem_mm_ar_310p3/README.md) | 两个 rank 用 SDMA 交换各自完整的矩阵乘结果，再由各自的 Vector 核求和。 |
| [`smem_mm_iar_310p3`](smem_mm_iar_310p3/README.md) | 每个 rank 用 SDMA `ADD_FP16` 将一半结果加到对端，再用 `COPY` 交换已经求和的部分。 |

两个算子都要求 FP16，矩阵乘的 `K`、`N` 均为 2048，输入行数 `M` 在 1024 到 4096 之间且是 256 的倍数。vLLM Ascend 默认不启用融合。已有测试中，部分行数下算子耗时下降，但单请求和低负载下未观察到明确的端到端收益，满载时表现更差。具体测试范围见下文「怎样看性能结果」。

## 软硬件环境

算子级和 E2E 示例都在 Linux aarch64 上运行，需要一张 Ascend 310P3 卡的两个 die，两个 rank 各用一个 die。下文的 `4,5` 是设备编号示例，请按目标机的实际编号替换。

| 用途 | 环境与依赖 |
| --- | --- |
| 两类示例共用 | CANN 9.1.0 GA、PyTorch 2.10、torch_npu 2.10，以及 `trunk_for_escc_v1.2.0` 版本的 MemFabric。以上是示例使用的版本。 |
| 构建和运行融合算子 | 构建需要 MemFabric 设备侧头文件；运行需要 `libmf_smem.so` 和 SDMA 编排内核清单。具体路径和构建命令见上表中的两篇算子文档。 |
| E2E 示例额外需要 | vLLM 0.24.0、本目录的 `vllm-ascend/` 接入代码，以及脚本使用的 Qwen3.6-35B-A3B-W8A8 模型。脚本默认开启 DFlash，还需要对应的 DFlash 模型。服务配置要求 `TP=2`、`PP=1`、`DP=1`、FP16。 |

独立运行算子不需要安装 vLLM，也不需要下载模型。

## 算子级示例

这组命令直接运行算子，不启动 vLLM 服务。先检查两个 rank 的输出，再比较算子耗时。

1. 按对应算子文档准备 CANN、MemFabric 和 PyTorch 环境，构建自定义算子包并安装 `xops` wheel。运行两种算子的对比脚本前，先构建并安装两种算子。
2. 在目标机上检查测试脚本中的 CANN、MemFabric 和模型路径。部分脚本写入了原测试环境的绝对路径，需要按实际安装位置修改。
3. 在本目录运行以下命令。`4,5` 是一组设备编号示例，换成目标机上同卡的两个 die。

先检查两端输出：

```bash
DEVICES=4,5 bash benchmark/functional/run_m2_v120.sh
```

确认脚本输出的 `summary.txt` 中本次各组均为 `PASS`；脚本在某组失败后仍会继续执行，不能只看退出码。再比较原生矩阵乘加 HCCL AllReduce 与两个融合算子：

```bash
DEVICES=4,5 bash benchmark/op_perf/run_op_bench_v120.sh
```

测试参数和判定方法见 [benchmark/README.md](benchmark/README.md)。

## E2E 示例：在 vLLM Ascend 中使用

### 1. 安装接入代码并启动服务

按「软硬件环境」准备依赖和模型，检查启动脚本中的路径。从本目录执行：

```bash
source /usr/local/Ascend/cann-9.1.0/set_env.sh
(cd vllm-ascend && python -m pip install -e . --no-build-isolation)
SMEM_OP=ar VLLM_ASCEND_DEVICE_OFFSET=4 bash benchmark/e2e_perf/vllm_serve_smem_v120.sh
```

脚本默认用 `ar`，改为 `SMEM_OP=iar` 可切换。用 `VLLM_ASCEND_DEVICE_OFFSET` 指定设备，不用 `ASCEND_RT_VISIBLE_DEVICES` 重映射编号。

<details>
<summary>配置备注：自行启动服务、图模式和行数条件</summary>

- 手动配置时，在 `--additional-config` 中只启用 `enable_smem_mm_ar_310p3` 或 `enable_smem_mm_iar_310p3` 之一，并将 `ASCEND_CUSTOM_OPP_PATH` 指向对应的 vendor 目录。
- 示例脚本默认开启 DFlash，图模式只覆盖 decode。自行启动时参考脚本中的配置。
- vLLM Ascend 默认从 1280 行起尝试使用融合算子，输入还须满足其他接入条件。1280 行不是性能收益的分界线。详见 [`smem_mm_310p3.py`](vllm-ascend/vllm_ascend/ops/smem_mm_310p3.py)。

</details>

### 2. 发送请求并确认融合生效

在另一个终端进入本目录，发送长、短两类请求，检查是否返回生成结果：

```bash
python3 benchmark/functional/e2e_functional_test.py
```

`/health` 只说明服务已启动。长请求发出后，检查两个 worker 的日志是否都有会话建立、层接入和 `first fused call verified bit-exact` 的记录。若出现 `FALLBACK to native`，查看该条日志给出的原因。

### 3. 比较首 token 延迟（TTFT）

脚本依次测试原生服务、`ar` 和 `iar`。

> **执行前**：停止手动启动的服务。脚本会停止匹配的 vLLM 进程，只在没有其他 vLLM 服务运行的专用测试机上执行。

```bash
bash benchmark/e2e_perf/run_e2e_matrix_v120.sh
```

## 怎样看性能结果

| 测试 | 看哪个指标 | 能回答什么 |
| --- | --- | --- |
| 算子级 | `msprof` 记录的设备侧耗时，取较慢 rank 的 P50 | 给定输入行数下，矩阵乘和 AllReduce 合计是否更快？ |
| E2E 单请求 | 用相同请求测量原生服务和融合服务的首 token 延迟（TTFT） | 单个请求等到首个 token 的时间是否缩短？ |
| E2E 满载 | 在相同满载条件下对比原生服务和融合服务 | 服务在持续高负载下是否受益？单请求结果不能代替满载测试。 |
