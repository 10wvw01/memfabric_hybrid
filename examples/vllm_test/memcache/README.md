# vLLM TTFT 测试环境部署包

从指定上游仓库获取分支、构建安装，然后在**打包目录内**运行测试。集成测试保留在本目录。

## 目录结构

```
ttft_test/
├── README.md
├── setup.sh                 # 部署脚本：获取指定上游分支并构建安装
├── verify.sh                # 验证部署结果
├── run_test.sh              # 统一测试运行脚本（workload / integration / largecap / bench）
├── start_service.sh         # 启动 MetaService + vLLM（支持 --detach 后台模式）
├── pytest.ini               # pytest 配置（注册 marker：throughput / rank1 / rank2 / rank4 / workload / largecap）
├── source_repos.sh          # 上游仓库与分支同步
├── source_manifest.py       # 提交版本、构建产物和加载路径核对
├── cases/                    # 新增测试文件
│   ├── config/
│   │   ├── mmc-local-workload.conf          # Layer 2 配置
│   │   ├── mmc-local-integration.conf       # Layer 3 2-rank 配置
│   │   ├── mmc-local-integration-4rank.conf # Layer 3 4-rank 配置 (world_size=4)
│   │   └── mmc-local-eviction-repro.conf    # Eviction 复现配置
│   └── python/memcache/
│       ├── workload/                        # Layer 2: prefill TTFT（6 个核心场景）
│       ├── largecap/                        # 大容量测试: HBM→DRAM eviction + rewarm（5 场景）+ eviction repro（2 场景）
│       └── integration/                      # 统一测试文件 vllm_integration_test.py
│           └── vllm_integration_test.py     # TP=1/2/4 延迟用例及缓存来源正确性
├── aux_scripts/             # 辅助脚本（环境清理、故障复现）
│   ├── clean_stale_env.sh
│   ├── repro_207001.c / repro_207001_run.sh
│   ├── repro_store_init_207001.py
│   └── repro_sdid_207000.py / run_sdid_207000.sh
├── retest_logs/             # 复测日志和错误分析报告
│   └── error_analysis_report.md
└── docs/
    └── vllm_ttft_test_report.md             # 测试报告
```

## 基准信息

| 项目 | 基准 |
|------|------|
| memcache | `GDD_ESCC/memcache` 的 `trunk_for_escc_v1.2.1` |
| memfabric_hybrid | `GDD_ESCC/memfabric_hybrid` 的 `trunk_for_escc_v1.2.0` |
| vLLM-Ascend | `GDD_ESCC/vllm-ascend` 的 `trunk_for_escc_v24.0` |
| vLLM | `v0.24.0`（`ee0da84a`） |

## 310P 迁移验证状态

当前使用原版 vLLM 0.24，适配集中在 vLLM-Ascend、MemCache 和 MemFabric。两个模型为 `Qwen3.5-4B-W8A8-310P` 和 `Qwen3.6-35B-A3B-w8a8`。最终代码的测试结果以 [迁移报告](docs/vllm_024_migration_results.md) 为准，旧实验不计为本版本通过结果。TP=4 按用户要求暂停。

测试开启本地 prefix cache 以使用原版 vLLM 的 Mamba `align` 路径。原有三项延迟测试在请求结束后清除本地 prefix cache、保留外部 MemCache；新增缓存来源测试保留本地缓存，分别核对本地、外部和混合命中的指标增量与生成结果。重置操作在 TTFT 计时之外执行，测得的是串行首 token 延迟，不是吞吐。

MemFabric 使用远端 VA 管理区，通过 HAL 远端模式预留窗口，再由 ACL 分配和映射物理页；线程执行 ACL 操作前检查设备和上下文。`host_shm` 需要足够的 `/dev/shm` 容量，运行前检查。

## 前置条件

