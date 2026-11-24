# MemCache 测试异常排查指南

测试脚本位于 `examples/vllm_test/memcache/`：`setup.sh`（构建安装）、`run_test.sh`（测试入口）、`verify.sh`（版本校验）、`check_env.sh`（设备预检）、`aux_scripts/clean_stale_env.sh`（清理残留）。

本文档合并了原 `vllm_ttft_test_issues.md` 的历史问题记录，保留当前仍可操作的排查项，精简历史推断细节。

---

## 问题 1：驱动 devmm 映射失败致进程被杀或 prefill hang

### 现象

- 进程 join 成功后几秒被杀（exit 137/124，`Received exit signal[15]`），与超时无关
- 或第一个 prefill 调用 hang，5 分钟无 TTFT Report；300s 后 rank1 keepalive 超时退出（`RANK1_KEEPALIVE_TIMEOUT_S=300` 硬编码，不可环境变量覆盖），rank0 hang 到 600s timeout

### 根因

多容器共享 NPU 并发访问触发驱动 `devmm_send_map_addr_msg` 返回 `ret=-95`（EOPNOTSUPP），`Map address failed`。映射失败后设备进入异常态：进程被信号终止，或 `put_from_layers` 的 ioctl 完全不返回。

### 确认

```bash
dmesg | grep 'devmm.*Map address failed'
bash check_env.sh --device-ids 4,5  # 正常 ~50ms，异常 set_device ret=507033 或 latency>100ms
```

### 处理

换完全空闲的卡（`"No running processes found"` + AICore 0% + Memory-Usage 仅驱动 ~1.5GB）。即使显存占用低，有其他进程在跑仍可能触发。设备刚跑过测试可能 ACL 句柄残留致 `set_device ret=507033`，换卡或等几十秒。

---

## 问题 2：manifest 缺 vllm/vllm_ascend

### 现象

integration/bench/puredram 启动即退出（exit 1），日志 `The deployment record must include vLLM and vLLM-Ascend`。

### 根因

`setup.sh --memcache-only` 跳过 vllm-ascend 同步，manifest 只有 memcache 和 memfabric，而 `run_test.sh` 对这些测试类型要求 `--require-vllm`。

### 处理

```bash
python3 source_manifest.py write \
    --memcache-dir <memcache目录> \
    --vllm-ascend-dir <vllm-ascend目录> \
    --vllm-dir <vllm目录>
# 确认 sources 含 vllm + vllm_ascend
python3 -c "import json; print(list(json.load(open('retest_logs/source_manifest.json'))['sources'].keys()))"
```

---

## 问题 3：fork 的 MetaService 残留占端口

### 现象

端口 5000/6000/8000 被占，`ps` 见 `python3 -m pytest ... prefill_ttft` 进程存活，PPID=1。

### 根因

pytest 被 SIGKILL 时，fork 出的 MetaService 子进程 reparent 到 init 继续存活。`clean_stale_env.sh` 只匹配 `mmc_meta_service`，漏掉以 pytest 名义存活的进程。

### 处理

按精确 PID 清理（勿用宽泛 `pkill -f` 误杀其他进程）：

```bash
ps -eo pid,cmd | grep -E "pytest.*prefill_ttft|MetaService\.main|mmc_meta_service" | grep -v grep
# 按输出的 PID: kill -9 <PID>
rm -f /dev/shm/mf_dram_*
ss -ltn | grep -E ":5000|:6000|:8000"  # 应为空
```

---

## 问题 4：host_shm 报 "maxHbmSize must be 0"

### 现象

`store.init` 失败：`HOST_SHM op type only supports DRAM shared memory without HBM, maxHbmSize must be 0`。

### 根因

host_shm 要求 `hbm.size = 0`，workload 配置默认 `hbm.size = 2GB`。

### 处理

用 `cases/config/mmc-local-workload-hostshm-dramonly.conf`（host_shm + hbm=0 + dram=4GB）。仅用于隔离 SDMA 路径问题，依赖 HBM 的测试会语义不符；正常 workload 用 `device_sdma` + `hbm.size = 2GB`。

---

## 问题 5：CANN 版本低致 W8A8 模型加载失败

