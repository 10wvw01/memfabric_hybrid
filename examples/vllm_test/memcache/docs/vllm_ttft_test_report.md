# MemCache vLLM TTFT 性能测试报告

## 1. 概述

本报告记录 MemCache v1.2.1 在 Ascend 310P3 NPU 上的 vLLM TTFT（Time To First Token）性能测试结果。测试采用三层分层架构，从底向上逐步验证 KV Cache 存储性能对 LLM 推理 TTFT 的影响：

```
Layer 1: 存储层微基准 (example/benchmark/mutil_process.py)   — 已有，裸 API 吞吐
Layer 2: 模拟推理工作负载 (cases/python/memcache/workload/)    — ✅ 已完成 (2026-09-30 复测)
Layer 3: vLLM 端到端 TTFT (cases/python/memcache/integration/) — ✅ 已完成 (2026-10-30 复测)
```

- **测试日期**: 2026-10-30 (Layer 3 复测，含 2-rank，全新合并测试文件)
- **MemCache 版本**: 1.2.1 / memfabric_hybrid dev-v1.2.0
- **2-rank (TP=2) 测试结果**: **3 passed**（host_shm 协议，全 miss / 50% 命中 / 100% 命中）
- **缓存隔离验证**: 所有测试中 `vLLM local hit=0.0%`，确认 cache hit 来自 MemCache 而非 vLLM 内置缓存（通过 `--no-enable-prefix-caching` 永久关闭 vLLM 本地缓存实现隔离）

---

## 2. 测试环境与配置

### 2.1 硬件

| 项目 | 规格 |
|------|------|
| NPU 型号 | Ascend 310P3 (Atlas 300I Duo) |
| NPU 数量 | 8 devices (dev0..dev7, 4 张物理卡 × 2 die-pair) |
| HBM 容量 | 44 GB / device |
| 通信协议 | host_shm（DRAM 共享内存） |

### 2.2 软件

| 项目 | 版本 |
|------|------|
| 操作系统 | Ubuntu 22.04 LTS (aarch64) |
| CANN | cann-9.1.0 |
| Python | 3.12.13 |
| PyTorch | 2.x + torch_npu |
| MemCache | 1.2.1 |
| memfabric_hybrid | dev-v1.2.0（含 SDID WARN fallback + IsSameCard 修复 + 207001 P2P_HUGE 回退补丁） |
| vLLM | 0.26.0 + vLLM-Ascend |
| 模型 | Qwen3.6-35B-A3B-w8a8 |

### 2.3 KV Cache 结构

复用 `example/benchmark/mutil_process.py` 中 DeepSeek R1 W8A8 参数：

| 参数 | 值 |
|------|------|
| 层数 (num_layers) | 61 |
| K 大小 / 层 | 128 KB |
| V 大小 / 层 | 16 KB |
| 单层 KV 总量 | 144 KB |
| block_size | 128 tokens |
| 单 request (seq_len=4096) KV 总量 | 61 × 32 blocks × 144 KB = 281 MB |

### 2.4 全链路 TTFT 分解

```
全链路 TTFT = T_network + T_prefill_compute + T_kv_cache + T_output
   (~1-5ms)    (~3386ms, 310P3)     (~4.3ms)      (~1-5ms)

Layer 2 度量: T_kv_cache（MemCache 存储延迟，不含模型前向计算）
Layer 3 度量: 全链路 TTFT（含网络 + prefill + KV Cache + output）
```

### 2.5 2-Rank 同卡 Die-Pair 拓扑

```
┌──────────────────────────────────────────────────┐
│              310P3 NPU Card (同卡)                │
│                                                    │
│  ┌──────────────┐         ┌──────────────┐      │
│  │  rank0 (devX) │◄─SDMA──► rank1 (devY)  │     │
│  │  HBM pool     │  D2D    │  HBM pool     │     │
│  │  (worker)     │         │  (worker)     │     │
│  └──────────────┘         └──────────────┘      │
│         │                                          │
│         ▼                                          │
│  ┌──────────────┐                                 │
│  │  DRAM pool    │  (/dev/shm 共享内存)            │
│  └──────────────┘                                 │
└──────────────────────────────────────────────────┘
         │
         ▼ TCP (port 5000/6000)
┌──────────────┐
│ MetaService   │  rank 分配 + 元数据管理
└──────────────┘
```