1. **memcache 仓库**（setup.sh 可自动 clone 指定分支）
2. **memfabric_hybrid** 子模块 remote 可 fetch `trunk_for_escc_v1.2.0` 分支
3. **vLLM-Ascend** 已安装（Layer 3 需要，Layer 2 不需要）
4. **CANN** 已安装（使用当前 CANN 环境脚本设置的路径）
5. **W8A8 量化模型**已部署（示例模型 `/workspace/models/Qwen3.5-4B-W8A8-310P` 和 `/workspace/models/Qwen3.6-35B-A3B-w8a8`，Layer 3 需要，Layer 2 不需要；可通过环境变量 `MMC_TEST_VLLM_MODEL_PATH` 覆盖）

## 快速部署

```bash
# 部署（需在独立 Python 环境执行，并准备 vLLM 0.24 和 vLLM-Ascend 源码）
bash setup.sh --memcache-dir /path/to/memcache --vllm-dir /path/to/vllm --vllm-ascend-dir /path/to/vllm-ascend --yes

# 仅 Layer 2（不碰 vLLM-Ascend）
bash setup.sh --memcache-dir /path/to/memcache --yes --layer2-only

# 仅同步源码和记录版本，不构建
bash setup.sh --memcache-dir /path/to/memcache --vllm-dir /path/to/vllm --vllm-ascend-dir /path/to/vllm-ascend --yes --skip-build
```

部署包含 MemCache、MemFabric 和 vLLM-Ascend 的构建，具体耗时记录在构建日志中。

## 验证

```bash
bash verify.sh --memcache-dir /path/to/memcache --vllm-dir /path/to/vllm --vllm-ascend-dir /path/to/vllm-ascend
```

## 运行测试

测试在**打包目录**中执行，通过 `--memcache-dir` 指向 memcache 仓库（用于查找构建产物和 `config/mmc-meta.conf`）。

### 310P 单 rank 示例

先在独立 Python 环境安装并构建 vLLM 0.24、vLLM-Ascend 和上述补丁。以下命令使用小模型，设备和显存比例按实际空闲资源调整：

```bash
source /usr/local/Ascend/cann/set_env.sh
source /usr/local/Ascend/nnal/atb/set_env.sh
export PATH=/workspace/vllm024-venv/bin:$PATH
export MMC_TEST_VLLM_MODEL_PATH=/workspace/models/Qwen3.5-4B-W8A8-310P
bash run_test.sh --memcache-dir /workspace/memcache-v121-test \
    --type integration --rank 1 --device-ids 6 --gpu-mem-util 0.50
```

测试只处理文本，启动前等待目标设备的已用显存降至总量的 25% 以下。启动前确认端口 5000、6000、8000 和测试 HTTP 端口空闲。测试使用的 MetaService 异常退出时应查看其日志。

### 测试前检查

```bash
# 检查 NPU 设备状态（确认目标设备空闲、无残留进程）
npu-smi info

# 8 设备 die-pair 对应：dev0+1 / dev2+3 / dev4+5 / dev6+7
# 选空闲的 die-pair 传给 --device-ids
```

### 基本用法

