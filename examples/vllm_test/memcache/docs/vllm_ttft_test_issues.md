# MemCache vLLM TTFT 测试问题记录

测试用例、TTFT/P50 和常用术语见[测试说明](../../TEST_GUIDE.md)。

本文保留历史环境的测试数据和问题记录。当前指定分支的部署流程见[部署说明](vllm_deployment_flow.md)，最新验证条件与结果见[迁移报告](vllm_024_migration_results.md)。

历史记录中的 `--no-enable-prefix-caching` 对应当时的测试配置。
当前 vLLM 0.24 基线开启 prefix caching，并在请求后重置本地前缀；
新增原生缓存对照则保留本地前缀且不配置外部 connector。三种方式的实测对比见上述迁移报告。

## 1. device_sdma HBM→DRAM Eviction 107002 错误

### 1.1 问题概述

vLLM 端到端 TTFT 测试（device_sdma 协议）中，HBM→DRAM eviction 全部失败，错误码 107002（`ACL_ERROR_RT_CONTEXT_NULL`）。在该次记录中，HBM→DRAM 迁移未成功，缓存数据未能按预期转入 DRAM。

| 测试 | media=0→1 (HBM→DRAM) 次数 | 107002 错误数 | 成功次数 |
|------|---------------------------|--------------|---------|
| Test 3.7 MemCache 阶段 | 69 | 170 | 0 |
| Test 3.8 Prefix cache 阶段 | 65 | 135 | 0 |

所有 HBM→DRAM `CopyBlob` 操作均失败，无一次成功。

### 1.2 错误码定义

错误码 107002 定义于 CANN 头文件 `/usr/local/Ascend/cann-9.1.0/aarch64-linux/include/acl/error_codes/rt_error_codes.h:23`：

```c
#define ACL_ERROR_RT_CONTEXT_NULL  107002  // current context null
```

含义：当前线程的 ACL runtime context 为空。当线程未通过 `AclrtSetDevice` 绑定设备就调用 ACL 操作（如 `AclrtMemcpyAsync`）时返回此错误。

### 1.3 调用链

```
MmcLocalServiceDefault::CopyBlob          [mmc_local_service_default.cpp:432]
  └─ else 分支（src=HBM, dst=DRAM）
     └─ MmcBmProxy::Copy(..., SMEMB_COPY_G2G)   [mmc_bm_proxy.cpp:175]
        └─ MFSmemApi::SmemBmCopy
           └─ MemEntityDefault::CopyData         [hybm_entity_default.cpp:808]
              ├─ SetThreadAclDevice()             [hybm_entity_default.cpp:1398]
              │   └─ AclrtSetDevice(HybmGetInitDeviceId())
              └─ dataOperator_->DataCopy
                 └─ HostComposeDataOp::DataCopy  [hybm_compose_data_op.cpp:115]
                    └─ HostDataOpAclMemcpy::CopyD2H  [hybm_data_op_acl_memcpy.cpp:191]
                       └─ AclrtMemcpyAsync(..., stream_)  ← 返回 107002
```

### 1.4 历史日志与当时的排查假设

日志记录了复制调用返回 `107002`，以及线程记录的设备号与参与数据地址。
当时曾推测 stream 与 context 不匹配；下面的图表保留该排查过程。
当前 `SetThreadAclDevice` 的实现同时检查线程的设备号和 ACL context，缺少 context 时重新调用 `AclrtSetDevice`。
现有日志没有直接记录 stream 创建及跨进程传递，图中的对应关系应作为当时假设阅读。

#### 证据

同一线程（tid 556520）的完整时序：

```
05:00:17.552032  SetThreadAclDevice: deviceId=0 success        ← 绑定 device 0
05:00:17.552052  CopyData enter: curDevice=0 initDeviceId=0
                 src=0x280086324000 dest=0x280100000000 size=6553600
05:00:17.552659  CopyD2H failed: ret:107002 rankId:1          ← rankId=1 的 entity
                 src:0x12da06324000 curDevice:0                 ← src 在 device 0 地址范围
                 stream:0xfffe27aba408
```

#### 地址范围对照

| Rank | PID | HBM 起始地址 | 设备 |
|------|-----|-------------|------|
| rank0 | 555001 (Worker_TP0) | `0x12da00000000` | device 0 (phyId 4) |
| rank1 | 555009 (Worker_TP1) | `0x12db00000000` | device 1 (phyId 5) |

