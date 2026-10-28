# MemCache vLLM TTFT 测试问题记录

## 1. device_sdma HBM→DRAM Eviction 107002 错误

### 1.1 问题概述

Layer 3 vLLM 端到端 TTFT 测试（device_sdma 协议）中，HBM→DRAM eviction 全部失败，错误码 107002（`ACL_ERROR_RT_CONTEXT_NULL`）。DRAM 存储层形同虚设，MemCache 实际只有 HBM 单层在用。

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

### 1.4 根因

**`HostDataOpAclMemcpy` 的 `stream_` 在对端进程的设备上创建，但 eviction 线程绑定了本端进程的设备，导致 stream 与 context 不匹配。**

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

#### 关键矛盾

- 所有 34 次 `CopyD2H` 失败：`rankId:1`，`src:0x12da*`（device 0 地址范围），`curDevice:0`
- `SetThreadAclDevice` 用 `HybmGetInitDeviceId()` 设置 device 0（调用方 Worker_TP0 的 initDeviceId）
- 但 `CopyD2H` 使用的 `stream_` 属于 rank1 entity（Worker_TP1 进程），在 device 1 上创建
- device 0 的 context + device 1 的 stream → `AclrtMemcpyAsync` 返回 107002

#### 机制详解

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

### 1.5 为什么 Layer 2 没有此问题

| 维度 | Layer 2 (模拟推理) | Layer 3 (vLLM) |
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
| Layer 3 测试通过 | 无影响 | 测试门限（开销 < 20%、hit/miss < 70%）仍满足 |
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

**实际复现方式**：带诊断日志重跑 Layer 3 vLLM 测试，在 vLLM log 中捕获 107002 错误和 `SetThreadAclDevice`/`CopyData`/`CopyD2H` 诊断输出。

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

**验证状态**：修复已编译安装，但因 NPU 设备 HBM 被孤儿驱动上下文占用，暂无法运行 Layer 3 测试验证。待设备恢复后重跑。

### 3.3 验证结果

修复后在 dev4,5 上重跑 Layer 3 device_sdma 测试，2 passed。vLLM 日志验证 eviction 恢复正常：

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