```bash
# 1. 准备环境：拉取源码 + 补丁 + 编译安装
bash setup.sh --memcache-dir /path/to/memcache --vllm-dir /path/to/vllm --vllm-ascend-dir /path/to/vllm-ascend --yes

# 2. Layer 2 workload 测试（2-rank, device 6,7, ~1 分钟）
bash run_test.sh --memcache-dir /path/to/memcache --type workload --device-ids 6,7 --log

# 3. Layer 3 集成测试（2-rank, device 6,7, ~25 分钟，含 vLLM 启动）
bash run_test.sh --memcache-dir /path/to/memcache --type integration --rank 2 --protocol host_shm --device-ids 6,7 --log

# 4. Layer 3 集成测试（4-rank, device 4,5,6,7, ~16 分钟）
bash run_test.sh --memcache-dir /path/to/memcache --type integration --rank 4 --protocol device_sdma --device-ids 4,5,6,7 --gpu-mem-util 0.60 --log

# 4b. 4-rank 分次运行（推荐，避免 310P3 孤儿 NPU 上下文问题）
bash run_test.sh --memcache-dir /path/to/memcache --type integration --rank 4 --protocol device_sdma --device-ids 4,5,6,7 --gpu-mem-util 0.60 --split --log

# 5. 吞吐基准测试（2-rank, cache-miss vs cache-hit）
bash run_test.sh --memcache-dir /path/to/memcache --type bench --rank 2 --device-ids 6,7

# 6. 纯 DRAM 测试（4-rank, host_shm, 0 HBM, 10GB DRAM, ~16 分钟）
bash run_test.sh --memcache-dir /path/to/memcache --type puredram --device-ids 4,5,6,7 --log

# 6b. 纯 DRAM 分次运行（推荐，避免 310P3 孤儿 NPU 上下文问题）
bash run_test.sh --memcache-dir /path/to/memcache --type puredram --device-ids 4,5,6,7 --split --log
```

### 测试过滤（`--filter`）

`--filter` 透传给 `pytest -m`（marker 过滤），值为单个单词，无空格。不传 `--filter` 时按 `--type` 和 `--rank` 自动选择默认 marker。

| `--type` | `--rank` | 默认 marker | 说明 |
|----------|----------|-------------|------|
| workload | — | `workload` | Layer 2 全部 6 个场景 |
| integration | 1 | `rank1` | L3 单 rank 变体（TP=1） |
| integration | 2 | `rank2` | L3-1/L3-2/L3-3（含 30%/50% 参数化） |
| integration | 4 | `rank4` | L3-4/L3-5（4-rank 变体） |
| largecap | — | `largecap`（L2）/ `rank4`（L3） | 大容量测试 |
| puredram | — | `rank4` | 纯 DRAM（host_shm, 0 HBM, 10GB DRAM）4-rank |
| bench | 1/2/4 | — | 吞吐基准，无 marker 过滤 |

可用 marker：`workload`、`largecap`、`rank1`、`rank2`、`rank4`、`throughput`

```bash
# 只跑 throughput bench
bash run_test.sh --memcache-dir /path/to/memcache --type integration --rank 2 --device-ids 6,7 --filter throughput
```

### `run_test.sh` 完整参数

| 参数 | 说明 | 默认值 |
|------|------|--------|
| `--memcache-dir <DIR>` | memcache 仓库目录（必选） | — |
| `--type <TYPE>` | 测试类型：workload / integration / largecap / bench / puredram | workload |
| `--rank <N>` | rank 数：2 或 4（integration/bench 必选） | — |
| `--protocol <PROTO>` | 协议：host_shm 或 device_sdma（integration） | 配置默认 |
| `--device-ids <a,b,...>` | NPU 设备 ID（逗号分隔） | 2-rank: 6,7 / 4-rank: 0,1,2,3 / largecap,puredram: 4,5,6,7 |
| `--filter <name>` | pytest `-m` marker 过滤 | 按 type/rank 自动 |
| `--gpu-mem-util <float>` | GPU 显存利用率 | 0.80（largecap: 0.70, puredram: 0.60） |
| `--log` | 输出 tee 到 `retest_logs/` 目录 | 关 |
| `--skip-clean` | 跳过测试前环境清理 | 关 |
| `--split` | 4-rank integration/puredram: 逐个测试分次运行，中间清理 NPU | 关 |
| `--bench-num-prompts <N>` | bench: 每轮请求数 | 20 |
| `--bench-request-rate <N>` | bench: 请求速率 | 5 |
| `--bench-input-len <N>` | bench: 输入 token 长度 | 1024 |
| `--bench-output-len <N>` | bench: 输出 token 长度 | 32 |

## 上游实现说明

### MemCache Python 绑定

