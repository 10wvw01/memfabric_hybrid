# Mooncake 接入 MemFabric 到 vLLM 的修改总结

## 功能架构

```
用户请求
  │
  ▼
vLLM EngineCore
  │  KV cache 生成在 NPU HBM
  │
  ├── MooncakeConnector (P2P 直传)
  │     kv_role: kv_producer → kv_consumer
  │     协议: mooncake_protocol=mf (vLLM CLI JSON)
  │     TransferEngine.installTransport("mf")
  │     → MF Transport (SMEM_TRANS 模式)
  │     → HYBM 注册 NPU HBM (memType=DEVICE)
  │     → SDMA 传输: NPU HBM → NPU HBM
  │     场景: vllm_kvboth_mf, vllm_pd_disagg_mf
  │
  └── MooncakeStoreConnector (存储池 offload)
        kv_role: kv_both
        协议: MOONCAKE_CONFIG_PATH JSON (protocol=mf)
        MooncakeDistributedStore.setup()
        → MF Transport (SMEM_BM 模式)
        → HYBM 分配 DRAM 段 (memType=HOST, 1GB)
        → SDMA 传输: NPU HBM → HYBM DRAM 段
        → 前缀缓存: 按 block hash 去重
        场景: vllm_kv_offload_mf

MemFabric (底层 SDMA 引擎)
  HYBM: 统一设备/主机内存地址空间
  SDMA: NPU 芯片内 DMA 拷贝
  Config Store: 拓扑发现与 rank 协调
  310P3: HBM (128GB) + DRAM (1GB 段)
```

两种接入路径的核心区别：

- **MooncakeConnector**：prefiller 和 decoder 各自注册 NPU HBM，SDMA 直接在 NPU 之间搬运 KV cache。两端都是设备内存，HYBM 天然支持。
- **MooncakeStoreConnector**：单实例将 KV cache offload 到 CPU 内存池。需要 HYBM 分配 DRAM 段作为存储池，SDMA 从 NPU HBM 搬到 DRAM 段。

---

## 适配改动总览

```
┌──────────────────────────────────────────────────────────────┐
│  vLLM 层 (Python) — 混合注意力模型兼容                        │
│  ① utils.py:     KV cache tuple → list 展开                  │
│  ② store/worker.py: _repr_tensor 支持 tuple                  │
│  ⑫ store/worker.py: register_kv_caches 按组分配段地址          │
│  ⑬ store/worker.py: save_put/load_get 传输重试                │
├──────────────────────────────────────────────────────────────┤
│  Mooncake Store 层 (C++) — mf 协议 + DRAM 段挂载              │
│  ③ client_service.cpp: mf 加入协议白名单                     │
│  ④ client_service.h/cpp: MountSegment 新增 skip_register    │
│  ⑤ real_client.cpp: MemFabricGetSegment 段挂载 + 强制 BM 模式 │
├──────────────────────────────────────────────────────────────┤
│  MF Transport 层 (C++) — BM 模式 DRAM 分配 + 时序 + 容错      │
│  ⑥ memfabric_smem_dl.h:   MF_SMEM_TYPE 环境变量              │
│  ⑦ memfabric_transport.cpp: install 先加载 config 再读 flag   │
│  ⑧ memfabric_transport.cpp: registerLocalMemory HYBM 容错    │
│  ⑨ memfabric_smem_dl.cpp:  GetSegment 先 HOST 再 LOCAL_HOST  │
├──────────────────────────────────────────────────────────────┤
│  构建辅助                                                      │
│  ⑩ libub_allocator.so: 解决 store.so 未定义符号               │
│  ⑪ tag_v0.3.13.patch:  MF transport 编译支持                  │
└──────────────────────────────────────────────────────────────┘
```

---

## 第一层：vLLM 适配混合注意力模型

> **适配问题**：310P3 上 Qwen3.5 是混合注意力模型（linear + full attention），KV cache 以 `(k, v)` tuple 形式传递，但 vLLM 0.26.0 代码假设是单个 Tensor 或 list，导致 `AssertionError`。

### ① `utils.py` — KV cache tuple 展开

**文件**: `vllm/distributed/kv_transfer/kv_connector/utils.py` (~line 609)

原代码只处理 `torch.Tensor` 和 MambaSpec 的 list，遇到 tuple 直接断言失败。新增分支将 tuple 展开为 list：

```python
if isinstance(cache, tuple):
    return list(cache)
```

### ② `store/worker.py` — `_repr_tensor` 支持 tuple

**文件**: `vllm/distributed/kv_transfer/kv_connector/v1/mooncake/store/worker.py` (~line 1218)

原断言 `isinstance(v, torch.Tensor | list)` 对 tuple 失败。扩展为：

