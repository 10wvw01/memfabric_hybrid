# 310P3 与原有平台数据路径实现差异分析

**平台**: 310P3 (CANN 9.1.0) vs 910B/910C/950
**分析范围**: SDMA operator、Stream 管理、批量拷贝、内存注册
**日期**: 2026-09-08

---

## 1. 架构根源：平台能力定义

所有差异的根因在 `src/hybm/csrc/common/hybm_define.h:196-216`：

```cpp
enum D2dCopyMode {
    D2D_COPY_SQE = 0,             // HAL SDMA Submission Queue Entry (910B/C, 950)
    D2D_COPY_ACL_ASYNC_ONLY = 1,  // aclrtMemcpyAsync only (310P3)
};

struct HybmPlatformCapability {
    D2dCopyMode d2dCopy = D2D_COPY_SQE;
    bool hostRegister = false;     // HalHostRegister 可用性
    bool sameCardNeedsP2P = false;
    uint32_t p2pSlotsPerCard = 0;
};

static inline HybmPlatformCapability GetPlatformCapability(AscendSocType socType) noexcept {
    switch (socType) {
        case ASCEND_310P3:
            return HybmPlatformCapability{D2D_COPY_ACL_ASYNC_ONLY, false, false, 8};
        default: // 910b, 910c, 950 ...
            return HybmPlatformCapability{D2D_COPY_SQE, true, true, 0};
    }
}
```

| 能力 | 310P3 | 910B/C/950 | 影响 |
|------|-------|-----------|------|
| D2D 拷贝模式 | `ACL_ASYNC_ONLY` | `SQE`（HAL SDMA 队列） | D2D 拷贝用 `AclrtMemcpyAsync` 替代 HAL SDMA SQE |
| Host 注册 | `false` | `true`（`HalHostRegister`） | 无法将 host 内存映射为 device DMA 可达，禁用 paramSpace/BatchCopyExtend/QuantCopy |
| 同卡 P2P | `false`（SVM 模型） | `true` | 310P3 SVM 下 host/device 统一地址空间，无需显式 P2P |

三个能力差异衍生出全部后续实现分歧。

### 1.1 SDMA SQE 在 310P3 上的验证

`D2D_COPY_ACL_ASYNC_ONLY` 是软件设计选择还是硬件/驱动限制？实测验证（2026-09-08）：

**HAL 符号加载**：`libascend_hal.so`（driver/lib64/driver/）中 `halSqCqAllocate`、`halSqTaskSend`、`halResourceIdAlloc` 等符号**存在且可加载**（`DL_LOAD_SYM` mandatory）。代码中 `DlHalApi::LoadLibrary` 对所有 `ASCEND_NPU` 构建无条件调用，310P3 构建时 HAL 符号成功加载。

**运行时调用**：在 310P3 上使用 `rtGetLogicDevIdByUserDevId(0)` 获取 logicDeviceId=0 后，调用 `halSqCqAllocate(logicDevId, ...)` 分配 SQ/CQ 队列对，返回错误码 3（not support）。

**结论**：310P3 的 HAL 驱动**导出了 SDMA SQE 符号但运行时返回 not support**。这是 HAL 驱动层的限制——310P3 硬件不支持 SDMA SQE 队列机制。`D2D_COPY_ACL_ASYNC_ONLY` 是对 HAL 驱动限制的软件适配，而非纯粹的软件设计选择。

验证测试代码：`test/310p3_bounce_test.cpp`（使用裸 HAL API：`halResourceIdAlloc` + `halSqCqAllocate` + 手工构建 `rtStarsMemcpyAsyncSqe_t` + `halSqTaskSend` + `halSqCqQuery` 轮询 CQE）。

---

## 2. SDMA Operator 逐函数差异

文件：`src/hybm/csrc/data_operation/host/hybm_data_op_sdma.cpp`

### 2.1 Initialize — paramSpace 初始化（`sdma.cpp:76-111`）

**原有实现（910B/C，aarch64）**：