| 文件 | 修改内容 |
|------|---------|
| `src/memcache/csrc/python_wrapper/pymmc.cpp` | pybind11 枚举参数 `static_cast<int32_t>` 适配 |

**为什么需要：** `pymmc.cpp` 中 10 处 `py::arg("direct") = SMEMB_COPY_G2H` 使用 C 枚举值作为 pybind11 默认参数。pybind11 在运行时 `import` 时尝试将枚举值转换为 Python 对象，但 `SMEMB_COPY_*` 所属的 C 风格 `typedef enum` 未向 pybind11 注册类型，导致 `ImportError: could not convert default argument into a Python object (type not registered yet?)`，`memcache_hybrid` 模块完全无法导入。加 `static_cast<int32_t>()` 将枚举值显式转为 `int32_t`（已注册类型），解决导入失败。

**编译影响：** 无。C++ 编译器允许枚举到 `int32_t` 的隐式转换，原默认参数也能通过 C++ 编译，问题仅在 `python3 -c "from memcache_hybrid import ..."` 时触发。

### memfabric_hybrid 子模块

MemFabric 从 `GDD_ESCC/memfabric_hybrid.git` 获取 `trunk_for_escc_v1.2.0`，并切换到该分支。窗口由地址管理器选择并精确预留，执行线程检查 ACL 上下文。构建后安装本次产物，并由 `verify.sh` 比较已安装运行库与构建文件的 SHA256。


### vLLM-Ascend 适配

| 文件 | 修改内容 |
|------|---------|
| `vllm_ascend/_310p/model_runner_310p.py` | 310P 允许 AscendStoreConnector + MemCache，保留其他 connector/backend 检查 |
| `vllm_ascend/distributed/kv_transfer/kv_pool/ascend_store/pool_worker.py` | 使用 0.24 的 hash block size 解析；未满足 2 MiB 起始对齐时保留原区域 |
| `vllm_ascend/distributed/kv_transfer/kv_pool/ascend_store/backend/memcache_backend.py` | 协议读取、ACL 设备初始化、缓存区域注册日志 |
| `vllm_ascend/patch/platform/patch_kv_cache_coordinator.py` | 使用包含 Mamba 状态的共同有效前缀，按缓存组返回命中 token 数 |
| `csrc/moe/chunk_fwd_o/op_kernel/arch20/compat_310p.h` | Bisheng 编译器已有 bf16 类型时避免重复定义，保留 310P 转换 shim |
| `csrc/moe/chunk_gated_delta_rule_fwd_h/op_kernel/arch20/compat_310p.h` | 同上 |
| `csrc/moe/chunk_gated_delta_rule_compute_wy/op_kernel/arch20/compat_310p.h` | 同上 |

`tests/ut/patch/platform/test_prefix_cache_cp_patches.py` 中的回归用例由 vLLM-Ascend 仓库维护。

## 测试设计

测试采用三层分层架构，从底向上逐步验证 KV Cache 存储对 LLM 推理 TTFT 的影响：

| 层级 | 度量目标 | 含模型前向 | 测试目录 | --type | marker |
|------|---------|-----------|---------|--------|--------|
| Layer 2 | T_kv_cache 存储延迟 | 否 | `workload/` | workload | `workload` |
| Layer 3 | 全链路 TTFT | 是 | `integration/` | integration | `rank2` / `rank4` |
| 大容量 L2 | 大容量存储延迟 | 否 | `largecap/` | largecap | `largecap` |
| 大容量 L3 | 大容量全链路 TTFT | 是 | `integration/` | largecap | `rank4` |
| 吞吐基准 | throughput/TTFT/ITL | 是 | `integration/` | bench | — |

### 全链路 TTFT 分解

```
全链路 TTFT = T_network + T_prefill_compute + T_kv_cache + T_output
   (~1-5ms)    (~3400ms, 96%+)   (~4.3ms)     (~1-5ms)

Layer 2 度量: T_kv_cache（MemCache 存储延迟，不含模型前向计算）
Layer 3 度量: 全链路 TTFT（含网络 + prefill + KV Cache + output）
```

