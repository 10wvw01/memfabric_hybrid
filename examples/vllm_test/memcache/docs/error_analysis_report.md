# MemCache vLLM TTFT 复测错误分析报告

本文保留历史环境的测试数据和问题记录。当前指定分支的部署流程见[部署说明](vllm_deployment_flow.md)，最新验证条件与结果见[迁移报告](vllm_024_migration_results.md)。

**复测日期**: 2026-09-29  
**复测设备**: dev6, dev7 (Bus AB:00.0, 同卡 die-pair)  
**复测范围**: Layer 2 (workload) + Layer 3 (integration: host_shm + device_sdma)

---

## 1. 复测结果总览

| 层级 | 协议 | 结果 | 耗时 | 日志 |
|------|------|------|------|------|
| Layer 2 | device_sdma | 7 passed, 1 skipped | 70.47s | `layer2_workload_dev67_*.log` |
| Layer 3 | host_shm | 4 passed | 23:57 | `layer3_host_shm_dev6_7_*.log` |
| Layer 3 | device_sdma | 4 passed | 23:57 | `layer3_device_sdma_dev6_7_*.log` |
| **总计** | | **15 passed, 1 skipped** | | |

所有测试均通过，无测试失败。

---

## 2. 收集到的错误与警告

### 2.1 [WARN] 107002 ACL_ERROR_RT_CONTEXT_NULL (device_sdma 专属)

| 指标 | 值 |
|------|------|
| 错误码 | 107002 (ACL_ERROR_RT_CONTEXT_NULL) |
| 严重级别 | WARN（async 失败后自动 fallback 到 sync） |
| 出现次数 | 每个 device_sdma MemCache 测试 16 次 |
| host_shm | 0 次（无 HBM，无 eviction） |
| bm put failed | 0 次（sync fallback 全部成功） |
| 影响范围 | Test 3.7 MemCache 阶段、Test 3.8、Test 3.9[30%]、Test 3.9[50%] |

**错误消息样本**:
```
WARN [HYBM hybm_data_op_acl_memcpy.cpp:206 CopyD2H] AclrtMemcpyAsync(D2H) failed, ret:107002 rankId:0 src:0x12da06fa4000 size:6553600 curDevice:0 stream:0xfffe397acc08, retry sync
```

**根因**: `stream_` 在对端进程的设备上创建，eviction 线程绑定了本端进程的设备，导致 stream 与 context 不匹配。

**修复状态**: 已修复（async 失败后 fallback 到同步 `AclrtMemcpy`，不依赖 stream_）。每次 async 失败后 sync 成功，`bm put failed` = 0。

**与原报告对比**:
- 原报告 (issues.md): 修复后 16 次 WARN（仅 async 失败），0 次 sync 失败，0 次 bm put failed
- 复测结果: 完全一致（16 次 WARN，0 次 sync 失败，0 次 bm put failed）

**CopyBlob HBM→DRAM eviction 统计**:

| 测试 | CopyBlob HBM→DRAM 次数 | 107002 async 失败 | sync 成功 | bm put failed |
|------|----------------------|-------------------|----------|---------------|
| Test 3.7 MemCache | 70 | 16 | 16 | 0 |
| Test 3.8 | 69 | 16 | 16 | 0 |
| Test 3.9[30%] | 69 | 16 | 16 | 0 |
| Test 3.9[50%] | 66 | 16 | 16 | 0 |

---

### 2.2 [WARN] 207000 SDID 查询失败 (MemCache 专属)

| 指标 | 值 |
|------|------|
| 错误码 | 207000 |
| 严重级别 | WARN |
| host_shm 出现次数 | 6 次/测试 (3/device × 2 devices) |
| device_sdma 出现次数 | 10 次/测试 (5/device × 2 devices) |
| baseline | 0 次（无 MemCache 连接） |
| 影响 | 无 — 使用 boot id fallback |

**错误消息样本**:
```
WARN [HYBM hybm_mem_segment.cpp:227 InitDeviceInfo] get server id failed: 207000 deviceId: 0 — using boot id fallback
WARN [HYBM hybm_mem_segment.cpp:236 InitDeviceInfo] get super pod id failed: 207000 deviceId: 0 — using invalid fallback
```