```cpp
// 1. 分配 64MB host buffer
AclrtMallocHost(&paramSpace_, HYBM_PARAM_SPACE_SIZE);  // 64MB

// 2. HalHostRegister：将 host 内存映射到 device DMA 可达地址空间
DlHalApi::HalHostRegister(paramSpace_, HYBM_PARAM_SPACE_SIZE, HOST_MEM_MAP_DEV,
                          HybmGetInitedLogicDeviceId(), &output);

// 3. 初始化 170 个原子计数器（位于 paramSpace 末尾 256K 元数据区）
auto mask = reinterpret_cast<uint64_t*>(paramSpace_ + HYBM_PARAM_SPACE_META_OFFSET);
for (uint32_t i = 0; i < HYBM_PARAM_SPACE_CAP; i++) {
    __atomic_store_n(mask + i * HYBM_PARAM_META_IDX_BASE, HYBM_EXTEND_CONCURRENT, __ATOMIC_RELEASE);
}

// 4. 记录 device 侧偏移（HalHostRegister 返回的 device 地址 - host 地址）
paramOffset_ = reinterpret_cast<uint64_t>(output) - reinterpret_cast<uint64_t>(paramSpace_);
```

paramSpace 的作用：AICore 侧批量描述符拷贝机制的核心。将多个 (src, dst, len) 三元组写入 paramSpace 的 host 区域，device 通过 `paramOffset_` 偏移读取同一块内存（`HalHostRegister` 建立的 device 可达映射），一次 SDMA 提交完成多段拷贝。原子计数器用于并发槽位管理（最多 170 个并发 batch，每个最多 16K 个描述符）。

**310P3 实现**：

```cpp
// paramSpace_ 保持 nullptr，paramOffset_=0，paramSpaceIdx_=0
BM_LOG_WARN("A5, x86 or 310P3 not support batch extend copy now!");
```

直接跳过整个初始化块。`paramSpace_=nullptr` 导致：
- `BatchCopyExtend`：`BM_VALIDATE_RETURN(paramSpace_ != nullptr, ...)` → 返回 `BM_ERROR`
- `QuantCopy`：同上 → 返回 `BM_ERROR`
- `CopyG2G`/`CopyG2GAsync` 中 `COPY_EXTEND_FLAG` 路径：返回 `BM_NOT_SUPPORTED`

**差异原因**：310P3 的 `hostRegister=false`。ACL VMM API 不支持 `HalHostRegister` 等价的 host→device DMA 映射。没有这个映射，device 无法通过 DMA 读取 host 侧的 paramSpace 描述符，整个 AICore 侧批量拷贝机制不可用。

### 2.2 CopyLH2GD — host→HBM GVA（H2D，`sdma.cpp:217-256`）

**原有实现（910B/C）**：三段式 bounce buffer

```cpp
// 1. 分配临时 device buffer
void *copyDevice;
AclrtMalloc(&copyDevice, count, 0);

// 2. H2D 到临时 buffer（ACL 同步 memcpy）
AclrtMemcpy(copyDevice, count, hostAddr, count, ACL_MEMCPY_HOST_TO_DEVICE);

// 3. SDMA SQE 搬运到目标 GVA
CopyG2G(gvaAddr, copyDevice, count, 0, nullptr);
//   ↳ InitG2GStreamTask(task, gvaAddr, copyDevice, count)
//   ↳ hStream->SubmitTasks(task)     // HalSqTaskSend
//   ↳ hStream->Synchronize()         // 轮询 CQE

// 4. 释放临时 buffer
AclrtFree(copyDevice);
```

为什么需要 bounce：原有平台上 GVA 是 fabric 地址，ACL `AclrtMemcpy(H2D)` 无法直接写入 GVA 空间（ACL 只认 device VA，不认 fabric GVA）。必须先写入本地 device VA（temp buffer），再通过 SDMA SQE 搬运到 GVA 空间。每次拷贝需要一次 `AclrtMalloc` + 一次 H2D memcpy + 一次 SDMA SQE + 一次 `AclrtFree`。

**310P3 实现**：直接 ACL async memcpy

```cpp
void *st = (stream != nullptr) ? stream : HybmStreamManager::GetThreadAclStream();
auto ret = AclrtMemcpyAsync(gvaAddr, count, hostAddr, count, ACL_MEMCPY_HOST_TO_DEVICE, st);
if (ret != 0 || AclrtSynchronizeStream(st) != 0) {
    // fallback：reset stream + 同步重试
    if (stream == nullptr) { HybmStreamManager::ResetThreadAclStream(); }
    ret = AclrtMemcpy(gvaAddr, count, hostAddr, count, ACL_MEMCPY_HOST_TO_DEVICE);
}
```

310P3 不需要 bounce：SVM（Shared Virtual Memory）使 host VA 和 device GVA 处于统一地址空间，ACL `AclrtMemcpyAsync(H2D)` 可以直接写入 GVA 地址。消除了 `AclrtMalloc`/`AclrtFree` 开销和临时 buffer 的额外拷贝。