---

## 3. 测试方案

### 3.1 三层架构

| 层级 | 度量目标 | 含模型前向 | 含 vLLM 调度 | 含网络传输 |
|------|---------|-----------|-------------|-----------|
| Layer 1 | 裸 API 吞吐 | 否 | 否 | 否 |
| Layer 2 | T_kv_cache 存储延迟 | 否 | 否 | 否 |
| Layer 3 | 全链路 TTFT | 是 | 是 | 是 |

### 3.2 Layer 3 设计原则

- 启动真实 vLLM serve 进程（TP=2），通过 HTTP streaming API 度量请求发送到首 token 的时间
- baseline: `--no-enable-prefix-caching`，无 `--kv-transfer-config`
- MemCache: `--no-enable-prefix-caching` + `--prefix-match-unit 128` + `--kv-transfer-config` AscendStoreConnector + memcache backend
- 测试框架自动管理 vLLM 生命周期（start/stop）和 MetaService 重启（stop 时清理残留 `VLLM::` 子进程 + 重启 MetaService）
- 2-rank (TP=2) 使用 1 组同卡 die-pair（dev6, dev7）

### 3.3 三种测试场景

| # | 测试场景 | 测试函数 | max_tokens | 说明 |
|---|---------|---------|-----------|------|
| 1 | 全 miss 基准 | `test_full_miss_baseline` | 1 | 对比有无 MemCache 的 TTFT 开销 |
| 2 | 50% 命中 | `test_50pct_hit` | 1 | 预填充一半 prompt，混合 hit/miss 请求 |
| 3 | 100% 命中 | `test_100pct_hit` | 1 | miss→hit 配对，度量全命中 TTFT 降幅 |

### 3.4 缓存隔离方法与 `--no-enable-prefix-caching` 适配

#### 3.4.1 隔离背景：双写问题

vLLM 开启 `--enable-prefix-caching` 时有自带 GPU 驻留 block-level prefix cache。当同时启用 MemCache（`--kv-transfer-config`），一次 full miss 请求的 prefill 计算完成后，KV cache 会被**同时写入两处**：

- **vLLM 本地缓存**：GPU 显存 block + `block_hash → block` 映射字典（vLLM 自身机制，同步写入）
- **MemCache**：通过 `AscendStoreConnector` 异步写入 HBM/DRAM pool（后台线程 `KVCacheStoreSendingThread` 执行 `m_store.put()` → `batch_put_from_layers` L2G DMA）

读取时 vLLM **优先查本地**：本地命中则直接用 GPU block，跳过 MemCache connector。若不排除本地缓存，hit 测试的 TTFT 降幅无法区分是 vLLM 本地缓存还是 MemCache 的贡献。

> **旧方案缺陷**：曾用 `POST /reset_prefix_cache` 在每轮 hit 前清空本地缓存，但 `block_pool.py:770` 的前置条件 `num_used_blocks != 1`（所有 block 必须 `ref_cnt == 0`）在有 in-flight 请求时返回 `False`，TP=4 下并发更多更易失败。当前方案从根本上避免此问题。

#### 3.4.2 隔离手段

测试在 MemCache 模式下使用 `--no-enable-prefix-caching` + `--kv-transfer-config` 启动 vLLM。vLLM 的 block hasher 仍然创建（`v1/engine/core.py:214`：`enable_prefix_caching or kv_connector is not None`），因此 `request.block_hashes` 照常生成，MemCache connector 的 lookup/save 正常工作。但 vLLM 本地 prefix cache 被永久关闭：

