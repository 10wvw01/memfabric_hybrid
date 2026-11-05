# MemFabric + Mooncake MF Transport 测试套件

一键拉取源码、打补丁、编译、运行 10 个 PD 传输测试场景 + 4 个 vLLM 集成测试场景 + 3 个 TTFT 基准测试场景，验证 Mooncake `protocol="mf"` 底层调用 MemFabric SDMA 能力。

## 目录结构

```
mooncake_test/
├── patches/mooncake/
│   └── tag_v0.3.13.patch          # Mooncake MF transport 补丁 (1909行)
├── scripts/
│   ├── run.sh                      # 一键入口脚本
│   └── patch_vllm_store_worker.py  # vLLM store/worker.py 按组段分配+重试补丁
└── tests/
    ├── demo_pd_transfer.py         # 10 个 PD 传输测试场景 (565行)
    ├── demo_vllm_pd.py             # 4 个 vLLM 集成测试场景
    └── demo_vllm_ttft.py           # 2 个 TTFT 基准测试场景
```

## 快速开始

```bash
# 全流程：拉取源码 → 编译 MemFabric → 打补丁 → 编译 Mooncake → 运行 10+4+3 个测试
bash scripts/run.sh /tmp/test_workdir
```

## 命令参数

```bash
bash scripts/run.sh <工作目录> [选项]
```

| 参数 | 说明 |
|------|------|
| `<工作目录>` | **必选**。所有源码、编译产物的根目录 |
| `--skip_build_mf` | 跳过 MemFabric 编译，复用已有产物 |
| `--skip_build_mooncake` | 跳过 Mooncake 编译 |
| `--skip_test` | 跳过 PD 传输测试 |
| `--skip_vllm_test` | 跳过 vLLM 集成测试 |
| `--skip_ttft_test` | 跳过 TTFT 基准测试 |
| `--only_test` | 只运行 PD 传输测试（跳过所有编译步骤） |
| `--only_vllm_test` | 只运行 vLLM 集成测试（跳过所有编译步骤） |
| `--only_ttft_test` | 只运行 TTFT 基准测试（跳过所有编译步骤） |
| `--single <test_id>` | 只运行指定 PD 传输测试（如 `single_write`） |
| `--vllm_model <path>` | vLLM 测试模型路径（默认 `/home/models/Qwen3.5-2B-W8A8-310P`） |
| `--vllm_device <id>` | vLLM 测试 NPU 设备 ID（默认自动选择空闲设备） |
| `--vllm_port <port>` | vLLM 测试端口（默认 8010） |
| `--mooncake_repo <url>` | 指定 Mooncake 仓库地址 |
| `--mooncake_branch <b>` | 指定 Mooncake 分支 |
| `--mf_repo <url>` | 指定 MemFabric 仓库地址 |
| `--mf_branch <b>` | 指定 MemFabric 分支 |
| `--help` | 显示帮助 |

## 使用示例

```bash
# 全流程（PD 传输测试 + vLLM 集成测试）
bash scripts/run.sh /tmp/test_workdir

# 跳过 MemFabric 编译（复用 /home/l30023043/memfabric_hybrid/output 产物）
bash scripts/run.sh /tmp/test_workdir --skip_build_mf

# 只跑单个 PD 传输测试
bash scripts/run.sh /tmp/test_workdir --skip_build_mf --single single_write

# 只跑 PD 传输测试（需已编译完成）
bash scripts/run.sh /tmp/test_workdir --only_test

# 只跑 vLLM 集成测试（需已编译完成）
bash scripts/run.sh /tmp/test_workdir --only_vllm_test

# 只编译不测试
bash scripts/run.sh /tmp/test_workdir --skip_test --skip_vllm_test

# 跳过 vLLM 集成测试（只跑 PD 传输测试）
bash scripts/run.sh /tmp/test_workdir --skip_build_mf --skip_vllm_test

# 只跑 TTFT 基准测试（需已编译完成）
bash scripts/run.sh /tmp/test_workdir --only_ttft_test

# 指定 vLLM 测试模型和设备
bash scripts/run.sh /tmp/test_workdir --only_vllm_test \
    --vllm_model /home/models/Qwen3.5-2B-W8A8-310P --vllm_device 0
```

## 流程