```python
def _repr_tensor(v: torch.Tensor | list | tuple) -> torch.Tensor:
    assert isinstance(v, (torch.Tensor, list, tuple))
    return v if isinstance(v, torch.Tensor) else v[0]
```

---

## 第二层：Mooncake Store 适配 mf 协议 + DRAM 段挂载

> **适配问题**：Store 的 `client_service.cpp` 不认识 `mf` 协议；Store 用 `malloc` 分配 CPU 内存，但 SDMA 要求 HYBM 注册的内存；HYBM 已注册的 DRAM 段被 Store 重复注册导致 `address overlap`。

### ③ `client_service.cpp` — mf 协议白名单

**文件**: `mooncake-store/src/client_service.cpp` (~line 921)

`InitTransferEngine()` 协议分发没有 `mf` 分支。加入共用分支：

```cpp
} else if (protocol == "ascend" || protocol == "ubshmem" ||
           protocol == "sunrise_link" || protocol == "mf") {
```

### ④ `client_service.h` / `client_service.cpp` — MountSegment skip_register

**文件**: `mooncake-store/include/client_service.h` (~line 357), `client_service.cpp` (~line 3434)

`MountSegmentAndGetId()` 内部调 `registerLocalMemory()`，但 HYBM 在 `MemFabricInitSmemBm()` 中已注册同一段 DRAM，导致 `address overlap`。新增 `skip_register` 参数跳过重复注册：

```cpp
// 签名新增 skip_register=false
tl::expected<UUID, ErrorCode> MountSegmentAndGetId(
    ..., bool skip_register = false);

// 实现中跳过注册
int rc = 0;
if (!skip_register) {
    rc = transfer_engine_->registerLocalMemory((void*)buffer, size,
                                                 location, true, true);
}
```

### ⑤ `real_client.cpp` — MemFabric DRAM 段挂载 + 强制 BM 模式

**文件**: `mooncake-store/src/real_client.cpp` (~line 872)

Store 原先用 `malloc` 分配 CPU 内存，SDMA 无法传输。改为优先使用 `MemFabricGetSegment()` 返回的 HYBM DRAM 段，并用 `skip_register=true` 挂载避免重复注册。段无效时 fall through 到原有 `malloc` 路径。

同时在配置解析中强制 SMEM_BM 模式，使 HYBM 分配 DRAM 段：

```cpp
MemFabricSmemDl::SetSmemTypeFlag(mooncake::SMEM_BM);
```

---

## 第三层：MF Transport 适配 BM 模式 DRAM 分配

> **适配问题**：默认 SMEM_TRANS 模式不分配 DRAM 段（`maxDRAMSize=0`）；install 时序导致 flag 修改在读取之后；`MemFabricGetSegment` 用错内存类型；HYBM 注册 CPU 内存失败时无容错。

### ⑥ `memfabric_smem_dl.h` — MF_SMEM_TYPE 环境变量

**文件**: `mooncake-transfer-engine/include/transport/mf_transport/memfabric_smem_dl.h` (~line 591)

`smemTypeFlag_` 默认 `SMEM_TRANS`，该模式不分配 DRAM。新增环境变量选择模式：

```cpp
auto smemTypeStr = std::getenv("MF_SMEM_TYPE");
if (smemTypeStr) {
    if (strcmp(smemTypeStr, "bm") == 0)
        SetSmemTypeFlag(SMEM_BM);   // 分配 DRAM 段
    else if (strcmp(smemTypeStr, "trans") == 0)
        SetSmemTypeFlag(SMEM_TRANS); // 默认
}
```

### ⑦ `memfabric_transport.cpp` — install 时序修复

**文件**: `mooncake-transfer-engine/src/transport/mf_transport/memfabric_transport.cpp` (~line 282)

`install()` 先读 `GetSmemTypeFlag()`（默认 TRANS），后调 `LoadMemFabricBmAPI()` → `InitMustConfig()` → `SetSmemTypeFlag(BM)`。flag 修改发生在读取之后，导致始终走 TRANS。改为先加载 config：

```cpp
MemFabricSmemDl::LoadMemFabricBmAPI();  // 触发 config 解析 + SetSmemTypeFlag
smemType_ = MemFabricSmemDl::GetSmemTypeFlag();  // 现在能读到正确值
```

### ⑧ `memfabric_transport.cpp` — registerLocalMemory 容错

**文件**: 同上 (~line 331)

TRANS 模式下 `SmemTransRegisterMem` 对 CPU 内存失败（HYBM 不支持），原代码直接返回错误。改为容错返回 0：

```cpp
case SMEM_TRANS: {
    int ret = SmemTransRegisterMem(...);
    if (ret != 0) {
        LOG(WARNING) << "...CPU memory may not support HYBM; continuing anyway";
        return 0;  // 允许 store 继续挂载
    }
    return ret;
}
```

