# vLLM 0.24 / 310P / MemCache 迁移结果

测试用例、TTFT/P50 和常用术语见[测试说明](../../TEST_GUIDE.md)。

## 原生 prefix cache 与 MemCache 对照（10006 新增实测）

本轮在同一组设备上重新测量无复用基线、原生 prefix cache 和 MemCache，先执行两个模型的
TP=1、TP=2，再执行 TP=4。下表仅列出已完成全部五项验证的组合。原有数据保留在后文，
不与本轮新增样本混算差值。

原有基线始终开启 KV cache 和 prefix caching，每个请求后清理本地前缀；它代表不发生前缀复用的请求。
新增原生对照不配置外部 connector、不启动 MemCache MetaService，在请求之间保留本地前缀。
MemCache 延迟用例则清理本地前缀、保留外部数据。清理、等待请求结束和查询指标均在 TTFT 计时之外。

### 测试版本和条件

| 仓库 | 测试提交 |
|---|---|
| MemCache | `2b806531a5aa11d16163b1bf53ce5d76dce20f35` |
| MemFabric | `4208cdb2dca1a64f4ab0cc87bc95821b0b6c0908` |
| vLLM-Ascend | `e1349c8fc4a9225368bf8e57450e754412dfb30a` |
| vLLM | `ee0da84ab9e04ac7610e28580af62c365e898389` |

MemCache 源码包含 PR #7 的符号隔离修复，与 `d4e1e208` 的源码树内容一致。
旧运行库试跑在服务退出时复现堆损坏，已停止并保留日志；本表为切换修复版本后重测的完整结果。

310P3、CANN 9.2.1、驱动 26.1.1；TP=1 使用设备 4，TP=2 使用同卡设备 4、5，TP=4 使用设备 4、5、6、7。
`device_sdma`，MemCache 每 rank 2 GiB HBM + 2 GiB DRAM，`expandable_segments:True`。
小模型 TP=1/2/4 显存比例 0.50；大模型 TP=1 为 0.90、TP=2 为 0.75、TP=4 为 0.50。
float16 + Ascend W8A8、eager、最大长度 8192、批次 token 4096、最多 10 个序列、chunked prefill 开启。
输入为相同随机种子的 4096 个有效 token，生成 1 token，temperature=0；预热 1 次，串行测量 10 次。
50% 场景为 5 个预填输入和 5 个新输入；配对场景为 10 组先写后读。另使用文本请求检查生成 token 一致性。
原生对照在同一个服务中依次测量三种场景，场景之间重置本地缓存；MemCache 延迟用例各自启动服务。

### 全 miss：未发生前缀复用时的耗时

单位 ms，P50 为样本中位数。原生相对基线差值为 `(原生 miss / 无复用基线 - 1)`，
MemCache 相对原生差值为 `(MemCache miss / 原生 miss - 1)`。

| 模型 | TP | 无复用基线 P50 | 原生 miss P50 | MemCache miss P50 | 原生相对基线 | MemCache 相对原生 |
|---|---:|---:|---:|---:|---:|---:|
| Qwen3.5-4B | 1 | 2222.99 | 2218.11 | 2228.97 | -0.22% | +0.49% |
| Qwen3.5-4B | 2 | 1306.48 | 1305.82 | 1313.01 | -0.05% | +0.55% |
| Qwen3.5-4B | 4 | 959.38 | 966.41 | 981.22 | +0.73% | +1.53% |
| Qwen3.6-35B-A3B | 1 | 3633.84 | 3632.99 | 3647.64 | -0.02% | +0.40% |
| Qwen3.6-35B-A3B | 2 | 2513.04 | 2508.41 | 2519.20 | -0.18% | +0.43% |
| Qwen3.6-35B-A3B | 4 | 2076.43 | 2075.54 | 2084.49 | -0.04% | +0.43% |

### 配对命中：相同输入的本地复用与外部复用

单位 ms。配对 miss/hit 对同一个输入先发送一次，再发送一次；两种缓存采用相同输入种子和等待时间。
最后一列为 `(MemCache 配对 hit / 原生配对 hit - 1)`。