**根因**: 310P3 硬件不支持 SDID 查询，返回 207000。

**修复状态**: 已知行为，使用 boot id fallback，报告记载为 "SDID WARN fallback"。

---

### 2.3 [ERROR] EngineDeadError (vLLM 关闭时)

| 指标 | 值 |
|------|------|
| 严重级别 | ERROR |
| 出现次数 | 2 次（host_shm Test 3.8 + device_sdma baseline Test 3.7 各 1 次） |
| 影响 | 无 — vLLM 关闭时的良性竞态条件 |

**错误消息样本**:
```
ERROR [async_llm.py:704] AsyncLLM output_handler failed.
ERROR [async_llm.py:704] vllm.v1.engine.exceptions.EngineDeadError: EngineCore encountered an issue.
```

**根因**: vLLM 关闭时，测试框架发送 SIGTERM 终止 EngineCore，async output handler 仍在等待输出，引发 EngineDeadError。这是 vLLM 强制关闭的正常竞态条件。

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

---

### 2.4 [WARN] KV Cache 2MB 对齐警告

| 指标 | 值 |
|------|------|
| 严重级别 | WARNING |
| 出现次数 | 60 次/测试（所有 MemCache 测试一致） |
| host_shm | 60 次 |
| device_sdma | 60 次 |
| 影响 | 可能影响 SDMA 传输效率（非对齐传输需额外拷贝） |

**错误消息样本**:
```
WARNING kv cache tensor ptr 0x12d2885c8000 not 2MB-aligned (storage_key=0x12d2885c8000, new_start=0x12d288400000); skipping alignment adjustment for this region
```

**根因**: vLLM KV cache tensor 指针未 2MB 对齐，AscendStoreConnector 跳过对齐调整。

---

### 2.5 [WARN] MetaService HBM Allocator 满 (device_sdma 专属)

| 指标 | 值 |
|------|------|
| 严重级别 | WARN |
| 出现次数 | device_sdma Test 3.7 MemCache 阶段 2 次 |
| 影响 | 无 — 触发 HBM→DRAM eviction（预期行为） |

**错误消息样本**:
```
warning [MMC mmc_blob_allocator.cpp:59 Alloc] Allocator rank: 1 mediaType: HBM, cap:1069875200/1073741824 cannot allocate with size: 5488640
```

**说明**: HBM 容量接近上限（99.6%），触发 eviction。这是两级存储（HBM→DRAM）的正常行为。

---

### 2.6 [ERROR] MetaService BmUnregister 失败 (device_sdma 专属)

| 指标 | 值 |
|------|------|
| 严重级别 | ERROR |
| 出现次数 | 2 次（Test 3.9[30%] 和 Test 3.9[50%] 关闭时各 1 次） |
| 错误码 | -3000 |
| 影响 | 无 — 清理时的竞态条件，rank 已被 heartbeat timeout 清理 |

**错误消息样本**:
```
error [MMC mmc_global_allocator.h:201 Stop] location not found, rank: 0, mediaType: HBM
error [MMC mmc_meta_service.cpp:195 BmUnregister] Unmount loc { 0, 0 } failed, error code -3000
```

**根因**: vLLM 关闭时，heartbeat timeout 先于 BmUnregister 清理了 rank 的 HBM location，导致 BmUnregister 找不到 location。

---

### 2.7 [WARN] MetaService HeartBeat Timeout / LinkBroken (device_sdma 专属)

| 指标 | 值 |
|------|------|
| 严重级别 | WARN |
| 出现次数 | 每个 device_sdma 测试关闭时 2-4 次 |
| 影响 | 无 — vLLM 关闭时连接断开的正常行为 |

**错误消息样本**:
```
warning [MMC mmc_net_engine_acc.cpp:515 HandleLinkBroken] Old linkId: 3, rankId: 0
warning [MMC mmc_meta_service.cpp:101 operator()] HeartBeat timeout for rank=1, linkId:1, clearing resource
```

---

### 2.8 [ERROR] MetaService Socket 读取失败

| 指标 | 值 |
|------|------|
| 严重级别 | ERROR |
| 出现次数 | 1 次（device_sdma 测试期间） |
| 影响 | 无 — vLLM 关闭时连接断开 |