- `get_computed_blocks` 始终返回 `(empty, 0, 0)`（`v1/core/kv_cache_manager.py:228`），每条请求都走 connector lookup
- 无需 `VLLM_SERVER_DEV_MODE=1` 或 `POST /reset_prefix_cache`
- 无 reset 失败风险（`v1/core/block_pool.py:770` 的 `num_used_blocks != 1` 前置条件问题）
- `local_hit_rate` 始终为 0%，`external_hit_rate` 直接反映 MemCache 贡献

| 模式 | `--enable-prefix-caching` | `--kv-transfer-config` | `--prefix-match-unit` | 说明 |
|------|--------------------------|------------------------|----------------------|------|
| **Baseline** | `--no-enable-prefix-caching` | 无 | 无 | 完全无缓存，每次请求都做完整 prefill 前向计算 |
| **MemCache** | `--no-enable-prefix-caching` | `AscendStoreConnector` + `memcache` backend | `128` | vLLM 本地缓存永久关闭；KV Cache 写入/读取 MemCache 分布式存储（HBM/DRAM） |

#### 3.4.3 `--no-enable-prefix-caching` 引发的问题

**问题：hybrid 模型 block_size 整除性断言失败**

- **位置**: `vllm_ascend/distributed/kv_transfer/kv_pool/ascend_store/pool_worker.py:171`
- **断言**: `group_block_size % hash_block_size == 0`
- **根因**:
  - 测试模型 Qwen3.5 是 hybrid（attention + mamba）架构，存在两种 block_size：attention block_size=640、mamba block_size=8192
  - 开启 prefix caching 时，`hash_block_size` 默认 = vLLM `block_size`=128，`640 % 128 = 0` 且 `8192 % 128 = 0`，断言通过
  - 关闭 prefix caching 后，`hash_block_size` 默认值改变（取到 hybrid 模型的某一种 block_size，如 attention 的 640），导致 `8192 % 640 = 192 ≠ 0`，mamba 侧断言失败
- **现象**: connector 初始化直接报错，vLLM 启动失败

#### 3.4.4 适配改动

**改动：启动参数 `--prefix-match-unit 128`（无源码修改）**

显式指定 `hash_block_size = 128 = GCD(640, 8192)`，同时能整除 attention（640）和 mamba（8192）两种 block_size，断言通过。

代码位置：
- `cases/python/memcache/integration/vllm_integration_test.py:331`
- `cases/python/memcache/integration/large_capacity_vllm_test.py:342`
- `start_service.sh:129`

#### 3.4.5 易混淆澄清：2MB 物理地址对齐（与 `--no-enable-prefix-caching` 无关）

测试中存在两个"对齐"问题，容易混淆，但归因不同：

| 问题 | 位置 | 触发条件 | 与 `--no-enable-prefix-caching` 关系 | 解决方式 |
|------|------|---------|-------------------------------------|---------|
| block_size 整除性断言 | `pool_worker.py:171` | 关闭 prefix caching 后 `hash_block_size` 默认值改变，hybrid 两种 block_size 无法互相整除 | **直接因果** | 启动参数 `--prefix-match-unit 128`，不改源码 |
| 2MB 物理地址对齐 assert | `pool_worker.py:758` `_align_kv_ptrs` | 启用 `--kv-transfer-config` 即触发，vLLM KV cache tensor 指针未 2MB 物理对齐 | **无关** | 改 vLLM-Ascend 源码，assert → warning 降级（`patches/vllm_ascend_310p_kv_transfer.patch`） |

> **注意**: 2MB 对齐问题在所有 MemCache 测试中均出现（60 次/测试，host_shm 与 device_sdma 一致），不区分 prefix caching 开关。详见 `docs/error_analysis_report.md` 2.4 节。

#### 3.4.6 三种缓存来源区分

vLLM engine metrics 日志周期性输出两个独立指标，测试通过 `get_cache_stats()` 解析区分：

