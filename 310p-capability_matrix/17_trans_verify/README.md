# 17_trans_verify — TRANS 模式外部内存共享路径验证

## 背景

TRANS 模式注册用户已分配的 `aclrtMalloc` 设备内存进行跨进程传输。当前使用 `HybmDevUserLegacySegment`（runtime IPC：`RtIpcSetMemoryName`/`RtIpcOpenMemory`），310P3 同卡 die import 返回 507899。

## 验证目标

确认哪种 ACL API 路径能处理 `aclrtMalloc` 外部内存在 310P3 上的跨进程共享。

## 测试环境

- 硬件：Atlas 300I Duo (310P3), dev0/dev1 同卡 die 对
- CANN 9.1.0, aarch64
- MPI 双进程, rank0=exporter(dev0), rank1=importer(dev1)

## 三条验证路径

| 路径 | Export API | Import API | 外部内存兼容 |
|------|-----------|-----------|-------------|
| A. ACL IPC | `aclrtIpcMemGetExportKey` | `aclrtIpcMemImportByKey(DEFAULT)` | 直接接受 `aclrtMalloc` 指针 |
| B. VMM | `aclrtMemExportToShareableHandle` | `aclrtMemImportFromShareableHandle(ctx trick)` | 需要 `aclrtDrvMemHandle`（物理句柄） |
| C. VMM 基线 | `aclrtMemExportToShareableHandle` | `aclrtMemImportFromShareableHandle(ctx trick)` | 用 `aclrtMallocPhysical` 分配 |

## 验证结果

```
[17-A] exporter aclrtIpcMemGetExportKey ret=0
[17-A] importer aclrtIpcMemImportByKey(DEFAULT) ret=0 importedMem=0x12c140000000
[17-A] importer D2D read verify PASS
[17-A] *** Path A PASS ***
[17-A] exporter verify back-write PASS

[17-B] SKIPPED — aclrtMalloc has no aclrtDrvMemHandle, VMM export segfaults (expected)
[17-B] CONCLUSION: VMM export requires aclrtMallocPhysical, cannot export aclrtMalloc memory

[17-C] exporter aclrtMallocPhysical ret=0 handle=0xaaaaf4905d80 size=16777216
[17-C] exporter MapMem ret=0 va=0x12c140000000
[17-C] exporter ExportToShareableHandle ret=0 sh=0xba4
[17-C] importer ImportFromShareableHandle(ctx trick) ret=0
[17-C] importer MapMem ret=0 va=0x12c280000000
[17-C] importer D2D read verify PASS
[17-C] *** Path C PASS ***

[17] ===== SUMMARY: pass=2 fail=0 =====
[17] ===== OVERALL: PASS =====
```

## 结论

| 路径 | aclrtMalloc 外部内存 | 310P3 同卡 die | 机制 |
|------|---------------------|---------------|------|
| **A. ACL IPC** | ✅ 可直接导出 | ✅ DEFAULT flag 可用 | `aclrtIpcMemGetExportKey` 直接接受 VA 指针 |
| **B. VMM export** | ❌ 不可用 | N/A | `aclrtMemExportToShareableHandle` 需要 `aclrtDrvMemHandle`，`aclrtMalloc` 不返回句柄 |
| **C. VMM baseline** | N/A（需重新分配） | ✅ ctx trick 可用 | `aclrtMallocPhysical` 提供句柄，但需重新分配内存 |

### 核心发现

1. **ACL IPC (`aclrtIpcMemGetExportKey`/`aclrtIpcMemImportByKey`) 是 TRANS 模式在 310P3 上处理外部 `aclrtMalloc` 内存的正确路径**
   - 直接接受 `aclrtMalloc` 返回的 VA 指针，无需物理内存句柄
   - 同卡 die 对用 DEFAULT flag 即可，不需要 PEER_ACCESS
   - 与能力矩阵 02_ipc_die 测试结果一致

2. **VMM 路径不能直接导出 `aclrtMalloc` 内存**
   - `aclrtMemExportToShareableHandle` 的第一个参数是 `aclrtDrvMemHandle`（物理内存句柄）
   - `aclrtMalloc` 返回的是 VA 指针，没有对应的物理句柄
   - 若用 VMM 路径，需在内部用 `aclrtMallocPhysical` 重新分配内存（破坏 TRANS "注册外部内存"语义）

3. **当前 `HybmDevUserLegacySegment` 失败根因**
   - 使用 `RtIpcSetMemoryName`/`RtIpcOpenMemory`（runtime 层 IPC）
   - runtime 层 IPC import 内部触发 `EnableP2PWithOtherDevice`，同卡 die 被 507899 拒绝
   - ACL 层 IPC (`aclrtIpcMemImportByKey` DEFAULT) 不触发 P2P 前置检查，直接可用

## 生产代码方案建议

TRANS 在 310P3 上应改用 **ACL 层 IPC API**（`aclrtIpcMemGetExportKey`/`aclrtIpcMemImportByKey`），而非当前的 runtime 层 IPC（`RtIpcSetMemoryName`/`RtIpcOpenMemory`）。

具体方案：
1. `HybmDevUserLegacySegment` 的 export 路径：310P3 改用 `aclrtIpcMemGetExportKey`
2. `HybmDevUserLegacySegment` 的 import 路径：310P3 改用 `aclrtIpcMemImportByKey(DEFAULT)`
3. 跨卡场景：310P3 用 `aclrtIpcMemImportByKey(PEER_ACCESS)` flag（自动建立双向 P2P）

这与 BM 模式 VMM segment 使用 ACL API 替代 HAL API 的思路一致。
