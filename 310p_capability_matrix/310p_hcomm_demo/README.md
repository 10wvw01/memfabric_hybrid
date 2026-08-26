# 310p_hcomm_demo — 严格复现 hcomm Ascend910 的 IPC 实现

## 测试拓扑

```
Socket 0:
  card0 (Duo): dev0 ←HCCS→ dev1
  card1 (Duo): dev2 ←HCCS→ dev3
           ←── PCIe ──→
```

- 4 rank: rank0=dev0, rank1=dev1, rank2=dev2, rank3=dev3
- die pair: (0,1), (2,3) — HCCS 连接
- 跨卡: 0↔2, 0↔3, 1↔2, 1↔3 — PCIe 连接

## 严格匹配 hcomm Ascend910 (legacy) 的 IPC 实现

| 步骤 | hcomm 实现 | 本测试实现 |
|------|-----------|-----------|
| 内存分配 | `aclrtMalloc(ACL_MEM_MALLOC_HUGE_FIRST)` | ✅ 相同（不是 P2P） |
| 页对齐 | `aclrtPointerGetAttributes` → 向下对齐 | ✅ 相同 |
| Export | `aclrtIpcMemGetExportKey(..., 0UL)` | ✅ flag=0 (DEFAULT) |
| PID 白名单 | `aclrtDeviceGetBareTgid` → `aclrtIpcMemSetImportPid` | ✅ 使用 bare PID |
| 交换 | Socket | MPI_Allgather（替代） |
| Import | `aclrtIpcMemImportByKey(..., 0UL)` | ✅ flag=0 (DEFAULT) |
| 偏移恢复 | `ptr += offset` | ✅ 相同 |
| 传输 | `aclrtMemcpyAsync(D2D)` | ✅ 相同 |
| Cleanup | 无 DisablePeerAccess | ✅ 相同 |

## 测试结果

```
[hcomm] === Link Matrix (row=importer, col=exporter) ===
[hcomm]            p0(d0)     p1(d1)     p2(d2)     p3(d3)   
[hcomm] r0(d0)     ---       OK        FAIL        FAIL     
[hcomm] r1(d1)     OK        ---       FAIL        FAIL     
[hcomm] r2(d2)     FAIL      FAIL      ---         OK       
[hcomm] r3(d3)     FAIL      FAIL      OK          ---      
[hcomm] === Summary ===
[hcomm] Total:      4 OK + 8 FAIL = 12 (expect 12)
[hcomm] Die pair:   4 OK + 0 FAIL = 4
[hcomm] Cross-card: 0 OK + 8 FAIL = 8
```

## 关键发现

### 1. bare PID 与 getpid 不同

```
bare_pid=3186012 (getpid=183580)
```

容器环境中，`getpid()` 返回容器 PID 命名空间中的 PID，而 `aclrtDeviceGetBareTgid()` 返回宿主机真实 PID。

### 2. DEFAULT flag 只能用于同卡 die pair

| 连接类型 | import flag=0 (DEFAULT) | import flag=1 (PEER_ACCESS) |
|---------|------------------------|---------------------------|
| 同卡 die pair (HCCS) | ✅ OK | ❌ FAIL (507899) |
| 跨卡 (PCIe) | ❌ FAIL (507899) | ✅ OK |

### 3. hcomm 在 Atlas 300I Duo 上的跨卡 IPC 应该失败

hcomm 使用 DEFAULT flag 进行 IPC import，但我们的测试表明 DEFAULT flag 在 Atlas 300I Duo 上不支持跨卡 import。

### 4. 可能的解释

1. **hcomm 为 310P 超节点设计**（全 HCCS 连接），不是 Atlas 300I Duo（混合拓扑）
2. **hcomm 在 Atlas 300I Duo 上只使用 die pair 内的通信**（2 rank per card）
3. **hcomm 的 `Is310PDuoCard()` 假设全 HCCS 连接**，在 Atlas 300I Duo 上判断错误

## hcomm 源码关键参考

| 文件 | 行号 | 内容 |
|------|------|------|
| `adapter_rts.cc` | 1244-1271 | `hrtIpcSetMemoryName`: `aclrtIpcMemGetExportKey(..., 0UL)` |
| `adapter_rts.cc` | 1318-1350 | `hrtIpcOpenMemory`: `aclrtIpcMemImportByKey(..., 0UL)` |
| `adapter_rts.cc` | 1357 | `hrtIpcSetMemoryPid`: `aclrtIpcMemSetImportPid` |
| `adapter_rts.cc` | 1408-1461 | `hrtDevMemAlignWithPage`: 页对齐 |
| `adapter_rts.cc` | 2114-2137 | `hrtDeviceGetBareTgid`: `aclrtDeviceGetBareTgid` |
| `mem_name_repository.cc` | 41-163 | `SetIpcMem`: 对齐 + export + PID |
| `mem_name_repository.cc` | 165-228 | `OpenIpcMem`: import + offset |
| `local_ipc_rma_buffer_impl.cc` | 107-123 | `Grant`: bare PID 白名单 |
| `p2p_mgmt.cc` | 52-54 | 310P 跳过 P2P 使能 |
| `adapter_rts.cc` | 1139-1142 | 310P 硬编码返回 HCCS_TYPE |

## 运行

```bash
./run.sh        # 4 NPU, timeout 120s
make && mpirun --allow-run-as-root -np 4 ./test
```