### Layer 2：存储延迟微基准

`LLMWorkloadSimulator` 直连 `DistributedObjectStore` API（`put_from_layers`/`get_into_layers`），不经 vLLM，使用 `time.perf_counter()` + `torch_npu.npu.current_stream().synchronize()` 精确度量 NPU 异步操作完成时间。6 个场景覆盖 KV Cache 全生命周期：

| # | 测试场景 | 方法 |
|---|---------|------|
| L2-1 | 单请求 prefill TTFT | 100 次 L2G write（seq_len=4096），统计 P99 |
| L2-2 | 变长序列 prefill TTFT | seq_len 512~16384，验证线性扩展 |
| L2-3 | 并发 prefill TTFT | 多 rank 并行（需多卡环境）；同卡共享 SDMA 场景跳过 |
| L2-4 | prefill→decode 生命周期 | 1 次 prefill + 256 步 decode |
| L2-5 | prefix cache hit vs miss | 20 轮 miss(写) vs hit(读) 配对 |
| L2-6 | DRAM→HBM rewarm TTFT | 50 次 G2L 跨层级读取 |

### Layer 3：vLLM 端到端 TTFT

启动真实 vLLM serve 进程（TP=1、TP=2 或 TP=4），通过 HTTP streaming API 度量请求发送到首 token 的时间。三种运行模式对比：

| 模式 | `--enable-prefix-caching` | `--kv-transfer-config` | 说明 |
|------|--------------------------|------------------------|------|
| Baseline | 开启 | 无 | 请求后重置本地缓存，完整 prefill |
| Cache miss | 开启 | memcache | 请求后重置本地缓存，prefill + KV 写入 |
| Cache hit | 开启 | memcache | 本地缓存已重置，从 MemCache 读 KV |

| # | 测试场景 | 方法 | marker |
|---|---------|------|--------|
| L3-1 | MemCache vs Baseline 开销 | 全 miss，MemCache P50 vs Baseline P50 | `rank2` |
| L3-2 | Prefix cache hit 降幅 | 10 轮 miss→hit 配对 | `rank2` |
| L3-3 | 部分命中率 TTFT | 5 个预热请求与 5 个新请求混合 | `rank2` |
| L3-4 | 4-rank MemCache vs Baseline | 同 L3-1，TP=4 | `rank4` |
| L3-5 | 4-rank Prefix cache hit | 同 L3-2，TP=4 | `rank4` |
| L3-1r | 1-rank MemCache vs Baseline | 同 L3-1，TP=1 | `rank1` |
| L3-2r | 1-rank Prefix cache hit | 同 L3-2，TP=1 | `rank1` |
| L3-3r | 1-rank 部分命中率 TTFT | 同 L3-3，TP=1 | `rank1` |
| L3-source | 缓存来源及正确性 | 对照独立基线生成；验证本地/外部/混合命中指标 | `rank1` / `rank2` / `rank4` |
| L3-6 | Throughput bench（可选） | `vllm bench serve` | `throughput` |

### 大容量测试

以下为历史测试模式，尚未完成本次 0.24 迁移回归。`large_capacity_vllm_test.py` 仍使用旧参数 `--prefix-match-unit` 和关闭本地 prefix cache 的配置，当前请使用 `--type integration` 验证。

大容量测试（`--type largecap`）使用 3GB HBM + 10GB DRAM（4-rank, TP=4），专门验证 HBM→DRAM eviction、DRAM rewarm、大规模 prefix cache hit 等大容量场景，与 Layer 2/3 使用不同的配置文件（`mmc-local-largecap-4rank.conf`）。

### 缓存隔离与正确性

最终方案保留原版 vLLM 0.24 的本地 prefix cache，使用 Mamba `align` 配置；vLLM 核心不打补丁。vLLM-Ascend 的混合前缀匹配包含对应 Mamba 状态，避免只恢复注意力 KV 时生成错误结果。