| 模型 | TP | 原生配对 miss P50 | MemCache 配对 miss P50 | 原生配对 hit P50 | MemCache 配对 hit P50 | MemCache hit 相对原生 |
|---|---:|---:|---:|---:|---:|---:|
| Qwen3.5-4B | 1 | 2218.12 | 2223.33 | 189.72 | 194.18 | +2.35% |
| Qwen3.5-4B | 2 | 1306.82 | 1317.65 | 128.65 | 136.77 | +6.31% |
| Qwen3.5-4B | 4 | 963.10 | 981.65 | 122.23 | 136.72 | +11.85% |
| Qwen3.6-35B-A3B | 1 | 3631.97 | 3638.99 | 373.13 | 378.09 | +1.33% |
| Qwen3.6-35B-A3B | 2 | 2508.90 | 2524.52 | 253.45 | 263.75 | +4.06% |
| Qwen3.6-35B-A3B | 4 | 2078.23 | 2087.43 | 215.24 | 226.39 | +5.18% |

### 50% 重复请求负载：实际命中率与整批耗时

输入为 5 条预填后的重复请求和 5 条新请求，按同一随机顺序发送。
原生缓存根据实际查询与命中计数分类；容量不足时，重复输入也可能因淘汰而 miss。
下表的 hit P50 仅统计实际命中的请求，分母同时列出；整批 P50 使用全部 10 条请求。

| 模型 | TP | 原生 KV 容量/token | 原生实际 hit/10 | MemCache 实际 hit/10 | 原生整批 P50 | MemCache 整批 P50 | 原生 hit P50 | MemCache hit P50 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Qwen3.5-4B | 1 | 167,058 | 5/10 | 5/10 | 1204.54 | 1209.88 | 189.91 | 194.78 |
| Qwen3.5-4B | 2 | 401,408 | 5/10 | 5/10 | 716.40 | 725.24 | 127.62 | 132.14 |
| Qwen3.5-4B | 4 | 886,198 | 5/10 | 5/10 | 542.62 | 587.44 | 121.63 | 132.11 |
| Qwen3.6-35B-A3B | 1 | 15,952 | 1/10 | 5/10 | 3636.48 | 2008.77 | 376.68 | 376.64 |
| Qwen3.6-35B-A3B | 2 | 412,618 | 5/10 | 5/10 | 1379.13 | 1387.23 | 254.03 | 262.10 |
| Qwen3.6-35B-A3B | 4 | 400,237 | 5/10 | 5/10 | 1144.87 | 1181.93 | 215.09 | 230.53 |

首次试跑中，大模型 TP=1 的本地 KV 容量为 15,952 token，小于 5 条预填输入的 20,480 token；
插入新请求后还会发生替换。最初用例假设重复请求全部命中，在这里失败。
修正后保留相同内存比例和请求流，记录真实命中率及延迟；配对用例仍逐组验证命中和生成结果。
MemCache 额外使用每 rank 2 GiB HBM 与 2 GiB DRAM，缓存容量与纯原生模式不同。

### 命中来源和完成情况

原生请求检查本地命中、外部计数不增加；MemCache 延迟请求检查外部命中、本地计数不增加。
下面记录配对 hit 请求的实际复用 token 数，范围表示各测量请求的最小值至最大值。

| 模型 | TP | 原生本地复用 token | MemCache 外部复用 token | 测试结果 |
|---|---:|---:|---:|---|
| Qwen3.5-4B | 1 | 3840 | 3840 | 5/5 通过 |
| Qwen3.5-4B | 2 | 3840 | 3840 | 5/5 通过 |
| Qwen3.5-4B | 4 | 3840 | 3840 | 5/5 通过 |
| Qwen3.6-35B-A3B | 1 | 3840 | 3840 | 5/5 通过 |
| Qwen3.6-35B-A3B | 2 | 3840 | 3840 | 5/5 通过 |
| Qwen3.6-35B-A3B | 4 | 3840 | 3840 | 5/5 通过 |

