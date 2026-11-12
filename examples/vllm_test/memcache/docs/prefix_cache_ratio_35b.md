# Qwen 35B 在 310P 上的共享前缀测试

比较相同输入在无前缀复用、vLLM 本地前缀缓存、MemCache DRAM 存储三种配置下的 TTFT。
模型为 Qwen3.6-35B-A3B-w8a8，正式请求输入 4096 tokens、输出 2048 tokens；TP=1 使用一个 die，TP=2 使用同一张 Duo 卡的两个 die。

| 共享比例 | 共享 tokens | 独有 tokens |
|---|---:|---:|
| 0% | 0 | 4096 |
| 25% | 1024 | 3072 |
| 50% | 2048 | 2048 |
| 75% | 3072 | 1024 |
| 100% | 4096 | 0 |

共享比例按每条请求的输入前缀长度定义。100% 表示输入完全重复，实际复用长度受 vLLM 最后 token 重算和块对齐限制，必须单独报告。

数据使用 ModelScope 的 vllm-ascend/prefix75-in3500-bs210 中 question 文本作为语料，加入 Agent 的任务背景、工具描述和当前步骤信息。用目标模型 tokenizer 编码后构造固定长度的 token ID 请求，避免聊天模板或二次编码改变共享边界。原始数据集在不同 tokenizer 下长度不同，请求长度以目标 tokenizer 的实际编码为准。

每个正式样本使用独立前缀，先清理本地缓存。原生缓存和 MemCache 用例先运行完整 4096-token 种子请求，输出一个 token 来建立对应位置的 Attention/Mamba 状态；种子请求与正式请求在共享边界的第一个 token 分叉，100% 用例则完全相同；预热不计入正式统计。MemCache 用例在预热后只清理本地缓存，保留外部数据。0% 不做前缀预热。每条正式请求完整接收 2048 个输出 tokens，记录首个实际生成 token 的时间和完整请求耗时。

先做每种比例一个样本的功能验证，再根据实际生成速度执行重复测量。结果保留每条请求的本地命中、外部命中、输入输出长度和输出 token 摘要。P50 是样本耗时的中位数。

本轮是控制共享边界的 Agent 背景复用测试。启动时显式设置 block-size=1024、max-num-batched-tokens=1024，使完整长请求分步计算，在 1K/2K/3K 边界保存 Mamba 状态。默认自动块大小为 640 时无法精确对应这些共享长度；一次计算全部 4K 时也可能缺少中间状态。正式结果使用三种配置一致的分块参数。

运行前准备服务依赖和目标模型，按当前用例设置 mmc-local.conf 的 world_size、device_sdma、dram.size=2GB、hbm.size=0。启动 MetaService 后，可以直接运行：

```bash
python cases/python/memcache/integration/prefix_cache_ratio_test.py \
  --model /path/to/Qwen3.6-35B-A3B-w8a8 \
  --corpus /path/to/prefix75.jsonl \
  --mode memcache_dram --tp 2 --devices 2,3 \
  --samples 3 --output-tokens 2048 --result-dir /path/to/results
```

该脚本通过命令行显式运行，常规 pytest 收集不会启动这组长时间性能测试。

mode 可选 disabled、native、memcache_dram。disabled 和 native 不需要 MetaService。TP=1 时指定单个设备。服务启动参数、每条请求的测量和汇总会写入 results.json，完整输入和种子请求写入 dataset.json。--output-tokens 1 可用于先检查命中和 TTFT，两种输出长度的完整请求耗时分别统计。

## 正式测量前的功能验证

在 TP=1 下，输出一个 token 检查缓存来源；0%～75% 每档一个样本，完整重复输入额外使用三个输入做冷请求和命中请求配对。使用完整 4K 种子请求、1K 预填充分块，两种缓存配置的结果如下：

| 共享比例 | 本地缓存实际复用 tokens | MemCache DRAM 实际复用 tokens |
|---|---:|---:|
| 0% | 0 | 0 |
| 25% | 1024 | 1024 |
| 50% | 2048 | 2048 |
| 75% | 3072 | 3072 |
| 100% | 3072 | 3072 |

五档使用相同输入；完整重复输入额外以三个样本检查，修复后冷请求与外部命中请求的首 token 及其 top-5 logprob 全部一致。MemCache 测量前清除了本地缓存，本地命中均为零；单独的数据搬运探测确认对象位于 DRAM，HBM 配置为零。此表用于验证功能，不作为 2048-token 正式性能结果。

100% 输入重复时，混合模型的缓存匹配仍受 Mamba 状态边界和末尾重算约束。当前配置先恢复 3072 tokens，再重算末尾 1024 tokens。三个输入的首 token 和 top-5 logprob 均与冷请求一致。该正确性补查在设备 3 使用固定 512 MiB KV 缓存预算；正式性能矩阵使用各 TP 对应的自动内存预算。

## 输出一致性检查