**差异总结**：

| 维度 | 原有（910B/C） | 310P3 |
|------|---------------|-------|
| DMA 调用次数 | 2 次（H2D 到 temp + SDMA 到 GVA） | 1 次（H2D 直接到 GVA） |
| 内存分配 | 每次 `AclrtMalloc`/`AclrtFree` | 无 |
| 地址空间 | GVA 非 ACL 可达，需 bounce | SVM 下 GVA 对 ACL 直接可达 |
| 同步机制 | SDMA SQE + CQE 轮询 | `AclrtSynchronizeStream` |
| 故障恢复 | CQE 错误码上报 | `ResetThreadAclStream` + 同步重试 |

### 2.3 CopyGD2LH — HBM GVA→host（D2H，`sdma.cpp:258-297`）

**原有实现**：三段式 bounce（与 H2D 对称）

```cpp
AclrtMalloc(&copyDevice, count, 0);
CopyG2G(copyDevice, gvaAddr, count, 0, nullptr);  // SDMA GVA → temp device
AclrtMemcpy(hostAddr, count, copyDevice, count, ACL_MEMCPY_DEVICE_TO_HOST);  // temp → host
AclrtFree(copyDevice);
```

**310P3 实现**：

```cpp
if (stream == nullptr) {
    // 无 stream 时直接同步 memcpy
    AclrtMemcpy(hostAddr, count, gvaAddr, count, ACL_MEMCPY_DEVICE_TO_HOST);
} else {
    // 有 stream 时 async + sync + fallback
    AclrtMemcpyAsync(hostAddr, count, gvaAddr, count, ACL_MEMCPY_DEVICE_TO_HOST, stream);
    if (ret != 0 || AclrtSynchronizeStream(stream) != 0) {
        HybmStreamManager::ResetThreadAclStream();
        AclrtMemcpy(hostAddr, count, gvaAddr, count, ACL_MEMCPY_DEVICE_TO_HOST);
    }
}
```

注意 310P3 的 D2H 有一个 H2D 没有的细节：当 `stream==nullptr` 时直接走同步 `AclrtMemcpy`（不经过 async + sync），而 H2D 即使 `stream==nullptr` 也走 async 路径（使用 `GetThreadAclStream`）。原因见后续带宽分析——D2H 的 `CopyGD2LH` 实际只被 RD2H 路径调用，而 D2H 路径在 dispatcher 中被路由到 `BatchCopyG2G`（D2D async）。

### 2.4 CopyLH2GH / CopyGH2LH — host↔DRAM GVA（`sdma.cpp:425-501`）

与 CopyLH2GD/CopyGD2LH 完全对称的差异模式：

| | 原有（910B/C） | 310P3 |
|---|---|---|
| CopyLH2GH (host→DRAM GVA) | `AclrtMalloc` + H2D + `CopyG2G` SDMA + `AclrtFree` | `AclrtMemcpyAsync(H2D)` 直接到 DRAM GVA |
| CopyGH2LH (DRAM GVA→host) | `CopyG2G` SDMA + D2H + `AclrtFree` | `AclrtMemcpyAsync(D2H)` 直接从 DRAM GVA |

310P3 SVM 下 DRAM GVA（通过 `shm_open` + `mmap(MAP_FIXED)` 映射的 host 内存）对 ACL memcpy 直接可达。原有平台需要 `HalHostRegister` 将 DRAM pool 注册为 device DMA 可达，再通过 SDMA SQE 搬运。

### 2.5 CopyG2G — device 间（`sdma.cpp:619-660`）

**原有实现（910B/C）**：HAL SDMA SQE

```cpp
// 1. 构建 SDMA SQE
StreamTask task{};
InitG2GStreamTask(task, destVA, srcVA, count);
//   ↳ 填充 rtStarsMemcpyAsyncSqe_t 结构体：
//     task.type = STREAM_TASK_TYPE_SDMA
//     sqe->header.type = RT_STARS_SQE_TYPE_SDMA
//     sqe->header.wr_cqe = hStream->GetWqeFlag()
//     sqe->header.rt_stream_id = hStream->GetId()
//     sqe->sssv = 1, sqe->dssv = 1, sqe->sns = 1, sqe->dns = 1
//     sqe->qos = 6
//     sqe->length = count
//     sqe->src_addr_low/high = srcVA 拆分
//     sqe->dst_addr_low/high = destVA 拆分

// 2. 提交到 HAL SDMA 队列
auto hStream = HybmStreamManager::GetThreadHybmStream(devId);
hStream->SubmitTasks(task);
//   ↳ HalSqTaskSend(deviceId, &info)
//   ↳ SQE 写入 SQ 队列尾部，DMA 引擎异步消费

// 3. 轮询 CQE 等待完成
hStream->Synchronize();
//   ↳ while (sqHead_ != sqTail_) {
//       HalSqCqQuery() → 查询 SQ head 和 CQE status
//       if (CQE ready) HalCqReportRecv() → 取回 CQE，解析错误码
//     }
```