#### 当时用于比较的日志信息

- 所有 34 次 `CopyD2H` 失败：`rankId:1`，`src:0x12da*`（device 0 地址范围），`curDevice:0`
- `SetThreadAclDevice` 用 `HybmGetInitDeviceId()` 设置 device 0（调用方 Worker_TP0 的 initDeviceId）
- 但 `CopyD2H` 使用的 `stream_` 属于 rank1 entity（Worker_TP1 进程），在 device 1 上创建
- device 0 的 context + device 1 的 stream → `AclrtMemcpyAsync` 返回 107002

#### 当时的推测示意

```
vLLM Worker_TP0 (pid 555001)          vLLM Worker_TP1 (pid 555009)
  │                                     │
  ├─ acl.rt.set_device(0)               ├─ acl.rt.set_device(1)
  ├─ store.init(4) → rank0              ├─ store.init(5) → rank1
  │   └─ HybmGetInitDeviceId() = 0      │   └─ HybmGetInitDeviceId() = 1
  │                                     │
  ├─ MemEntityDefault (rank0)           ├─ MemEntityDefault (rank1)
  │   └─ HostDataOpAclMemcpy           │   └─ HostDataOpAclMemcpy
  │       └─ stream_ created on dev 0   │       └─ stream_ created on dev 1
  │                                     │
  └─ eviction 触发时:                   └─ (对端 entity 的 stream_ 被跨进程使用)
      SetThreadAclDevice → dev 0
      CopyD2H(rankId=1) → 用 rank1 的 stream_
      ❌ dev 0 context + dev 1 stream → 107002
```

### 1.5 为什么 MemCache 工作负载测试没有此问题

| 维度 | MemCache 工作负载测试 (模拟推理) | vLLM 集成测试 (vLLM) |
|------|--------------------|-----------------|
| 进程模型 | 单进程（fork rank1 keepalive 后数据操作在 rank0 线程） | 多进程（Worker_TP0 和 Worker_TP1 是独立 OS 进程） |
| stream_ 创建 | rank0 线程在 device 0 创建，eviction 也在同一线程 | rank1 进程在 device 1 创建 stream_，但 rank0 进程的 eviction 线程用 device 0 context |
| SetThreadAclDevice | 与 stream_ 创建在同一进程/线程，device 一致 | 与 stream_ 创建在不同进程，device 不一致 |
| eviction 触发 | 显式填满 HBM（rewarm 测试），本进程内操作 | vLLM KV transfer 写入 HBM 满 90% 后自动触发，跨进程 CopyBlob |

### 1.6 为什么最小复现未触发 eviction

Python API（`put_from_layers`）的分配器有跨 rank + DRAM fallback 逻辑：

1. `PutFromLayers` 设置 `type = MEDIA_HBM`（`mmcache_store.cpp:549-551`），请求 MetaService 分配 HBM
2. MetaService `GlobalAllocator::Alloc` 将 blob 分配到多个 rank（`mmc_global_allocator.h:81-103`）
3. 当单个 rank HBM 不足时，分配器尝试其他 rank 或 DRAM fallback，而非触发 eviction
4. eviction 仅在 HBM 使用率超过 high watermark（90%）时触发（`mmc_global_allocator.h:337`）
5. 剩余空间不足但使用率 < 90% 时，直接拒绝分配（`mmc_global_allocator.h:344-350`）

因此，通过 Python API 写入数据时，分配器优先跨 rank 分散 + DRAM fallback，很难将单个 rank HBM 填到 90%。vLLM 的 `AscendStoreConnector` 使用不同的分配策略（更集中写入单个 rank），才能触发 eviction。

### 1.7 对测试结论的影响

| 结论 | 影响程度 | 说明 |
|------|---------|------|
| MemCache cache hit 降幅 85%+ | 需修正归因 | 降幅主要来自 vLLM 内部 prefix cache（5.65 GiB >> 20 prompt），而非 MemCache 分布式存储 |
| device_sdma DRAM 层可用 | 需修正 | DRAM 层实际不可用（eviction 全部失败），MemCache 在 device_sdma 下实际只有 HBM 单层 |
| device_sdma vs host_shm 对比 | 需修正 | 两种协议实际都是单层存储（host_shm 只有 DRAM，device_sdma 只有 HBM），开销差异来自 HBM SDMA vs DRAM 共享内存的带宽差异 |