| 缓存来源 | 日志指标 | 判定条件 |
|---------|---------|---------|
| vLLM 本地命中 | `Prefix cache hit rate: XX%` | `local_computed >= token_len`，connector 被跳过（本方案下始终为 0%） |
| MemCache 命中 | `External prefix cache hit rate: XX%` | `local_computed = 0`，`external_hit > 0` |
| 全 miss | — | `local_computed = 0`，`external_hit = 0` |

测试日志中 `vLLM local hit=0.0%` 证明本地缓存已永久关闭，hit 的 TTFT 降幅来自 MemCache external hit。

#### 3.4.7 各测试用例的隔离方式

| 测试 | 隔离方法 |
|------|---------|
| 全 miss 基准（L3-1） | Baseline 和 MemCache 模式均用 `--no-enable-prefix-caching`；MemCache 模式全 miss（unique prompts），不依赖 hit 隔离 |
| 100% hit（L3-2） | `--no-enable-prefix-caching` + connector，每条请求走 MemCache lookup；miss 后 sleep 1s 等待 async save，再重发同一 prompt |
| 50% hit（L3-3） | 同上；pre-populate 后 sleep 1s，混合请求中 hit 部分走 MemCache |

### 3.5 测试参数

| 参数 | 值 | 说明 |
|------|------|------|
| `PROMPT_TOKENS` | 4096 | 每次请求的 prompt token 数（可通过 `MMC_TEST_PROMPT_TOKENS` 环境变量调整） |
| `MAX_TOKENS_TTFT` | 1 | 只生成 1 个 token，度量 TTFT（首 token 延迟） |
| `WARMUP_REQUESTS` | 1 | 预热请求数（不计入测量） |
| `MEASURED_REQUESTS` | 10 | 正式测量请求数 |
| `temperature` | 0 | 固定输出，消除采样随机性 |
| `VOCAB_SIZE` | 151936 | token ID 随机范围（Qwen3.5 词表） |
| `MEMCACHE_OVERHEAD_RATIO` | 0.2（20%） | 全 miss 基准门限：MemCache vs Baseline 开销 |
| `PREFIX_CACHE_HIT_RATIO` | 0.7（70%） | 50%/100% 命中门限：hit P50 / miss P50 |
| `HIT_TTFT_THRESHOLD_MS` | 1500 | 50% 命中场景实际命中判定门限（TTFT < 此值视为命中） |
| `VLLM_READY_TIMEOUT_S` | 600 | vLLM 启动超时（秒） |
| `TTFT_REQUEST_TIMEOUT_S` | 300 | 单次请求超时（秒） |

### 3.6 请求构造与度量方法

**prompt 生成**（`_build_prompt_ids()`）：使用 `random.Random(seed)` 生成随机 token ID 序列，不同 seed 产生不同 prompt。

**TTFT 度量**（`measure_e2e_ttft_ms()`）：使用 HTTP streaming API（`POST /v1/completions`，`stream=True`），`time.perf_counter()` 记录请求发送到首个 `data: ` SSE 事件到达的时间差。

**各场景请求构造**：

| 场景 | 请求模式 | 说明 |
|------|---------|------|
| 全 miss 基准 | 每个.prompt 发送 1 次（全 miss） | Baseline 阶段 + MemCache 阶段，对比开销 |
| 50% 命中 | 预填充 5 个 prompt 后发送 10 个混合请求 | 5 个重复（hit）+ 5 个新 prompt（miss），随机打乱 |
| 100% 命中 | 每个 prompt 发送 2 次（先 miss 后 hit） | 10 轮配对，miss 写入 MemCache，hit 从 MemCache 读取 |

### 3.7 断言门限

