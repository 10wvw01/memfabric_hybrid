# hcomm 对 310P Duo 的支持分析

## 1. 310P Duo 支持情况

**是的，hcomm 支持 310P Duo（每卡 2 die，4 卡 = 8 die）场景。**

关键代码：
- `src/legacy/ascend910/framework/communicator/impl/hccl_communicator_attrs_host.cc:70-73`
```cpp
bool HcclCommunicatorAttrs::Is310PDuoCard()
{
    return (Is310P3Common() && (pairLinkInfo_[static_cast<u32>(LinkTypeInServer::HCCS_TYPE)].size() == userRankSize_));
}
```

- `src/legacy/ascend910/algorithm/base/alg_template/temp_alltoallv/alltoallv_for_310p.cc`
  - 专门针对 310P 的 AlltoAllV 实现
  - 支持 8 rank 场景（`userRankSize_ == DUO_RANK_NUM`）

## 2. IPC 使用情况

**hcomm 在 310P 上使用 IPC，但使用 `DEFAULT` flag，不消耗 P2P 槽位。**

关键代码：
- `src/legacy/ascend910/platform/common/adapter/adapter_rts.cc:1251`
```cpp
aclError rtRet = aclrtIpcMemGetExportKey(ptr, ptrMaxLen, name, nameMaxLen, 0UL);  // flag=0 (DEFAULT)
```

- `src/legacy/ascend910/platform/common/adapter/adapter_rts.cc:1324`
```cpp
aclError ret = aclrtIpcMemImportByKey(ptr, name, 0UL);  // flag=0 (DEFAULT)
```

**关键区别**：
- test 07 使用 `ENABLE_PEER_ACCESS` flag，自动触发 P2P 建立，消耗槽位
- hcomm 使用 `DEFAULT` flag，不触发 P2P 建立，不消耗槽位

## 3. P2P 槽位限制的绕过

**hcomm 通过以下方式绕过 P2P 槽位限制：**

### 3.1 不建立 P2P 连接

```cpp
// src/legacy/ascend910/platform/common/p2p_mgmt/p2p_mgmt.cc:50-54
HcclResult P2PMgmt::EnableP2P(uint32_t remoteDevicePhysicID)
{
    if (Is310PDevice()) {
        return HCCL_SUCCESS;  // 310P 不建立 P2P！
    }
    ...
}
```

### 3.2 使用 SDMA 引擎进行跨卡通信

```cpp
// src/legacy/ascend910/platform/common/adapter/adapter_rts.cc:996
aclError ret = aclrtMemcpyAsync(dst, destMax, src, count, rtKind, stream);
```

- `aclrtMemcpyAsync` 在跨卡时使用 SDMA（System DMA）引擎
- SDMA 是硬件 DMA 引擎，不依赖 P2P 连接
- 通过设备虚拟地址（DVA）直接访问远端内存

### 3.3 对称内存使用 VMM/Fabric Handle

```cpp
// src/legacy/ascend910/framework/communicator/impl/symmetric_memory/symmetric_memory.cc:1241-1295
aclrtMemExportToShareableHandleV2(..., ACL_MEM_SHARE_HANDLE_TYPE_FABRIC, ...);
aclrtMemImportFromShareableHandleV2(..., ACL_MEM_SHARE_HANDLE_TYPE_FABRIC, ...);
aclrtMapMem(targetVa, mapSize, 0, importedHandle, 0);
```

- 对称内存场景使用 Fabric Handle（VMM 机制）
- 完全绕过 P2P 槽位限制

## 4. 310P 8 卡场景的 IPC 交换

**是的，hcomm 在 310P 8 卡场景下使用 IPC 交换，但不涉及 P2P 槽位限制。**

### 4.1 IPC 交换流程

```
1. LocalIpcRmaBufferImpl::Init()
   └── MemNameRepository::SetIpcMem(devAddr, size, memName.ipcName, ...)
       └── hrtIpcSetMemoryName() → aclrtIpcMemGetExportKey(..., 0UL)  // DEFAULT flag

2. RemoteIpcRmaBufferImpl::Open()
   └── MemNameRepository::OpenIpcMem(&devAddr, size, memName.ipcName, ...)
       └── hrtIpcOpenMemory() → aclrtIpcMemImportByKey(..., 0UL)  // DEFAULT flag

3. 跨卡数据传输
   └── HcclD2DMemcpyAsync() → aclrtMemcpyAsync()  // SDMA 引擎
```

### 4.2 为什么不涉及 P2P 槽位限制

| 机制 | test 07 | hcomm |
|------|---------|-------|
| IPC flag | `ENABLE_PEER_ACCESS` | `DEFAULT` |
| P2P 建立 | 自动触发 | 不触发（310P 跳过） |
| 跨卡传输 | 依赖 P2P 连接 | SDMA 引擎（不依赖 P2P） |
| 是否消耗 P2P 槽位 | ✅ 是 | ❌ 否 |

## 5. 关键结论

1. **hcomm 支持 310P Duo 8 卡场景**
2. **使用 `DEFAULT` flag 的 IPC**，不消耗 P2P 槽位
3. **310P 设备不建立 P2P 连接**（`Is310PDevice()` 返回 true 时跳过）
4. **跨卡通信使用 SDMA 引擎**，不依赖 P2P 连接
5. **对称内存使用 VMM/Fabric Handle**，完全绕过 P2P 限制

## 6. 对 test 07 的启示

要在 310P Duo 上实现 8 卡全连接，应该：

1. **使用 `DEFAULT` flag** 替代 `ENABLE_PEER_ACCESS`
2. **不显式调用 `aclrtDeviceEnablePeerAccess`**
3. **使用 `aclrtMemcpyAsync`** 进行跨卡 D2D 拷贝（SDMA 引擎）
4. **或者使用 VMM API**（`aclrtMemExportToShareableHandleV2` 等）

**关键代码修改**：
```cpp
// test.c:244-245
// 原代码：
uint32_t flag = die ? ACL_RT_IPC_MEM_IMPORT_FLAG_DEFAULT
                    : ACL_RT_IPC_MEM_IMPORT_FLAG_ENABLE_PEER_ACCESS;

// 修改为：
uint32_t flag = ACL_RT_IPC_MEM_IMPORT_FLAG_DEFAULT;  // 全部使用 DEFAULT
```

**但是**：我们之前的测试表明 `DEFAULT` flag 跨卡会失败（返回 507899）。这说明：
- hcomm 可能在驱动层有特殊处理
- 或者 hcomm 使用了其他机制（如 VMM）来支持跨卡 DEFAULT import
- 需要进一步分析 hcomm 的驱动层实现

## 7. 进一步分析方向

1. **分析 hcomm 的驱动层**：看看 310P 上 `aclrtIpcMemImportByKey` 是否有特殊处理
2. **分析 SDMA 引擎**：看看 SDMA 是如何访问远端内存的
3. **分析 VMM 机制**：看看 hcomm 是否在 310P 上使用了 VMM API
4. **实际测试**：在 310P Duo 上运行 hcomm 的 8 卡测试，验证是否成功
