# Memfabric 单节点 8 卡 GVA 内存池分析

## 核心结论

Memfabric 绕过了 310P Duo 每卡 8 个 P2P 槽位限制，因为它使用了完全不同的机制：
- **VMM API** 替代 `aclrtIpcMemImportByKey`
- **SDMA 硬件 DMA 引擎** 做 D2D 数据拷贝，不依赖 P2P 连接
- **GVA 统一地址空间** 提供全局一致的虚拟地址

## 架构

```
┌─────────────────────────────────────────────────────────────┐
│  API 层: hybm_big_mem.h (C API)                              │
├─────────────────────────────────────────────────────────────┤
│  Entity 层: MemEntityDefault                                 │
├──────────────┬──────────────────┬───────────────────────────┤
│  Segment层    │  DataOperator层   │  Transport层              │
│  (mm/)       │  (data_operation/)│  (transport/)             │
│  GVA管理      │  SDMA/RDMA/URMA  │  RDMA/URMA/HCom           │
├──────────────┴──────────────────┴───────────────────────────┤
│  Driver 层: devmm_ioctl / VMM API (halMem*)                  │
└─────────────────────────────────────────────────────────────┘
```

## 关键机制：VMM API

文件: `src/hybm/csrc/mm/hybm_vmm_based_segment.cpp`

```cpp
HalMemAddressReserve(&base, size, 0, addr, MEM_RSV_TYPE_REMOTE_MAP);
HalMemCreate(&handle, size, &prop, 0);
HalMemMap(addr, size, 0, handle, 0);
HalMemExport(handle, MEM_HANDLE_TYPE_FABRIC, 0, &shareHandle);
HalMemImport(MEM_HANDLE_TYPE_FABRIC, &shareHandle, logicDevId, &importedHandle);
HalMemMap(lva, size, 0, importedHandle, 0);
```

底层加载的函数符号 (`dl_hal_api.cpp` 行 147-155):
- `halMemExport` -> `halMemExportToShareableHandleV2`
- `halMemImport` -> `halMemImportFromShareableHandleV2`

## D2D 数据拷贝：SDMA

文件: `src/hybm/csrc/data_operation/host/hybm_data_op_sdma.cpp`

```cpp
Result HostDataOpSDMA::CopyG2G(void *destVA, const void *srcVA, size_t count, ...)
{
    StreamTask task{};
    InitG2GStreamTask(task, destVA, srcVA, count);  // 直接填 DVA
    auto hStream = HybmStreamManager::GetThreadHybmStream(logicDevId);
    hStream->SubmitTasks(task);
    hStream->Synchronize();
}
```

## GVA 实现

文件: `src/hybm/csrc/mm/hybm_va_manager.cpp`

地址空间布局:
- HBM 区域: 0x100000000000 - 0x180000000000 (8T)
- GVM 池: 0x280040000000 (40T+1G) ~ 40T+1G+128T

每个 rank 占一段: `rank_id * max_size`

## 为什么不需要 P2P 槽位

| 机制 | 需要 P2P | 说明 |
|------|---------|------|
| VMM API | 否 | shareable handle 跨设备映射 |
| SDMA | 否 | 直接使用 DVA，设备 MMU 翻译 |
| Device RDMA | 否 | 网络协议栈，通过 NIC |
| Legacy (devmm ioctl) | 可选 | 仅控制面 |

## BM 场景初始化流程

```
hybm_init() → hybm_create_entity() → hybm_reserve_mem_space()
→ hybm_alloc_local_memory() → hybm_export() → [交换 exInfo]
→ hybm_import() → hybm_mmap()
```

## 关键文件

| 功能 | 文件 |
|------|------|
| VMM Segment | `src/hybm/csrc/mm/hybm_vmm_based_segment.cpp` |
| SDMA | `src/hybm/csrc/data_operation/host/hybm_data_op_sdma.cpp` |
| GVA 管理 | `src/hybm/csrc/mm/hybm_va_manager.cpp` |
| Entity | `src/hybm/csrc/entity/hybm_entity_default.cpp` |
| Driver API | `src/hybm/csrc/driver/hybm_gva.cpp` |
| Reachability | `src/hybm/csrc/mm/hybm_mem_segment.cpp:261-273` |
