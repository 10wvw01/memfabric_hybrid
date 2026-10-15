# 310P3 TP=2 FP16 o_proj + MemFabric ABI v7 实现设计

> 最终计算通信方案：
> [计算通信协作优化方案.md](计算通信协作优化方案.md)
>
> 开发/实机 Gate：
> [310p_memfabric_mm_ar_development_plan.md](310p_memfabric_mm_ar_development_plan.md)
>
> 使用：
> [310p_memfabric_mm_ar_usage.md](../user_guide/feature_guide/310p_memfabric_mm_ar_usage.md)

## 1. 目标路径

```text
eligible Qwen full-attention o_proj
  -> one custom op
  -> 8-core cooperative local MM
  -> per-batch MemFabric exchange
  -> per-batch FP16 local reduce
  -> already TP-reduced output
```

命中目标层后 `reduce_results=False`，不得再执行 generic HCCL all-reduce。

## 2. 固定计算形态

```text
baseM = 256
baseN = 256
baseK = 64
blockDim = 8
TCubeTiling.usedCoreNum = 8
```

`batch_m = 256 * q`，其中 `q in {1,2,4}`，默认 q=2。

每个 producer launch 只处理一个 batch。8 个 AI Core 对同一 A/B/C 逻辑矩阵做
cooperative MM。每个 core MM 完成后 clean cache，并写：

```text
ready[batch][core] = generation
```

每个 ready cell 独占 64B。core0 同样参加 MM；它确认 8 个 ready 后，作为唯一
producer 执行：

```cpp
smem_shm_sdma_signal(gva, send_batch, peer_recv_batch, batch_bytes, batch_id)
```

## 3. Batch pipeline

runtime 使用 lookahead=1：

```text
gate(wave)
P0
P1
W0
A0
P2
W1
A1
...
drain W/A(last)
quiet()
ack(wave credit)
```

因此目标 overlap 是：

```text
SDMA(n)   || MM(n+1)
SDMA(n+1) || wait/reduce(n)
```

`wait_batch` 每次只消费一封 data mail，严格校验：

```text
status == OK
dst == expected_recv_base + batch_index * batch_bytes
len == batch_bytes
imm == batch_index
```

wait 热路径不做 quiet。所有 batch drain 后一波只调用一次 public `quiet()`。

## 4. Arena / wave

FP16 N=2048 时：

```text
row_bytes = 4096
arena_bytes = arena_rows * row_bytes
max_batches = arena_rows / batch_m
```

`arena_rows` 从 `VLLM_ASCEND_310P_MEMFABRIC_LOCAL_BYTES` 的固定预算推导，
上限 8192 rows，并向下对齐到 `batch_m`。默认 local pool 为 96 MiB。

send/recv arena 大小不因 q=1/2/4 直接成倍扩张；batch 大小只改变一波可容纳的
batch 数。

wave 复用仍由 fixed credit 保护：

```text
prepare -> gate(credit) -> batch pipeline -> quiet -> ack(credit)
```

## 5. Tail

最后一个不满 `batch_m` 的 batch：

```text
scratch zero
copy valid rows
8-core cooperative MM on full batch_m
signal full batch payload
wait full batch mail
add only valid rows into output
```

不再选择 16/32/64/128-row 单核 bucket。

## 6. MemFabric public boundary

Host 只使用 public lifecycle/readiness/control API；device 只使用：

```cpp
smem_shm_sdma_signal(...)
smem_shm_sdma_wait(...)
smem_shm_sdma_quiet(...)
```

peer-space mail dst 通过 public control allgather 交换 GVA geometry 后校验。

vLLM 不读取 MemFabric private ring/mailbox/reserved/SQE/orchestrator 布局。

## 7. Fail-stop

状态码：

- READY_TIMEOUT
- SIGNAL_FAILED
- QUIET_FAILED
- WAIT_FAILED
- MAIL_MISMATCH
- CREDIT_MISMATCH
- ACK_FAILED

device 记录状态、clean status cache line 后 `AscendC::Trap()`。host 对已发生
异常的 runtime 标记 poisoned；不得把部分 recv 数据当成正常结果继续推进。

## 8. Graph / lifecycle

capture 前必须完成：

- context create；
- producer scratch allocation；
- fixed credit/protocol init；
- producer/wait/add/protocol kernel binary warmup。

capture 内禁止 malloc/create/host barrier/warmup sync。

普通 eager context 固定一个 execution stream。Graph-used context 当前保守保持
process lifetime，避免无法证明 replay stream quiescent 时销毁 pool。

## 9. 代码地图

```text
vllm_ascend/_310p/ops/memfabric_mm_ar.py
vllm_ascend/_310p/quantization/modelslim_config.py

csrc/_310P/memfabric_mm_ar/
  memfabric_mm_ar_binding.cpp
  memfabric_mm_ar_runtime.cpp
  memfabric_mm_ar_torch_adpt.h
  memfabric310p_adapter_api.h
  memfabric310p_adapter.cpp
  memfabric310p_device.asc

cmake/memfabric_310p.cmake
tests/ut/_310p/test_memfabric_mm_ar_source.py
benchmarks/scripts/bench_310p_memfabric_mm_ar_layer.py
benchmarks/scripts/analyze_310p_memfabric_overlap.py
```

## 10. 当前状态

代码已迁移到 ABI v7 并完成两轮静态代码审视；性能、正确性和稳定性结论必须
绑定当前 commit 在目标 310P3 上重新验证。尤其不能把 v6 的 +15% 单层数据直接
当成 v7 实测结果。