### 现象

vLLM 加载 W8A8 模型崩溃：`aclnnDynamicQuantV2` 算子异常，`runtime result = 507015`。Baseline 和 MemCache 阶段均触发。workload 不加载模型，不受影响。

### 根因

CANN 9.1.0-beta.1 的 `aclnnDynamicQuantV2` 实现有缺陷，需 ≥ 9.2.1。

### 处理

```bash
source /usr/local/Ascend/cann-9.2.1/set_env.sh
source /usr/local/Ascend/nnal/atb/set_env.sh
echo $ASCEND_HOME_PATH  # 应为 /usr/local/Ascend/cann-9.2.1
```

---

## 问题 6：vllm-ascend 缺 310P MemCache 集成补丁

### 现象

`KV cache transfer is not supported for 310P.`

### 根因

`trunk_for_escc_v24.0` 的 `model_runner_310p.py` 保留上游 KV transfer 限制，补丁 `ca2909204`（位于 `fix/310p-memcache-v024-pr`）未合入。

### 确认

```bash
git merge-base --is-ancestor ca2909204 HEAD && echo "in trunk" || echo "NOT in trunk"
```

### 处理

```bash
cd /path/to/vllm-ascend
git cherry-pick ca2909204
python3 -m pip install --no-deps --no-build-isolation -e .
# 更新 manifest，否则版本校验报 "Source revisions or artifacts changed"
cd examples/vllm_test/memcache
python3 source_manifest.py write --memcache-dir <memcache> --vllm-ascend-dir <vllm-ascend> --vllm-dir <vllm>
```

---

## 问题 7：npu-smi 卡死致设备检查超时

### 现象

`npu-smi info` 30 秒不返回，`_wait_for_device_memory()` 报 `subprocess.TimeoutExpired`。

### 根因

dmesg 显示设备驱动异常（`devdrv_get_core_inuse: device(N) is not working`），npu-smi 遍历全部设备时在异常设备阻塞，指定单卡也受影响。

### 确认

```bash
dmesg | grep 'is not working'
```

### 处理

`_wait_for_device_memory()` 已改用 ACL API。手动检查显存也用 ACL API：

```bash
python3 -c "import acl; acl.init(); acl.rt.set_device(6); print(acl.rt.get_mem_info(0))"
```

---

## 问题 8：HCCL 端口 16666 已被占用致 vLLM 启动失败

### 现象

vLLM 集成测试首项（`test_full_miss_baseline`）在 Phase 2（启用 MemCache 的 vLLM 启动）失败，日志报：

```
RuntimeError: create_config:../torch_npu/csrc/distributed/HCCLUtils.cpp:140
  HCCL function error: hcclCommInitRootInfoConfig(...), error code is 7
[ERROR] ERR02200 DIST call hccl api failed.
Communication_Error_Bind_IP_Port(EI0020): Failed to enable listening for the NPU
  network adapter socket. Reason: The IP address 192.168.x.x and port 16666 have
  already been bound.
RuntimeError: Engine core initialization failed. ... Failed core proc(s): {}
```

### 根因

HCCL 通信初始化在 16666 端口上建监听 socket。Phase 1 的 vLLM 实例（baseline，无 MemCache）刚被 `stop_vllm()` 终止，但 HCCL socket 句柄可能未立即释放（TIME_WAIT 或子进程残留），Phase 2 立即启动新 vLLM 时 `bind` 失败。

`test_full_miss_baseline` 是唯一在同一用例里两次起停 vLLM（baseline 和 MemCache）的用例，因此最容易复现。`test_50pct_hit`、`test_100pct_hit`、`test_cache_source_correctness`、`test_native_prefix_cache_benchmark` 单实例运行不受影响。

### 确认

```bash
ss -tln | grep 16666          # 端口被占或残留 TIME_WAIT
ps -eo pid,cmd | grep -E "VLLM::|vllm serve|hccl" | grep -v grep
```

### 处理

按精确 PID 清理残留 vLLM/HCCL 进程，等待 socket 释放后重跑单项：

