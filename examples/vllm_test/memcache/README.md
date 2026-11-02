# vLLM TTFT 测试环境部署包

在 memcache 基准源码上应用补丁、更新子模块、构建安装，然后在**打包目录内**直接运行测试。新增的测试文件不拷贝回原工程。

## 目录结构

```
ttft_test/
├── README.md
├── setup.sh                 # 部署脚本：对 memcache 仓库打补丁 + 构建安装
├── verify.sh                # 验证部署结果
├── run_test.sh              # 统一测试运行脚本（workload / integration / largecap / bench）
├── start_service.sh         # 启动 MetaService + vLLM（支持 --detach 后台模式）
├── pytest.ini               # pytest 配置（注册 marker：throughput / rank2 / rank4 / workload / largecap）
├── patches/
│   ├── memcache_modified_files.patch                  # memcache pymmc.cpp pybind11 枚举 static_cast 适配
│   ├── fix_310p_p2p_huge_fallback_to_hbm_huge.patch   # memfabric_hybrid 207001 P2P_HUGE 回退补丁
│   └── vllm_ascend_310p_kv_transfer.patch             # vLLM-Ascend 310P3 适配（3 文件）
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
│           └── vllm_integration_test.py     # 6 个测试函数：全 miss 基准 + 50% 命中 + 100% 命中 + 4rank 变体
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
| memcache | tag `v1.2.1` |
| memfabric_hybrid | `dev-v1.2.0` 分支（`https://gitcode.com/GDD_ESCC/memfabric_hybrid.git`） |
| vLLM-Ascend | `v0.26.0rc1`（`f2f74a16c`） |

## 前置条件

1. **memcache 仓库**（setup.sh 自动 clone + checkout v1.2.1，无需手动准备）
2. **memfabric_hybrid** 子模块 remote 可 fetch `dev-v1.2.0` 分支
3. **vLLM-Ascend** 已安装（Layer 3 需要，Layer 2 不需要）
4. **CANN** 已安装（默认 `/usr/local/Ascend/cann-9.1.0`）
5. **W8A8 量化模型**已部署（默认路径 `/home/models/Qwen3.6-35B-A3B-w8a8`，Layer 3 需要，Layer 2 不需要；可通过环境变量 `MMC_TEST_VLLM_MODEL_PATH` 覆盖）

## 快速部署

```bash
# 一键部署（memcache 目录不存在则自动 clone + checkout v1.2.1）
bash setup.sh --memcache-dir /path/to/memcache --yes

# 仅 Layer 2（不碰 vLLM-Ascend）
bash setup.sh --memcache-dir /path/to/memcache --yes --skip-patch-vllm

# 仅打补丁不构建
bash setup.sh --memcache-dir /path/to/memcache --yes --skip-build
```

> 首次编译约 15-20 分钟（207 个编译目标），后续增量编译更快。

## 验证

```bash
bash verify.sh --memcache-dir /path/to/memcache
```

## 运行测试