950 平台使用不同的 SQE 格式（`InitG2GStreamTaskV2`）：
```cpp
// task.type = STREAM_TASK_TYPE_DAVID_SDMA
// RtDavidStarsMemcpySqeT *sqe = &task.sqe.davidMemcpySqe;
// sqe->kernelCredit = RT_STARS_DEFAULT_KERNEL_CREDIT_DAVID
// sqe->u.strideMode0.dstStreamId / dstSubStreamId 等不同字段布局
```

**310P3 实现**：ACL async memcpy

```cpp
void *st = (stream != nullptr) ? stream : HybmStreamManager::GetThreadAclStream();
auto ret = AclrtMemcpyAsync(destVA, count, srcVA, count, ACL_MEMCPY_DEVICE_TO_DEVICE, st);
if (ret != 0 || AclrtSynchronizeStream(st) != 0) {
    if (stream == nullptr) { HybmStreamManager::ResetThreadAclStream(); }
    ret = AclrtMemcpy(destVA, count, srcVA, count, ACL_MEMCPY_DEVICE_TO_DEVICE);
}
```

**差异总结**：

| 维度 | 原有（910B/C） | 310P3 |
|------|---------------|-------|
| DMA 提交方式 | 手工构建 64B SDMA SQE → `HalSqTaskSend` | `AclrtMemcpyAsync(D2D)` |
| 队列管理 | 自管理 SQ/CQ（`HalSqCqAllocate`），深度 `HYBM_SQCQ_DEPTH` | ACL runtime 管理 |
| 同步方式 | `HalSqCqQuery` 轮询 + `HalCqReportRecv` 取回 CQE | `AclrtSynchronizeStream` |
| 错误处理 | CQE 错误码（16 种 SDMA 错误类型，如 "transfer page error"、"DAW address error" 等） | `AclrtMemcpyAsync` 返回码 |
| SQE 格式 | 910B/C: `rtStarsMemcpyAsyncSqe_t`（Milan）；950: `RtDavidStarsMemcpySqeT`（David） | 无 SQE |
| 大小限制 | 910B/C: `count > UINT32_MAX` 拒绝；950: 支持 64 位长度 | ACL 内部处理 |

### 2.6 CopyG2GAsync — 异步 D2D（`sdma.cpp:662-694`）

四种子路径：

| 条件 | 原有（910B/C） | 310P3 |
|------|---------------|-------|
| 用户 stream + `COPY_EXTEND_FLAG` | `HybmCopyExtend(srcVA, destVA, count, CONCURRENT, stream)` | `BM_NOT_SUPPORTED`（paramSpace_ null） |
| 用户 stream，无 flag | `RtMemcpyAsync(D2D, stream)` — HAL runtime async | `AclrtMemcpyAsync(D2D, stream)` — ACL async |
| 无 stream，无 flag | 构建 SDMA SQE → `HalSqTaskSend`（不 sync） | `AclrtMemcpyAsync(D2D, GetThreadAclStream())`（不 sync） |

`RtMemcpyAsync` vs `AclrtMemcpyAsync`：前者使用 `RT_MEMCPY_*` 枚举，由 HAL runtime 层处理，配合 HAL stream 使用；后者使用 `ACL_MEMCPY_*` 枚举，由 ACL runtime 层处理，配合 ACL stream 使用。

### 2.7 Wait — 同步等待（`sdma.cpp:404-413`）

```cpp
// 310P3：no-op
if (GetAscendSocType() == ASCEND_310P3) {
    return BM_OK;  // 每次拷贝已 inline sync
}

// 原有：轮询 HybmStream CQE
auto hStream = HybmStreamManager::GetThreadHybmStream(devId);
return hStream->Synchronize();
```

**差异原因**：310P3 的 `CopyLH2GD`/`CopyG2G` 等每次 async 后立即 `AclrtSynchronizeStream`（inline sync），调用 `Wait` 时所有操作已完成。原有平台通过 `CopyG2GAsync` 只提交 SQE 不 sync，由 `Wait` 统一轮询 CQE 完成同步——这正是原有平台 D2D pipeline 的基础。