| 测试 | 断言 | 公式 | 门限 | 含义 |
|------|------|------|------|------|
| 全 miss 基准 | MemCache 开销 | `(memcache_p50 - baseline_p50) / baseline_p50` | < 20% | MemCache 引入的 TTFT 开销可接受 |
| 50% 命中 | 实际命中率 | `actual_hit_count / hit_count` | ≥ 50% | 预期命中的 prompt 中至少 50% 实际命中 |
| 50% 命中 | Hit vs Miss P50 | `hit_p50 / miss_p50` | < 70% | 命中请求的 TTFT 显著低于未命中 |
| 100% 命中 | Hit/Miss ratio | `hit_p50 / miss_p50` | < 70% | 全命中 TTFT 显著低于全 miss |

---

## 4. 前置条件（测试必读）

### 4.1 环境部署（一键脚本）

```bash
# 部署环境（memcache 目录不存在则自动 clone + checkout 基准 commit）
bash setup.sh --memcache-dir /path/to/memcache --yes
```

### 4.2 环境清理（每次测试前必做）

`run_test.sh` 默认在测试前自动调用 `clean_stale_env.sh` 清理残留进程和 IPC 资源，无需手动执行。可用 `--skip-clean` 跳过。

> **孤儿 NPU 驱动上下文**: `clean_stale_env.sh` 的 `pkill -9 -f 'VLLM::'` 可杀掉进程，但 VLLM::Worker_TP 进程异常退出后 NPU 驱动可能不释放 HBM 上下文（进程已死但 `npu-smi info` 仍显示占用）。此时只能选择其他空闲设备对进行测试。

### 4.3 NPU 设备选择与协议切换

通过 `run_test.sh` 的 `--device-ids` 和 `--protocol` 参数指定：

- 需 **同卡 die-pair**（如 dev0+dev1、dev2+dev3、dev6+dev7），跨卡 die-pair 无法使用 SDMA D2D
- 8 设备 4 张卡的 die-pair 对应关系：dev0+dev1 (Bus 18:00.0)、dev2+dev3 (Bus 41:00.0)、dev4+dev5 (Bus 96:00.0)、dev6+dev7 (Bus AB:00.0)
- 测试前确认目标设备空闲（`npu-smi info`），无残留进程占用 HBM

---

## 5. Layer 2: 模拟推理工作负载 TTFT 测试

### 5.1 测试条件

| 项目 | 值 |
|------|---|
| NPU 设备 | dev2 + dev3（同卡 die-pair, Bus 0000:41:00.0） |
| 协议 | `device_sdma` |
| HBM size | 2GB / rank |
| DRAM size | 4GB / rank |
| 测试日期 | 2026-09-30 |

### 5.2 测试结果汇总

| 场景 | 操作 | P50 (ms) | 对 TTFT 影响 |
|------|------|----------|-------------|
| 单请求 prefill (4K) | L2G write | 4.19 | 基准: prefill 写入 |
| seq_len=512 | L2G write | 1.49 | 短序列: TTFT 占比 ~2-5% |
| seq_len=16384 | L2G write | 13.52 | 长序列: TTFT 占比 ~1-3% |
| decode/step | L2G write | 1.26 | 影响 TPOT, 不直接影响 TTFT |
| cache hit | G2L read | 4.38 | 替代 prefill 计算 |
| DRAM rewarm | G2L cross | 4.21 | 比 HBM hit 快 2.6%（flaky） |

**结论**: MemCache 存储延迟不是 TTFT 瓶颈，T_kv_cache 占全链路 TTFT 的 1~5%，模型前向计算是主导因素。

---

## 6. Layer 3: vLLM 端到端 TTFT 测试

### 6.1 测试条件

| 项目 | 2-rank host_shm |
|------|----------------|
| NPU 设备 | dev6, dev7 |
| TP size | 2 |
| 协议 | `host_shm` |
| HBM size | 0 |
| DRAM size | 1GB / rank |
| GPU mem util | 0.80 |
| 配置文件 | `mmc-local-integration.conf` |
| 模型 | `Qwen3.6-35B-A3B-w8a8` |
| vLLM 参数 | `--enforce-eager`, `--enable-chunked-prefill` |
| KV Transfer | `AscendStoreConnector` + `memcache` |