### ⑨ `memfabric_smem_dl.cpp` — GetSegment 内存类型回退

**文件**: `mooncake-transfer-engine/src/transport/mf_transport/memfabric_smem_dl.cpp` (~line 332)

`MemFabricGetSegment()` 固定用 `SMEM_MEM_TYPE_HOST`，但 BM 模式下 DRAM 段的内存类型可能不同。改为先试 HOST，size=0 再回退 LOCAL_HOST：

```cpp
size_t dramSize = SmemBmGetLocalMemSizeByMemType(handle, SMEM_MEM_TYPE_HOST);
void* ptr = nullptr;
if (dramSize > 0) {
    ptr = SmemBmPtrByMemType(handle, SMEM_MEM_TYPE_HOST, bmRankId);
} else {
    dramSize = SmemBmGetLocalMemSizeByMemType(handle, SMEM_MEM_TYPE_LOCAL_HOST);
    ptr = SmemBmPtrByMemType(handle, SMEM_MEM_TYPE_LOCAL_HOST, bmRankId);
}
```

---

## 第四层：构建辅助

### ⑩ `libub_allocator.so` — 解决 store.so 未定义符号

`store.so` 引用 `ub_allocate_memory`/`ub_free_memory` 但未链接。从已有 `.o` 编译独立 `.so`，通过 `LD_PRELOAD` 预加载：

```bash
g++ -shared -o libub_allocator.so ub_allocator.cpp.o \
    -I${MOONCAKE_DIR}/mooncake-transfer-engine/include \
    -lnuma -lglog -lpthread
```

### ⑪ `tag_v0.3.13.patch` — MF transport 编译支持

`patches/mooncake/tag_v0.3.13.patch`：CMake/源码补丁，启用 `USE_MF=ON`，添加 `mf_transport` 到 CMakeLists，修复 yalantinglibs 路径。所有后续修改的基础。

### ⑫ `store/worker.py` — `register_kv_caches` 按组分配段地址

**文件**: `vllm/distributed/kv_transfer/kv_connector/v1/mooncake/store/worker.py` (~line 1227)

**问题**：混合注意力模型（如 Qwen3.5，FullAttention + Mamba）有多个 KV cache 组，每组有不同的 block_size 和内存布局。原代码将所有层的段地址收集到一个扁平列表，然后赋给**所有**组的 `token_db`。这导致：
- FullAttention 组使用 Mamba 层的 block_len 计算地址，地址越界 → `TRANSFER_FAIL`
- Mamba 组使用 FullAttention 层的地址，数据错误

**修复**：
1. 构建 `layer_to_group` 映射（从 `KVCacheGroupSpec.layer_names`）
2. 按组收集段地址（`per_group_addrs[g_idx]`），每个 `token_db` 只使用自己组层的地址
3. 使用 `cache.data_ptr()`（含 storage_offset）而非 `cache_storage.data_ptr()`（存储基址），确保共享存储的层各自映射到正确的组
4. `register_buffer` 仍按存储去重注册

### ⑬ `store/worker.py` — save_put / load_get 传输重试

**文件**: `vllm/distributed/kv_transfer/kv_connector/v1/mooncake/store/worker.py` (~line 676, ~line 920)

**问题**：310P3 NPU 上 SDMA 首次传输（HBM↔DRAM）可能失败（`TRANSFER_FAIL`），但第二次成功。推测原因是 `torch.cuda.Event` 在 NPU 上不能完全同步所有计算流，首次传输时 KV cache 数据可能尚未就绪。

**修复**：
- `save_put`：首次 `batch_put_from_multi_buffers` 失败后，重试 2 次（间隔 50ms/100ms），仅重试失败的 key
- `load_get`：首次 `batch_get_into_multi_buffers` 失败后，同样重试 2 次
- 重试成功后正常记录 `_record_operation` 和 `_record_saved`

---

## 两种接入路径对比

| | MooncakeConnector (P2P) | MooncakeStoreConnector (Store) |
|---|---|---|
| **vLLM 参数** | `kv_connector=MooncakeConnector` + `mooncake_protocol=mf` | `kv_connector=MooncakeStoreConnector` + `MOONCAKE_CONFIG_PATH` |
| **协议指定** | vLLM CLI JSON | mooncake JSON 配置文件 |
| **代码路径** | TransferEngine `installTransport("mf")` | Store `InitTransferEngine("mf")` |
| **MF 模式** | SMEM_TRANS（HYBM DEVICE only） | SMEM_BM（HYBM DEVICE + DRAM） |
| **数据路径** | NPU HBM → SDMA → NPU HBM | NPU HBM → SDMA → HYBM DRAM 段 |
| **需要的适配** | ⑪ + ① | ⑪ + ①②③④⑤⑥⑦⑧⑨⑩ |
| **测试场景** | `vllm_kvboth_mf`, `vllm_pd_disagg_mf` | `vllm_kv_offload_mf` |