| 步骤 | 动作 | 说明 |
|------|------|------|
| 1 | 拉取源码 | `git clone` mooncake、memfabric_hybrid、yalantinglibs |
| 2 | 编译 MemFabric | 调用 `build_and_pack_run.sh` (RELEASE, NPU, SDMA) |
| 3 | 打补丁 | `git apply tag_v0.3.13.patch` + 修正 yalantinglibs 路径 |
| 4 | 编译 Mooncake | CMake `USE_MF=ON` + `make -j`，组装 Python 包 |
| 5 | 运行 PD 传输测试 | 10 个双进程 PD 传输场景 |
| 6 | 运行 vLLM 集成测试 | 4 个 vLLM + MooncakeConnector/MooncakeStoreConnector 场景 |
| 7 | 运行 TTFT 基准测试 | 3 个 streaming TTFT 量化度量场景（baseline / store_hit / offload） |

## 工作目录结构

脚本执行后，工作目录内容：

```
<工作目录>/
├── memfabric_hybrid/     # MemFabric 源码 (git clone)
│   └── output/           # MemFabric 编译产物 (libmf_smem.so 等)
├── mooncake/             # Mooncake 源码 (git clone + 打补丁)
│   └── build/            # Mooncake 编译产物 (engine.so, store.so)
├── ylt_src/              # yalantinglibs 源码 (git clone)
└── mooncake_pkg/         # Python 包目录 (mooncake/engine.so, store.so)
```

## 10 个 PD 传输测试场景

| # | 测试 ID | 覆盖功能 |
|---|---------|---------|
| 1 | `single_write` | `transfer_sync_write` 单缓冲区写 (4KB) |
| 2 | `batch_write` | `batch_transfer_sync_write` 多缓冲区写 (3×4KB) |
| 3 | `single_read` | `transfer_sync_read` 单缓冲区读（反向传输） |
| 4 | `batch_read` | `batch_transfer_sync_read` 多缓冲区读 |
| 5 | `async_write` | `batch_transfer_async_write` + `transfer_check_status` 轮询 |
| 6 | `async_read` | `batch_transfer_async_read` + 状态轮询 |
| 7 | `large_buffer` | 大缓冲区写 (1MB) |
| 8 | `multi_segment` | 多段注册 + `batch_transfer_sync_write` 写入 3 个独立段 |
| 9 | `probe` | `send_probe` 连通性检查 + 后续传输 |
| 10 | `register_unregister` | `register_memory`/`unregister_memory`/`batch_register_memory` 生命周期 |

每个测试启动两个进程：
- **Sender** (Prefill, rank 0, `MF_SMEM_TRANS_ROLE=Prefill`)
- **Receiver** (Decode, rank 1, `MF_SMEM_TRANS_ROLE=Decode`)

通过文件协调（`/tmp/mf_pd/`），数据完整性用 `numpy.array_equal` 校验。

## 4 个 vLLM 集成测试场景

| # | 测试 ID | 覆盖功能 |
|---|---------|---------|
| 1 | `vllm_baseline` | vLLM serve 加载 W8A8 模型，无 KV transfer，验证推理功能 |
| 2 | `vllm_kvboth_mf` | vLLM serve + MooncakeConnector (`kv_both`, `protocol=mf`)，验证 TransferEngine 初始化 + KV cache 注册到 MemFabric + 推理 |
| 3 | `vllm_pd_disagg_mf` | 完整 PD 分离：prefiller (`kv_producer`) + decoder (`kv_consumer`) + proxy，验证端到端 KV transfer via MF |
| 4 | `vllm_kv_offload_mf` | KV offloading：MooncakeStoreConnector (`kv_both`, `protocol=mf`) + mooncake_master，验证 KV cache offload 到分布式存储 + 前缀缓存命中 |

每个测试：
- 启动一个 `vllm serve` 子进程
- 轮询 `/v1/models` 直到就绪（超时 360s）
- 发送 chat completion 请求验证推理
- 对 MF 测试，额外检查日志确认 MemFabric 初始化标记
- 自动清理 vLLM 进程和 EngineCore 子进程