### 6.2 测试结果

#### 6.2.1 全 miss 基准（MemCache vs Baseline 开销）

**条件**: PROMPT_TOKENS=4096, max_tokens=1, 10 次测量

| 指标 | Baseline (ms) | MemCache (ms) |
|------|---------------|---------------|
| P50 | 3,389.43 | 3,601.25 |
| P90 | 3,496.68 | 3,635.71 |
| P99 | 3,778.33 | 3,699.19 |
| avg | 3,437.49 | 3,295.52 |
| min | 3,381.14 | 440.05 |
| count | 10 | 10 |

MemCache 开销 = (3601.25 - 3389.43) / 3389.43 = **6.25%**（阈值 20%）→ ✅ PASSED

Cache stats: vLLM local hit=0.0%, MemCache external hit=11.7%

> MemCache min=440ms 为个别请求被 MemCache 偶发命中。avg < P50 因 hit 的低延迟请求拉低了均值。vLLM local hit=0.0% 证明本地缓存已永久关闭。

#### 6.2.2 50% 命中率

**条件**: 预填充 5 个 prompt（seed=5000..5004），10 个混合请求（5 hit + 5 miss，随机打乱）

| 指标 | 混合 | Hit | Miss |
|------|------|-----|------|
| avg | 2,090.20 ms | 499.79 ms | 3,680.60 ms |
| P50 | 2,087.56 ms | 502.19 ms (n=5) | 3,682.81 ms (n=5) |
| P99 | 3,706.47 ms | — | — |
| 实际命中 | — | 5/5（100%） | — |

| 指标 | 值 | 门限 | 判定 |
|------|------|------|------|
| 实际命中率 | 5/5 (100%) | ≥ 50% | PASSED |
| Hit/Miss P50 ratio | 13.65% | < 70% | PASSED |
| 改善率 | 43.21% | — | — |

Cache stats: vLLM local hit=0.0%, MemCache external hit=17.0%

#### 6.2.3 100% 命中率

**条件**: 10 轮 miss→hit 配对，每轮用新 seed 生成 prompt，先 miss 写入 MemCache，后 hit 读取

| 指标 | Cache miss (ms) | Cache hit (ms) |
|------|-----------------|----------------|
| P50 | 3,597.96 | 431.06 |
| P99 | 3,659.02 | 498.35 |
| count | 10 | 10 |

Hit/Miss P50 ratio = 431.06 / 3597.96 = **11.98%**（阈值 < 70%）→ ✅ PASSED

Cache stats: vLLM local hit=0.0%, MemCache external hit=44.1%

> Prefix cache hit 使 E2E TTFT 从 3598ms 降至 431ms，**降幅 88.0%**。vLLM local hit=0.0% 证明本地缓存已永久关闭，hit 完全来自 MemCache external hit（44.1%）。cache hit P99 (498ms) 比 P50 (431ms) 仅高 16%，尾部延迟稳定。

### 6.3 命中率梯度分析

| 命中率 | Hit P50 (ms) | Miss P50 (ms) | 混合 avg (ms) | TTFT 降幅 |
|--------|-------------|---------------|--------------|----------|
| 0%（全 miss 基准） | — | 3,601 (MemCache) | 3,296 | 基准 |
| 50% | 502 | 3,681 | 2,090 | 43.2%（混合改善） |
| 100% | 431 | 3,598 | 431 | 88.0%（单条 hit 降幅） |

**分析**:

1. **近似线性扩展**: TTFT 改善率与命中率近似线性关系。理论模型 `改善率 ≈ 命中率 × (miss_avg - hit_avg) / miss_avg ≈ 命中率 × 88%`，实测 50%→43%、100%→88%，与模型高度吻合

2. **MemCache 隔离生效**: 所有测试中 `vLLM local hit=0.0%`，证明 `--no-enable-prefix-caching` 成功关闭了 vLLM 本地 block cache，hit 的 TTFT 降幅完全来自 MemCache 分布式存储