### 1.8 涉及的源文件

| 文件 | 路径 | 关键行 | 作用 |
|------|------|--------|------|
| `hybm_data_op_acl_memcpy.cpp` | `3rdparty/memfabric_hybrid/src/hybm/csrc/data_operation/host/` | 191-209 | `CopyD2H` 调用 `AclrtMemcpyAsync`，返回 107002 |
| `hybm_entity_default.cpp` | `3rdparty/memfabric_hybrid/src/hybm/csrc/entity/` | 1398-1414 | `SetThreadAclDevice` 用 `HybmGetInitDeviceId()` 绑定设备 |
| `hybm_entity_default.h` | `3rdparty/memfabric_hybrid/src/hybm/csrc/entity/` | 111 | `static thread_local bool isSetDevice_` |
| `mmc_local_service_default.cpp` | `src/memcache/csrc/local_service/` | 432-440 | `CopyBlob` else 分支，HBM→DRAM eviction |
| `mmc_bm_proxy.cpp` | `src/memcache/csrc/local_service/` | 175-189 | `Copy` 调用 `SmemBmCopy` |
| `mmc_global_allocator.h` | `src/memcache/csrc/meta_service/` | 315-355 | `GetNeedEvictList` watermark 90% 阈值 |
| `rt_error_codes.h` | `/usr/local/Ascend/cann-9.1.0/.../acl/error_codes/` | 23 | `#define ACL_ERROR_RT_CONTEXT_NULL 107002` |

### 1.9 修复方向建议

根因是 **跨进程的 `stream_` 与 `SetThreadAclDevice` 设备不匹配**。可能的修复方向：

1. **在 `CopyD2H` 中重新绑定正确设备**：根据 `rankId_` 对应的 `deviceId` 调用 `AclrtSetDevice`，而非依赖 `HybmGetInitDeviceId()`
2. **在 `CopyData` 中按 src 地址对应的设备重新绑定**：`LocateAddrAndRank` 已解析出 `p2pInfo`，可在 `DataCopy` 前按 src rank 的 deviceId 重新 `SetDevice`
3. **为 eviction 创建专用 stream**：在 `CopyBlob` 中按实际设备创建临时 stream，而非复用对端进程创建的 `stream_`

方向 2 改动最小且最准确：`LocateAddrAndRank` 已经知道 src 属于哪个 rank/device，只需在 `DataCopy` 前按该 device 重新 `AclrtSetDevice` 即可。

---

## 2. 最小复现用例

### 2.1 文件

| 文件 | 路径 | 说明 |
|------|------|------|
| 测试脚本 | `cases/python/memcache/workload/hbm_dram_eviction_repro_test.py` | 两个测试场景：init 线程 eviction + worker 线程 eviction |
| 配置文件 | `cases/config/mmc-local-eviction-repro.conf` | device_sdma + HBM 256MB + DRAM 512MB |

### 2.2 复现结果

最小复现**未能**通过 Python API 触发 107002 错误。原因：

- Python API 的分配器有跨 rank + DRAM fallback 逻辑，难以将单个 rank HBM 填到 90% watermark
- `PutFromLayers` 的 blob 分散到多个 rank，单 rank HBM 使用率达不到 eviction 阈值
- vLLM `AscendStoreConnector` 使用不同的分配策略（更集中写入），才能触发 eviction

**实际复现方式**：带诊断日志重跑 vLLM 集成测试 vLLM 测试，在 vLLM log 中捕获 107002 错误和 `SetThreadAclDevice`/`CopyData`/`CopyD2H` 诊断输出。

### 2.3 诊断日志增强

为定位问题，在以下位置添加了诊断日志：

| 文件 | 修改 | 日志内容 |
|------|------|---------|
| `hybm_data_op_acl_memcpy.cpp:191` | `CopyD2H` 错误日志增加 `curDevice` 和 `stream` | `AclrtMemcpyAsync(D2H) failed, ret:107002 ... curDevice:0 stream:0x...` |
| `hybm_entity_default.cpp:1398` | `SetThreadAclDevice` 从 DEBUG 升为 INFO | `SetThreadAclDevice: deviceId=X success (isSetDevice_ was false, now set)` |
| `hybm_entity_default.cpp:815` | `CopyData` 入口增加设备信息日志 | `CopyData enter: direction=X curDevice=X initDeviceId=X src=... dest=... size=...` |
| `mmc_local_service_default.cpp:432` | `CopyBlob` else 分支增加媒体类型日志 | `CopyBlob: src_media=0(HBM) dst_media=1(DRAM) key=... size=...` |