`vllm_pd_disagg_mf` 特殊说明：
- 310P3 不支持跨芯片 PeerAccess，prefiller 和 decoder 共用同一设备
- 使用 `gpu_memory_utilization=0.5` 对称分配设备内存
- 同时启动两个进程以确保 MemFabric topology preflight 成功
- 启动 proxy server (来自 vLLM examples) 协调 PD 请求路由

`vllm_kv_offload_mf` 特殊说明：
- 使用 MooncakeStoreConnector（不同于 MooncakeConnector 的 P2P 传输，Store 模式将 KV cache offload 到分布式存储池）
- 需要先启动 `mooncake_master` 进程管理元数据
- 通过 `MOONCAKE_CONFIG_PATH` 指定 JSON 配置文件（`protocol=mf`, `mode=embedded`）
- 预加载 `libub_allocator.so`（LD_PRELOAD）解决 store.so 的 undefined symbol
- 发送两次相同前缀的请求验证前缀缓存命中
- 需要修复 `client_service.cpp` 添加 `mf` 协议支持（run.sh 自动完成）

## 2 个 TTFT 基准测试场景

使用 vLLM streaming completions API（`POST /v1/completions` + `stream: true`）度量请求发送到首 token 的时间（TTFT）。规模刻意控制为小规模（2 warmup + 10 measured，768-token prompts），保证整套测试 ~15-20 分钟内完成。

测试设计参考 `/home/l30023043/ttft_test` 的 Layer 3 全链路 TTFT 测试，适配 Mooncake `protocol="mf"` 传输栈，使用 MooncakeStoreConnector + `mooncake_master`。

| # | 测试 ID | 度量目标 | 方法 |
|---|---------|---------|------|
| 1 | `ttft_baseline` | 基线 prefill TTFT | 无 KV transfer、无 prefix caching，10 次 unique prompt，统计 P50/P90/P99 |
| 2 | `ttft_store_hit` | 精确命中 vs 未命中 TTFT | MooncakeStoreConnector + prefix caching：Phase 1 miss（unique prompt → KV 写入 store）→ Phase 2 hit（reset local cache → 同一 prompt 从 store 读 KV），100% prefix 重叠 |
| 3 | `ttft_offload` | KV offloading 前缀扩展 TTFT | MooncakeStoreConnector + prefix caching：Phase 1 miss（768-token base prompt → KV offload 到 store）→ Phase 2 offload hit（reset local cache → base + 64-token suffix，base 前缀从 store 加载，仅 suffix 需要 prefill），模拟实际 KV offloading 场景 |

`ttft_store_hit` vs `ttft_offload` 区别：
- `ttft_store_hit`：**精确命中**（100% prefix 重叠），整个 prompt 的 KV 从 store 加载，无 prefill 计算。度量纯 store 读取延迟。
- `ttft_offload`：**前缀扩展**（部分重叠），长 base 前缀从 store 加载 + 短 suffix 需要 prefill。度量实际 KV offloading 场景的收益（共享长前缀 + 每请求短后缀）。

缓存隔离方法（三个场景一致）：
- `VLLM_SERVER_DEV_MODE=1` 启用 `POST /reset_prefix_cache` dev API
- hit/offload 请求前调 `reset_local_prefix_cache()` 清除 vLLM 本地 block cache（不清除 store）
- 对比 miss P50 vs hit P50，验证 Mooncake store prefix cache 命中降低 TTFT
- 显式验证：解析日志中 `Prefix cache hit rate`（应≈0%）和 `External prefix cache hit rate`（应>0%），fetch `/metrics` 检查 `external_prefix_cache_queries_total` / `hits_total`

环境变量（可选，控制规模）：

| 环境变量 | 说明 | 默认值 |
|---------|------|--------|
| `TTFT_PROMPT_TOKENS` | prompt token 长度（需 > 640 以填满 310P 一个 scheduler block） | 768 |
| `TTFT_OFFLOAD_SUFFIX` | offload 测试的 suffix token 长度 | 64 |
| `TTFT_WARMUP` | warmup 请求数 | 2 |
| `TTFT_MEASURED` | 度量请求数 | 10 |

## 预期输出