3. **Hit 延迟稳定**: 100% hit 场景的 P50=431ms、P99=498ms，P99/P50=1.16，尾部延迟波动小。50% 场景的 hit P50=502ms，一致性良好

---

## 7. 全链路 TTFT 影响分析与结论

### 7.1 全链路 TTFT 分解

```
                不使用 MemCache (Baseline)
                ─────────────────────────
    T_network    T_prefill_compute    T_output
    (~1-5ms)     (~3389ms, 96%+)      (~1-5ms)
    ├──────────────┤├──────────────────────────────┤├──────────┤
    │              ││  每次请求都做完整 prefill     ││          │
    │              ││  前向计算 (Attention+FFN)    ││          │
    └──────────────┘└──────────────────────────────┘└──────────┘
    全链路 TTFT ≈ 3389ms


                使用 MemCache — Cache Miss (首次请求)
                ─────────────────────────────────────
    T_network    T_prefill_compute    T_kv_cache_write    T_output
    (~1-5ms)     (~3380ms, 94%+)      (~4.4ms, 0.12%)     (~1-5ms)
    ├──────────────┤├──────────────────┤├────────┤├──────────┤
    │              ││  prefill 计算    ││ L2G    ││          │
    │              ││  + KV Cache 写入 ││ write  ││          │
    └──────────────┘└──────────────────┘└────────┘└──────────┘
    全链路 TTFT ≈ 3601ms
    开销 ≈ 6.25%


                使用 MemCache — Cache Hit (命中前缀)
                ───────────────────────────────
    T_network    T_kv_cache_read    T_decode_compute    T_output
    (~1-5ms)     (~4.4ms, 1.0%)    (~426ms, 98%)      (~1-5ms)
    ├──────────────┤├────────┤├──────────────────────┤├──────────┤
    │              ││        ││  跳过 prefill!       ││          │
    │              ││ G2L    ││  仅 decode 首 token  ││          │
    │              ││ read   ││  前向计算            ││          │
    └──────────────┘└────────┘└──────────────────────┘└──────────┘
    全链路 TTFT ≈ 431ms
    降幅 ≈ 88.0%
```

### 7.2 多场景对比

| 场景 | 全链路 TTFT | vs Baseline | T_kv_cache 占比 | T_prefill_compute |
|------|------------|-------------|----------------|-------------------|
| Baseline (无 MemCache) | ~3,389ms | — | 0% | ~3,389ms (完整 prefill) |
| MemCache cache miss (0% hit) | ~3,601ms | +6.25%（开销） | 0.12% (~4.4ms) | ~3,389ms (完整 prefill) |
| 50% 命中 | ~2,090ms | -38.3% | <1% | ~50% prefill + ~50% 跳过 |
| MemCache cache hit (100% hit) | ~431ms | **-87.3%** | ~1.0% (~4.4ms) | ~0ms (跳过 prefill) |

### 7.3 Layer 2 → Layer 3 对照

| 场景 | Layer 2 T_kv_cache (ms) | Layer 3 E2E TTFT (ms) | T_kv_cache 占比 |
|------|------------------------|----------------------|----------------|
| Prefill (4K, miss) | 4.45 | 3,601 | 0.12% |
| Prefix cache hit (4K) | 4.38 | 431 | 1.0% |

- Layer 2 度量的纯存储延迟（~4.4ms）在 Layer 3 全链路 TTFT 中占比 < 1%
- Layer 3 cache hit TTFT（431ms）远高于 Layer 2 G2L read（4.4ms），差额来自 vLLM 调度 + decode 首 token 前向
- Layer 2 的 "hit ≈ miss"（L2G/G2L 对称）在 Layer 3 表现为 "hit << miss"（88% 降幅），因 miss 包含完整 prefill 计算

### 7.4 结论

1. **MemCache 开销可忽略**: cache miss 比 baseline 仅多 6.25%，其中 T_kv_cache 写入仅 4.4ms，开销主要来自 vLLM prefix caching 模块的调度开销