三项 TTFT 用例在每个完成请求之后调用 `POST /reset_prefix_cache?reset_external=false`，只清理本地缓存；检查 HTTP 成功及引擎新增的成功重置日志。重置发生在计时之外。相同隔离方法用于无 MemCache 的基线。

| 测试 | 缓存处理 |
|------|---------|
| 全 miss | 不同输入，基线和 MemCache 请求后均重置本地缓存 |
| 50% hit | 先写入部分输入，等待外部保存，再混合命中和 miss；每请求后重置本地缓存 |
| 100% hit | 先写后读相同输入，等待外部保存；每请求后重置本地缓存 |
| 缓存来源正确性 | 独立基线生成 A/B 参考 token；MemCache 预填 A/B，再分别构造外部、本地和混合命中，与参考 token 比较 |

缓存来源测试从 `/metrics` 读取 `vllm:prefix_cache_hits_total` 和 `vllm:external_prefix_cache_hits_total`，等待指标刷新并检查增量：

| 场景 | 本地命中增量 | 外部命中增量 |
|------|-------------|-------------|
| 纯外部命中 | 0 | >0 |
| 纯本地命中 | >0 | 0 |
| 混合命中 | >0 | >0 |

TTFT 以首个实际 token 的 SSE 响应计时，支持仅有 token ID 的特殊 token；HTTP 错误、SSE error 和空流均失败。当前完整测试结果及限制见 [迁移报告](docs/vllm_024_migration_results.md)。

## 注意事项

1. **设备选择**: 测试需同卡 die-pair（dev0+1, dev2+3, dev4+5, dev6+7）；4-rank 需 2 组 die-pair（如 dev4,5,6,7），通过 `--device-ids` 指定
2. **TP 由 --rank 指定**: `run_test.sh --type integration` 通过 `--rank 1`、`--rank 2` 或 `--rank 4` 选择 TP 和配置文件（1→integration-1rank.conf, 2→integration.conf, 4→integration-4rank.conf），设备数必须匹配
3. **gpu-mem-util**: 默认 0.80；4-rank 若设备显存被其他进程占用，可降低到 0.60（如 `--gpu-mem-util 0.60`）
4. **环境清理**: integration 测试管理自己启动的进程，不执行全局清理；其他测试类型仍需检查其清理脚本对共享环境的影响
5. **孤儿 NPU 上下文**: VLLM::Worker_TP 异常退出后 HBM 可能不释放，需改选其他设备
6. **4-rank 分次运行**: `--split` 分别运行用例。性能会受其他设备任务影响，应在空闲设备上测试。
7. **/dev/shm**: host_shm 协议下 dram.size 不超过 1GB；4-rank 时 4 × 1GB = 4GB（/dev/shm 8GB 充足）；largecap 模式自动 resize 到 100GB
8. **测试过滤**: `--filter` 使用 pytest `-m`（marker），值为单个单词无空格；可用 marker：`workload`、`largecap`、`rank1`、`rank2`、`rank4`
9. **文件日志**: 加 `--log` 参数将输出 tee 到 `retest_logs/` 目录


## 源码与运行库记录

部署固定获取上述三个上游分支。目录不存在时 clone，存在时 fetch、切换到指定分支并进行快进更新；
有未提交源码修改或无法快进时报告错误，不覆盖文件。
部署记录保存在 `retest_logs/source_manifest.json`，包含各仓库实际提交号及关键构建文件 SHA256。
`verify.sh` 核对当前版本、Python 包加载路径、MemCache wheel、已安装 MemFabric 运行库和 MetaService 构建产物。
仅同步源码的记录写入 `retest_logs/source_revisions.json`，不会替代完整构建后的部署记录。
测试结果引用该记录中的提交号，分支更新后重新部署、验证并运行测试。