```
======================================================================
 RESULTS
======================================================================
  single_write                   PASS
  batch_write                    PASS
  single_read                    PASS
  batch_read                     PASS
  async_write                    PASS
  async_read                     PASS
  large_buffer                   PASS
  multi_segment                  PASS
  probe                          PASS
  register_unregister            PASS

  10/10 passed
======================================================================

======================================================================
 VLLM TEST: vllm_baseline
======================================================================
[VLLM-TEST] server is ready, sending chat request...
[VLLM-TEST] response: 我是一个由阿里巴巴通义实验室研发的超大规模语言模型...

  [vllm_baseline] PASS

======================================================================
 VLLM TEST: vllm_kvboth_mf
======================================================================
[VLLM-TEST] kv-transfer-config: {"kv_connector":"MooncakeConnector","kv_role":"kv_both","kv_connector_extra_config":{"mooncake_protocol":"mf"}}
[VLLM-TEST] server is ready, sending chat request...
[VLLM-TEST] response: 我是一个由阿里巴巴通义实验室研发的超大规模语言模型...
[VLLM-TEST] all MF init markers found in log
[VLLM-TEST] KV cache registered to MemFabric (AddVaInfo success)

  [vllm_kvboth_mf] PASS

======================================================================
 VLLM TEST: vllm_pd_disagg_mf
======================================================================
[VLLM-TEST] starting prefiller (kv_producer) and decoder (kv_consumer) simultaneously...
[VLLM-TEST] prefiller is ready
[VLLM-TEST] decoder is ready
[VLLM-TEST] sending chat request via proxy (with retries)...
[VLLM-TEST] proxy response: 您好！
[VLLM-TEST] [prefiller] all 5 MF markers found
[VLLM-TEST] [decoder] all 4 MF markers found

  [vllm_pd_disagg_mf] PASS

======================================================================
 VLLM TEST: vllm_kv_offload_mf
======================================================================
[VLLM-TEST] mooncake_master pid=..., port=50051
[VLLM-TEST] mooncake_master is ready
[VLLM-TEST] request 1 (store KV): prompt='请用一句话介绍人工智能的发展历史。'
[VLLM-TEST] response 1: 人工智能的发展历史是一部从早期基于符号逻辑的...
[VLLM-TEST] request 2 (prefix cache hit): prompt='请用一句话介绍人工智能的发展历史。另外...'
[VLLM-TEST] response 2: 人工智能的发展历史大致可分为三个关键阶段...
[VLLM-TEST] [kv_offload_mf] all 8 MF markers found
[VLLM-TEST] [kv_offload_mf] found store marker: prefix

  [vllm_kv_offload_mf] PASS

======================================================================
 VLLM RESULTS
======================================================================
  vllm_baseline                  PASS
  vllm_kvboth_mf                 PASS
  vllm_pd_disagg_mf              PASS
  vllm_kv_offload_mf             PASS

  4/4 passed
======================================================================

======================================================================
 TTFT TEST: ttft_baseline
======================================================================
[VLLM-TEST] starting vllm serve (baseline, device=0, port=8010)
[VLLM-TEST] server is ready, measuring baseline TTFT...
[VLLM-TEST]   warmup 1/2 done
[VLLM-TEST]   warmup 2/2 done
[VLLM-TEST]   measured 1/10: TTFT=3400.52ms
[VLLM-TEST]   ...
[VLLM-TEST]   [Baseline] P50=3450.20ms P90=3650.80ms P99=3700.00ms avg=3500.00ms (n=10)

  [ttft_baseline] PASS

======================================================================
 TTFT TEST: ttft_store_hit
======================================================================
[VLLM-TEST] mooncake_master pid=..., port=50051
[VLLM-TEST] mooncake_master is ready
[VLLM-TEST] starting vllm serve (store, device=0, port=8010)
[VLLM-TEST] ===== Phase 1: cache miss TTFT (unique prompts) =====
[VLLM-TEST]   miss 1/10: TTFT=3600.00ms
[VLLM-TEST]   ...
[VLLM-TEST] waiting 10s for async KV store writes to complete...
[VLLM-TEST] ===== Phase 2: cache hit TTFT (reset local cache, hit Mooncake store) =====
[VLLM-TEST]   hit  1/10: TTFT=1200.00ms (miss=3600.00ms)
[VLLM-TEST]   ...

===== MooncakeStore TTFT: Miss vs Hit =====
  Cache miss  P50=3650.00ms P90=3800.00ms P99=3900.00ms avg=3700.00ms (n=10)
  Cache hit   P50=1150.00ms P90=1300.00ms P99=1400.00ms avg=1200.00ms (n=10)
  Hit/Miss P50 ratio=31.51%
  TTFT reduction (avg)=67.6%

  [ttft_store_hit] PASS

======================================================================
 TTFT TEST: ttft_offload
======================================================================
[VLLM-TEST] offload: model=..., base_tokens=768, suffix_tokens=64
[VLLM-TEST] starting vllm serve (offload, device=0, port=8010)
[VLLM-TEST] ===== Phase 1: cache miss TTFT (base=768 tokens) =====
[VLLM-TEST]   miss 1/10: TTFT=820.00ms
[VLLM-TEST]   ...
[VLLM-TEST] waiting 10s for async KV store writes to complete...
[VLLM-TEST] ===== Phase 2: offload hit TTFT (base=768 from store + suffix=64 prefill) =====
[VLLM-TEST]   offload 1/10: TTFT=210.00ms (miss=820.00ms)
[VLLM-TEST]   ...

===== KV Offloading TTFT: Full Prefill vs Prefix Extension =====
  Full prefill  (768 tokens)    P50=815.00ms P90=900.00ms P99=920.00ms avg=810.00ms (n=10)
  Offload hit   (768+64 tokens) P50=205.00ms P90=230.00ms P99=260.00ms avg=210.00ms (n=10)
  Hit/Miss P50 ratio=25.15%
  TTFT reduction (avg)=74.1%

===== Cache Isolation Verification =====
  vLLM local prefix cache hit rate: 0.0%
  Mooncake external prefix cache hit rate: 35.2%
  /metrics external_prefix_cache_queries: 16896.0
  /metrics external_prefix_cache_hits: 6400.0
  load_get retries in log: 1
  save_put_count (Phase 1 writes): 12
[VLLM-TEST] CONFIRMED: vLLM local cache cleared, Mooncake store active.
[VLLM-TEST] PASS: MF infra OK + store save_put=12 + offload TTFT 25% of full prefill (reduction=74.1%)

  [ttft_offload] PASS

======================================================================
 TTFT RESULTS
======================================================================
  ttft_baseline                   PASS
  ttft_store_hit                  PASS
  ttft_offload                    PASS

  3/3 passed
======================================================================
```