测试在**打包目录**中执行，通过 `--memcache-dir` 指向 memcache 仓库（用于查找构建产物和 `config/mmc-meta.conf`）。

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
bash setup.sh --memcache-dir /path/to/memcache --yes

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
```

### 测试过滤（`--filter`）

`--filter` 透传给 `pytest -m`（marker 过滤），值为单个单词，无空格。不传 `--filter` 时按 `--type` 和 `--rank` 自动选择默认 marker。

| `--type` | `--rank` | 默认 marker | 说明 |
|----------|----------|-------------|------|
| workload | — | `workload` | Layer 2 全部 6 个场景 |
| integration | 2 | `rank2` | L3-1/L3-2/L3-3（含 30%/50% 参数化） |
| integration | 4 | `rank4` | L3-4/L3-5（4-rank 变体） |
| largecap | — | `largecap`（L2）/ `rank4`（L3） | 大容量测试 |
| bench | 2/4 | — | 吞吐基准，无 marker 过滤 |

可用 marker：`workload`、`largecap`、`rank2`、`rank4`、`throughput`

```bash
# 只跑 throughput bench
bash run_test.sh --memcache-dir /path/to/memcache --type integration --rank 2 --device-ids 6,7 --filter throughput
```

### `run_test.sh` 完整参数

| 参数 | 说明 | 默认值 |
|------|------|--------|
| `--memcache-dir <DIR>` | memcache 仓库目录（必选） | — |
| `--type <TYPE>` | 测试类型：workload / integration / largecap / bench | workload |
| `--rank <N>` | rank 数：2 或 4（integration/bench 必选） | — |
| `--protocol <PROTO>` | 协议：host_shm 或 device_sdma（integration） | 配置默认 |
| `--device-ids <a,b,...>` | NPU 设备 ID（逗号分隔） | 2-rank: 6,7 / 4-rank: 0,1,2,3 |
| `--filter <name>` | pytest `-m` marker 过滤 | 按 type/rank 自动 |
| `--gpu-mem-util <float>` | GPU 显存利用率 | 0.80（largecap: 0.70） |
| `--log` | 输出 tee 到 `retest_logs/` 目录 | 关 |
| `--skip-clean` | 跳过测试前环境清理 | 关 |
| `--split` | 4-rank integration: 逐个测试分次运行，中间清理 NPU | 关 |
| `--bench-num-prompts <N>` | bench: 每轮请求数 | 20 |
| `--bench-request-rate <N>` | bench: 请求速率 | 5 |
| `--bench-input-len <N>` | bench: 输入 token 长度 | 1024 |
| `--bench-output-len <N>` | bench: 输出 token 长度 | 32 |

## 补丁说明

### memcache 补丁（1 个文件）

| 文件 | 修改内容 |
|------|---------|
| `src/memcache/csrc/python_wrapper/pymmc.cpp` | pybind11 枚举参数 `static_cast<int32_t>` 适配 |

**为什么需要：** `pymmc.cpp` 中 10 处 `py::arg("direct") = SMEMB_COPY_G2H` 使用 C 枚举值作为 pybind11 默认参数。pybind11 在运行时 `import` 时尝试将枚举值转换为 Python 对象，但 `SMEMB_COPY_*` 所属的 C 风格 `typedef enum` 未向 pybind11 注册类型，导致 `ImportError: could not convert default argument into a Python object (type not registered yet?)`，`memcache_hybrid` 模块完全无法导入。加 `static_cast<int32_t>()` 将枚举值隐式转为 `int32_t`（已注册类型），解决导入失败。

**编译影响：** 无。C++ 编译器允许枚举到 `int32_t` 的隐式转换，不打补丁也能编译通过，问题仅在 `python3 -c "from memcache_hybrid import ..."` 时触发。

### memfabric_hybrid 补丁（1 个文件）

| 文件 | 修改内容 |
|------|---------|
| `3rdparty/memfabric_hybrid/src/hybm/csrc/mm/hybm_vmm_based_segment.cpp` | 207001 P2P_HUGE 回退：`AclrtMallocPhysical` P2P 分区 OOM 时回退 `ACL_HBM_MEM_HUGE` |

**为什么需要：** 310P3 跨卡 die-pair P2P 映射时，`AclrtMallocPhysical` 分配 P2P_HUGE 大页可能返回 207001（OUT_OF_MEMORY），导致 store.init 失败。补丁在 OOM 时自动回退到普通 HBM_HUGE 大页。

### vLLM-Ascend 补丁（3 个已有文件）

| 文件 | 修改内容 |
|------|---------|
| `vllm_ascend/_310p/model_runner_310p.py` ~L704 | 移除 310P KV transfer `raise ValueError` 阻断 |
| `vllm_ascend/distributed/kv_transfer/kv_pool/ascend_store/pool_worker.py` ~L763 | 2MB 对齐 `assert` 改为 `logger.warning` + `continue` |
| `vllm_ascend/distributed/kv_transfer/kv_pool/ascend_store/backend/memcache_backend.py` | 新增 `_is_host_shm()`/`_get_protocol()`/`_resolve_physical_device_id()`、ACL init、register_buffer 错误处理 |

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

| # | 测试场景 | 方法 | 门限 |
|---|---------|------|------|
| L2-1 | 单请求 prefill TTFT | 100 次 L2G write（seq_len=4096），统计 P99 | P99 < 50ms |
| L2-2 | 变长序列 prefill TTFT | seq_len 512~16384，验证线性扩展 | R² > 0.9 |
| L2-3 | 并发 prefill TTFT | 多 rank 并行（需多卡环境） | SKIP（同卡共享 SDMA） |
| L2-4 | prefill→decode 生命周期 | 1 次 prefill + 256 步 decode | Decode P50 < prefill × 0.5 |
| L2-5 | prefix cache hit vs miss | 20 轮 miss(写) vs hit(读) 配对 | Hit P50 ≤ Miss P50 × 1.0 |
| L2-6 | DRAM→HBM rewarm TTFT | 50 次 G2L 跨层级读取 | rewarm > hit 且 < prefill × 1.1 |

### Layer 3：vLLM 端到端 TTFT

启动真实 vLLM serve 进程（TP=2 或 TP=4），通过 HTTP streaming API 度量请求发送到首 token 的时间。三种运行模式对比：

| 模式 | `--enable-prefix-caching` | `--kv-transfer-config` | 说明 |
|------|--------------------------|------------------------|------|
| Baseline | 关闭 | 无 | 完整 prefill，无缓存 |
| Cache miss | 开启 | memcache | prefill + KV 写入 MemCache |
| Cache hit | 开启 | memcache | 从 MemCache 读 KV，跳过 prefill |

| # | 测试场景 | 方法 | 门限 | marker |
|---|---------|------|------|--------|
| L3-1 | MemCache vs Baseline 开销 | 全 miss，MemCache P50 vs Baseline P50 | 开销 < 20% | `rank2` |
| L3-2 | Prefix cache hit 降幅 | 20 轮 miss→hit 配对 | Hit P50 / Miss P50 < 70% | `rank2` |
| L3-3 | 部分命中率 TTFT | 30%/50% 混合请求 | 实际命中 ≥70%，改善 > hit_rate×20% | `rank2` |
| L3-4 | 4-rank MemCache vs Baseline | 同 L3-1，TP=4 | 同上 | `rank4` |
| L3-5 | 4-rank Prefix cache hit | 同 L3-2，TP=4 | 同上 | `rank4` |
| L3-6 | Throughput bench（可选） | `vllm bench serve` | 默认 deselected | `throughput` |

### 大容量测试

大容量测试（`--type largecap`）使用 3GB HBM + 10GB DRAM（4-rank, TP=4），专门验证 HBM→DRAM eviction、DRAM rewarm、大规模 prefix cache hit 等大容量场景，与 Layer 2/3 使用不同的配置文件（`mmc-local-largecap-4rank.conf`）。

### vLLM 本地缓存排除方案

#### 背景：双写问题

vLLM 开启 `--enable-prefix-caching` 时有自带的 GPU 驻留 block-level prefix cache。当同时启用 MemCache（`--kv-transfer-config`），一次 full miss 请求的 prefill 计算完成后，KV cache 会被**同时写入**两处：

- **vLLM 本地缓存**：GPU 显存 block + `block_hash → block` 映射字典（vLLM 自身机制，同步写入）
- **MemCache**：通过 `AscendStoreConnector` 异步写入 HBM/DRAM pool（后台线程 `KVCacheStoreSendingThread` 执行 `m_store.put()` → `batch_put_from_layers` L2G DMA）

读取时 vLLM **优先查本地**：本地命中则直接用 GPU block，跳过 MemCache connector。若不排除本地缓存，hit 测试的 TTFT 降幅无法区分是 vLLM 本地缓存还是 MemCache 的贡献。

#### 排除手段：`--no-enable-prefix-caching` + `--kv-transfer-config`

测试在 MemCache 模式下使用 `--no-enable-prefix-caching` + `--kv-transfer-config` 启动 vLLM。vLLM 的 block hasher 仍然创建（`v1/engine/core.py:214`：`enable_prefix_caching or kv_connector is not None`），因此 `request.block_hashes` 照常生成，MemCache connector 的 lookup/save 正常工作。但 vLLM 本地 prefix cache 被永久关闭：

- `get_computed_blocks` 始终返回 `(empty, 0, 0)`（`v1/core/kv_cache_manager.py:228`），每条请求都走 connector lookup
- 无需 `VLLM_SERVER_DEV_MODE=1` 或 `POST /reset_prefix_cache`
- 无 reset 失败风险（`v1/core/block_pool.py:770` 的 `num_used_blocks != 1` 前置条件问题）
- `local_hit_rate` 始终为 0%，`external_hit_rate` 直接反映 MemCache 贡献

> **注意**：关闭 prefix caching 后，hybrid 模型（如 Qwen3.5）的 attention block_size=640、mamba block_size=8192，AscendStoreConnector 的 `pool_worker.py:171` 断言 `group_block_size % hash_block_size == 0` 会因 `8192 % 640 ≠ 0` 失败。需显式传 `--prefix-match-unit 128`（GCD(640, 8192)）解决。

各测试用例的隔离方式：

| 测试 | 隔离方法 |
|------|---------|
| L3-1（开销对比） | Baseline 和 MemCache 模式均用 `--no-enable-prefix-caching`；MemCache 模式全 miss（unique prompts），不依赖 hit 隔离 |
| L3-2（100% hit） | `--no-enable-prefix-caching` + connector，每条请求走 MemCache lookup；miss 后 sleep 1s 等待 async save，再重发同一 prompt |
| L3-3（30%/50% hit） | 同上；pre-populate 后 sleep 1s，混合请求中 hit 部分走 MemCache |
| LC-L3-2（大容量 hit） | 同 L3-2 模式，40-key pre-populate |
| LC-L3-3（eviction 混合） | pre-populate 30 keys 后混合 30%/70% hit |

#### 三种缓存来源区分

vLLM engine metrics 日志周期性输出两个独立指标，测试通过 `get_cache_stats()` 解析区分：

| 缓存来源 | 日志指标 | 判定条件 |
|---------|---------|---------|
| vLLM 本地命中 | `Prefix cache hit rate: XX%` | `local_computed >= token_len`，connector 被跳过（本方案下始终为 0%） |
| MemCache 命中 | `External prefix cache hit rate: XX%` | `local_computed = 0`，`external_hit > 0` |
| 全 miss | — | `local_computed = 0`，`external_hit = 0` |

测试日志中 `vLLM local hit=0.0%` 证明本地缓存已永久关闭，hit 的 TTFT 降幅来自 MemCache external hit。

## 注意事项

1. **设备选择**: 测试需同卡 die-pair（dev0+1, dev2+3, dev4+5, dev6+7）；4-rank 需 2 组 die-pair（如 dev4,5,6,7），通过 `--device-ids` 指定
2. **TP 由 --rank 指定**: `run_test.sh --type integration` 通过 `--rank 2` 或 `--rank 4` 选择 TP 和配置文件（2→integration.conf, 4→integration-4rank.conf），设备数必须匹配
3. **gpu-mem-util**: 默认 0.80；4-rank 若设备显存被其他进程占用，可降低到 0.60（如 `--gpu-mem-util 0.60`）
4. **环境清理**: 每次测试前必须执行 `clean_stale_env.sh`（run_test.sh 自动调用，可用 `--skip-clean` 跳过）
5. **孤儿 NPU 上下文**: VLLM::Worker_TP 异常退出后 HBM 可能不释放，需改选其他设备
6. **4-rank 分次运行**: 310P3 上 vLLM 正常退出后 HBM 也可能残留（孤儿 NPU 上下文），连续运行两个 4-rank 测试时第二个 vLLM 会 OOM。**推荐加 `--split` 参数**，自动逐个测试分次运行，每个测试之间清理 NPU + 重启 MetaService。也可手动分次：
   ```bash
   # 手动分次运行（等效于 --split）
   bash run_test.sh --memcache-dir /path --type integration --rank 4 --device-ids 2,3,6,7 --filter rank4 --gpu-mem-util 0.60 --log
   # 清理 NPU 后再运行下一个
   bash /path/memcache/debug/test/clean_stale_env.sh
   # 用 -k 指定单个测试
   MMC_TEST_TENSOR_PARALLEL_SIZE=4 MMC_TEST_DEVICE_IDS=2,3,6,7 \
       pytest cases/python/memcache/integration/vllm_integration_test.py -v -s -k test_prefix_cache_hit_e2e_ttft_4rank
   ```
7. **/dev/shm**: host_shm 协议下 dram.size 不超过 1GB；4-rank 时 4 × 1GB = 4GB（/dev/shm 8GB 充足）；largecap 模式自动 resize 到 100GB
8. **测试过滤**: `--filter` 使用 pytest `-m`（marker），值为单个单词无空格；可用 marker：`workload`、`largecap`、`rank2`、`rank4`
9. **文件日志**: 加 `--log` 参数将输出 tee 到 `retest_logs/` 目录