---

## 3. 修复

### 3.1 第一层修复：SetThreadAclDevice 使用实体级 devId

**问题**：`SetThreadAclDevice` 用进程级 `HybmGetInitDeviceId()` 而非实体级 `options_.devId`，导致跨实体操作时设备绑定错误。

**修改文件**：

| 文件 | 修改 |
|------|------|
| `hybm_entity_default.h:111` | `isSetDevice_` 从 `static thread_local bool` 改为 `static thread_local int32_t`（-1=未绑定，其他=已绑定的 deviceId） |
| `hybm_entity_default.cpp:34` | 初始化改为 `-1` |
| `hybm_entity_default.cpp:1405-1422` | `SetThreadAclDevice` 用 `options_.devId` 替代 `HybmGetInitDeviceId()`，当已绑定设备与当前实体设备不同时重新绑定 |

**验证结果**：修复后 `SetThreadAclDevice` 正确设置实体设备（`entityDevId=0` 与 `curDevice=0` 一致），但 107002 仍存在——`stream_` 在对端进程的设备上创建，async 操作仍失败。

### 3.2 第二层修复：async 失败时 fallback 到同步 AclrtMemcpy

**问题**：`CopyD2H`/`CopyH2D`/`SubmitD2D` 中 `AclrtMemcpyAsync` 失败后直接 return，未执行 fallback。`stream_` 在对端进程的设备上创建，当前进程的 context 与 stream 不匹配导致 107002。同步 `AclrtMemcpy` 不需要 stream，只用当前 device context（已被 `SetThreadAclDevice` 正确设置）。

**修改文件**：

| 文件 | 函数 | 修改 |
|------|------|------|
| `hybm_data_op_acl_memcpy.cpp:191` | `CopyD2H` | async 失败后改为 WARN + fallback 到同步 `AclrtMemcpy`，不再直接 return |
| `hybm_data_op_acl_memcpy.cpp:171` | `CopyH2D` | 同上 |
| `hybm_data_op_acl_memcpy.cpp:123` | `SubmitD2D` | 同上 |

**验证状态**：修复已编译安装，但因 NPU 设备 HBM 被孤儿驱动上下文占用，暂无法运行 vLLM 集成测试验证。待设备恢复后重跑。

### 3.3 验证结果

修复后在 dev4,5 上重跑 vLLM 集成测试 device_sdma 测试，2 passed。vLLM 日志验证 eviction 恢复正常：

| 指标 | 修复前 | 修复后 |
|------|--------|--------|
| `AclrtMemcpyAsync(D2H)` 失败（107002） | 170/135 次 (Test 3.7/3.8) | 16/16 次（仅 WARN 级别） |
| async 失败后行为 | 直接 return，eviction 失败 | fallback 到同步 `AclrtMemcpy` |
| `AclrtMemcpy(D2H fallback)` 失败 | N/A | **0/0** |
| `bm put failed` | 34/38 | **0/0** |
| CopyBlob HBM→DRAM 总数 | 69/65 | 69/68 |
| eviction 成功 | 0 | **69/68（100%）** |

> 表格中斜杠分隔两个 vLLM 日志文件（Test 3.7 MemCache 阶段 / Test 3.8 Prefix cache 阶段），非同一测试的成功/失败计数。

**修复机制**：
- `AclrtMemcpyAsync` 仍返回 107002（`stream_` 在对端进程的设备上创建，跨进程不可用）
- 但 async 失败后不再直接 return，而是 fallback 到同步 `AclrtMemcpy`（不依赖 `stream_`，只用当前 device context）
- 同步 `AclrtMemcpy` 成功完成 HBM→DRAM eviction
- `bm put failed` 从 34/38 降为 0，eviction 成功率从 0% 提升到 100%，DRAM 层恢复可用

---

## 4. 其他已知错误与根因分析

除 107002 外，测试过程中收集到的其他错误/警告。以下内容剥离测试次数等数据，聚焦根因、影响与修复状态。