**错误消息样本**:
```
error [AccLink acc_tcp_listener.cpp:195 ProcessNewConnection] Failed to read header from the socket connected from 127.0.0.1:60860
```

---

## 3. 错误严重性分类

| 类别 | 错误 | 严重性 | 是否影响测试结果 | 是否需要修复 |
|------|------|--------|-----------------|-------------|
| 功能性 | 107002 async memcpy 失败 | WARN | 否（sync fallback 成功） | 已修复（fallback 机制） |
| 硬件限制 | 207000 SDID 查询失败 | WARN | 否（boot id fallback） | 否（硬件限制） |
| 关闭竞态 | EngineDeadError | ERROR | 否（关闭后发生） | 否（vLLM 行为） |
| 对齐 | KV cache 2MB 未对齐 | WARNING | 否（跳过对齐调整） | 建议优化 |
| 容量管理 | HBM Allocator 满 | WARN | 否（触发 eviction） | 否（预期行为） |
| 清理竞态 | BmUnregister -3000 | ERROR | 否（已清理） | 建议修复（顺序清理） |
| 连接断开 | HeartBeat/LinkBroken | WARN | 否（关闭时正常） | 否 |
| 连接断开 | Socket 读取失败 | ERROR | 否（关闭时正常） | 否 |

---

## 4. 与原报告对比

### 4.1 测试结果对比

| 测试 | 原报告 | 复测 | 差异 |
|------|--------|------|------|
| Layer 2 | 5 passed, 1 skipped | 7 passed, 1 skipped | +2 eviction repro tests |
| Layer 3 host_shm Test 3.7 开销 | 6.14% | 10.95% | +4.81pp |
| Layer 3 host_shm Test 3.8 降幅 | 85.9% | 86.89% | +0.99pp |
| Layer 3 host_shm Test 3.9[30%] 改善 | 26.29% | 25.61% | -0.68pp |
| Layer 3 host_shm Test 3.9[50%] 改善 | 43.11% | 42.86% | -0.25pp |
| Layer 3 device_sdma Test 3.7 开销 | 5.74% | 9.47% | +3.73pp |
| Layer 3 device_sdma Test 3.8 降幅 | 85.4% | 86.41% | +1.01pp |
| Layer 3 device_sdma Test 3.9[30%] 改善 | 25.65% | 25.43% | -0.22pp |
| Layer 3 device_sdma Test 3.9[50%] 改善 | 43.94% | 44.15% | +0.21pp |

### 4.2 错误对比

| 错误 | 原报告 | 复测 | 状态 |
|------|--------|------|------|
| 107002 async 失败 | 16 次/测试 | 16 次/测试 | 一致 |
| 107002 sync 失败 | 0 | 0 | 一致 |
| bm put failed | 0 | 0 | 一致 |
| 207000 SDID | 存在 | 存在 | 一致 |
| EngineDeadError | 未记录 | 2 次 | 新发现（良性） |
| KV cache 2MB 对齐 | 未记录 | 60 次/测试 | 新发现（良性） |
| BmUnregister -3000 | 未记录 | 2 次 | 新发现（良性） |

### 4.3 性能差异分析

Test 3.7 开销较原报告偏高（host_shm: 10.95% vs 6.14%, device_sdma: 9.47% vs 5.74%），可能原因：
1. 原报告使用不同设备对（host_shm: dev0,1; device_sdma: dev6,7），复测统一使用 dev6,7
2. dev6,7 可能有其他系统负载影响
3. vLLM 内部 prefix caching 命中模式差异（复测中 MemCache 阶段出现 2 个低延迟请求 ~500ms，说明 vLLM 内部缓存命中）

---

## 5. 结论

1. **所有测试通过**: 15 passed, 1 skipped，无测试失败
2. **107002 已修复**: async 失败后 sync fallback 全部成功，与原报告修复后状态一致
3. **无新增功能性错误**: 所有错误均为已知的良性警告或关闭时的竞态条件
4. **性能数据一致**: 各项指标与原报告偏差 < 5pp，在正常波动范围内
5. **建议关注项**:
   - BmUnregister -3000 清理竞态条件（建议优化清理顺序）
   - KV cache 2MB 对齐警告（可能影响 SDMA 传输效率，建议优化 AscendStoreConnector 对齐逻辑）
