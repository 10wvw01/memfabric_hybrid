# TP=2 SHMEM与HCCL统一AllReduce基准

构建项目并加载CANN环境后执行：

```bash
DEVICE_BASE=2 BACKEND=both LOG_DIR=/path/to/logs bash run.sh
```

同一程序提供MF/HCCL后端，两进程、FP32原地SUM，测试256、512、1024、2048、4096方阵。BACKEND=both依次运行mf-signal和hccl，BACKEND=all运行全部五种模式，也可指定空格分隔的后端列表：

```bash
DEVICE_BASE=2 BACKEND=all \
 BLOCKS=8 CHUNKS=8 WARMUP=10 ROUNDS=100 LOG_DIR=/path/to/logs bash run.sh
```

| 后端 | 数据与通知组织 |
|---|---|
| mf-sync | 逐块同步put，独立SET通知 |
| mf-nbi | 分块NBI提交，统一quiet，独立SET通知 |
| mf-signal | 最后一块同步put_signal |
| mf-signal-nbi | 最后一块NBI put_signal，再统一quiet |
| hccl | HcclAllReduce，同样的输入与原地输出 |

BLOCKS取1/2/4/8，每block处理独立片段；CHUNKS是每block每阶段分块数，可取1～64的2次幂。HCCL处理整个矩阵。默认预热10轮、正式100轮；支持TIMEOUT、STORE_URL、MF_SDMA_ORCH_DIR、HCCL_BUFFSIZE，HCCL默认64MiB缓冲、启用PCIe并关闭RoCE。

每轮恢复输入并执行相同barrier后计时，完成后逐元素检查。Host总耗时从记录开始Event前到stream同步返回；Event耗时为两个Event之间的间隔；enqueue为提交调用耗时。初始化、输入恢复、barrier和检查在计时外，这三个计时不能相加。

`summarize.py LOG_DIR`按同轮两个rank的较大值聚合，输出平均、P50/P95/P99及最大值，写入日志目录summary.json。运行前确认设备可用，测试程序不会清理其他进程。基准不调整库内部等待或调度策略。

## 四种 MemFabric 实现的 API 与一次传输流程

四种模式均由 AICore 按 block 向请求队列提交请求，由 AICPU 编排 SDMA 执行。
它们沿两个维度组合：数据逐块同步完成或 NBI 提交后统一等待，以及通知独立提交或附在最后一块数据请求中。
具体调用见 [main.asc](main.asc) 的 `reducePhase`，
接口定义见 [smem_shm_aicore_base_sdma.h](../../src/smem/include/device/smem_shm_aicore_base_sdma.h)。

### 使用的 API 及返回行为

`ctx = smem_shm_sdma_ctx(gva)` 从共享池基础地址获取当前 rank、block、对称段大小和队列等信息。
带 `_at` 后缀的接口显式接收这个上下文；对应的 `gva` 便利封装在内部获取上下文后转发到 `_at` 接口。
本基准使用 `_at` 接口，以下说明对应基准中的非零长度传输。

| API | 行为与成功返回时的状态 |
|---|---|
| `smem_shm_sdma_put_at(ctx, src, dst, bytes, op)` | 提交数据请求并调用 quiet，成功返回时当前 block 此前提交的传输已完成 |
| `smem_shm_sdma_put_nbi_at(ctx, src, dst, bytes, op)` | 成功返回表示请求已提交；源缓冲保持有效至后续 quiet 成功，请求环满时会等待空位 |
| `smem_shm_sdma_put_signal_at(ctx, src, dst, bytes, signal, value, op)` | 在一个请求中组织数据操作与随后对远端通知槽的 SET，再调用 quiet 等待完成 |
| `smem_shm_sdma_put_signal_nbi_at(ctx, src, dst, bytes, signal, value, op)` | 提交携带 SET 通知的数据请求，数据与通知完成由后续 quiet 等待 |
| `smem_shm_sdma_quiet_at(ctx)` | 等待调用 block 向各对端队列提交的请求完成，成功后可复用这些请求的源缓冲 |
| `smem_shm_sdma_signal_set_at(ctx, signal, value)` | 独立提交一次远端 64 位 SET 通知，并调用 quiet 等待完成 |
| `smem_shm_signal_wait_until(signal, SMEM_SHM_CMP_EQ, value)` | 维护本地通知缓存行并轮询，观察到指定值后成功返回，轮询预算耗尽时返回失败 |