### 4.1 207000 SDID 查询失败（MemCache 专属，硬件限制）

**错误码**: 207000

**错误消息样本**:
```
WARN [HYBM hybm_mem_segment.cpp:227 InitDeviceInfo] get server id failed: 207000 deviceId: 0 — using boot id fallback
WARN [HYBM hybm_mem_segment.cpp:236 InitDeviceInfo] get super pod id failed: 207000 deviceId: 0 — using invalid fallback
```

**根因**: 310P3 硬件不支持 SDID（Server ID / Super Pod ID）查询，ACL 接口返回 207000。`InitDeviceInfo` 在 `hybm_mem_segment.cpp` 中尝试查询 SDID 用于跨节点寻址，310P3 单卡场景无此能力。

**修复状态**: 已知行为，使用 boot id fallback。`HybmGetServerId()` / `HybmGetSuperPodId()` 在 207000 时分别降级为 boot id 和 invalid 值，不影响同卡 die-pair 内的 SDMA / P2P / host_shm 通信。

**影响**: 无。仅在 MemCache 模式下出现（baseline 无 store 初始化，不触发），属于硬件限制下的预期降级。

### 4.2 EngineDeadError（vLLM 关闭时，良性竞态）

**严重级别**: ERROR

**错误消息样本**:
```
ERROR [async_llm.py:704] AsyncLLM output_handler failed.
ERROR [async_llm.py:704] vllm.v1.engine.exceptions.EngineDeadError: EngineCore encountered an issue.
```

**根因**: vLLM 关闭时，测试框架发送 SIGTERM 终止 EngineCore，而 async output handler 仍在等待输出，引发 `EngineDeadError`。这是 vLLM 强制关闭时的正常竞态条件。

**时序**:
```
1. API server: shutdown triggered
2. Process manager: force killing remaining processes
3. EngineCore: trigger received signal=SIGTERM
4. EngineCore: start mode=abort
5. MPClient: complete
6. AsyncLLM output_handler failed → EngineDeadError  ← 良性错误
7. API server: signalling HTTP server shutdown
```

**修复状态**: 无需修复，vLLM 自身行为。错误发生在测试框架主动 `stop_vllm()` 之后，不影响测试结果与度量数据。

### 4.3 KV Cache 2MB 物理地址对齐警告（已通过 patch 降级）

**严重级别**: WARNING（patch 后）；patch 前为 assert 崩溃

**错误消息样本**:
```
WARNING kv cache tensor ptr 0x12d2885c8000 not 2MB-aligned (storage_key=0x12d2885c8000, new_start=0x12d288400000); skipping alignment adjustment for this region
```

**根因**: vLLM KV cache tensor 指针由 vLLM 显存分配器决定，未保证 2MB 物理对齐。`AscendStoreConnector` 的 `KVPoolWorker._align_kv_ptrs`（`pool_worker.py:758`）将 tensor 起始地址向下 2MB 对齐（memfabric_hybrid 大页要求），当 `new_start < storage_key`（对齐后地址越过 raw tensor 边界）时，原代码 `assert new_start >= storage_key` 崩溃，vLLM 启动失败。

**适配改动**: 通过 vLLM-Ascend 指定分支中的适配实现 将 `pool_worker.py:758` 的 assert 改为 `logger.warning` + `continue`，跳过该 region 的对齐调整，不再阻断启动：

```python
# patch 前
assert new_start >= storage_key, "invalid kv cache tensor, raw tensor ptr must be align to 2MB"
# patch 后
if new_start < storage_key:
    logger.warning("kv cache tensor ptr 0x%x not 2MB-aligned ...; skipping alignment adjustment for this region", ...)
    continue
```

**影响**: 跳过对齐调整的 region 可能影响 SDMA 传输效率（非对齐传输需额外拷贝），但不影响功能正确性。所有 MemCache 测试均出现此 WARN，host_shm 与 device_sdma 一致。

**与 `--no-enable-prefix-caching` 的关系**: **无关**。该问题在启用 `--kv-transfer-config`（MemCache connector）时即触发，是 `register_kv_caches` → `_align_kv_ptrs` 的必经路径，不区分 prefix caching 开关。详见测试报告 3.4.5 节"易混淆澄清"。

### 4.4 HBM Allocator 满（预期 eviction 触发）

**严重级别**: WARN（device_sdma 专属）