---

## KV Offloading 数据路径（修复后）

```
vLLM EngineCore
  │
  ├── store.setup()
  │     ├── SetSmemTypeFlag(SMEM_BM)              ← ⑤⑥
  │     ├── MemFabricInitSmemBm()
  │     │     └── SmemBmCreate2(localDRAMSize=1GB)
  │     │         → HYBM 分配 DRAM 段 (shm_open)
  │     ├── MemFabricGetSegment() → {ptr, 1GB}   ← ⑨
  │     └── MountSegment(skip_register=true)      ← ④⑤
  │
  ├── register_kv_caches()                         ← ⑫
  │     ├── layer_to_group 映射 (KVCacheGroupSpec.layer_names)
  │     ├── 按组收集段地址: per_group_addrs[g_idx]
  │     │     group 0 (FullAttention): 6 段 × 655360 bytes
  │     │     group 1-3 (Mamba): 6 段 × 36864 bytes
  │     ├── register_buffer (按存储去重)
  │     └── 各 token_db 仅使用本组地址 ✓
  │
  ├── save_put (request 1)                         ← ⑬
  │     └── batch_put(keys, NPU_HBM → DRAM)
  │           └── SDMA batch_copy (6 段/key)
  │                 ├── group 0: TRANSFER_FAIL (首次) → 重试成功
  │                 └── group 1-3: 成功
  │           → save_put_failed_keys=0 ✓
  │
  ├── POST /reset_prefix_cache (清除本地缓存)
  │
  ├── lookup (request 2)
  │     └── batch_is_exist → 4 keys all exist
  │     └── find_longest_cache_hit → hit_length=640 ✓
  │
  └── load_get (request 2)                         ← ⑬
        └── batch_get(DRAM → NPU_HBM)
              ├── group 0: TRANSFER_FAIL (首次) → 重试成功
              └── group 1-3: 成功
        → external_prefix_cache_hits=640.0 ✓
        → External prefix cache hit rate: 44.7% ✓
```

---

## 补丁文件清单

| 文件 | 覆盖改动 | 应用方式 |
|------|---------|---------|
| `patches/mooncake/tag_v0.3.13.patch` | ⑪ MF transport 编译支持 | run.sh `step_patch_mooncake` |
| `patches/mooncake/store_mf_protocol.patch` | ③ mf 协议白名单 | run.sh `_fix_mooncake_store()` 第 1 步 |
| `patches/mooncake/store_mf_host_memory.patch` | ④⑤ skip_register + DRAM 段挂载 | 已编入 store.so |
| `scripts/patch_vllm_store_worker.py` | ⑫⑬ 按组段分配 + 传输重试 | run.sh `_patch_vllm_store_worker()` |
| (直接修改源文件) | ①② vLLM tuple 修复 | run.sh `_patch_vllm_hybrid_kv()` + `_fix_mooncake_store()` 第 3 步 |
| (直接修改源文件) | ⑥⑦⑧⑨ MF transport BM 模式 | MF transport 源文件为 untracked，直接修改 |

---

## 环境变量

| 变量 | 作用 | 值 |
|------|------|-----|
| `MF_SMEM_TYPE` | 选择 MF 传输模式 | `bm` (Store offloading) / `trans` (P2P, 默认) |
| `MF_DRAM_SIZE` | BM 模式下 DRAM 段大小 | `1073741824` (1GB) |
| `MF_HBM_SIZE` | HBM 段大小 | `0` (Store 不用 HBM) |
| `MF_OP_TYPE` | 数据操作类型 | `device_sdma` |
| `MF_PD_OP_TYPE` | PD 传输操作类型 | `device_sdma` |
| `MF_STORE_URL` | MF config store 地址 | `tcp://127.0.0.1:8570` |
| `MF_SMEM_TRANS_ROLE` | TRANS 模式角色 | `Prefill` / `Decode` (仅 P2P) |
| `MOONCAKE_CONFIG_PATH` | Store JSON 配置路径 | 指向 mooncake_config.json |
| `PYTHONHASHSEED` | 固定 hash 种子 | `0` (跨进程 block hash 一致) |
| `VLLM_SERVER_DEV_MODE` | vLLM dev API | `1` (启用 `/reset_prefix_cache`) |
| `VLLM_LOGGING_LEVEL` | vLLM 日志级别 | `DEBUG` (查看 store 操作) |
| `LD_PRELOAD` | 预加载 libub_allocator.so | 解决 store.so 未定义符号 |