这里的 `src` 是本端源地址，`dst` 是对端目标地址。
`op=SMEM_SHM_SDMA_OP_ADD_FP32` 将源数据加到远端目标上，
`op=SMEM_SHM_SDMA_OP_COPY` 将源数据复制到远端目标。
携带通知的请求在数据操作完成后写入通知值；接收方观察到该值时，该请求的数据操作已完成。
数据区域的缓存维护由调用方负责；当前基准由 Host DMA 恢复输入，AllReduce kernel 通过 SDMA 操作数据。

`quiet_at` 检查本端提交请求的完成状态，`signal_wait_until` 检查对端写入本地通知槽的值。
接收方完成数据消费、发送方复用远端目标缓冲的时机由应用同步协议协调。
本基准在每轮恢复输入和完成结果检查后通过 Host 栅栏同步，两个阶段分别使用独立通知槽和本轮序号。

### 四种模式怎样组合 API

| 模式 | 前 CHUNKS−1 块 | 最后一块 | 本阶段的数据完成等待 | 本阶段的通知 |
|---|---|---|---|---|
| `mf-sync` | 同步 put_at | 同步 put_at | 每个 put_at 内部执行 quiet | 数据完成后额外调用 signal_set_at |
| `mf-nbi` | put_nbi_at | put_nbi_at | 全部分块提交后调用一次 quiet_at | 数据完成后额外调用 signal_set_at |
| `mf-signal` | 同步 put_at | 同步 put_signal_at | 每块的同步接口内部执行 quiet | 随最后一块数据请求提交 |
| `mf-signal-nbi` | put_nbi_at | put_signal_nbi_at | 全部分块提交后调用一次 quiet_at | 随最后一块数据请求提交 |

表中的数据与通知提交接口均带 `smem_shm_sdma_` 前缀。
两种独立通知模式的 `signal_set_at` 还会等待通知完成。
CHUNKS=1 时直接走“最后一块”列。四种模式在阶段结束前都调用
`smem_shm_signal_wait_until` 等待对端的同阶段通知，再进入下一阶段。

### 伪代码：一个 rank、一个 block 的一次阶段传输

以下伪代码对应 `reducePhase`。每个分块是 `(src, dst, bytes)`，
`peer_flag` 指向对端接收本 block 通知的槽，`local_flag` 指向本端接收对端通知的槽。
两个 rank 对称执行，通知槽按 block 和阶段独立分配，`sequence` 随轮次更新。
`require(ok)` 表示调用失败时记录错误状态并终止当前 block 的后续传输。

```text
function transfer_stage(mode, ctx, chunks, peer_flag, local_flag, sequence, op):
    nbi   = mode in {"mf-nbi", "mf-signal-nbi"}
    fused = mode in {"mf-signal", "mf-signal-nbi"}

    for i, (src, dst, bytes) in enumerate(chunks):
        last = (i == len(chunks) - 1)
        if fused and last:
            if nbi:
                require(smem_shm_sdma_put_signal_nbi_at(
                    ctx, src, dst, bytes, peer_flag, sequence, op))
            else:
                require(smem_shm_sdma_put_signal_at(
                    ctx, src, dst, bytes, peer_flag, sequence, op))
        else:
            if nbi:
                require(smem_shm_sdma_put_nbi_at(ctx, src, dst, bytes, op))
            else:
                require(smem_shm_sdma_put_at(ctx, src, dst, bytes, op))

    if nbi:
        require(smem_shm_sdma_quiet_at(ctx))

    if not fused:
        require(smem_shm_sdma_signal_set_at(ctx, peer_flag, sequence))

    require(smem_shm_signal_wait_until(local_flag, SMEM_SHM_CMP_EQ, sequence))

# 一轮 AllReduce 按顺序执行两个阶段：
ctx = smem_shm_sdma_ctx(gva)
transfer_stage(mode, ctx, scatter_chunks, peer_scatter_flag, local_scatter_flag,
               sequence, SMEM_SHM_SDMA_OP_ADD_FP32)
transfer_stage(mode, ctx, gather_chunks, peer_gather_flag, local_gather_flag,
               sequence, SMEM_SHM_SDMA_OP_COPY)
```

矩阵总字节数为 N，每个 block 在每阶段处理 N/(2×BLOCKS) 字节，再等分为 CHUNKS 块。
`scatter_chunks` 对应“对端负责的半区”，将该区数据发送到对端相同偏移并执行 ADD_FP32；
`gather_chunks` 对应“本端负责的、已经求和的半区”，将该区数据复制到对端相同偏移。
因此每个 rank 每轮发送 N 字节，并在两个阶段的通知同步后获得完整 AllReduce 结果。