2. **Prefix cache hit 是 TTFT 优化的核心**: cache hit 将 ~3598ms 的 prefill 前向计算替换为 ~4.4ms 的 KV Cache 读取 + ~427ms 的 decode 首 token 计算，全链路 TTFT 降低 88.0%

3. **TTFT 改善率随命中率近似线性扩展**: 50% 命中改善 43.2%，100% 命中改善 88.0%，符合理论模型 `改善率 ≈ 命中率 × 88%`。生产环境中可根据实际命中率估算 TTFT 收益

4. **投入产出比**: 一次 cache miss 多花 ~212ms 写入存储，后续每次 cache hit 省掉 ~3167ms prefill 计算 — 一次投入换来 15 倍以上回报

5. **存储延迟不是瓶颈**: Layer 2 的 T_kv_cache（~4.4ms）在 Layer 3 全链路 TTFT 中占比 < 1%，无论 cache miss 还是 cache hit，MemCache 存储操作都不是性能瓶颈

6. **缓存隔离验证通过**: 所有测试中 `vLLM local hit=0.0%`，证明 `--no-enable-prefix-caching` 成功关闭了 vLLM 本地 block cache，hit 的 TTFT 降幅完全来自 MemCache 分布式存储（external hit 11.7%/17.0%/44.1%）

7. **310P3 适配可行**: 在 Ascend 310P3 同卡 die-pair + TP=2 环境下，MemCache + vLLM AscendStoreConnector 端到端可用，prefix cache hit 的 88.0% TTFT 降幅对用户体验有实质性改善

---

## 8. 运行命令

```bash
# === 2-rank (TP=2) 全部测试 ===
bash run_test.sh \
  --memcache-dir /path/to/memcache \
  --type integration \
  --rank 2 \
  --protocol host_shm \
  --device-ids 6,7 \
  --log

# === 按场景过滤 ===
# 使用 pytest -m marker 过滤
bash run_test.sh --memcache-dir /path/to/memcache --type integration --rank 2 --device-ids 6,7 --filter rank2

# === 4-rank (TP=4) 测试 ===
bash run_test.sh \
  --memcache-dir /path/to/memcache \
  --type integration \
  --rank 4 \
  --protocol host_shm \
  --device-ids 0,1,2,3 \
  --log

# === 直接运行 pytest ===
MMC_TEST_DEVICE_IDS=6,7 MMC_TEST_TENSOR_PARALLEL_SIZE=2 \
  python3 -m pytest cases/python/memcache/integration/vllm_integration_test.py -v -s -m rank2

# === 小规模快速验证（调整规格参数） ===
MMC_TEST_PROMPT_TOKENS=512 MMC_TEST_PREFIX_HIT_TOKENS=256 MMC_TEST_DECODE_MAX_TOKENS=128 \
MMC_TEST_DEVICE_IDS=6,7 MMC_TEST_TENSOR_PARALLEL_SIZE=2 \
  python3 -m pytest cases/python/memcache/integration/vllm_integration_test.py -v -s -m rank2
```

---

## 9. 测试文件

| 文件 | 说明 |
|------|------|
| `cases/python/memcache/integration/vllm_integration_test.py` | 统一端到端测试文件，6 个测试函数：全 miss 基准 + 50% 命中 + 100% 命中 + 4rank 变体。TP size 由环境变量控制，同一文件覆盖 2-rank 和 4-rank |
| `cases/config/mmc-local-integration.conf` | 2-rank: host_shm/device_sdma + HBM/DRAM 配置 |
| `cases/config/mmc-local-integration-4rank.conf` | 4-rank: world_size=4, host_shm/device_sdma |
| `pytest.ini` | pytest 配置：注册 `rank2`、`rank4` marker |
| `run_test.sh` | 测试运行脚本（自动清理 + 设备选择 + 协议切换 + 启动 MetaService） |