每个组合包含全 miss、50% 命中、配对 miss/hit、缓存来源与生成正确性、原生 prefix cache 对照五项。
已完成 6 个模型/TP 组合，共 30 项通过。
原始数据位于 `retest_logs/native-prefix-tp-matrix/`：`results.json` 汇总六组结果，
各模型/TP 目录保存 pytest 日志、原生逐请求 JSON 和服务日志。
部分原生用例的复测日志为 `native-retest.log`，首次试跑记录保留供核对。
P99、逐请求延迟及原生缓存指标均可从这些记录中核查。
HTTP/SSE、缓存指标、无 NPU 跳过及进程清理回归共 16 项通过。

## 前次指定上游分支验证（2026-11-11，10006 完整复测）

部署直接获取指定上游仓库和分支，构建仓库中的实现。测试包不再附带或应用运行时补丁。
用户操作流程见[部署说明](vllm_deployment_flow.md)，各次测试的实际版本记录于 `retest_logs/source_manifest.json`。

### 本轮源码记录

| 仓库 | 分支/版本 | 本轮提交 |
|---|---|---|
| MemCache | `trunk_for_escc_v1.2.1` | `19fa9ee58def1792c095368f58a72a4558a8903e` |
| MemFabric | `trunk_for_escc_v1.2.0` | `4208cdb2dca1a64f4ab0cc87bc95821b0b6c0908` |
| vLLM-Ascend | `trunk_for_escc_v24.0` | `e1349c8fc4a9225368bf8e57450e754412dfb30a` |
| vLLM | `v0.24.0` | `ee0da84ab9e04ac7610e28580af62c365e898389` |

### 修改和基础验证

- 移除三个部署补丁和应用逻辑；同步脚本固定获取、切换指定分支并快进更新。
- 新建空目录时直接获取指定 MemFabric 分支；已有目录不会为了初始化子模块切回旧版本。
- 部署核对源码提交、MemCache wheel、Python 加载路径以及实际加载的 MemFabric 运行库 SHA256。
- 仓库同步、部署版本检查、HTTP/SSE 和基于命中指标的采样及进程清理回归共 18 项通过。
- MemFabric 完整 UT：2060 通过、10 项环境跳过，退出 0；行覆盖率 70.1%、分支覆盖率 46.5%。
- vLLM-Ascend 本轮选定 Python 回归：359 项通过、10 项跳过。干净构建发现 compute-wy 的 bf16 类型兼容遗漏，
  已与另外两处兼容头保持一致，算子和 Python 扩展重新编译安装通过。
  10 个跳过项为仓库中已标记过时的 `LayerMultiBlockReqMeta` 单元用例。

### NPU 条件与实测结果

环境：Ascend310P3，CANN 9.2.1，驱动 26.1.1，aarch64。TP=1 使用设备 4，TP=2 使用同卡设备 4、5。
协议为 `device_sdma`，每 rank 2 GiB HBM + 2 GiB DRAM，`expandable_segments:True`。
小模型显存比例为 0.50，大模型 TP=2 为 0.75、TP=1 为 0.90。

float16 + Ascend W8A8，eager，最大序列 8192、批次 token 4096、最多 10 个序列，chunked prefill 开启。
TTFT 输入为 4096 个有效随机 token、生成 1 token，temperature=0；1 次预热、10 次串行测量。
50% 场景使用 5 个预填输入和 5 个新输入；重复输入场景包含 10 组先写后读。
本地 prefix cache 开启，每次延迟请求后只重置本地缓存，重置及命中指标检查位于计时之外。
命中由本地/外部计数增量校验，延迟用于记录 P50/P99 和相对差值。
另用聊天输入生成最多 8 token，检查本地、外部、混合命中的计数及与独立基线的逐 token 一致性。

单位 ms；本表只列本轮完成四项验证的组合。