## 环境要求

- CANN/Ascend NPU 驱动已安装，`ASCEND_HOME_PATH` 已设置
- CMake >= 3.20, GCC >= 11.4, Python >= 3.8, pybind11 >= 2.10
- NPU 设备 0 可用（`acl.rt.set_device(0)` 能成功）
- 端口 8570 未被占用（MemFabric config store）
- 端口 50051 未被占用（MooncakeStoreConnector 的 mooncake_master，仅 `vllm_kv_offload_mf`）
- vLLM >= 0.26.0 + vllm-ascend（vLLM 集成测试）
- W8A8 量化模型（默认 `/home/models/Qwen3.5-2B-W8A8-310P`，310P 需要 `--dtype float16`）

## 底层调用链确认

PD 传输测试运行时日志确认底层调用 MemFabric 而非 TCP fallback：

```
load memfabric api success                              # dlopen("libmf_smem.so") + 28 个 dlsym
hybm init successfully, library version: 1.2.0          # HYBM 核心库初始化
smem trans initialized success                          # SMEM TRANS 子系统初始化
bmDataOpType: 2                                         # device_sdma (NPU SDMA 硬件搬运)
role: SENDER / role: RECEIVER                           # PD 角色区分
topology preflight PASS: rank:0 worldSize:2             # MemFabric 拓扑预检通过
IpcOpenMemory(...) success                              # 跨进程 NPU 设备内存 IPC 导入
AddVaInfoFromExternal: gva:0x1000002000000000           # 全局虚拟地址映射到对端 rank
```

vLLM 集成测试运行时日志确认 MooncakeConnector 使用 MF 协议：

```
MemFabricTransport: add segment type:1 deviceId:0 segment name:127.0.0.1:16101 protocol:mf
Mooncake Bootstrap Server started at 0.0.0.0:8998
Registering KV_Caches. use_mla: False
AddVaInfo success: AllocatedGvaInfo{gva: 0x1000000000000000, size: 0x70fa00000, ...}
```
