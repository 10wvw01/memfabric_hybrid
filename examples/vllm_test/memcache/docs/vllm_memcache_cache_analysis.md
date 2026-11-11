# vLLM + MemCache 缓存协作机制分析（源码核实版）

测试用例、TTFT/P50 和常用术语见[测试说明](../../TEST_GUIDE.md)。

> 本文保留历史环境的源码笔记，原记录未固定全部提交号。下文旧路径及行号对应原环境；当前 vLLM 0.24 的测试方式见 3.8、4.7—4.9 和迁移报告。

> 原分析基于 vLLM-Ascend 源码（`/vllm-workspace/vllm-ascend`）、vLLM v1 源码（`/vllm-workspace/vllm/vllm`）、MemCache 源码（`/home/l30023043/memcache/src/memcache/csrc`）核实整理，所有结论附 `file:line` 引用。

---

## 目录

1. [进程模型](#1-进程模型)
2. [两套缓存的角色与配合](#2-两套缓存的角色与配合)
3. [vLLM 自身缓存与 MemCache 的协作更新](#3-vllm-自身缓存与-memcache-的协作更新)
4. [vLLM 自身缓存管理](#4-vllm-自身缓存管理)
5. [MemCache 分配与多层淘汰](#5-memcache-分配与多层淘汰)
6. [buffer 注册：vLLM GPU 地址桥接](#6-buffer-注册vllm-gpu-地址桥接)
7. [TP 模型下的隔离性](#7-tp-模型下的隔离性)
8. [修正记录](#8-修正记录)

---

## 1. 进程模型

以单节点、DP=1 为例：API 服务接收请求，EngineCore 负责调度与缓存索引。
本轮 TP=1 在 EngineCore 内执行模型，TP=2 另启动两个 worker 执行模型计算。

### 1.1 各进程的职责

| 进程 | 类 | 职责 | 持有缓存 |
|----|----|----|---------|
| API 前端 | `AsyncLLM` / `SyncMPClient`（`core_client.py:467`） | 接收 HTTP 请求，ZMQ 转发给 EngineCore | 无 |
| EngineCore | `EngineCoreProc`（`core.py:995`） | 单进程，内含 Scheduler + BlockPool + KVCacheManager | **唯一一份 BlockPool** |
| TP worker | `VLLM::Worker_TPrank`（`worker.py`） | 每进程绑定一个 NPU 逻辑设备，跑前向 | 仅 KV tensor + block_table |

### 1.2 多 worker 配置的进程启动关系

```
API 前端进程
  └─ launch_core_engines (utils.py:1072)
       └─ CoreEngineProcManager (utils.py:130)
            └─ context.Process(target=EngineCoreProc.run_engine_core, ...)  (utils.py:164-170)
                 │  EngineCore 进程（唯一）
                 │  └─ Scheduler (core.py:153)
                 │       └─ KVCacheManager (scheduler.py:269)
                 │            └─ KVCacheCoordinator (kv_cache_coordinator.py:90)
                 │                 └─ BlockPool（唯一一份 cached_block_hash_to_block 字典）
                 │
                 └─ Executor spawn N 个 worker 子进程
                      └─ NPUWorker (worker.py)
                           └─ torch.npu.set_device(local_rank)  (worker.py:434)
                           └─ kv_caches: dict[str, Tensor]  （物理 KV tensor，无 BlockPool）
```

### 1.3 调度与缓存索引属于 EngineCore 实例

`EngineCoreProc` 由 `context.Process` spawn（`utils.py:164`），同一 EngineCore 实例中有一份 `Scheduler`（`core.py:153`）→ 一份 `KVCacheManager`（`scheduler.py:269`）→ 一份 `BlockPool`（`kv_cache_coordinator.py:90`）。

**关键**：worker 进程**不持有** BlockPool / `cached_block_hash_to_block` 字典（grep `/vllm-workspace/vllm/vllm/v1/worker/` 确认无任何 `BlockPool` 引用）。worker 只按 scheduler 下发的 `block_ids` 列表操作本地 KV tensor。

### 1.4 TP worker 与设备的对应关系

TP=2 的配置使用两个 worker，各自执行对应分片。`NPUWorker._init_device`（`worker.py:385-434`）：每个 worker 进程按 `local_rank` 绑定**单个** NPU：

```python
# worker.py:431-434
visible_device_index = current_platform.logical_device_id_to_visible_device_id(self.local_rank)
device = torch.device(f"npu:{visible_device_index}")
torch.npu.set_device(device)
```

跨卡数据访问通过 MetaService 协调 + memfabric_hybrid P2P/SDMA/DRAM 共享，对端卡数据通过地址映射访问，不需在本进程跑对端卡的计算。

---

## 2. 两套缓存的角色与配合

### 2.1 两套缓存独立存在，无共享状态

| 维度 | vLLM 本地 prefix cache | MemCache (AscendStoreConnector) |
|------|------------------------|--------------------------------|
| 进程位置 | EngineCore 进程内（`block_pool.py:184`） | 每模型执行 worker 一份（`memcache_backend.py:74`） |
| 存储介质 | NPU HBM 内的 KV block 池（`gpu_memory_utilization` 预留） | 独立 HBM pool + DRAM pool（memfabric_hybrid） |
| 索引 | `Dict[block_hash, block_id]`（`block_pool.py:184`） | 跨进程分布式 key→blob（MetaService LRU） |
| 读写时机 | 同步（prefill 完成立即更新字典） | 异步（后台线程 `KVCacheStoreSendingThread`） |
| 生命周期 | 进程级，进程退出即丢 | 由 worker 存储池与 MetaService 元数据共同管理 |
| TP 共享 | **全局唯一字典**，所有 rank 共用 | **按 rank 隔离**，每 worker 独立 store |

### 2.2 查询（Lookup 阶段）：vLLM 本地优先，本地未命中才查 connector

> **重要**：vLLM 对 connector 有两个独立阶段——**Lookup（读决策）**和 **Save（写更新）**。本节只描述 Lookup 阶段。即使 Lookup 被跳过，Save 阶段仍会独立运行并更新 MemCache（详见第 3 章）。

vLLM scheduler 在请求调度阶段调 `AscendStoreConnector.get_num_new_matched_tokens`（`ascend_store_connector.py:145`）→ `KVPoolScheduler.get_num_new_matched_tokens`（`pool_scheduler.py:521`），入参 `num_computed_tokens` 即 vLLM 本地已命中 token 数：

```python
# pool_scheduler.py:566
if num_computed_tokens >= token_len:
    return 0, False          # 本地全命中 → 跳过 connector Lookup
```

只有本地不足时，才走 `LookupKeyClient.lookup`（`pool_scheduler.py:570`，zmq REQ→`LookupKeyServer`，`ascend_store_connector.py:308`）到 worker 侧 `KVPoolWorker.lookup_scheduler`（`pool_worker.py:2358`），最终调 `self.m_store.exists(multi_tp_keys)`（`pool_worker.py:2393`）→ `MemcacheBackend.exists`（`memcache_backend.py:174`）→ `store.batch_is_exist(keys)`。

`lookup_scheduler` 会 `_expand_lookup_keys_by_rank`（`pool_worker.py:2244`）把单 rank 的 key 扩展为所有 TP rank 的 key 一起查，**任一 rank 缺失即判该 block 未命中**（`pool_worker.py:2419` 连续命中扫描）。

### 2.3 三种命中状态

vLLM engine metrics 日志每周期输出两条独立指标：

| 状态 | 判定 | 日志 |
|------|------|------|
| vLLM 本地命中 | `local_computed >= token_len`，connector 被跳过 | `Prefix cache hit rate: XX%` |
| MemCache 命中 | `local_computed = 0`，`external_hit > 0` | `External prefix cache hit rate: XX%` |
| 全 miss | `local_computed = 0`，`external_hit = 0` | — |

---

## 3. vLLM 自身缓存与 MemCache 的协作更新

本节回答核心问题：**当一侧缺失、另一侧命中时，缺失侧是否会被更新？**

### 3.0 总体决策流程

```
请求到达 scheduler
  │
  ▼
┌─────────────────────────────────────────────┐
│           Lookup 阶段（prefill 前）           │
│           scheduler.py:753-790               │
└─────────────────────────────────────────────┘
  │
  ├─ 查 vLLM 本地字典 (block_pool.py:198 get_cached_block)
  │
  ├─ 本地全命中？
  │   ├─ YES → 跳过 connector Lookup (pool_scheduler.py:566)
  │   │         num_external_computed_tokens = 0
  │   │
  │   └─ NO  → connector 查 MemCache (pool_scheduler.py:570)
  │            ├─ MemCache hit  → ext_tokens > 0, 从 MemCache 加载 KV
  │            └─ MemCache miss → ext_tokens = 0, 完整 prefill
  │
  ▼
┌─────────────────────────────────────────────┐
│              prefill 计算                     │
│  本地命中的 block 直接复用，缺失部分重新计算    │
└─────────────────────────────────────────────┘
  │
  ├─ vLLM 本地同步更新字典 (block_pool.py:293)
  │   新计算的 block 和从 MemCache 加载的 block 都写入字典
  │
  ▼
┌─────────────────────────────────────────────┐
│           Save 阶段（prefill 后）             │
│           独立于 Lookup，只要                 │
│    num_tokens_to_save >= chunk_boundary      │
│    就触发 (config_data.py:1052)              │
└─────────────────────────────────────────────┘
  │
  ├─ 后台线程 KVCacheStoreSendingThread (kv_transfer.py:608)
  │
  ├─ 先查 MemCache 哪些 block 缺失 (kv_transfer.py:810)
  │
  ├─ 有缺失？
  │   ├─ YES → 只 put 缺失的 block → MemCache 被更新 (kv_transfer.py:891)
  │   └─ NO  → 跳过 put (kv_transfer.py:812)
  │
  ▼
  完成
```

> **核心**：Lookup 和 Save 是两个独立阶段。Lookup 被跳过（本地全命中）不代表 Save 被跳过。Save 有自己的触发条件，与 Lookup 结果无关。

### 3.1 两阶段模型（详解）：Lookup 与 Save 是独立的

vLLM 对 connector 的操作分为两个**完全独立**的阶段，这是理解协作更新的关键：

| 阶段 | 时机 | 作用 | 本地 hit 时是否运行 |
|------|------|------|-------------------|
| **Lookup**（读决策） | prefill 前 | 决定是否需要从 MemCache 读 KV、是否可以跳过部分 prefill | `num_computed_tokens >= token_len` 时跳过（`pool_scheduler.py:566`） |
| **Save**（写更新） | prefill 后 | 把本次计算的 block 写入 MemCache | **独立运行**，只要 `num_tokens_to_save >= chunk_boundary` 就触发（`config_data.py:1052-1053`） |

**关键**：Lookup 被跳过不等于 Save 被跳过。即使 vLLM 本地全命中导致 connector Lookup 被跳过，Save 阶段仍会运行——它检查 MemCache 中哪些 block 缺失，把缺失的写入 MemCache。

Save 阶段的跳过条件（`config_data.py:1052-1056`）与 Lookup 无关：

```python
# config_data.py:1052-1056
skip_save = skip_save or (
    num_tokens_to_save < chunk_boundary and partial_block_index is None and not should_save_partial_block
)
if skip_save and load_spec is None:
    return None   # 请求不进入 connector metadata → 不 save 也不 load
```

只有当「待保存 token 数不足一个 chunk」**且**「无需从 MemCache 加载」时，请求才完全跳过 connector。对于正常 prefill 请求（即使本地全命中，仍需重算最后一个 token 以获取 logits，`kv_cache_manager.py:237` `max_cache_hit_length = num_tokens - 1`），`num_tokens_to_save` 通常 >= `chunk_boundary`，Save 会被触发。

### 3.2 场景一：vLLM 本地 miss、MemCache hit → vLLM 本地会被更新

**完整流程：**

```
请求到达 scheduler
  │
  ├─ scheduler.py:753-762  get_computed_blocks → 查 vLLM 本地字典
  │    num_new_local_computed_tokens = 0（本地 miss）
  │
  ├─ scheduler.py:765-770  connector.get_num_new_matched_tokens
  │    → MemCache lookup → ext_tokens > 0（MemCache hit）
  │
  ├─ scheduler.py:780      num_external_computed_tokens = ext_tokens
  │
  ├─ scheduler.py:935-947  allocate_slots(
  │      num_external_computed_tokens=ext_tokens,
  │      delay_cache_blocks=load_kv_async)
  │
  │   ┌─ kv_cache_manager.py:468-479  allocate_new_computed_blocks
  │   │   → allocate_external_computed_blocks (single_type_kv_cache_manager.py:282)
  │   │   → 从 BlockPool 分配 NEW block（不查 block_hash 字典）
  │   │   → 这些 block 标记为 "not cached by vLLM, but cached by connector"
  │   │      （kv_cache_manager.py:345-348 图示）
  │   │
  │   ├─ 同步模式 (load_kv_async=False):
  │   │   kv_cache_manager.py:498-502  cache_blocks(request, num_tokens_to_cache)
  │   │   → num_tokens_to_cache 包含 ext_comp tokens
  │   │   → cache_full_blocks (block_pool.py:225)
  │   │   → _insert_block_hash (block_pool.py:607)
  │   │   → block_hash 插入 cached_block_hash_to_block 字典 ✅
  │   │
  │   └─ 异步模式 (load_kv_async=True):
  │       kv_cache_manager.py:490-491  delay_cache_blocks=True → 跳过 cache_blocks
  │       scheduler.py:982  request → WAITING_FOR_REMOTE_KVS
  │       ↓ 异步传输完成后
  │       scheduler.py:2543  _update_waiting_for_remote_kv
  │       scheduler.py:2577  kv_cache_manager.cache_blocks(request, num_computed_tokens)
  │       → _insert_block_hash → block_hash 插入字典 ✅
  │
  └─ worker 侧 start_load_kv → m_store.get → 从 MemCache G2L DMA 到 GPU block
```

**关键源码证据：**

`kv_cache_manager.py:498-502`（同步模式，cache 范围包含 ext_comp）：

```python
num_tokens_to_cache = min(
    total_computed_tokens + num_new_tokens,  # total_computed_tokens 含 ext_comp
    request.num_tokens,
)
self.coordinator.cache_blocks(request, num_tokens_to_cache)
```

`block_pool.py:293`（实际插入字典）：

```python
self._insert_block_hash(block_hash_with_group_id, blk, num_tokens=num_hash_tokens)
```

`block_pool.py:627`（插入操作）：

```python
self.cached_block_hash_to_block.insert(block_hash_with_group_id, block)
```

`scheduler.py:2577`（异步模式，传输完成后补 cache）：

```python
# Now that the blocks are ready, actually cache them.
self.kv_cache_manager.cache_blocks(request, request.num_computed_tokens)
```

**结论**：MemCache hit 后，vLLM 本地缓存**会被更新**。从 MemCache 加载的 KV 数据对应的 block_hash 会插入 vLLM 的 `cached_block_hash_to_block` 字典。下次同一 prompt 到来时，vLLM 本地直接命中，跳过 MemCache connector。

### 3.3 场景一b：vLLM 本地部分命中、MemCache 命中更多 → 补充加载差额

> 本地命中 N tokens，MemCache 命中 M tokens（M > N），从 MemCache 拉取 (M−N) tokens 补充到本地。

**触发条件**：`num_computed_tokens < token_len`（本地未全命中），所以 connector Lookup **不会被跳过**（`pool_scheduler.py:566` 的条件不满足）。

**完整流程：**

```
请求到达 scheduler
  │
  ├─【Lookup 阶段】
  │  ├─ scheduler.py:753-762  get_computed_blocks → 查 vLLM 本地字典
  │  │    num_new_local_computed_tokens = N（本地命中 N tokens，N < token_len）
  │  │
  │  ├─ scheduler.py:765-770  connector.get_num_new_matched_tokens(
  │  │      request, num_computed_tokens=N)
  │  │
  │  │    pool_scheduler.py:566  N < token_len → 不跳过，走 connector 查询
  │  │
  │  │    pool_scheduler.py:298  query_start_block = N // block_size
  │  │    → 从本地命中位置之后开始查 MemCache（跳过本地已有的 block）
  │  │
  │  │    pool_scheduler.py:328  num_hit_blocks = query_start_block + queried_hit_blocks
  │  │    → 返回 M = N + (MemCache 补充命中) tokens
  │  │
  │  │    pool_scheduler.py:592  need_to_allocate = M - N（需分配的新 block 数）
  │  │
  │  │    pool_scheduler.py:609-614  LoadSpec(
  │  │        vllm_cached_tokens=N,        # 本地已有
  │  │        kvpool_cached_tokens=M,      # 总命中（含本地）
  │  │        kvpool_store_skip_tokens=M)  # save 时跳过这些 block
  │  │
  │  ├─ scheduler.py:780  num_external_computed_tokens = M - N
  │  ├─ scheduler.py:788  num_computed_tokens = N + (M - N) = M
  │  │
  │  └─ scheduler.py:935-947  allocate_slots(
  │        num_external_computed_tokens = M - N)
  │      → kv_cache_manager.py:468-479  allocate_new_computed_blocks
  │      → 为 (M-N) tokens 分配新 block（不查 block_hash 字典）
  │
  ├─【加载阶段】worker 侧 start_load_kv (pool_worker.py:883)
  │  ├─ pool_worker.py:946  mask_num = vllm_cached_tokens = N
  │  │   → load_mask 跳过本地已有的 block [0..N)
  │  │
  │  ├─ pool_worker.py:949-950  chunk_filter 只选 [N..M) 范围的 block
  │  │
  │  └─ pool_worker.py:992  m_store.get(keys, addrs, sizes)
  │      → 从 MemCache G2L DMA 加载 [N..M) tokens 的 KV 到新分配的 block
  │
  ├─【vLLM 本地更新】
  │  ├─ 同步模式: cache_blocks(request, M + new_tokens)
  │  │   → block [0..M) 的 block_hash 全部插入字典（含本地原有 N + 新加载 M-N）
  │  │
  │  └─ 异步模式: WAITING_FOR_REMOTE_KVS → 传输完成后
  │      _update_waiting_for_remote_kv → cache_blocks → 字典更新
  │
  ├─【prefill】只需计算 (total - M) tokens（而非 total - N）
  │
  └─【Save 阶段】prefill 后
     ├─ pool_worker.py:1064-1071  save 跳过 [0..M) block（kvpool_store_skip_tokens=M）
     │   → 避免向 MemCache READABLE blob 重复写入
     └─ 只 save [M..total) 新计算的 block 到 MemCache
```

**关键源码证据：**

`pool_scheduler.py:298`（从本地命中位置之后查 MemCache）：

```python
query_start_block = 0 if self.use_layerwise else min(num_computed_tokens // self._block_size, num_blocks)
block_hashes_to_query = request.block_hashes[query_start_block:num_blocks]
```

`pool_scheduler.py:328`（返回本地+远端总命中）：

```python
num_hit_blocks = query_start_block + num_queried_hit_blocks
return num_hit_blocks * self._block_size
```

`pool_scheduler.py:589-592`（需要分配的 = 远端总命中 - 本地已有）：

```python
if num_external_hit_tokens < num_computed_tokens:
    need_to_allocate = 0
else:
    need_to_allocate = num_external_hit_tokens - num_computed_tokens
```

`pool_worker.py:946`（加载时跳过本地已有的 block）：

```python
mask_num = load_spec.vllm_cached_tokens // group_block_size * group_block_size
```

**结论**：本地部分命中时，connector Lookup **不会被跳过**，而是从本地命中位置之后继续查 MemCache。MemCache 命中更多时，差额部分被拉取到本地新分配的 block，vLLM 本地字典更新为总命中数 M。prefill 只需计算 (total − M) tokens，比不查 MemCache 少算 (M − N) tokens。

### 3.4 场景二：vLLM 本地 hit、MemCache miss → MemCache 会被更新

> 即使 Lookup 阶段被跳过（本地全命中），Save 阶段仍独立运行，检查并更新 MemCache。

**完整流程：**

```
请求到达 scheduler
  │
  ├─【Lookup 阶段】
  │  ├─ scheduler.py:753-762  get_computed_blocks → 查 vLLM 本地字典
  │  │    num_new_local_computed_tokens > 0（本地 hit）
  │  │
  │  ├─ scheduler.py:765-770  connector.get_num_new_matched_tokens
  │  │    → pool_scheduler.py:566  num_computed_tokens >= token_len → return 0, False
  │  │    → connector Lookup 被跳过 ✅
  │  │
  │  └─ prefill 使用本地命中的 block（不重新计算，仅重算最后 1 token 获取 logits）
  │
  └─【Save 阶段】（独立于 Lookup，prefill 后触发）
      │
      ├─ pool_scheduler.py:958  build_connector_meta
      │   → config_data.py:1077  from_request_tracker 生成 ReqMeta
      │   → config_data.py:1052  skip_save = (num_tokens_to_save < chunk_boundary)
      │   → num_tokens_to_save 通常 >= chunk_boundary → can_save = True
      │
      ├─ pool_worker.py:1766  wait_for_save → send_thread.add_request
      │
      └─ kv_transfer.py:718  _handle_stored_request
          │
          ├─ kv_transfer.py:810  exists_states = self.lookup(keys)
          │   → 查 MemCache 是否已有这些 block
          │
          ├─ kv_transfer.py:811  missing_indices = [i for i, e in enumerate(exists_states) if not e]
          │   → MemCache miss 的 keys
          │
          └─ kv_transfer.py:891  self.m_store.put(keys, addrs, sizes)
              → 只 put MemCache 中不存在的 keys → MemCache 被更新 ✅
```

**关键源码证据：**

`kv_transfer.py:810-816`（put 前先查 MemCache，只写缺失的）：

```python
exists_states = self.lookup(keys)
missing_indices = [i for i, e in enumerate(exists_states) if not e]
if not missing_indices:
    continue                       # MemCache 全有 → 跳过
# 只 put MemCache 中缺失的 keys
self.m_store.put(keys, addrs, sizes)
```

**结论**：vLLM 本地 hit 时，Lookup 阶段被跳过（不查 MemCache），但 Save 阶段仍独立运行——save 线程会查 MemCache 是否已有该 block，缺失的才写入。**Lookup 被跳过 ≠ Save 被跳过**，这是两阶段独立设计的核心。

### 3.5 场景三：双 miss（全 miss）→ 双写

```
请求到达 → vLLM 本地 miss → MemCache miss → 完整 prefill 计算
  │
  ├─【Lookup 阶段】本地 miss → connector 查询 → MemCache miss → 完整 prefill
  │
  └─【Save 阶段】prefill 完成后
      ├─ vLLM 本地同步写入 block_hash → block_id 字典
      │   (block_pool.py:293 _insert_block_hash)
      └─ 后台 save 线程异步写入 MemCache
          (kv_transfer.py:891 m_store.put)
```

两个缓存同时被写入，这就是「双写问题」。

### 3.6 场景四：双 hit → 无更新

```
【Lookup 阶段】vLLM 本地 hit → connector Lookup 被跳过（pool_scheduler.py:566）
  → prefill 直接复用本地 block

【Save 阶段】save 线程查 MemCache → 已存在 → 跳过 put (kv_transfer.py:812-813)
```

两侧都不更新，数据保持不变。

### 3.7 更新流程总结

| 场景 | vLLM 本地 | MemCache | Lookup 阶段 | Save 阶段 | 最终结果 |
|------|-----------|----------|------------|----------|---------|
| 双 miss | miss | miss | connector 查询 → miss → 完整 prefill | 写入两侧 | 双写 |
| 本地 miss、MemCache hit | miss(N=0) | hit(M) | connector 查询 → hit → 加载全部 M tokens | vLLM 本地写字典（`block_pool.py:293`） | vLLM 本地被更新 |
| 本地部分命中、MemCache 更多 | hit(N) | hit(M>N) | connector 查询 → hit → 加载差额 M−N tokens | vLLM 本地写字典至 M | vLLM 本地被补充更新 |
| 本地全命中、MemCache miss | hit(N=全部) | miss | **跳过**（`pool_scheduler.py:566`） | **仍运行**，查 MemCache → miss → 写入（`kv_transfer.py:891`） | MemCache 被更新 |
| 双 hit | hit | hit | **跳过** | 仍运行，查 MemCache → hit → 跳过 put | 无更新 |

**关键**：Lookup 阶段被跳过 ≠ Save 阶段被跳过。Save 阶段有独立的跳过条件（`config_data.py:1052`，`num_tokens_to_save < chunk_boundary`），与 Lookup 是否被跳过无关。

### 3.8 测试如何区分本地缓存与外部缓存

一次外部命中后，恢复的数据也可以被 vLLM 本地缓存。继续发送相同输入时，请求可能直接复用本地数据。
当前 vLLM 0.24 测试保留本地 prefix cache 和 Mamba `align` 模式；三项延迟用例在每次请求完成后，
调用 `POST /reset_prefix_cache?reset_external=false` 清理本地索引，保留外部数据。
测试检查 HTTP 响应及引擎新增的成功重置日志，重置在 TTFT 计时之外执行。

缓存来源正确性用例有意保留本地数据，分别构造本地、外部和混合命中，核对计数与生成 token。
这些步骤见[测试用例说明](../../TEST_GUIDE.md#memcache-tests)。早期使用关闭本地缓存的方案，
其配置和结果保留在历史性能报告中。

### 3.9 MemCache 侧的额外跳过逻辑

除了 `kv_transfer.py:810` 的 put 前去重，`pool_worker.py:1061-1071` 在 layerwise save 时还有一层跳过：

```python
# pool_worker.py:1061-1071
# Skip blocks that are hit in the KV pool — their KV is already
# in the pool (loaded via load_prepare), so re-saving would write
# to a READABLE blob and fail with MMC_UNMATCHED_KEY.
if request.load_spec is not None and request.load_spec.can_load:
    pool_hit_tokens = (
        request.load_spec.kvpool_store_skip_tokens
        if request.load_spec.kvpool_store_skip_tokens is not None
        else request.load_spec.kvpool_cached_tokens
    )
    hit_full_blocks = pool_hit_tokens // block_size
    save_start_block = max(save_start_block, hit_full_blocks)
```

即：如果某些 block 已从 MemCache 加载（`kvpool_cached_tokens > 0`），save 时跳过这些 block，避免向 READABLE blob 重复写入导致 `MMC_UNMATCHED_KEY` 错误。

---

## 4. vLLM 自身缓存管理

### 4.1 单一全局 BlockPool

`BlockPool.__init__`（`block_pool.py:162-196`）在 EngineCore 进程内创建**唯一实例**：

```python
# block_pool.py:184-185
self.cached_block_hash_to_block: BlockHashToBlockMap = BlockHashToBlockMap()
self.cached_block_hashes_by_block: dict[int, set[BlockHashWithGroupId]] = {}
# block_pool.py:175-177
self.blocks: list[KVCacheBlock] = [KVCacheBlock(idx) for idx in range(num_gpu_blocks)]
```

- `cached_block_hash_to_block`：`block_hash → KVCacheBlock` 全局唯一索引。
- `blocks`：block_id 是 `[0, num_gpu_blocks)` 全局编号空间，**不按 TP rank 划分**。

### 4.2 block_hash 与 rank 无关

`block_hash` 由 prompt token 计算（`kv_cache_utils.py`），所有 TP rank 拿到同样 prompt → 算出同样 `block_hash`。scheduler 字典里 `block_hash → block_id` 是全局共享索引。

TP 模型的隐含契约：同一个 `block_id` 在所有 rank 上同时存在、同时有效——每个 rank 持有该 block 的不同 head 分片，合起来才是完整 KV。所以 scheduler 只需记一份 `block_id`，不需要 per-rank 索引。

### 4.3 分配

- `gpu_memory_utilization` 预留显存 → `num_gpu_blocks` 固定数量 block，每 block `block_size` tokens。
- 请求到来按 `block_hash` 去重分配（`block_pool.py:198` `get_cached_block`）。
- `FreeKVCacheBlockQueue`（`block_pool.py:181`）维护空闲块双向链表，含 eviction 候选。

### 4.4 更新

prefill 每完成一个 block，`cache_full_blocks`（`block_pool.py:225`）同步写入 `cached_block_hash_to_block`（`block_pool.py:293` `_insert_block_hash`）。

### 4.5 释放与 delayed free

`request_finished`（`pool_scheduler.py:1085`）对有 save 任务的请求 `delay_free`（`pool_scheduler.py:1106`，加入 `_delayed_free_req_ids`），等 worker 报告 `done_sending` 后由 `update_connector_output`（`pool_scheduler.py:1061`）真正 `free_blocks`（`pool_scheduler.py:1081`）。

### 4.6 Eviction

BlockPool 自带 LRU eviction：
- block 引用计数为 0 时进入 `free_block_queue` 的 eviction 候选。
- 新请求分配时若空闲块不足，从 eviction 候选尾部淘汰（LRU）。
- 与 MemCache 不同，vLLM 本地缓存**无 watermark**，按需淘汰。

### 4.7 reset_prefix_cache 的当前用途

vLLM 0.24 的 `vllm/v1/core/block_pool.py` 中，`reset_prefix_cache` 清除缓存 hash 映射，
并重置 block 上的 hash。物理缓冲区仍由模型执行进程管理。
当前集成测试用这个接口让下一次请求重新查询外部缓存。

### 4.8 什么时候重置会失败

`BlockPool.reset_prefix_cache` 检查是否还有请求占用缓存块。除内部保留的空块外还有已用块时，
函数返回 `False`；成功时记录 `Successfully reset prefix cache`。
因此测试要等待请求结束，并确认引擎记录了本次重置成功。HTTP 200 本身无法说明底层检查的结果。

### 4.9 本地重置与外部数据的生命周期

当前测试明确传入 `reset_external=false`。只重置本地缓存后，MemCache 中的数据仍可被后续请求查询。
外部缓存由存储池及 MetaService 元数据共同管理；重启、清理池和条目淘汰应与本地索引重置分别理解。

---

## 5. MemCache 分配与多层淘汰

### 5.1 配置

配置文件（如 `mmc-local-integration.conf:30-33`）：

```
ock.mmc.local_service.protocol = device_sdma | host_shm
ock.mmc.local_service.hbm.size = 1GB      # 每 rank HBM pool
ock.mmc.local_service.dram.size = 1GB      # 每 rank DRAM pool（/dev/shm）
```

### 5.2 Put 路径

`mmcache_store.cpp:594` `BatchPutFromLayers`：

1. 按 `direct` 参数决定目标介质：`L2G→MEDIA_HBM`，`H2G→MEDIA_DRAM`，`AUTO` 按地址范围判断（`mmcache_store.cpp:614-624`，`IsInHybmDeviceRange` 在 `mmcache_store.cpp:530`）。
2. 调 `MmcClientDefault::BatchPut(keys, bufferArrays, options, ALLOC_RANDOM, results)`（`mmcache_store.cpp:651`）发往 MetaService。
3. MetaService 侧 `MmcMetaMgrProxy::BatchAlloc`（`mmc_meta_mgr_proxy.cpp:76`）：对每个 key 先 `CheckAndEvict`（`mmc_meta_mgr_proxy.cpp:92`）再 `Alloc`（`mmc_meta_mgr_proxy.cpp:96`）。
4. `MmcMetaManager::Alloc`（`mmc_meta_manager.cpp:731`）调 `globalAllocator_->Alloc(allocOpt, blobs)`（`mmc_meta_manager.cpp:741`），按 `ALLOC_RANDOM`/`ALLOC_ARRANGE`/`ALLOC_FORCE_BY_RANK`（`mmc_global_allocator.h:382-398`）策略把 blob 分配到合适的 rank。
5. Alloc 返回 blob 描述（`gva_` + `rank_` + `mediaType_`），客户端再 `BatchCopy` 把数据 DMA 到 blob 的 gva 地址。

`MmcBmProxy::Copy`（`mmc_bm_proxy.cpp:175`）→ `MFSmemApi::SmemBmCopy(handle_, &params, type, 0)`（`mmc_bm_proxy.cpp:186`）。`Put` 用 `ASYNC_COPY_FLAG`（`mmc_bm_proxy.cpp:212`），`Copy`（eviction 路径）用同步 flag `0`（`mmc_bm_proxy.cpp:186`）。

### 5.3 读取：多层分类 + rewarm

`MmcMetaManager::GetByRank`（`mmc_meta_manager.cpp:273`）：

1. `CheckAndEvict(MEDIA_DRAM, 0)`（`mmc_meta_manager.cpp:286`）先检查 DRAM 容量使用比例。
2. `ClassifyAndGroupKeys`（`mmc_meta_manager.cpp:321`）对每个 key 分类（`mmc_meta_manager.cpp:347-367`）：
   - 上层有 `READABLE` blob（HBM）→ `selectedBlob`，加入 `deferredLockList` 直接加读锁返回。
   - 有 `ALLOCATED` 状态的 pending blob（rewarm 进行中）→ `pendingWaitList`。
   - 仅下层（DRAM）有 `READABLE` blob 且上层不是 HBM → 直接用 lowerBlob。
   - 仅下层有且上层是 HBM → 触发 rewarm：`lowerBlob->UpdateState(READ_START)`（`mmc_meta_manager.cpp:384`）+ `globalAllocator_->Alloc(ALLOC_FORCE_BY_RANK, dstMedia=HBM)`（`mmc_meta_manager.cpp:396`）+ `AddBlob`（`mmc_meta_manager.cpp:420`），加入 `rankGroups`。
3. rewarm 按 rank 分组，`rewarmThreadPool_->Enqueue` 并发执行（`mmc_meta_manager.cpp:297-301`）。
4. `RewarmRankGroup`（`mmc_meta_manager.cpp:554`）→ `SendBatchRpc`（`mmc_meta_manager.cpp:449`）`BatchBlobCopyRequest` RPC 到目标 rank → `ApplyRewarm`（`mmc_meta_manager.cpp:502`）置 `WRITE_OK` + `READ_START` → `InsertLru`（`mmc_meta_manager.cpp:547`）。

### 5.4 Eviction：high watermark 触发 LRU 搬迁

`MmcMetaManager::CheckAndEvict`（`mmc_meta_manager.cpp:697`）：

```cpp
// mmc_meta_manager.cpp:701-702
const auto needEvictList =
    globalAllocator_->GetNeedEvictList(evictWatermarks, nowMemoryThresholds, media, wantAllocSize);
```

`GlobalAllocator::GetNeedEvictList`（`mmc_global_allocator.h:315`）检查 `usedSize * LEVEL_BASE > totalSize * level`（`mmc_global_allocator.h:337`），即使用率超过 high watermark。

Eviction **异步执行**（`mmc_meta_manager.cpp:715` `threadPool_->Enqueue`），且用 `evictCheck_` CAS 保证同一时刻只有一个 eviction 任务（`mmc_meta_manager.cpp:706-709`）。

`MmcMetaContainerLru::MultiLevelElimination`（`mmc_meta_container_lru.cpp:251`）计算 eviction 数量：

```cpp
// mmc_meta_container_lru.cpp:275-276
const size_t numEvictObjs =
    std::max(std::min(oriNum * (nowThreshold - low) / high, oriNum), static_cast<size_t>(1));
```

对每个 key 调 `EvictOneLeastRecentlyUsed`（`mmc_meta_container_lru.cpp:207`），从 LRU 尾部取（`std::prev(lruLists_[mediaType].end())`，`mmc_meta_container_lru.cpp:220`），回调 `EvictCallBackFunction`（`mmc_meta_manager.cpp:1718`）：

- `dstMedium = MoveDown(srcMediaType)`（`mmc_meta_manager.cpp:1730`）。
- `dstMedium == MEDIA_NONE`（最底层）→ `EvictRemoveSrc` → `REMOVE`（`mmc_meta_manager.cpp:1743-1746`）。
- 下层空间不足（`GetFreeSpace(dstMedium) < objMeta->Size()`，`mmc_meta_manager.cpp:1758`）→ `EvictRemoveSrc`（删源）。
- 否则 → `DispatchMoveBlob`（`mmc_meta_manager.cpp:1687`）→ `MoveBlob`（`mmc_meta_manager.cpp:1532`）→ `CopyBlob`（拷贝到下层）+ `FreeBlobs`（释放源 blob）→ 返回 `MOVE_DOWN`（从当前层 LRU 摘除，`InsertLru` 到下层，`mmc_meta_manager.cpp:1593`）。

### 5.5 lazy init（device_sdma 专用）

`MemcacheBackend.__init__`（`memcache_backend.py:76`）：`self._lazy_init = lazy_init and _is_device_sdma()`。device_sdma 协议下首个 `put` 才触发 `ensure_initialized` → `_setup_store`（`memcache_backend.py:100`）→ `acl.init + acl.rt.set_device + store.init`（`memcache_backend.py:111-122`）。scheduler 端用 `create_scheduler_client`（`memcache_backend.py:134`）`init_bm=False` 只做元数据查询，不初始化存储层。

### 5.6 key 生成与命名

`KVPoolScheduler.generate_keys`（`pool_scheduler.py:238`）：`f"{model_name}@{block_hash.hex()}"`。

`_generate_store_query_keys`（`pool_scheduler.py:250`）按 `head_or_tp_rank`/`pcp_rank`/`dcp_rank`/`pp_rank`/`kv_cache_group_id`/`cache_family` 生成 `PoolKey`（`pool_scheduler.py:267-284`）。scheduler 和 worker 用同一套 key 格式，保证两端一致。

---

## 6. buffer 注册：vLLM GPU 地址桥接

`KVPoolWorker.register_kv_caches`（`pool_worker.py:770`）：

1. 遍历 vLLM KV tensor，按 `untyped_storage().data_ptr()` 去重（`pool_worker.py:815`）。
2. `_align_kv_ptrs`（`pool_worker.py:747`）将 start 向下 2MB 对齐（memfabric_hybrid 大页要求）。
3. `m_store.register_buffer(ptrs, lengths)`（`pool_worker.py:880`）→ `MemcacheBackend.register_buffer`（`memcache_backend.py:153`）暂存到 `_pending_buffers`。
4. `ensure_initialized`（`memcache_backend.py:87`）后 `_register_buffers_if_needed`（`memcache_backend.py:157`）逐个 `store.register_buffer(ptr, size)` → `pymmc.cpp:717` → `MmcacheStore::RegisterBuffer`。

注册后 LocalService 知道这些 GPU 地址属于本 rank，后续 `BatchCopy` 时 SDMA D2D / P2P 映射 / `/dev/shm` memcpy 可直接访问。

---

## 7. TP 模型下的隔离性

### 7.1 vLLM 自身缓存：逻辑共享，物理隔离

- **scheduler 侧**：单一全局 `block_hash → block_id` 字典（`block_pool.py:184`），所有 TP rank 共用，**不隔离**。
- **worker 侧**：每 rank 独立 KV tensor pool（`gpu_worker.py:722`），物理隔离，互不可见。
- **契约**：block_id 全局编号，所有 rank 同步持有同一 block_id 的不同 head 分片。
- **reset**：只清 scheduler 字典（`block_pool.py:780`），保留 worker NPU 缓冲区。

### 7.2 MemCache：按 rank 隔离

- 每 worker 进程一份 `MemcacheBackend`（`memcache_backend.py:74`），`local_rank = get_world_group().local_rank`。
- `_setup_store`（`memcache_backend.py:113`）`acl.rt.set_device(self.local_rank)` + `store.init(self.local_rank)`，每 worker 绑定一个逻辑设备，注册到 MetaService 拿到自己的 `bmRankId_`（`mmc_bm_proxy.cpp:60`）。
- HBM/DRAM pool 按 rank 划分（`mmc_bm_proxy.cpp:80-81`）。

### 7.3 跨 rank 协作

`lookup_scheduler`（`pool_worker.py:2358`）调 `_expand_lookup_keys_by_rank`（`pool_worker.py:2244`）把单 rank key 扩展为所有 TP rank 的 key 一起 `batch_is_exist`，确保 TP=2/4 下任一 rank 缺失都判为未命中。

### 7.4 MLA / put_step 特例

当 `num_kv_head < tp_size`（如 MLA，`pool_scheduler.py:193-196`）时，`put_step = tp_size // num_kv_head`，多个 rank 共享同一份 KV。此时 AscendStoreConnector 把这些 rank 当成一个 rank shard 处理（`_make_layerwise_gva_keys_for_hit_check` 只生成 `tp_size // put_step` 个 key，`pool_scheduler.py:338-342`）。但 vLLM 自身 BlockPool 不关心这点，依然只管全局 block_id。

---

## 8. 修正记录

本节记录在源码核实过程中对前述分析的修正，避免误导。

### 8.1 修正一：worker 进程不持有 BlockPool

**错误说法**：vLLM core 的 BlockPool 在每个 worker 进程也有一份用于本地查询。

**源码核实**：grep `/vllm-workspace/vllm/vllm/v1/worker/` 确认无任何 `BlockPool`/`cached_block_hash_to_block`/`BlockHashToBlockMap` 引用。BlockPool / `cached_block_hash_to_block` 字典只在 EngineCore（scheduler）进程**唯一一份**（`block_pool.py:184`，`kv_cache_coordinator.py:90`），worker 进程完全没有副本。

### 8.2 TP 数量与重置条件

同一个 EngineCore 管理调度和缓存索引。TP worker 数量增加不会自动增加并发请求数。
重置能否成功取决于是否仍有请求占用 block，应按请求状态和引擎日志排查。
本轮只验证 TP=1/TP=2；历史 TP=4 的记录属于当时的环境。

### 8.3 重置成功的检查方式

当前测试检查 HTTP 状态，再确认引擎新增了本次 `Successfully reset prefix cache` 日志。
重置结果以该版本接口及引擎的成功记录为准。

---

## 附录：关键源文件索引

### vLLM-Ascend 侧

| 文件 | 关键位置 | 作用 |
|------|---------|------|
| `ascend_store_connector.py` | `:80` `AscendStoreConnector` | connector 入口，scheduler/worker 分流 |
| `ascend_store_connector.py` | `:308` `LookupKeyServer` | zmq REP，worker 侧 lookup 服务 |
| `pool_scheduler.py` | `:521` `get_num_new_matched_tokens` | scheduler 侧外部命中查询 |
| `pool_scheduler.py` | `:958` `build_connector_meta` | 打包 save 请求 |
| `pool_worker.py` | `:770` `register_kv_caches` | 注册 GPU buffer |
| `pool_worker.py` | `:883` `start_load_kv` | worker 侧加载入口 |
| `pool_worker.py` | `:1031` `_process_save_for_layer_batch` | layerwise save（含跳过已命中逻辑） |
| `pool_worker.py` | `:1766` `wait_for_save` | worker 侧保存入口 |
| `pool_worker.py` | `:2358` `lookup_scheduler` | worker 侧命中查询 |
| `kv_transfer.py` | `:608` `KVCacheStoreSendingThread` | 异步保存线程 |
| `kv_transfer.py` | `:718` `_handle_stored_request` | put 前先查 MemCache 去重 |
| `kv_transfer.py` | `:896` `KVCacheStoreRecvingThread` | 异步加载线程 |
| `backend/memcache_backend.py` | `:66` `MemcacheBackend` | MemCache 后端封装 |

### vLLM core 侧

| 文件 | 关键位置 | 作用 |
|------|---------|------|
| `v1/core/block_pool.py` | `:143` `BlockPool` | 唯一一份 block_hash→block 字典 |
| `v1/core/block_pool.py` | `:225` `cache_full_blocks` | prefill 后缓存 block（插入字典） |
| `v1/core/block_pool.py` | `:607` `_insert_block_hash` | 实际插入字典操作 |
| `v1/core/block_pool.py` | `:761` `reset_prefix_cache` | 清空本地缓存索引（当前测试使用） |
| `v1/core/kv_cache_coordinator.py` | `:90` `BlockPool(...)` | 创建 BlockPool |
| `v1/core/kv_cache_coordinator.py` | `:192` `allocate_new_computed_blocks` | 分配 ext_comp block |
| `v1/core/single_type_kv_cache_manager.py` | `:282` `allocate_external_computed_blocks` | 为外部 token 分配新 block |
| `v1/core/single_type_kv_cache_manager.py` | `:403` `cache_blocks` | 触发 block 缓存 |
| `v1/core/kv_cache_manager.py` | `:207` `get_computed_blocks` | 查本地 prefix cache（关闭时直接返回空） |
| `v1/core/kv_cache_manager.py` | `:283` `allocate_slots` | 分配 slot（含 ext_comp 处理） |
| `v1/core/kv_cache_manager.py` | `:498-502` | cache_blocks 调用（含 ext_comp token） |
| `v1/core/sched/scheduler.py` | `:269` `KVCacheManager(...)` | 创建 KVCacheManager |
| `v1/core/sched/scheduler.py` | `:753-790` | 本地 + 外部命中查询流程 |
| `v1/core/sched/scheduler.py` | `:935-947` | allocate_slots 调用 |
| `v1/core/sched/scheduler.py` | `:2326` `reset_prefix_cache` | scheduler 侧重置入口 |
| `v1/core/sched/scheduler.py` | `:2543` `_update_waiting_for_remote_kv` | 异步加载完成后补 cache |
| `v1/engine/core.py` | `:214` `request_block_hasher` | connector 存在时创建 hasher（`enable_prefix_caching or kv_connector`） |
| `v1/engine/core.py` | `:153` `Scheduler(...)` | EngineCore 创建 Scheduler |
| `v1/engine/core.py` | `:995` `EngineCoreProc` | EngineCore 进程封装 |
| `v1/engine/core_client.py` | `:467` `MPClient` | 多进程客户端 |
| `v1/engine/utils.py` | `:164` `context.Process(...)` | spawn EngineCore 进程 |
| `v1/worker/worker.py` | `:434` `torch.npu.set_device` | worker 绑定单卡 |
| `model_executor/models/config.py` | `:557-559` `MambaModelConfig` | 历史 Mamba 配置修改；当前使用原版 0.24 与本地 prefix cache |
| `config/vllm.py` | `:2300-2303` `validate_mamba_block_size` | 历史校验修改；当前 vLLM 核心未修改 |

### MemCache 侧

| 文件 | 关键位置 | 作用 |
|------|---------|------|
| `mmcache_store.cpp` | `:594` `BatchPutFromLayers` | 批量写入入口 |
| `mmcache_store.cpp` | `:715` `BatchGetIntoLayers` | 批量读取入口 |
| `meta_service/mmc_meta_manager.cpp` | `:731` `Alloc` | MetaService 分配 blob |
| `meta_service/mmc_meta_manager.cpp` | `:697` `CheckAndEvict` | eviction 触发 |
| `meta_service/mmc_meta_manager.cpp` | `:273` `GetByRank` | 批量读取 + rewarm |
| `meta_service/mmc_meta_manager.cpp` | `:1718` `EvictCallBackFunction` | eviction 回调 |
| `meta_service/mmc_meta_manager.cpp` | `:1532` `MoveBlob` | 跨层搬迁 |
| `meta_service/mmc_global_allocator.h` | `:315` `GetNeedEvictList` | watermark 检查 |
| `meta_service/mmc_meta_container_lru.cpp` | `:251` `MultiLevelElimination` | LRU eviction 执行 |
| `local_service/mmc_bm_proxy.cpp` | `:175` `Copy` | SDMA/P2P DMA |
| `local_service/mmc_local_service_default.cpp` | `:431` `CopyBlob` | HBM↔DRAM 拷贝 |
| `python_wrapper/pymmc.cpp` | `:717` `register_buffer` | Python buffer 注册绑定 |

---

原环境的源码索引保留如上，当前部署版本及实测条件见迁移报告。