“基线”是不使用 MemCache 的新输入请求；“MemCache miss”是接入后的未命中请求。
“配对 miss/hit”在同一个 MemCache 服务中，对同一输入先写后读。P50 是耗时中位数，
“50%”按请求数计算。操作步骤和计算示例见[用例与指标说明](../../TEST_GUIDE.md#memcache-tests)。

| 模型 | TP | 基线 P50 | MemCache miss P50 | 相对差值 | 50% 场景 hit P50 | 配对 miss P50 | 配对 hit P50 | 完成项 |
|---|---:|---:|---:|---:|---:|---:|---:|---|
| Qwen3.5-4B | 1 | 2223.04 | 2239.88 | +0.76% | 269.93 | 2230.22 | 195.80 | 4/4 |
| Qwen3.5-4B | 2 | 1307.20 | 1313.40 | +0.47% | 132.18 | 1311.29 | 133.50 | 4/4 |
| Qwen3.6-35B-A3B | 2 | 2506.56 | 2518.12 | +0.46% | 261.90 | 2513.10 | 259.82 | 4/4 |
| Qwen3.6-35B-A3B | 1 | 3633.92 | 3642.49 | +0.24% | 377.23 | 3644.90 | 378.34 | 4/4 |

本轮从构建、安装和部署核对重新开始，固定使用 10006 的设备 4、5。此前未完成的记录单独保存。

### 大模型内存预算修复

310P worker 原有逻辑将 KV cache 预算折半，为运行时工作空间留出空间。
Qwen3 实验模块原先无条件覆盖该方法并将预算乘 2，普通推理也会受到影响。
本轮将覆盖限定在现有 `PYPTO_DECODER_CACHE_RUN` 模式内，并测试开关两种分支。
独立的大模型 TP=2 无 MemCache 基线已完成启动及 4096 输入 token、1 输出 token 请求；
KV cache 预算约 5.84–6.09 GiB，预热后 torch reserved 约 30.23 GiB。

大模型 TP=1 最终使用设备 4、显存比例 0.90；MemCache 模式预算约 0.49 GiB，
预热后 torch allocated 约 37.55 GiB，4096 token 请求及后续完整四项测试通过。
前置检查中，设备 5、比例 0.90 的预算 0.18 GiB 小于 8192 长度所需的 0.23 GiB；
设备 4、比例 0.95 的无 MemCache 请求通过，但加上 2 GiB HBM 缓存池后请求显存不足。
最终参数同时满足序列长度和缓存池、请求临时内存的实际占用。

### 测试进程清理

测试结束时按本次服务的进程关系记录和清理 worker，覆盖独立进程组以及父进程提前退出的情况。
回归同时确认无关进程仍在运行。回退这部分可恢复原有按服务进程组退出的实现。

MetaService 使用本次构建的可执行文件启动，避免从已加载 torch/CANN 的多线程 Python 进程中 fork。
旧 fixture 两次出现无 MemCache 长请求输出不一致；独立请求、32 次 GDN 和 24 次 causal-conv 检查通过。
改为启动可执行文件后，原正确性用例定向回归通过。本轮完整矩阵使用该启动方式。

该历史轮次未执行 TP=4；新增 TP=4 结果见本文顶部。多并发吞吐及大容量 eviction/rewarm 未计入该轮结果。
历史测试条件、步骤和性能数据保留如下。

## 历史迁移结果

Qwen3.5-4B-W8A8-310P、Qwen3.6-35B-A3B-w8a8 的 TP=1 / TP=2，均完成全 miss、50% 命中、重复输入命中、聊天缓存来源正确性四项验证，合计16项通过。TP=4 按用户要求暂停。变更已推送，远程 PR 见下表。

vLLM 核心恢复原版0.24；适配位于 vLLM-Ascend、MemCache、MemFabric。保留共同固定地址机制，撤销独立动态选址、版本2元数据及 vLLM 核心校验例外。

## PR 分支与提交

| 内容 | 分支 | 提交 | PR |
|---|---|---|---|
| MemFabric运行库 | `fix/310p-fixed-vmm-context` | `6abf1066`、`4f0ca5f4` | [#31](https://gitcode.com/GDD_ESCC/memfabric_hybrid/merge_requests/31) |
| MemCache绑定和依赖 | `fix/python-copy-direction-defaults` | `3b12bb96`、`54d347c9` | [#3](https://gitcode.com/GDD_ESCC/memcache/merge_requests/3) |
| vLLM-Ascend适配 | `fix/310p-memcache-v024-pr` | `ca290920` | [#10](https://gitcode.com/GDD_ESCC/vllm-ascend/merge_requests/10) |
| 测试工具与文档 | `test/vllm024-memcache-310p` | 具体提交号见分支日志 | [#32](https://gitcode.com/GDD_ESCC/memfabric_hybrid/merge_requests/32) |

原 MemCache PR #1 的同内容修改已通过 PR #3 合入；其依赖的 MemFabric 修复也已合入指定分支。测试工具PR只修改`examples/vllm_test/memcache/`，与运行库源码提交分开。原`vllm-0.24-test`分支保留两部分修改的集成状态。

Ascend目标分支发布时已前进到`02d2a51d`。PR #10在独立worktree中适配最新目标，保留上游DFlash公共前缀最终校验，相关单测275 passed、10 skipped；该最新组合尚未重跑NPU端到端测试。下面的NPU结果仍对应原测试基线`b19127c7`加集成提交`a97b7320`。这些为该版本当时的运行记录，新的部署方式从指定上游分支构建。

## 仓库基线

| 仓库 | 工作目录 | 基线 |
|---|---|---|
| vLLM | `/workspace/vllm-v024-test` | GDD_ESCC/vllm，v0.24.0，`ee0da84`；工作区干净 |
| vLLM-Ascend | `/workspace/vllm-ascend-v24-test` | GDD_ESCC/vllm-ascend，`trunk_for_escc_v24.0`，`b19127c7` |
| MemCache | `/workspace/memcache-v121-test` | GDD_ESCC/memcache，`trunk_for_escc_v1.2.1`，`2f6c7fe4` |
| MemFabric | `/workspace/worktree-temp/vllm-0.24-test` | GDD_ESCC/memfabric_hybrid，`trunk_for_escc_v1.2.0`，`24ad5d66`；worktree分支 vllm-0.24-test |

MemCache 的 MemFabric 子模块同步相同运行时代码。虚拟环境为 `/workspace/vllm024-venv`。安装的 `libmf_hybm_core.so` 与本轮构建产物逐字节一致，SHA256：`3db5b82a622af1eb2793de0e7ef2b4e8eb3a07e4dfe776e69b2ad863135a1d46`。MemFabric 全量 UT 产物位于独立的 build_ut/output_ut，没有覆盖运行库。

## 测试条件

- 10006服务器、memcache-tg容器；310P3、CANN9.2.1；TP=2为同卡双die 6/7，TP=1为设备6。
- `device_sdma`，每rank 2GiB HBM + 2GiB DRAM；`PYTORCH_NPU_ALLOC_CONF=expandable_segments:True`。
- 显存比例：Qwen3.5 TP=1/2为0.50；Qwen3.6 TP=2为0.75，TP=1为0.90。
- float16 + Ascend W8A8，eager模式，最大序列8192、批次token4096、最大序列数10；chunked prefill开启，FlashComm/MC2关闭，Mamba align、SSM状态float16。
- TTFT：4096个有效随机token输入、生成1 token，temperature=0；1次预热、10次串行测量。50%场景为5个预填输入+5个新输入；重复输入场景为10组先写后读。
- 基线和MemCache模式均开启本地prefix cache；每完成请求后执行 `/reset_prefix_cache?reset_external=false`，检查HTTP成功及引擎新增成功日志，重置发生在计时之外。TTFT识别实际token ID或文本，HTTP/SSE错误、空流均失败。
- 正确性：使用各模型聊天模板，关闭思考、正常EOS，最多生成8 token。A/B分别2827/5661 token，公共前缀2823 token。比对独立无connector基线、重复基线、本地复用基线及MemCache的seed/外部/本地/混合输出，并检查Prometheus命中计数增量。

P50为本轮样本中位数。下表记录共享服务器上的首token延迟实测结果。

## 性能与功能结果

单位ms。全miss差值为 `(MemCache miss P50 / baseline P50 - 1)`；最后两列来自同一重复输入配对用例。

| 模型 | TP | baseline P50 | MemCache miss P50 | miss差值 | 50%场景hit P50 | 配对miss P50 | 配对hit P50 | hit/miss | 四项结果 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| Qwen3.5-4B | 1 | 3971.48 | 3962.37 | -0.23% | 473.75 | 4076.27 | 546.75 | 13.41% | 4/4通过 |
| Qwen3.5-4B | 2 | 2310.95 | 2342.95 | +1.38% | 428.30 | 2400.30 | 494.45 | 20.60% | 4/4通过 |
| Qwen3.6-35B-A3B | 1 | 5650.59 | 5729.70 | +1.40% | 748.73 | 5648.18 | 527.95 | 9.35% | 4/4通过 |
| Qwen3.6-35B-A3B | 2 | 3765.70 | 3556.53 | -5.55% | 1398.29 | 3610.64 | 468.65 | 12.98% | 4/4通过 |

缓存来源及生成一致性：

| 模型/TP | 纯外部命中token | 纯本地命中token | 混合命中token（本地+外部） | token输出 |
|---|---:|---:|---|---|
| Qwen3.5 TP=1/2 | 2688 | 2688 | 2688+2688 | 全部与基线一致 |
| Qwen3.6 TP=1/2 | 2560 | 2560 | 2560+2560 | 全部与基线一致 |

“100% hit”指重复请求占比；块大小、Mamba检查点和末块计算会使外部token复用比例低于100%。大模型配对日志显示外部加载3840/4096 token。

### 结果日志

日志统一保存在测试目录的 `retest_logs/vllm024-migration/`，被Git忽略；同名原始文件保留于 `/tmp/`。

| 内容 | 日志及pytest汇总 |
|---|---|
| 小模型TP=1四项 | `vllm024-final-qwen35-tp1-full.log`；4 passed，6 deselected，610.53s |
| 小模型TP=2四项 | `vllm024-final-qwen35-tp2-full.log`；4 passed，6 deselected，648.04s |
| 大模型TP=1四项 | `vllm024-final-qwen36-tp1-full.log`；4 passed，6 deselected，845.73s |
| 大模型TP=2全miss | `vllm024-final-qwen36-tp2-partial.log`；该项passed，后续项目被资源占用中断 |
| 大模型TP=2两项命中 | `vllm024-final-qwen36-tp2-hit-source.log`；两项passed，同文件早期来源测试因资源不足failed |
| 大模型TP=2最终聊天来源用例 | `vllm024-final-qwen36-tp2-source-final.log`；1 passed，9 deselected，240.98s |

大模型TP=2的四项通过结果由上述三个实际运行组成，不把同一历史日志内的失败来源用例记作通过。早期小模型分次实验已被最终完整四项结果取代。

## 单元回归与部署核对

- vLLM-Ascend提交前复核：171 passed；额外98 passed、10 skipped。日志 `vllm024-pr-ascend-ut.log`、`vllm024-pr-ascend-regression.log`。
- HTTP流校验：5 passed，覆盖文本token、特殊token ID、HTTP500、SSE error、空流；日志 `vllm024-pr-http-ut.log`。
- MemFabric定向回归：VMM9 passed、上下文1 passed。
- MemFabric全量ASAN：2068项中2058 passed、10 skipped（网络条件），无失败；行覆盖70.0%、分支46.4%。提交前复核日志为 `vllm024-pr-mf-full-ut.log`。
- 提交前重新执行完整 `bash script/run_ut.sh`，测试和覆盖率统计均完成，退出0；mockcpp与patch目录已恢复。格式整理校验了Python AST和C++非空白内容一致。
- 三个部署运行时补丁均通过反向应用检查，与工作区一致；主MemFabric与MemCache子模块三个运行时源文件一致；安装库cmp一致；部署脚本语法、源码diff检查通过。
- `setup.sh --skip-build --skip-install-libs --yes ...`在已提交的修复分支上复核通过，保持MemFabric子模块提交不变；基于指定分支的修复提交可重复部署。日志 `vllm024-pr-setup-check.log`。

## 历史测试条件与诊断记录

1. 固定基址`0x12da80000000`验证范围是本机设备6/7、上述两模型与TP=1/2；其他环境、跨卡及多池未验证。保留PR#23共同固定窗口和精确地址检查；所有rank必须使用相同基址与代码。未实现分布式地址协商。
2. 原始裸completion、ignore_eos=true的长输入出现token差异；无connector的同一输入重复计算也不一致，根因尚未解决。日志 `vllm024-final-qwen36-tp2-raw-baseline-unstable.log`。最终聊天用例保持逐token对比，验证范围是明确列出的输入，不属于通用模型精度验收。
3. 设备曾多次被其他容器的新任务占用，产生显存不足及一次HCCL分配失败；对应失败日志保留。按用户授权清理6/7，并临时监控本次窗口。初版监控误判环境变量缺失的自身子进程，已修正为完整进程树排除；该次失败单独归档。监控不属于产品补丁，测试结束已撤销。
4. TP=4按用户要求未执行；未完成多并发吞吐、长时间稳定性、HBM→DRAM eviction/rewarm大容量测试。历史largecap和可选多token辅助方法仍需单独迁移，具体见下文。

## 测试用例与目的

| 测试 | 条件和目的 |
|---|---|
| 全 miss 对比 | 相同启动参数，分别关闭/开启 MemCache connector，10 个不同 4096-token 输入；比较 P50 TTFT 与相对差值 |
| 50% hit | 预填充后混合 5 个命中与 5 个 miss，重置本地 prefix，检查外部缓存命中与 TTFT 分组 |
| 100% hit | 先写后读相同输入，重置本地 prefix，比较 miss/hit TTFT |
| 缓存来源正确性 | Paris/Berlin 自然语言 A/B 两段输入，生成 8 token；与独立基线 token ID 比较，并检查本地/外部命中计数增量 |
| HTTP 流校验 | 模拟正常文本、特殊 token ID、HTTP 500、SSE error、空响应，避免把错误或空流计作低延迟 |
| 固定窗口单元测试 | 模拟精确地址成功及返回其他地址，验证拒绝并释放异常地址 |
| ACL 线程上下文单元测试 | 验证线程设备已有但上下文缺失时仍初始化 |

这些是串行功能与首 token 延迟测试；没有完成多并发吞吐、长时间稳定性及 TP=4 基准。历史 `large_capacity_vllm_test.py` 仍有旧启动参数；可选多 token 辅助方法 `measure_e2e_full_latency` 仍按 SSE 事件统计，没有使用本轮 TTFT 流校验逻辑。这两条历史路径未计入当前迁移结果。


## 本次修改文件

`/workspace/worktree-temp/vllm-0.24-test`：

```text
examples/vllm_test/memcache/README.md
examples/vllm_test/memcache/cases/config/mmc-local-integration-1rank.conf
examples/vllm_test/memcache/cases/config/mmc-local-integration-4rank.conf
examples/vllm_test/memcache/cases/config/mmc-local-integration.conf
examples/vllm_test/memcache/cases/python/memcache/integration/vllm_http_test.py
examples/vllm_test/memcache/cases/python/memcache/integration/vllm_integration_test.py
examples/vllm_test/memcache/docs/error_analysis_report.md
examples/vllm_test/memcache/docs/vllm_024_migration_results.md
examples/vllm_test/memcache/docs/vllm_ttft_test_issues.md
examples/vllm_test/memcache/docs/vllm_ttft_test_report.md
examples/vllm_test/memcache/run_test.sh
examples/vllm_test/memcache/setup.sh
examples/vllm_test/memcache/start_service.sh
src/hybm/csrc/common/hybm_define.h
src/hybm/csrc/entity/hybm_entity_default.cpp
src/hybm/csrc/under_api/dl_acl_api.h
test/ut/testcase/hybm/hybm_entity_default_test.cpp
test/ut/testcase/hybm/mm/hybm_vmm_based_segment_test.cpp
```

`/workspace/memcache-v121-test`：

```text
3rdparty/memfabric_hybrid
src/memcache/csrc/python_wrapper/pymmc.cpp
```

`/workspace/vllm-ascend-v24-test`：

```text
csrc/moe/chunk_fwd_o/op_kernel/arch20/compat_310p.h
csrc/moe/chunk_gated_delta_rule_fwd_h/op_kernel/arch20/compat_310p.h
tests/ut/patch/platform/test_prefix_cache_cp_patches.py
vllm_ascend/_310p/model_runner_310p.py
vllm_ascend/distributed/kv_transfer/kv_pool/ascend_store/backend/memcache_backend.py
vllm_ascend/distributed/kv_transfer/kv_pool/ascend_store/pool_worker.py
vllm_ascend/patch/platform/patch_kv_cache_coordinator.py
```

`/workspace/vllm-v024-test`：

```text
(clean)
```