**错误消息样本**:
```
warning [MMC mmc_blob_allocator.cpp:59 Alloc] Allocator rank: 1 mediaType: HBM, cap:1069875200/1073741824 cannot allocate with size: 5488640
```

**根因**: HBM 容量接近上限（99.6%），`mmc_blob_allocator.cpp:59` 拒绝分配，触发 `GetNeedEvictList`（`mmc_global_allocator.h:337`，watermark 90%）执行 HBM→DRAM eviction。

**修复状态**: 无需修复，这是两级存储（HBM→DRAM）的正常行为。host_shm 协议无 HBM pool，不触发。

### 4.5 BmUnregister -3000（清理竞态，建议优化）

**严重级别**: ERROR（device_sdma 专属，vLLM 关闭时）

**错误消息样本**:
```
error [MMC mmc_global_allocator.h:201 Stop] location not found, rank: 0, mediaType: HBM
error [MMC mmc_meta_service.cpp:195 BmUnregister] Unmount loc { 0, 0 } failed, error code -3000
```

**根因**: vLLM 关闭时，MetaService 的 heartbeat timeout 先于 `BmUnregister` 清理了 rank 的 HBM location。当 `BmUnregister` 随后执行时已找不到 location，返回 -3000。

**修复状态**: 良性竞态，不影响测试结果（关闭阶段发生）。**建议优化**：调整关闭顺序，`BmUnregister` 在 heartbeat timeout 机制触发前主动执行，避免清理竞态。

### 4.6 HeartBeat Timeout / LinkBroken / Socket 读取失败（关闭时连接断开）

**严重级别**: WARN / ERROR（vLLM 关闭时）

**错误消息样本**:
```
warning [MMC mmc_net_engine_acc.cpp:515 HandleLinkBroken] Old linkId: 3, rankId: 0
warning [MMC mmc_meta_service.cpp:101 operator()] HeartBeat timeout for rank=1, linkId:1, clearing resource
error [AccLink acc_tcp_listener.cpp:195 ProcessNewConnection] Failed to read header from the socket connected from 127.0.0.1:60860
```

**根因**: vLLM 关闭时 worker 进程退出，与 MetaService 的 ACC link 连接断开。`HandleLinkBroken` 检测到链路断开，heartbeat timeout 清理 rank 资源，`acc_tcp_listener` 读取 socket header 失败。

**修复状态**: 无需修复，连接关闭时的正常行为。

---

## 5. 错误严重性分类

| 类别 | 错误 | 严重性 | 影响测试结果 | 需要修复 | 当前状态 |
|------|------|--------|-------------|---------|---------|
| 功能性 | 107002 async memcpy 失败 | WARN | 否（sync fallback 成功） | 已修复 | fallback 机制生效，eviction 100% 成功 |
| 硬件限制 | 207000 SDID 查询失败 | WARN | 否（boot id fallback） | 否 | 硬件限制，预期降级 |
| 关闭竞态 | EngineDeadError | ERROR | 否（关闭后发生） | 否 | vLLM 行为 |
| 对齐 | KV cache 2MB 未对齐 | WARNING | 否（跳过对齐调整） | 已通过 patch 降级 | assert → warning + continue |
| 容量管理 | HBM Allocator 满 | WARN | 否（触发 eviction） | 否 | 预期行为 |
| 清理竞态 | BmUnregister -3000 | ERROR | 否（已清理） | 建议优化 | 清理顺序待优化 |
| 连接断开 | HeartBeat/LinkBroken | WARN | 否（关闭时正常） | 否 | 正常行为 |
| 连接断开 | Socket 读取失败 | ERROR | 否（关闭时正常） | 否 | 正常行为 |

---

## 6. 建议关注项与修复状态

1. **BmUnregister -3000 清理竞态**: 建议优化 MetaService 关闭顺序，`BmUnregister` 在 heartbeat timeout 前主动执行，避免清理竞态。当前为良性错误，不影响功能。

2. **KV cache 2MB 对齐**: 当前通过 `vllm_ascend_310p_kv_transfer.patch` 将 assert 降级为 warning + continue，功能可用但非对齐 region 的 SDMA 传输效率可能受影响。长期建议优化 vLLM 显存分配器或 AscendStoreConnector 的对齐逻辑，从源头保证 KV tensor 2MB 对齐。