TP=1 的长输出测试中，关闭前缀缓存与原生缓存的 2048-token 输出摘要存在差异。同一原生缓存服务内额外执行三次清理本地缓存后的冷请求和两次 1K 前缀命中请求，输出 128 tokens；冷请求之间也出现不同输出。进一步检查 8-token 输出的 logprob，五次请求的首 token 和首 token top-5 logprob 完全一致，后续解码概率发生变化，接近的候选 token 会切换。该结果说明当前配置下完整长输出不满足逐 token 完全一致；首 token 的前缀恢复检查通过。完整输出摘要仍保留在原始结果中。

## 正式测量条件

| 项目 | 设置 |
|---|---|
| 模型 | Qwen3.6-35B-A3B-w8a8 |
| 运行环境 | CANN 9.2.1，驱动工具 npu-smi 26.1.1 |
| 设备 | 310P Duo；TP=1 使用设备 2，TP=2 使用同卡设备 2、3 |
| 请求 | 串行并发 1；每档三个正式样本 |
| 输入、输出 | 4096、2048 tokens；temperature=0、seed=42、ignore_eos=true，min_tokens=max_tokens=2048 |
| 数据 | 同一份 AISBench 前缀数据集语料构造的 token IDs；各配置使用相同输入 |
| 模型精度 | dtype=float16，quantization=ascend，mamba_ssm_cache_dtype=float16 |
| 执行 | eager、chunked prefill；max_model_len=8192，max_num_seqs=1 |
| 分块 | block_size=1024，max_num_batched_tokens=1024，缓存启用时 mamba_cache_mode=align |
| 内存预算 | TP=1 gpu_memory_utilization=0.90；TP=2 为 0.75；KV 容量由运行时估算 |
| MemCache | AscendStoreConnector、device_sdma；每 rank 2 GiB DRAM，HBM=0 |

TTFT 从开始发送请求计时，到收到第一个实际生成 token 为止。完整响应结束后核对输出长度，预热请求单独执行。

## TP=1 正式结果

表中数值为 TTFT P50，单位毫秒；每格三个样本。

| 共享比例 | 无前缀复用 | 原生缓存 | MemCache DRAM | 原生复用 tokens | 外部复用 tokens |
|---|---:|---:|---:|---:|---:|
| 0% | 4067.84 | 4096.62 | 4163.58 | 0 | 0 |
| 25% | 4078.44 | 3111.31 | 3168.92 | 1024 | 1024 |
| 50% | 4075.94 | 2098.59 | 2228.22 | 2048 | 2048 |
| 75% | 4070.74 | 1069.44 | 1217.02 | 3072 | 3072 |
| 100% | 4064.02 | 1060.10 | 1071.93 | 3072 | 3072 |

## TP=2 正式结果

表中数值为 TTFT P50，单位毫秒；每格三个样本。

| 共享比例 | 无前缀复用 | 原生缓存 | MemCache DRAM | 原生复用 tokens | 外部复用 tokens |
|---|---:|---:|---:|---:|---:|
| 0% | 2763.95 | 2767.60 | 2830.20 | 0 | 0 |
| 25% | 2761.93 | 2096.94 | 2153.96 | 1024 | 1024 |
| 50% | 2757.73 | 1403.18 | 1442.04 | 2048 | 2048 |
| 75% | 2760.64 | 719.15 | 742.80 | 3072 | 3072 |
| 100% | 2757.94 | 712.37 | 722.91 | 3072 | 3072 |

MemCache 正式样本的本地命中均为零。100% 表示输入完全重复，当前分块配置下实际恢复 3072 tokens，再重算尾部 1024 tokens。完整重复输入已有外部对象，重算的尾块可跳过重复保存；75% 用例的新后缀需要保存，两个用例的外部写入量不同。

TP=1 MemCache 的 0%～75% 保存恢复路径未受全命中修复影响，保留已完成的十二个样本；100% 档重启服务后用修复实现补测。TP=2 的三个配置均使用最终测试脚本，MemCache 使用修复实现。各轮启动命令和原始结果分别保存。

## 完整请求耗时

下表将每个配置的十五个正式请求汇总，单位为秒。它包含完整 2048-token 生成，和 TTFT 分开统计。

| TP | 无前缀复用 P50 | 原生缓存 P50 | MemCache DRAM P50 |
|---|---:|---:|---:|
| 1 | 210.30 | 264.03 | 237.10 |
| 2 | 253.01 | 285.53 | 276.49 |

前缀复用减少预填充工作，主要体现在 TTFT。长输出的总耗时还受后续解码影响，本轮完整请求耗时没有随 TTFT 等比例下降。

## 验证结果与产物

90 个正式请求的输入长度、输出长度及缓存来源检查通过；60 次缓存配置与同 TP 无前缀复用配置的首 token 比较全部一致。完整 2048-token 输出的逐 token 一致性未通过，原生缓存内的重复冷请求也观察到解码分歧，原始 token IDs 与摘要保留在结果中。

全命中修复的调度器 UT 为 68 通过；AscendStore 测试目录为 333 通过、10 跳过。另有三个完整重复输入的冷请求与外部恢复配对，首 token 和 top-5 logprob 全部一致。

每组 results.json 包含启动命令、全部样本、TTFT 和完整耗时；dataset.json 保存种子与正式输入。汇总为 summary.json、summary.csv，source_manifest.json 保存源码版本和构建产物摘要，完整服务日志按组保留。
