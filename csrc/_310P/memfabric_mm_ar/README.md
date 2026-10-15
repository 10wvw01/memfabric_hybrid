# CUSTOM 310P3 MemFabric mm_ar fusion

> **CUSTOMIZED 310P3 ONLY**
>
> Qwen3.6 TP=2 matmul + allreduce 融合（`self_attn.o_proj` / GDN
> `linear_attn.out_proj`）的 310P3 定制实现。所有平台特化 C++/AscendC
> 代码隔离在本目录。

## 当前合同

- Hardware: Ascend 310P3 / dav-2002
- Model: `qwen3_5_moe_text` full-attention `self_attn.o_proj` 与
  linear-attention (GDN) `linear_attn.out_proj`
- TP: 2
- Shape: global K=4096, local K=2048, N=2048（两种层一致）
- Compute / exchange / output: FP16（权重为 checkpoint FLOAT 未量化层）
- Transport: installed `wgm-dev-310p` MemFabric public SHM/SDMA API
- Adapter ABI: **v7**
- Cooperative MM: 8 blocks × explicit M-partition (`batch_m/8` rows each,
  classic AscendC Matmul is single-core semantics), per-block tiling
  `{batch_m/8, 2048, 2048}` basic `{batch_m/8, 256, 64}`, `CONFIG_NORM`
- Producer blockDim: **8**
- Communication batch: `batch_m = 256 * q`, `q in {1,2,4}`, default 1
- Threshold: **无 M 阈值**——启动选择融合路径后所有 M 均走融合
- Small-M exact path（默认开启，`VLLM_ASCEND_310P_MEMFABRIC_MM_AR_SMALL_M_PATH=0`
  可关回补零批流水）：`M < min(batch_m, 256)` 时跳过补零批，改走单波 N-split
  串行路径——8 核各自持有 256 列 NZ 权重切片（每层一次性 8 MiB strided
  re-layout，按权重指针缓存），模板阶梯 T∈{16,32,64,128,256} 覆盖 M，
  blocked slot 布局 `[8][T][256]` 保证 batch0 槽内 T*2048*2 字节连续、
  单条 SDMA signal；wait 内核经既有 `batchBytes` 入参复用，add 改为
  256 元素段（每段恰为一行×一个 N 块）的非阻塞寻址。行 [M,T) 的 staged
  零/陈旧数据被计算和交换但从不 reduce（行局部化，同尾批论证）。

1 MiB 是 `q=1`、N=2048、FP16 时推导出的 batch payload，不是协议常量。

## 数据流

```text
wave gate (fixed credit)
        |
        v
P0: 8-core cooperative MM
    all cores clean + ready[batch][core]=generation
    core0 sole signal owner
        |
P1 -------------------------- SDMA0
        |
W0 -> A0 -------------------- SDMA1
        |
P2 -------------------------- ...
        ...
drain last wait/add
        |
public quiet (once per wave)
        |
wave credit ack
```

8 个 AI Core 全部参加 MM；core0 不独占通信角色，只在完成自身 MM 后兼任唯一
data `signal()` owner。wait/reduce 以 batch 为单位，credit 仍以 wave 为单位。

## 文件职责

- `memfabric_mm_ar_binding.cpp`：PyTorch custom op
- `memfabric_mm_ar_runtime.cpp`：arena/wave、lookahead pipeline、Graph/lifecycle
- `memfabric_mm_ar_torch_adpt.h`：torch-facing 校验
- `memfabric310p_adapter_api.h`：内部 ABI v7
- `memfabric310p_adapter.cpp`：MemFabric public host API bridge
- `memfabric310p_device.asc`：8-core cooperative MM、public signal/wait/quiet、FP16 add

## 必须保持的边界

1. MemFabric 是外部黑盒；不得读取 request/arrival ring、head/tail、reserved
   layout、AICPU orchestrator、SQE 等私有状态。
2. device 数据面只调用 public `smem_shm_sdma_signal/wait/quiet`。
3. data `signal()` 只有 core0 调用。
4. data/credit mail 严格校验 `status/dst/len/imm`。
5. 协议异常写 status + `AscendC::Trap()`，失败 context 不继续复用。
6. ready 为 vLLM-owned `[batch][8 cores]` 独立 64B cache line，使用 generation：
   eager 波使用单调递增 generation（`0x40000000` 起，与 graph 捕获烘焙的
   `batch + 1` 小值域不相交），陈旧 cell 值永不等于当前波，**无需每波清零
   32 KiB ready 区**——该清零在 GVA 池内存上走慢速引擎路径，实测每波
   ~260µs 串行开销（dav-2002 profiler 时间线）；graph 捕获路径保持
   wave-invariant generation + 图内清零。
7. arena 按 rows/内存预算定容，不随 batch payload 成正比放大。
8. tail zero-pad 到完整 `batch_m` 后仍走 8-core cooperative MM；producer
   scratch 仅在分配时清零一次，之后尾批只拷贝有效行——补零/陈旧行会被计算
   和交换，但 `add_batch_async` 只读取 `valid_rows` 行，且 MM 输出行只依赖
   自身输入行（garbage 行局部化），正确性不受影响。该优化消除了实测每尾批
   ~0.6ms 的 2MiB 引擎 memset（全 M 融合后小 M 的主导开销）。
9. small-M N-split 路径与尾批共用 producer scratch（同 stream 串行，无并发）；
   权重切片在首个 eager 融合调用按需构建（8×`aclrtMemcpy2dAsync`，同 stream
   排序、无额外同步），graph 捕获前必须已由 eager warmup 构建；切片缓存按
   权重指针不淘汰（权重进程生命周期稳定），上限 64 条（~512 MiB）防病态调用。
10. eager 单 stream；Graph capture 前完成 context/scratch/protocol/kernel warmup
    （含全部 5 个 N-split 模板符号与 blocked add——首启 no-op 规则）。
11. feature-off 不要求安装 MemFabric。

cache visibility 为 ownership-scoped：每个 core 在 cooperative MM 结束后只
clean 自己写入的连续 C 行（dav-2002 实测）。

## 构建

`cmake/memfabric_310p.cmake` 从 `MEMFABRIC_HYBRID_HOME_PATH` 读取 public
headers/`libmf_smem.so`，并用 bisheng `--npu-arch=dav-2002` 编译当前
`memfabric310p_device.asc`。仓库不保存正式预编译 device object。