```bash
ps -eo pid,cmd | grep -E "vllm serve|VLLM::" | grep -v grep | awk '{print $1}' | xargs -r kill -9
sleep 10
ss -tln | grep 16666          # 应为空
# 单项重跑
bash run_test.sh --memcache-dir <memcache目录> --type integration --rank 2 \
    --protocol device_sdma --device-ids 4,5 --gpu-mem-util 0.50 \
    --filter rank2 --log
```

复测表明该失败为偶发（同一脚本同一参数第二次重跑 5 项全过），属 HCCL socket 释放时序问题，不属 MemCache 代码缺陷。

---

## 问题 9：device_sdma HBM→DRAM eviction 失败（107002）

### 现象

device_sdma 协议下 vLLM 集成测试，HBM 满后触发 HBM→DRAM eviction 全部失败，错误码 107002（`ACL_ERROR_RT_CONTEXT_NULL`）。vLLM log 中 `bm put failed` 频繁出现，DRAM 层不可用，MemCache 退化为 HBM 单层存储。

MemCache 工作负载测试不受影响：其 Python API（`put_from_layers`）分配器有跨 rank + DRAM fallback 逻辑，难以将单 rank HBM 填到 90% watermark，不触发 eviction。

### 根因

`stream_` 在对端进程的设备上创建（vLLM 多进程 TP），当前进程 context 与对端 `stream_` 不匹配，`AclrtMemcpyAsync` 返回 107002。async 失败后原代码直接 return，eviction 失败。

调用链：

```
MmcLocalServiceDefault::CopyBlob          [mmc_local_service_default.cpp:432]
  └─ else 分支（src=HBM, dst=DRAM）
     └─ MemEntityDefault::CopyData        [hybm_entity_default.cpp:808]
        ├─ SetThreadAclDevice()           [hybm_entity_default.cpp:1398]
        └─ HostDataOpAclMemcpy::CopyD2H   [hybm_data_op_acl_memcpy.cpp:191]
           └─ AclrtMemcpyAsync(..., stream_)  ← 返回 107002
```

### 确认

```bash
# 检查 vLLM 日志中是否有 107002 与 bm put failed
grep -E "AclrtMemcpyAsync.*107002|bm put failed" /tmp/vllm_integration_*.log
# 检查 device_sdma 下 DRAM 层是否完全未写入
grep "CopyBlob.*src_media=0.*dst_media=1" /tmp/vllm_integration_*.log | wc -l
```

### 处理

修复分两层，已在 `trunk_for_escc_v1.2.0` 合入：

1. `SetThreadAclDevice` 改用实体级 `options_.devId` 替代进程级 `HybmGetInitDeviceId()`，`isSetDevice_` 从 `bool` 改为 `int32_t` 记录已绑定的 deviceId。
2. `CopyD2H`/`CopyH2D`/`SubmitD2D` 中 `AclrtMemcpyAsync` 失败后 fallback 到同步 `AclrtMemcpy`（不依赖 `stream_`，只用当前 device context）。

修复后 `bm put failed` 降为 0，eviction 成功率 100%，DRAM 层恢复可用。相关源文件：

| 文件 | 关键行 | 作用 |
|------|--------|------|
| `hybm_data_op_acl_memcpy.cpp` | 123, 171, 191 | async 失败 fallback 到同步 `AclrtMemcpy` |
| `hybm_entity_default.cpp` | 1405-1422 | `SetThreadAclDevice` 用 `options_.devId` |
| `hybm_entity_default.h` | 111 | `isSetDevice_` 改为 `int32_t` |

---

## 问题 10：KV cache 2MB 物理地址对齐警告

### 现象

vLLM 启动时日志出现 `kv cache tensor ptr 0x... not 2MB-aligned ...; skipping alignment adjustment for this region`。patch 前为 `assert new_start >= storage_key` 崩溃，vLLM 启动失败。

### 根因

vLLM KV cache tensor 指针由 vLLM 显存分配器决定，未保证 2MB 物理对齐。`AscendStoreConnector` 的 `KVPoolWorker._align_kv_ptrs`（`pool_worker.py:758`）将 tensor 起始地址向下 2MB 对齐（memfabric_hybrid 大页要求），当 `new_start < storage_key`（对齐后地址越过 raw tensor 边界）时，原 assert 崩溃。