### 2.8 CleanUp — 资源清理（`sdma.cpp:415-423`）

```cpp
// 310P3：跳过
if (GetAscendSocType() != ASCEND_310P3) {
    HybmStreamManager::DestroyAllThreadHybmStream();
}
```

310P3 从未创建 HybmStream，`DestroyAllThreadHybmStream` 可能误删其他 BM entity 共享的 stream（生命周期测试场景），因此跳过。

### 2.9 TransformVa — 地址转换（`sdma.cpp:388-402`）

**平台无关**，无 310P3 分支。通过方向查找表（`HybmSdmaDirectionSrcMemType`/`DestMemType`）确定 src/dst 的内存类型（HOST/DEVICE），再调用 `HybmVaManager::TransformVa` 将 GVA 转换为 HVA（host VA）或 DVA（device VA）。

---

## 3. Stream 管理差异

文件：`src/hybm/csrc/ts_engine/hybm_stream_manager.cpp` + `hybm_stream.cpp`

### 3.1 两种完全不同的 Stream 类型

| 维度 | HybmStream（原有） | AclrtStream（310P3） |
|------|-------------------|---------------------|
| 分配方式 | `HalSqCqAllocate`：分配 SQ + CQ 队列对 | `AclrtCreateStreamWithConfig(FAST_LAUNCH \| FAST_SYNC)` |
| 队列深度 | `HYBM_SQCQ_DEPTH`（固定深度环形队列） | ACL runtime 内部管理 |
| 提交方式 | `HalSqTaskSend`：将 64B SQE 写入 SQ 尾部 | `AclrtMemcpyAsync`：ACL runtime 内部提交 |
| 同步方式 | `HalSqCqQuery` 轮询 SQ head + CQE status → `HalCqReportRecv` 取回 CQE | `AclrtSynchronizeStream`：ACL runtime 内部同步 |
| 作用域 | 进程级 `unordered_map<TID, HybmStreamPtr>`（共享读写锁） | thread_local 单例 |
| 销毁 | `DestroyAllThreadHybmStream()` 清空全局 map | `ResetThreadAclStream()` 销毁重建 thread_local 实例 |
| 错误诊断 | CQE 错误码（16 种 SDMA 错误类型） | ACL 返回码 |

### 3.2 HybmStream 初始化流程（原有，`hybm_stream.cpp:29-50`）

```cpp
// 1. 分配 stream ID
HalResourceIdAlloc(deviceId, DRV_STREAM_ID, ...) → streamId_

// 2. 分配 SQ/CQ 队列对
HalSqCqAllocate(deviceId, &input, &output) → sqId_, cqId_
//   input.sqeSize = 64, cqeSize = 32
//   input.sqeDepth = HYBM_SQCQ_DEPTH, cqeDepth = HYBM_SQCQ_DEPTH
//   950 额外：InitAscend950Stack → 分配 SIMT 栈内存

// 3. 分配逻辑 CQ
HalResourceIdAlloc(deviceId, DRV_LOGIC_CQ_ID, ...) → logicCq_

// 4. 初始化任务列表
taskList_.resize(HYBM_SQCQ_DEPTH);
```

### 3.3 HybmStream 提交流程（原有，`hybm_stream.cpp:254-287`）

```cpp
// 1. 检查 SQ 是否满
if ((sqTail_ + 1) % HYBM_SQCQ_DEPTH == sqHead_) {
    Synchronize(sqHead_);  // 队列满时先同步
}

// 2. 将 StreamTask 写入 taskList_ 对应槽位
taskList_[taskId] = task;
taskList_[taskId].sqe.memcpyAsyncSqe.header.task_id = taskId;

// 3. HalSqTaskSend 提交 SQE 到硬件 SQ
halTaskSendInfo info{};
info.sqe_addr = &taskList_[taskId].sqe;
info.sqe_num = 1;
info.sqId = sqId_;
HalSqTaskSend(deviceId, &info);

// 4. 移动尾指针
sqTail_ = (sqTail_ + 1) % HYBM_SQCQ_DEPTH;
```

### 3.4 HybmStream 同步流程（原有，`hybm_stream.cpp:403-440`）