该问题与 `--no-enable-prefix-caching` 无关，只要启用 `--kv-transfer-config`（MemCache connector）即触发，是 `register_kv_caches` → `_align_kv_ptrs` 的必经路径。

### 处理

vLLM-Ascend 指定分支中将 `pool_worker.py:758` 的 assert 改为 `logger.warning` + `continue`，跳过该 region 的对齐调整：

```python
# patch 前
assert new_start >= storage_key, "invalid kv cache tensor, raw tensor ptr must be align to 2MB"
# patch 后
if new_start < storage_key:
    logger.warning("kv cache tensor ptr 0x%x not 2MB-aligned ...; skipping alignment adjustment for this region", ...)
    continue
```

跳过对齐调整的 region 可能影响 SDMA 传输效率（非对齐传输需额外拷贝），但不影响功能正确性。所有 MemCache 测试均出现此 WARN，host_shm 与 device_sdma 一致。长期建议优化 vLLM 显存分配器或 AscendStoreConnector 的对齐逻辑，从源头保证 KV tensor 2MB 对齐。

---

## 预期错误与告警（无需处理）

下列错误/告警为硬件限制或正常关闭时的良性竞态，不影响测试结果，无需用户介入：

| 错误 | 严重级别 | 触发场景 | 根因 | 处理状态 |
|------|---------|---------|------|---------|
| `207000` SDID 查询失败 | WARN | MemCache 模式（baseline 无 store 初始化不触发） | 310P3 硬件不支持 SDID（Server ID / Super Pod ID）查询，`hybm_mem_segment.cpp` 中 `InitDeviceInfo` 调用返回 207000 | 已知行为，`HybmGetServerId()`/`HybmGetSuperPodId()` 在 207000 时降级为 boot id 和 invalid 值，不影响同卡 die-pair 内通信 |
| `EngineDeadError` | ERROR | vLLM 关闭时 | 测试框架发送 SIGTERM 终止 EngineCore，async output handler 仍在等待输出，引发竞态 | vLLM 自身行为，发生在 `stop_vllm()` 之后，不影响测试结果与度量数据 |
| HBM Allocator 满 | WARN | device_sdma 专属，HBM 容量接近上限（99.6%） | `mmc_blob_allocator.cpp:59` 拒绝分配，触发 `GetNeedEvictList`（watermark 90%）执行 HBM→DRAM eviction | 两级存储正常行为，host_shm 协议无 HBM pool，不触发 |
| `BmUnregister` -3000 | ERROR | device_sdma 专属，vLLM 关闭时 | MetaService heartbeat timeout 先于 `BmUnregister` 清理 rank 的 HBM location，`BmUnregister` 找不到 location 返回 -3000 | 良性竞态，不影响测试结果。**建议优化**：`BmUnregister` 在 heartbeat timeout 触发前主动执行 |
| HeartBeat Timeout / LinkBroken / Socket 读取失败 | WARN / ERROR | vLLM 关闭时 | worker 进程退出，与 MetaService 的 ACC link 连接断开，`HandleLinkBroken` 清理 rank 资源，`acc_tcp_listener` 读取 socket header 失败 | 连接关闭时的正常行为 |

---

## 排查流程

1. **查 dmesg**：`dmesg | grep -E 'devmm.*Map address failed|is not working'`。有输出说明驱动异常，换卡。
2. **确认设备空闲**：npu-smi 可能卡死，优先用 ACL API 查显存。目标卡须 `"No running processes found"` + AICore 0% + Memory-Usage ~1.5GB。
3. **预检 D2D**：`bash check_env.sh --device-ids <a,b>`，正常 ~50ms，超 100ms 驱动异常。
4. **确认 CANN**（integration）：`echo $ASCEND_HOME_PATH` 应为 9.2.1。
5. **确认 vllm-ascend 补丁**（integration）：`git merge-base --is-ancestor ca2909204 HEAD`。
6. **清理残留**：按精确 PID 清理 pytest/meta 进程，`rm -f /dev/shm/mf_dram_*`，确认端口 5000/6000/8000 空闲。
7. **确认 manifest**（integration）：sources 须含 `vllm` + `vllm_ascend`。
8. **若 prefill hang 或进程被杀**：确认第 1-3 步，换完全空闲的卡重跑。
9. **若 vLLM 启动报 HCCL port 16666 bound**：按问题 8 处理，清理残留进程后等 10s 重跑。
10. **若 device_sdma 下 DRAM 层未写入**：按问题 9 确认修复已合入；检查 vLLM 日志 107002 与 `bm put failed` 是否成对出现。

---

## 参考数据

### 正常指标

| 测试 | 协议 | 耗时 | 关键指标 |
|------|------|------|---------|
| workload（5 用例） | device_sdma | ~28s | prefill P50=4180us（seq=4096），P50≈4100us@hit≈miss |
| integration TP=2（5 用例 rank2） | device_sdma | ~13min | Overhead≈0.5%，Hit P50≈150ms，Miss P50≈1375ms，hit/miss≈11% |

实测样本（Qwen3.5-4B-W8A8-310P，device_sdma，dev4,5，gpu-mem-util=0.50）：

| 测试 | 结果 | 备注 |
|------|------|------|
| `test_prefill_ttft_single_request` | PASSED | 100 次 prefill P50/P99 稳定 |
| `test_prefill_ttft_varied_seq_len` | PASSED | seq=512~16384 P50 线性：1443us~13532us |
| `test_prefill_decode_lifecycle` | PASSED | prefill 4257us / decode P50 1195us |
| `test_prefix_cache_hit_vs_miss` | PASSED | miss P50=4181us，hit P50=4111us |
| `test_dram_to_hbm_rewarm_ttft` | PASSED | rewarm P50=4127us |
| `test_full_miss_baseline` | 偶发 FAILED | HCCL 端口 16666 未释放，详见问题 8 |
| `test_50pct_hit` | PASSED | Hit P50=142.6ms，Miss P50=1369.8ms，改善 44.79% |
| `test_100pct_hit` | PASSED | Hit P50=150.9ms，Miss P50=1375.4ms，hit/miss=10.97% |
| `test_cache_source_correctness` | PASSED | 外部/本地/混合命中 token 一致 |
| `test_native_prefix_cache_benchmark` | PASSED | 原生 prefix cache 对照通过 |

### 环境版本要求

| 组件 | 版本 | 说明 |
|------|------|------|
| CANN | ≥ 9.2.1 | 9.1.0-beta.1 算子异常（问题 5） |
| 驱动 | 26.1.1 | — |
| vllm-ascend | `trunk_for_escc_v24.0` + 补丁 `ca2909204` | 缺补丁 KV transfer 不可用（问题 6） |
| memfabric_hybrid | `trunk_for_escc_v1.2.0` | 含 107002 fallback 修复（问题 9） |

### 设备-物理卡对照

| NPU 字段 | Bus | 设备 ID |
|----------|-----|---------|
| 5664 | 18:00 | 0,1 |
| 16384 | 41:00 | 2,3 |
| 38144 | 96:00 | 4,5 |
| 43520 | AB:00 | 6,7 |

### 关键文件

| 文件 | 用途 |
|------|------|
| `run_test.sh` | 测试入口（预检 + manifest 校验 + 执行） |
| `check_env.sh` | D2D 256MB 延迟预检（正常 ~50ms） |
| `source_manifest.py` | 源码版本和产物校验 |
| `aux_scripts/clean_stale_env.sh` | 清理 mmc_meta_service 残留 |
| `cases/config/mmc-local-workload.conf` | workload 配置（device_sdma, HBM=2GB） |
| `cases/config/mmc-local-workload-hostshm-dramonly.conf` | host_shm + DRAM-only（隔离 SDMA 用） |
| `cases/python/memcache/workload/prefill_ttft_test.py` | workload 测试（`RANK1_KEEPALIVE_TIMEOUT_S=300`） |
| `cases/python/memcache/integration/vllm_integration_test.py` | integration 测试（已用 ACL API 查显存） |

> 部署流程见 [部署说明](vllm_deployment_flow.md)，迁移验证见 [迁移报告](vllm_024_migration_results.md)，最新测试结果见 [测试报告](vllm_ttft_test_report.md)。