```cpp
while (sqHead_ != sqTail_ && retry < MAX_RETRY) {
    // 1. 查询 SQ head 位置
    GetSqHead(head);  // HalSqCqQuery(DRV_SQCQ_PROP_SQ_HEAD)

    // 2. 检查 CQE 是否就绪
    if (!GetCqeStatus()) {  // HalSqCqQuery(DRV_SQCQ_PROP_SQ_CQE_STATUS)
        // 无 CQE：SQ head 前移表示任务完成
        sqHead_ = head;
    } else {
        // 有 CQE：取回 CQE 报告
        ReceiveCqe(lastTask);  // HalCqReportRecv
        // 解析 CQE 错误码（16 种 SDMA 错误类型）
        sqHead_ = (lastTask + 1) % HYBM_SQCQ_DEPTH;
    }
    usleep(1);  // 1us 轮询间隔
}
```

### 3.5 AclrtStream 初始化（310P3，`hybm_stream_manager.cpp:85-103`）

```cpp
static thread_local AclrtStream aclrtStream_;  // RAII 包装
if (aclrtStream_.Get() != nullptr) { return aclrtStream_.Get(); }

#if defined(ASCEND_NPU)
AclrtCreateStreamWithConfig(&stream, 0,
    ACL_STREAM_FAST_LAUNCH | ACL_STREAM_FAST_SYNC);
aclrtStream_.Reset(stream);
#endif
```

`ACL_STREAM_FAST_LAUNCH`（=1）：快速提交模式，减少提交路径开销。
`ACL_STREAM_FAST_SYNC`（=2）：快速同步模式，减少同步路径开销。

`#if defined(ASCEND_NPU)` 编译时开关：310P3 构建必须定义 `ASCEND_NPU`（对应 `XPU_TYPE=NPU`），否则 `GetThreadAclStream` 返回 nullptr，所有拷贝函数的 `st != nullptr` 断言失败。

### 3.6 AclrtStream 故障恢复（310P3，`hybm_stream_manager.cpp:105-113`）

```cpp
void HybmStreamManager::ResetThreadAclStream() {
    static thread_local AclrtStream aclrtStream_;
    if (aclrtStream_.Get() == nullptr) { return; }
    AclrtDestroyStream(aclrtStream_.Get());
    aclrtStream_.Reset(nullptr);  // 下次 GetThreadAclStream 时重建
}
```

async 失败后调用：销毁当前 thread-local ACL stream → 下次 `GetThreadAclStream` 创建新 stream → 同步 `AclrtMemcpy` 重试。原有平台的故障恢复依赖 CQE 错误码上报，无 stream 重建机制。

---

## 4. 批量拷贝差异

### 4.1 BatchCopyG2G — D2D 批量（`sdma.cpp:834-874`）

**两个平台共享同一函数体**，但内部调用的 `CopyG2GAsync` 走不同路径：

```cpp
// 合并连续段
for (auto i = 0U; i < batchSize; i++) {
    if (len > 0 && src + len == srcAddr && dest + len == destAddr) {
        len += count;  // 合并到当前段
        continue;
    }
    if (len > 0) { asyncFunc(); }  // 提交前一段
    src = srcAddr; dest = destAddr; len = count;
}
asyncFunc();  // 提交最后一段
return InnerWait(options, 0);
```

- **原有**：`asyncFunc` → `CopyG2GAsync` → `RtMemcpyAsync` 或 SDMA SQE submit（不 sync）→ `InnerWait` → `Wait` → `HybmStream::Synchronize`
- **310P3**：`asyncFunc` → `CopyG2GAsync` → `AclrtMemcpyAsync`（不 sync）→ `InnerWait` → `Wait` → no-op（因为 `CopyG2GAsync` 在 310P3 上也只提交不 sync）

**关键差异**：原有平台的 `BatchCopyG2G` 利用 SQE 队列实现 pipeline——多个 SDMA SQE 依次提交到 SQ，DMA 引擎内部多阶段重叠执行，最后一次 `Synchronize` 统一等待。310P3 的 `AclrtMemcpyAsync` 也支持批量提交 + 统一 sync，实测 D2H bs=8 时 pipeline 效果显著（153→1039 GB/s）。

### 4.2 BatchCopyExtend — AICore 侧批量描述符拷贝（`sdma.cpp:740-781`）

**原有实现**（仅 910B/C aarch64）：

```cpp
// 1. 每 16K 个描述符为一批
taskNum = (batchSize + 16K - 1) / 16K;

for (each batch) {
    // 2. 获取 paramSpace 槽位（原子 CAS 获取 mask 计数器）
    spaceId = TryGetOneParamSpace(&maskPtr);
    //   ↳ 遍历 170 个原子计数器，CAS(HYBM_EXTEND_CONCURRENT → 0)

    // 3. 将 (src, dst, len) 三元组写入 paramSpace
    tmpParam = paramSpace_ + spaceId * 384K;
    for (each item) {
        tmpParam[j++] = src;  tmpParam[j++] = dst;  tmpParam[j++] = len;
    }

    // 4. 提交到 AICore（device 通过 paramOffset_ 读取 paramSpace）
    remoteAddr = tmpParam + paramOffset_;  // device 侧地址
    HybmBatchCopyExtend(remoteAddr, batchSize, maskPtr + paramOffset_,
                        HYBM_EXTEND_CONCURRENT, stream);
}

// 5. 统一同步
AclrtSynchronizeStream(st);
```

这是原有平台的高级批量机制：一次提交最多 16K 个拷贝描述符，AICore 侧并发执行（concurrent=32），远比逐个 SDMA SQE 高效。

**310P3**：`paramSpace_=nullptr` → `BM_VALIDATE_RETURN` 直接返回 `BM_ERROR`，不可用。

### 4.3 BatchCopyLH2GD/GD2LH/LH2GH/GH2LH — H2D/D2H 批量（`sdma.cpp:940-999`）

**两个平台共享同一函数体**——简单串行循环调用单拷贝函数：

```cpp
for (auto i = 0U; i < batchSize; i++) {
    ret = CopyLH2GD(gvaAddrs[i], hostAddrs[i], counts[i], stream);
    if (ret != 0) { return ret; }
}
```

实际调用的 `CopyLH2GD` 内部走 310P3 或原有路径，继承各自的特性和限制。

### 4.4 两种批量机制对比

| 维度 | BatchCopyG2G（两平台都有） | BatchCopyExtend（仅原有） |
|------|--------------------------|--------------------------|
| 原理 | 合并连续段 + 逐段 async 提交 + 统一 sync | paramSpace 描述符 + AICore 侧并发执行 |
| 最大批量 | 无限制（逐段提交） | 16K 描述符/批，170 批并发 |
| 310P3 可用 | 是（`AclrtMemcpyAsync` 批量提交） | 否（`paramSpace_=nullptr`） |
| Pipeline 效果 | 有（D2D 多阶段重叠） | 有（AICore 并发 32） |
| 适用方向 | D2D 类方向 | D2D 类方向（需 `COPY_EXTEND_FLAG`） |

---

## 5. 无 310P3 特化的 Operator

| Operator | 文件 | 原因 |
|----------|------|------|
| `DataOpDeviceRDMA` | `hybm_data_op_device_rdma.cpp` | 基于 `transportManager_`（HCOM RDMA）+ `AclrtMemcpy` 本地拷贝，不使用 HAL SDMA |
| `DataOpDeviceURMA` | `hybm_data_op_device_urma.cpp` | 基于 `transportManager_`（HCOM URMA）+ `AclrtMemcpy`，不使用 HAL SDMA |
| `HostDataOpHostShm` | `hybm_data_op_host_shm.cpp` | 基于 `DlHybridApi::Memcpy`（H2H/D2H/H2D），纯 ACL 层 |
| `HostDataOpRDMA` | `hybm_data_op_host_rdma.cpp` | 基于 `transportManager_` + `DlHybridApi::Memcpy` + `mmap` swap |

这四个 operator 全部基于 ACL API 或 transport 层，从不构建 SDMA SQE 或调用 `HalHostRegister`，因此 310P3 与原有平台行为一致。

---

## 6. 内存注册与地址空间差异

### 6.1 paramSpace 机制对比

```
原有平台（910B/C）：
  Host 内存                         Device DMA 空间
  ┌──────────────┐                 ┌──────────────┐
  │ paramSpace_  │ ──HalHostRegister──> │ output (device VA) │
  │  64MB        │                 │  = paramSpace_ +     │
  │  ┌─────────┐ │                 │  paramOffset_        │
  │  │170 槽位 │ │                 │                      │
  │  │每槽384K │ │                 │  Device 通过此地址    │
  │  │16K描述符│ │                 │  读取 host 侧描述符   │
  │  └─────────┘ │                 │                      │
  │  ┌─────────┐ │                 │                      │
  │  │元数据区 │ │                 │                      │
  │  │170原子  │ │                 │                      │
  │  │计数器   │ │                 │                      │
  │  └─────────┘ │                 │                      │
  └──────────────┘                 └──────────────┘

310P3：
  Host 内存                         Device DMA 空间
  ┌──────────────┐                 ┌──────────────┐
  │  (无)        │                 │  (无)        │
  │  paramSpace_ │                 │  无 device   │
  │  = nullptr   │                 │  可达映射    │
  └──────────────┘                 └──────────────┘
  
  替代方案：SVM 统一地址空间
  Host VA 和 Device GVA 在同一地址空间，
  ACL memcpy 可直接访问 GVA，无需 HalHostRegister
```

### 6.2 H2D/D2H 地址可达性对比

```
原有平台（910B/C）：

  Host VA ──────── ACL H2D ────────> Device VA (local)
     │                                  │
     │                                  ├── CopyG2G SDMA SQE ──> GVA (fabric)
     │                                  │
     │  HalHostRegister                  │
     └───────────────────────────────> GVA (DRAM pool, device DMA 可达)

  GVA 非 ACL 可达，必须通过 bounce buffer 或 HalHostRegister

310P3：

  Host VA ──────── ACL H2D ────────> GVA (HBM 或 DRAM)
     │                                  │
     │  SVM 统一地址空间                  │
     └───────────────────────────────> GVA (DRAM, shm mmap)

  GVA 在 SVM 下对 ACL 直接可达，无需 bounce 或 HalHostRegister
```

---

## 7. 综合对比矩阵

| 维度 | 310P3 | 910B/C | 950 |
|------|-------|--------|-----|
| D2D 拷贝 | `AclrtMemcpyAsync(D2D)` | SDMA SQE（`rtStarsMemcpyAsyncSqe_t`） | SDMA SQE（`RtDavidStarsMemcpySqeT`） |
| H2D/D2H 到 GVA | 直接 ACL async memcpy | 三段式 bounce（malloc + memcpy + SDMA） | 同 910B/C |
| Stream 类型 | `aclrtStream`（thread_local，FAST_LAUNCH\|FAST_SYNC） | `HybmStream`（进程级 map，HAL SQ/CQ） | 同 910B/C + SIMT 栈 |
| 同步方式 | `AclrtSynchronizeStream` | `HalSqCqQuery` + `HalCqReportRecv` 轮询 | 同 910B/C |
| paramSpace | `nullptr`（禁用） | 64MB + `HalHostRegister` + 170 槽位 | `nullptr`（禁用） |
| BatchCopyExtend | `BM_ERROR` | 可用（AICore 侧并发 32） | `BM_ERROR` |
| QuantCopy | `BM_ERROR` | 可用 | `BM_ERROR` |
| COPY_EXTEND_FLAG | `BM_NOT_SUPPORTED` | 可用 | `BM_NOT_SUPPORTED` |
| Wait | no-op（inline sync） | `HybmStream::Synchronize` | 同 910B/C |
| 故障恢复 | `ResetThreadAclStream` + sync 重试 | CQE 错误码上报 | 同 910B/C |
| 地址空间 | SVM 统一地址空间，GVA 对 ACL 可达 | GVA 非 ACL 可达，需 bounce/register | 同 910B/C |
| 同卡 P2P | 不需要（SVM） | 需要（`AclrtDeviceEnablePeerAccess`） | 需要 |

---

## 8. 设计取舍总结

310P3 的实现方案是"**用 ACL 替代 HAL，用 SVM 替代 bounce**"的全面重构：

**获得**：
- 简化的 H2D/D2H 路径（消除 bounce buffer 的 malloc/free + 额外拷贝）
- 简化的 stream 管理（ACL runtime 管理 vs 手工 SQ/CQ 轮询）
- 简化的地址空间模型（SVM 统一寻址 vs GVA/HVA/DVA 分离 + HalHostRegister）

**失去**：
- AICore 侧批量描述符拷贝（`BatchCopyExtend`/`QuantCopy`）—— paramSpace 依赖 `HalHostRegister`
- SDMA SQE 级别的 DMA 引擎控制（手工 SQE 字段、QoS、错误诊断）
- CQE 错误码的精细错误报告（16 种 SDMA 错误类型）

**不变**：
- `TransformVa` 地址转换逻辑（方向查找表 + `HybmVaManager`）
- `BatchCopyG2G` 合并连续段策略（`CopyG2GAsync` + `InnerWait`）
- `BatchCopyLH2GD`/`GD2LH`/`LH2GH`/`GH2LH` 串行循环结构（内部走各自平台路径）
- RDMA/URMA/HostShm/HostRDMA 四个 operator（平台无关）
