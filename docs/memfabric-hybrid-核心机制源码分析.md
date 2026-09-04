# MemFabric_Hybrid 核心机制源码分析

本文结合源码调用链梳理 MemFabric_Hybrid 的三大核心机制：内存申请与句柄导出/导入、组网与 rank 间交互、访问内存的数据流向。

整体是分层架构：用户 C API（`src/smem`）→ SMEM Entry 编排（`smem_bm_entry.cpp`）→ HyBM 层（`src/hybm`）→ 内存段/传输/驱动层。

---

## 一、内存申请、句柄导出和导入

### 1.1 分层与 4 种内存段

调用入口都在 `src/smem/csrc/smem_bm/smem_bm.cpp`，编排在 `smem_bm_entry.cpp`，底层内存管理在 `src/hybm/csrc/mm/`。核心是 `SmemBmEntry::Initialize`（`smem_bm_entry.cpp:128`）：

```
smem_bm_create2 (smem_bm.cpp:318)
└─ smem_bm_create2_inner → entry->Initialize
   ├─ hybm_create_entity          (smem_bm_entry.cpp:153)  // MemEntityDefault
   ├─ hybm_reserve_mem_space      (smem_bm_entry.cpp:161)  // 预留 GVA 虚拟地址空间
   ├─ hybm_alloc_local_memory(HBM) (smem_bm_entry.cpp:172) // mmap 本地内存 + RegisterMemoryRegion
   ├─ AllocDramMem(DRAM)          (smem_bm_entry.cpp:189)
   └─ hybm_export(EXPORT_ENTITY)  (smem_bm_entry.cpp:196)  // 导出实体级 exchange_info
```

`MemEntityDefault::InitSegment`（`hybm_entity_default.cpp:1134`）按 SoC/GVA 版本/op 类型选 4 种 Segment 之一，每种底层机制不同：

| Segment | 底层申请机制 | 适用场景 |
|---------|------------|---------|
| `HybmDevLegacySegment` | `ioctl(DEVMM_SVM_ALLOC)` 内核 SVM 大页 | 910B 等 HBM 旧 GVA |
| `HybmVmmBasedSegment` | `HalMemCreate`+`HalMemMap`（dma-buf 风格 handle） | 910C/950/310P3 V4 |
| `HybmConnBasedSegment` | `mmap(MAP_HUGETLB)` 匿名大页（+可选 `HalHostRegister`） | DRAM 默认 |
| `HybmHostShmSegment` | `shm_open`+`mmap` 共享文件 | DRAM + HOST_SHM op |

地址空间在 reserve 阶段划分：`totalSize = rankCnt * maxSize` 一次性预留，rank N 的 GVA 段 = `gvaBase + N * maxSize`（`hybm_dev_legacy_segment.cpp:123-137`），各 rank 无重叠。

### 1.2 句柄导出（export）

`hybm_export`（`hybm_big_mem_entry.cpp:128`）把信息序列化进 `hybm_exchange_info`（1284 字节容器，`hybm_def.h:114`）。导出的核心信息随 Segment 类型而异：

- **VmmBased**（最典型）：导出 **128 字节不透明 `MemShareHandle`**（`HalMemExport` 产生，`hybm_vmm_based_segment.cpp:471`）+ 设备拓扑（`devicePhyId`/`physicalChipId`/`sdid` 等）+ `gva`/`deviceVa`。
- **DevLegacy**：导出**命名共享内存名 `shmName`**（`RtIpcSetMemoryName` 命名）+ `deviceVa` + 拓扑信息。
- **HostShm**：导出 `rankId`（推导出 `/dev/hugepages/memfabric_hybrid_<rank>` 文件路径）+ `mappingOffset`。
- **ConnBased**：**不导出任何底层句柄**，只导出 `gva`+`size`+`rankId`，远端访问纯靠 transport 层的 RDMA key。

此外 entity 级别还导出 `EntityExportInfo`（含 transport nic + `TransportMemoryKey`，`hybm_entity_default.cpp:347`），每片 slice 末尾附 `SliceExportTransportKey`（含该 slice 的 MR lkey/rkey）。

### 1.3 句柄导入（import）+ mmap

导入在 `JoinHandle` 里触发（见第二节），调用 `hybm_import` → `hybm_mmap`：

```
hybm_import (hybm_big_mem_entry.cpp:153)
└─ entity->ImportEntityExchangeInfo / ImportSliceExchangeInfo (hybm_entity_default.cpp:535/625)
   ├─ segment->Import(segInfo)   // 反序列化对端的 exchange_info
   ├─ transportManager_->ConnectWithOptions  // 用对端 MR key 建立连接
   └─ 记录到 imports_ 列表

hybm_mmap (hybm_big_mem_entry.cpp:172)
└─ entity->Mmap() (hybm_entity_default.cpp:713)
   └─ segment->Mmap()  // 把远端内存"接"到本地 GVA 槽位
```

mmap 阶段把远端句柄变成本地可访问 VA，差异最大：

- **VmmBased**：`HalMemImport` 把 128B shareHandle 还原成本地 `drv_mem_handle_t` → `HalMemMap(lva, handle)` 建立本地 VA→远端物理页映射（`hybm_vmm_based_segment.cpp:836`）。
- **DevLegacy**：`HalGvaOpen(deviceVa, shmName)` → `ioctl(DEVMM_SVM_IPC_MEM_OPEN)` 在本地 GVA 建立到远端物理页映射（`hybm_dev_legacy_segment.cpp:383`）。
- **HostShm**：`open("/dev/hugepages/memfabric_hybrid_<rank>")` + `mmap(MAP_FIXED|MAP_SHARED)` 直接映射对端共享文件（`hybm_host_shm_segment.cpp:472`）。
- **ConnBased**：**不建立任何物理映射**，仅 `AddVaInfoFromExternal` 记录 GVA→空（`hybm_conn_based_segment.cpp:227`），远端访问完全靠 transport 层 RDMA/URMA/TCP 用 key 完成，GVA 只是寻址令牌。

所有路径最终都调 `AddVaInfoFromExternal` 把 `{gva, lva, dva, hva, rankId}` 注册到 `HybmVaManager`，供后续 copy 时 `TransformVa` 查地址翻译表。

---

## 二、组网与 rank 间交互机制

### 2.1 组网架构：ConfigStore 为中心、acc_links 为底座

所有 rank 间协调**不走 `acc_links` 业务层**，而是通过 ConfigStore（tcp://etcd://reg://）完成。`acc_links/csrc/` 仅提供 TCP 收发底座，被 ConfigStore 的 client/server 复用。组网拓扑如下：

```
                 ┌─────────────────────────┐
                 │  ConfigStore Server     │
                 │  (rank0 所在节点起 server)│
                 │  smem_tcp_config_store_ │
                 │  server.cpp             │
                 └──────────┬──────────────┘
                            │ TCP (acc_links 底座)
            ┌───────────────┼───────────────┐
            ▼               ▼               ▼
       ┌────────┐      ┌────────┐      ┌────────┐
       │ Rank0  │      │ Rank1  │      │ Rank2  │
       │ client │      │ client │      │ client │
       └────────┘      └────────┘      └────────┘
```

- `tcp://` → server 持有内存哈希表（`SmemLocalMemoryBackend`）；`etcd://` → 走分布式 `HaConfigStore`；`reg://` → 外部注册后端。
- rank0 节点同时起 server + client（`CSM_BOTH`），其它 rank 只起 client（`CSM_CLIENT`），`smem_store_factory.cpp:175`。

### 2.2 Key 命名：三层前缀隔离

所有 key 经 `PrefixConfigStore` 累加前缀，做命名空间隔离：

| 层 | 追加前缀 | 累计结果 | 位置 |
|----|---------|---------|------|
| EntryManager | `BM_` | `BM_` | `smem_bm_entry_manager.cpp:126` |
| per-bm | `(id)_` | `BM_(id)_` | `smem_bm_entry_manager.cpp:185` |
| per-group | `D_` | `BM_(id)_D_` | `smem_net_group_engine.cpp:75` |

业务 key 形态：

| 实际 key | 用途 | 读写方式 |
|----------|------|---------|
| `BM_(id)_D_EVENT` | group 事件发布 | CAS |
| `BM_(id)_D_BMEX_<rank>` | rank 的 exchange_info | Set / PrefixGet |
| `BM_(id)_D_<ver>_<sn>_GA` | barrier 数据 | Append |
| `BM_(id)_D_<ver>_<sn>_GW` | barrier 完成标志 | Set "ok" / Get |
| `BM_AutoRanking#` | rankId 自动分配 | Get |
| `BM_check_rank_config_consistency` | 配置一致性校验 | CAS |

### 2.3 rankId 分配：init 阶段

`smem_bm_init`（`smem_bm.cpp:71`）→ `AutoRanking`（`smem_bm_entry_manager.cpp:148`）：

- client 发 `Get("BM_AutoRanking#")`；
- server 端 `AllocateAndReplyRank`（`smem_tcp_config_store_server.cpp:408`）扫描 `aliveRankSet_`，分配第一个空闲 id，记录到 `linkRankMap_`。

### 2.4 create 阶段：注册 Watch 订阅（推送式，非轮询）

`SmemBmEntry::Initialize`（`smem_bm_entry.cpp:128`）→ `CreateGlobalTeam`（line 1002）→ `SmemNetGroupEngine::Create` → `StartListen`（`smem_net_group_engine.cpp:1304`）启动两个监听线程：

- **GroupListenEvent**（line 926）：`store_->Watch("EVENT", GroupWatchCb)` 注册对事件 key 的订阅；
- **GroupListenLinkState**（line 980）：`Watch(WATCH_RANK_LINK_DOWN, ...)` 监听对端链路掉线。

Watch 是**推送式**：server 在任何修改 key 的 handler（`SetHandler`/`CasHandler`/`AppendHandler`）里 `Put` 后调 `GetWakeupList`（`smem_tcp_config_store_server.cpp:326`）+ `WakeupWaiters`（line 993）**主动经 TCP 推送 WATCH 消息**给所有订阅方，非轮询。

注意：create 时 exchange_info 只在本地内存（`entityInfo_`/`sliceInfos_`），**尚未发布到 store**，发布发生在 join 的 `JoinHandle` 里。

### 2.5 join 触发：CAS 抢事件权 + server 推送唤醒对端

rank A 调 `smem_bm_join`（`smem_bm.cpp:345`）→ `entry->Join`（`smem_bm_entry.cpp:601`）→ `globalGroup_->GroupJoin`（`smem_net_group_engine.cpp:1380`）：

```
1. GenerateInfo(JOIN_EVENT, rank, old)   // 构造事件：version+1, bitmap 置本 rank bit
2. store_->Cas("EVENT", old, new)         // CAS 抢事件发布权（line 1414）
   ├─ CAS 成功 → server Put + WakeupWaiters → 推送 WATCH 给所有订阅方
   └─ CAS 失败（RESTORE）→ 别人正在发布，usleep(100ms) 重试，最多 30 次
3. localOpSignal_.Timedwait(120s)          // 阻塞等所有对端处理完我的 JOIN
4. Cas("EVENT", NULL_EVENT)               // 复位 key，joined_=true
```

对端 rank B 收到 server 推送后的处理链：

```
[server 推送] → GroupWatchCb (smem_net_group_engine.cpp:1207)
   ├─ 解析 SmemGroupInfo，入队 eventCtx_.values
   └─ 唤醒 GroupListenEvent 线程
      └─ ProcessEventItems → JoinLeaveEventProcess (line 1030)
         ├─ TryUpdateInfo (version 更新 → groupInfo_ 更新 groupSize)
         └─ [JOIN 且自己已 joined] option_.joinCb(targetRank)
            = SmemBmEntry::JoinHandle(rk) (smem_bm_entry.cpp:275)
```

### 2.6 exchange_info 交换：Set + PrefixGet

`JoinHandle` 内用 `GroupGatherPrefixKey`（`smem_net_group_engine.cpp:839`）交换信息，角色分两种：

- **加入者（rk==自己）**：`GatherAllPrefixKeys`（line 785）→ `Set("BMEX_<rank>", localInfo)` **发布**自己的 exchange_info，再 `PrefixGet("BMEX_")` 拉取所有 rank 的信息。若收齐数量 ≠ groupSize 返回 `SM_INNER_BUSY` 触发上层重试。
- **观察者（rk≠自己）**：`StoreGetCanInterrupt("BMEX_<rk>", 120s)` **轮询**等加入者发布其 key（line 110）。

随后双方执行：`hybm_import`（entity + slice）→ `GroupOpBarrier("before mmap")` → `hybm_mmap` → `GroupOpBarrier("after mmap")` → `InvokeEventCb(rk, JOIN)`（`smem_bm_entry.cpp:365`）。

### 2.7 exchange_info 的组装与解析

exchange_info 是 join 流程中 rank 间交换的核心载荷——它携带了远端 rank 把本地内存"接"到全局地址空间所需的全部信息（句柄、拓扑、MR key）。

#### 容器结构

`hybm_exchange_info`（`hybm_def.h:114`）本身只是一个 1284 字节的容器：

```c
typedef struct {
    uint8_t  desc[1280];  // TLV 方式追加多种序列化结构
    uint32_t descLen;     // 实际写入字节数
} hybm_exchange_info;
```

由 `ExchangeInfoWriter`（`hybm_ex_info_transfer.h:151`）按顺序 `Append`，由 `ExchangeInfoReader`（line 62）按相同顺序 `Read`，可拼接多个结构体。

#### 发布方组装（create 阶段导出 + join 阶段发布）

create 阶段已调 `hybm_export` 把信息存到本地，join 时拼接发布：

```
// create 阶段（smem_bm_entry.cpp:196）
hybm_export(entity, nullptr, HYBM_FLAG_EXPORT_ENTITY, &entityInfo_)
  └─ entity->ExportEntityExchangeInfo(writer)  (hybm_entity_default.cpp:347)
     ├─ 填充 EntityExportInfo {magic, version, rankId, role, tag[32],
     │                        nic[64], transportPrivateData{ip[128]+TransportMemoryKey}}
     │    ├─ nic = transportManager_->GetNic()
     │    └─ transportPrivateData = transportManager_->GetPrivateData()
     └─ [trans 场景] hbmSegment_->Export(segInfo) → writer.Append

// 每个 slice 也单独导出（smem_bm_entry.cpp:180/189）
hybm_export(entity, slice, 0, &sliceInfos_[i])
  └─ entity->ExportSliceExchangeInfo(slice, writer)  (hybm_entity_default.cpp:409)
     ├─ segment->Export(realSlice, segInfo)    // 200B，4 种 segment 各不同
     ├─ writer.Append(segInfo)
     └─ writer.Append(SliceExportTransportKey{magic, rankId, gva, key})
        └─ transportManager_->QueryMemoryKey(vAddress_, key)  // 查 MR 的 lkey/rkey

// join 阶段 JoinHandle 内拼接发布（smem_bm_entry.cpp:283-290）
localInfo = entityInfo_ + sliceInfos_[0] + sliceInfos_[1] + ...
Set("BMEX_<rank>", localInfo)
```

4 种 Segment 导出的 200B segInfo 内容差异：

| Segment | 导出核心信息 | 关键字段 |
|---------|------------|---------|
| **VmmBased** | 128B 不透明 `MemShareHandle` + 设备拓扑 | `shareHandle[128]`, `devicePhyId`, `physicalChipId`, `gva`, `deviceVa` |
| **DevLegacy** | 命名共享内存名 + 设备拓扑 | `shmName[65]`, `deviceVa`, `devicePhyId`, `pid` |
| **HostShm** | rankId + 偏移 + 文件系统标识 | `mappingOffset`, `rankId`, `useHugetlbfs` |
| **ConnBased** | 仅 GVA + size（无底层句柄） | `gva`, `size`, `rankId` |

此外每个 slice 末尾附的 `SliceExportTransportKey`（`hybm_entity_default.h:38`）包含 `rankId` + `gva` + `TransportMemoryKey`（`keys[98]` 大数组，前 84 slot 给 device rdma/urma，后 14 slot 给 host hcom）。

#### 接收方解析（import 阶段）

`JoinHandle` 收齐 allInfo 后拆分并导入（`smem_bm_entry.cpp:299-349`）：

```
// 拆分：第一个单元 = entityInfos，后续 = sliceInfos
解析 allInfo → entityInfos[count] + sliceInfos[count]

// 第一步：导入 entity 级信息（smem_bm_entry.cpp:323）
hybm_import(entity, entityInfos, count, nullptr, HYBM_FLAG_EXPORT_ENTITY)
  └─ entity->ImportEntityExchangeInfo  (hybm_entity_default.cpp:625)
     ├─ desc[i].Read(EntityExportInfo)           // 反序列化每个 rank 的 entity 信息
     ├─ importedRanks_[rankId] = info             // 记录 nic/privateData/role/tag
     ├─ ImportForTransportManager (line 588)
     │  └─ transportManager_->Prepare + Connect   // 用 nic/privateData 建立 RDMA/URMA 连接
     └─ [有 segment info] hbmSegment_->Import

// 第二步：导入 slice 级信息（smem_bm_entry.cpp:334）
hybm_import(entity, sliceInfos, count, nullptr, 0)
  └─ entity->ImportSliceExchangeInfo  (hybm_entity_default.cpp:535)
     ├─ ImportForSegment (line 472)
     │  ├─ desc[i].Test(magic) → 判定 HBM/DRAM slice
     │  ├─ desc[i].Read(segInfo, 200B)           // 反序列化 segment-specific 信息
     │  └─ dramSegment_->Import / hbmSegment_->Import  // 4 种 segment 各自解析
     │     ├─ VmmBased: 记录 shareHandle[128] + devicePhyId → imports_
     │     ├─ DevLegacy: 记录 shmName + deviceVa → imports_；RtSetIpcMemorySuperPodPid 加白名单
     │     ├─ HostShm: 记录 mappingOffset + rankId → imports_
     │     └─ ConnBased: 仅记录 gva → imports_（无物理句柄）
     ├─ ImportForTransportPrecheck (line 1061)
     │  └─ desc[i].Read(SliceExportTransportKey)
     │     → importedMemories_[rankId].insert(key)  // 记录每片 MR key
     └─ ImportForTransport (line 1088)
        └─ transportManager_->ConnectWithOptions   // 把 memKeys 传给 transport 建立远程 MR 访问

// 第三步：barrier 同步后 mmap（smem_bm_entry.cpp:342/349）
GroupOpBarrier("before mmap")
hybm_mmap(entity, 0)  // 用 imports_ 列表建立本地 VA→远端物理页映射
GroupOpBarrier("after mmap")
```

import 阶段只做"解析+记录+建连接"，**不建物理映射**；物理映射推迟到 `hybm_mmap` 阶段统一执行（见章节一 1.3 的 4 种 Segment mmap 差异）。

#### 信息流总结

```
发布方(rank A)                          接收方(rank B)
┌─────────────┐                        ┌─────────────┐
│ hybm_export │                        │ hybm_import │
│  ├ EntityExportInfo  ──Set──>  BMEX  ──> Read EntityExportInfo
│  │  (nic, ip, TransportMemoryKey)    │  → transportManager->Connect
│  ├ SliceSegInfo(200B) ──────────────>│  → segment->Import (记录 imports_)
│  │  (shareHandle/shmName/gva)        │
│  └ SliceExportTransportKey ─────────>│  → importedMemories_ 记录 MR key
│     (rankId, gva, lkey/rkey)         │
└─────────────┘                        └─────────────┘
                                    hybm_mmap: 用 imports_ 建本地VA→远端物理页映射
```

### 2.8 barrier 同步：all-gather 模式

`GroupOpBarrier`（`smem_bm_entry.cpp:256`）→ `GroupGatherResult`（`smem_net_group_engine.cpp:150`）：

1. 每 rank `Append` 自己的 (rankId, retCode) 到 `addKey`；
2. 最后一个 rank（`val == unitSize * size`）`Set(waitKey, "ok")`；
3. 所有 rank `Get(waitKey)` 等待 "ok"，再 `Get(addKey)` 拉取全部结果检查。

barrier 等待数 = 当前 `groupInfo_.groupSize`（bitmap 中已置位的 rank 数），**不是固定的 worldSize**。worldSize 仅用于 rankId 分配上限和地址空间预留（`rankCnt * maxSize`）。

### 2.9 交互时序示例（3 个 rank）

```
R0 init:  起 server+client，Get("AutoRanking#") → rankId=0
R1 init:  起 client，Get("AutoRanking#") → rankId=1
R2 init:  起 client，Get("AutoRanking#") → rankId=2

各 rank smem_bm_create: StartListen → Watch("EVENT") 注册推送订阅

R0 join:  Cas(EVENT, JOIN, ver=1) → server 推送
          → R0 自己 JoinHandle(0): Set(BMEX_0)，无对端 import，直接 mmap+barrier
R1 join:  Cas(EVENT, JOIN, ver=3) → server 推送给 R0/R2
          → R0.JoinHandle(1): Get(BMEX_1) 轮询等 R1 发布
          → R1.JoinHandle(1): Set(BMEX_1)，PrefixGet 收齐 BMEX_0/1，import R0
          → 双方 barrier 同步 → mmap → barrier → 用户回调"R1 joined"
          → R1 的 localOpSignal 被唤醒，Cas(EVENT, NULL) 完成 join
R2 join:  类似，R0/R1 收推送执行 JoinHandle(2)
```

### 2.10 group event 回调

`smem_bm_set_group_event_handler`（`smem_bm.cpp:411`）→ `SmemBmEntry::SetEventListener`（`smem_bm_entry.cpp:737`）存入 `eventCb_`。触发点在 `JoinHandle`/`LeaveHandle` 末尾 `InvokeEventCb(rk, SMEM_GROUP_EVENT_JOIN/LEAVE)`（line 365/439）。因此已 join 的 rank 在新 rank join 完成时收到回调，参数 `rankId` = 新加入的 rank。

### 2.11 小结

| 机制 | 实现方式 |
|------|---------|
| rankId 分配 | client `Get` + server `AllocateAndReplyRank` 扫描 `aliveRankSet_` |
| 事件发布权 | `Cas("EVENT", old, new)` CAS 互斥 |
| 通知对端 | server **推送** WATCH 给所有订阅方（非轮询） |
| exchange_info 交换 | 加入者 `Set("BMEX_<rank>")`，观察者 `Get` 轮询 + 加入者 `PrefixGet` 收齐 |
| 全员同步 | all-gather barrier：`Append` + 最后一个 `Set("ok")` + 所有人 `Get("ok")` |
| barrier 等待数 | 当前已 join 的 rank 数（bitmap 置位数），非 worldSize |
| 链路掉线 | `Watch(WATCH_RANK_LINK_DOWN)` 推送 + `LeaveHandle` 清理 |

---

## 三、访问内存的数据流向

### 3.1 GVA 寻址

GVA 是一段连续预留的虚拟地址区间，按 rank 线性切分：**rank N 的 GVA 段 = `gvaBase + N * maxSize`**（`hybm_dev_legacy_segment.cpp:123-137`），不是 `rank_id << offset` 编码。`smem_bm_ptr_by_mem_type`（`smem_bm.cpp:426`）返回 `gvaBase + peerRank * maxHBMSize`。`smem_bm_gva_to_va`（`smem_bm.cpp:457`）→ `HybmVaManager::TransformVa`（`hybm_va_manager.cpp:144`）查 `allocatedMap_[HVM_GVA]` 翻译表，把 GVA 转成本地可访问的 DVA/HVA。

### 3.2 copy 调用链与方向推断

```
smem_bm_copy (smem_bm.cpp:481)
└─ SmemBmEntry::DataCopy (smem_bm_entry.cpp:819)
   ├─ TransToHybmDirection (smem_bm_entry.cpp:778)  // 用 ClassifyAddressMask 推断方向
   └─ hybm_data_copy (hybm_data_op_entry.cpp:22)
      └─ entity->CopyData (hybm_entity_default.cpp:783)
         ├─ LocateAddrAndRank: GetRankByGva(src/dest) 定位 rank
         └─ HostComposeDataOp::DataCopy (hybm_compose_data_op.cpp:115)
            └─ GetPrioritedDataOperators (line 258)  // 按优先级选算子
               优先级: SDMA > DEVICE_RDMA > DEVICE_URMA > HOST_RDMA > HOST_SHM
```

### 3.3 物理数据流向（按通道）

**SDMA 通道**（同 NPU/同机跨卡 P2P 可达）：`HostDataOpSDMA::CopyG2G`（`hybm_data_op_sdma.cpp:532`）提交 SDMA SQE，src/dest 都是本地 DVA（远端内存在 mmap 阶段已映射进本地设备 VA 空间），硬件 SDMA 引擎通过 SMMU 页表翻译：

```
本地 HBM ──NPU SDMA引擎──SMMU──PCIe P2P/RoCE──> 远端 HBM
```

**Device RDMA 通道**（跨 NPU）：`DataOpDeviceRDMA::CopyLD2GD`（`hybm_data_op_device_rdma.cpp:365`）→ `SafePut` → `CopyRDMA` → `transportManager_->WriteRemote`（`device_rdma_transport_manager.cpp:421`）→ `RemoteIO`（line 735）用 `RaSendWrV2` 提交 RDMA WRITE verbs：

```
L2G: 本地 HBM ──RDMA WRITE──> 远端 HBM(rAddr + rkey)
G2L: 远端 HBM ──RDMA READ──> 本地 HBM (ReadRemote)
```

若本地内存未注册 MR，先拷到预注册的 swap 内存中转。

**G2H/H2G**（global↔host DRAM）：SDMA 路径需经临时 device 内存两跳（`CopyLH2GD`/`CopyGD2LH`，`hybm_data_op_sdma.cpp:183/212`）；RDMA 路径走 host 的 HCOM transport（`host_hcom_transport_manager.cpp`）。

**HOST_SHM**（同节点）：直接 `aclrtMemcpy` 走 mmap 共享内存文件，无网络传输。

### 3.4 异步与等待

`ASYNC_COPY_FLAG` 或外部 stream 触发异步：SDMA 走自研 `HybmStream`（`hybm_stream.cpp`）——`SubmitTasks` 提交 SQE 到硬件 SQ，`Synchronize`（line 403）轮询 CQ 直到 `sqHead_==sqTail_`。`smem_bm_wait`（`smem_bm.cpp:588`）→ `HostComposeDataOp::Wait`（`hybm_compose_data_op.cpp:229`）只对 SDMA 算子做 `HybmStream::Synchronize`；Device RDMA 当前同步完成（`RemoteIO` 内部 sync=true 时已调 `Synchronize`）。

---

## 串联总结

1. **内存申请**：`smem_bm_create` → hybm 层选 4 种 Segment 之一，用 `ioctl`/`HalMemCreate`/`mmap`/`shm_open` 申请本地内存，再 `RegisterMemoryRegion` 注册 MR key。
2. **导出/导入**：`hybm_export` 把句柄（128B `MemShareHandle`/shmName/rankId）+ GVA + MR key 序列化进 `hybm_exchange_info`；对端 `hybm_import`+`hybm_mmap` 用这些信息重建本地到远端的地址映射（`HalMemImport`+`HalMemMap` / `HalGvaOpen` / shm 文件 mmap / 或仅记录 GVA 令牌）。
3. **rank 间 join**：rank A `Cas("EVENT", JOIN)` → server **推送**给所有 watch 的 rank → 对端 `JoinHandle` 用 `Set/Get("BMEX_<rank>")` 交换 exchange_info → import+mmap+barrier → 用户回调。
4. **数据访问**：用户拿 GVA → `TransformVa` 查翻译表得本地可访问 DVA/HVA → `HostComposeDataOp` 按优先级选 SDMA/RDMA/SHM 算子 → SDMA 走硬件 SQE、RDMA 走 verbs WRITE/READ、SHM 走共享内存。

---

## 四、BM / SHM / TRANS 三种模式实现与使用差异

三种模式（BM/SHM/TRANS）共享同一套 HyBM 底层（`hybm_create_entity`/`reserve`/`alloc`/`export`/`import`/`mmap`/`data_copy`），但在 API 设计、组网模型、内存归属、寻址方式、数据搬运发起方上有根本差异。

### 4.1 三种模式定位

| 维度 | BM（Big Memory） | SHM（Shared Memory） | TRANS（Transfer） |
|------|-----------------|--------------------|--------------------|
| **核心抽象** | 全局大内存池：host 编排跨 rank 数据拷贝 | 对称共享内存：AI Core 直接用 GVA 跨 rank 访存 | 点对点传输：host 发起按 uniqueId 的 read/write |
| **典型场景** | KVCache offload、DRAM 池化 | AllReduce/ShiftPutGet 等 NPU 集合通信 kernel | Prefill→Decode 跨进程 KVCache 推送 |
| **数据搬运发起方** | Host 侧（CPU 调 `smem_bm_copy`） | AI Core 侧（kernel 直接用 GVA 做 MTE/SDMA） | Host 侧（CPU 调 `smem_trans_write/read`） |
| **典型示例** | `examples/kv_offload/` | `examples/hbm_share_memory/ShiftPutGet/` | `examples/transfer/test_transfer_engine.py` |

### 4.2 初始化差异

| 维度 | BM | SHM | TRANS |
|------|----|-----|-------|
| **init API** | `smem_bm_init(storeURL, worldSize, deviceId, config)` | `smem_shm_init(ipPort, worldSize, rankId, deviceId, config)` | `smem_trans_init(config)` |
| **rankId 来源** | 默认 `autoRanking`：server 自动分配（`smem_bm_entry_manager.cpp:148`） | **用户指定**（init 参数） | **autoRanking**：推迟到 `create` 时从 store 获取（`smem_trans_entry_manager.cpp:48`） |
| **init 时是否连 store** | 是：`PrepareStore` + `AutoRanking`（`smem_bm.cpp:93`） | 是：`CreateStoreByUrl`（`smem_shm.cpp:332`） | **否**：只调 `hybm_init`（`smem_trans.cpp:45`），store 连接推迟到 `create` |
| **hybm_init flags** | `config->flags` | `config->flags | HYBM_FLAG_INIT_SHMEM_META`（bit63）（`smem_shm.cpp:338`） | `config->flags` |
| **store 前缀** | `BM_` + `(id)_` | `SHM_(id)_` + `S_` | `TRANS_` |

### 4.3 create 差异：内存归属与组网模型

| 维度 | BM | SHM | TRANS |
|------|----|-----|-------|
| **create API** | `smem_bm_create(id, memberSize, dataOpType, localDRAM, localHBM, flags)` | `smem_shm_create(id, rankSize, rankId, localSize, dataOpType, flags, &gva)` | `smem_trans_create(storeUrl, uniqueId, config)` |
| **内存归属** | **库分配**：指定 DRAM/HBM size，库负责 mmap/alloc | **库分配**：指定 localSize（仅 HBM），对称等长 | **用户自有**：用户先分配 device 内存，再 `register_mem` 注册 |
| **内存类型** | HBM + DRAM 可混合 | **仅 HBM**（DEVICE） | **仅 HBM**（DEVICE），但用户自己管理 |
| **bmType（发起方）** | `HOST_INITIATE`（`smem_bm.cpp:246`） | `AI_CORE_INITIATE`（`smem_shm.cpp:47`） | `HOST_INITIATE`（`smem_trans_entry.cpp:841`） |
| **scene** | `HYBM_SCENE_DEFAULT` | `HYBM_SCENE_SHM` | `HYBM_SCENE_TRANS` |
| **entityId 基址** | `HYBM_ENTITY_ID_BM_BASE` = 64 | `HYBM_ENTITY_ID_SHM_BASE` = 0 | `HYBM_ENTITY_ID_TRANS_BASE` = 128 |
| **group 类型** | **动态组** `dynamic=true`（`smem_bm_entry.cpp:1008`） | **静态组** `dynamic=false`（`smem_shm_entry.cpp:90`） | **动态组** `dynamic=true`（`smem_trans_entry.cpp:157`） |
| **角色** | `PEER`（对称，全量互导） | `PEER`（对称，全量互导） | `SENDER`/`RECEIVER`（**只异角色互导**，`smem_trans_entry.cpp:269`） |
| **标识** | rankId（数值） | rankId（数值） | **uniqueId**（"ip:port" 字符串） |
| **create 返回** | handle（gva 不直接返回） | handle **+ gva 输出参数**（`smem_shm.cpp:79`） | handle（不返 gva） |
| **enable56BitsGva** | 可配置，默认 false | **强制 false**（`smem_shm.cpp:64`） | **强制 true**（`smem_trans_entry.cpp:847`） |
| **maxHBMSize** | 用户指定，可不等长 | `ALIGN_UP(localSize, 1G)`（对称）（`smem_shm.cpp:60`） | 固定 128GB（`smem_trans_entry.cpp:843`） |

### 4.4 join / import / mmap 差异

| 维度 | BM | SHM | TRANS |
|------|----|-----|-------|
| **join API** | **有**：`smem_bm_join/leave`（公开 API）（`smem_bm.cpp:345`） | **无**（静态组，create 即全员 join） | **有**：`create` 内部自动调 `Join`（`smem_trans_entry.cpp:60`） |
| **create 内是否 import+mmap** | **否**：create 只 export，import+mmap 在 `JoinHandle` | **是**：create 的 6 个 init step 隐式完成（`smem_shm_entry.cpp:214-297`） | **是**：create 内部自动 `Join` → `JoinHandle`（`smem_trans_entry.cpp:467`） |
| **exchange_info 交换** | `Set("BMEX_<rank>")` + `PrefixGet` 增量拉取（`smem_bm_entry.cpp:293`） | `GroupAllGather` 一次性收齐（`smem_shm_entry.cpp:226`） | `GroupGatherPrefixKey` 走 `BMEX_<rank>`（同 BM）（`smem_trans_entry.cpp:314`） |
| **import 过滤** | 全量导入所有已 join rank | 全量导入所有 rank | **只导入异角色** peer（sender↔receiver，`smem_trans_entry.cpp:269`） |
| **动态成员** | 支持：`smem_bm_join/leave` + `set_group_event_handler` 回调 | 不支持 | 支持：内部 join + `set_peer_down_callback` 回调 |

### 4.5 寻址模型差异（核心设计分歧）

**BM：对称连续 GVA 空间**

```
GVA = gvaBase + rankId * maxSize
```
所有 rank 的 GVA 段等长排列在连续地址区间。用户调 `smem_bm_ptr_by_mem_type(handle, memType, peerRankId)` 获取对端 GVA（`smem_bm.cpp:426`），调 `smem_bm_copy(handle, {src, dst, size}, copyType)` 拷贝。copy 时 `HybmVaManager::TransformVa` 把 GVA 翻译成本地可访问的 DVA/HVA（`hybm_va_manager.cpp:144`）。

**SHM：对称连续 GVA 空间（同 BM，但直接给 kernel）**

```
GVA = gvaBase + rankId * symmetricSize
```
与 BM 相同的对称模型，但 `smem_shm_create` 直接把 GVA 返回给用户（`smem_shm.cpp:79`），用户把 GVA 传给 AI Core kernel。kernel 内用 `smem_shm_get_symmetric_size()` 获取步长（`shm_all_shift.cpp:88`），用 `gva + offset * rank` 直接寻址对端 rank 内存，无需 host 介入拷贝。典型 kernel 代码（`shm_all_shift.cpp:89`）：

```cpp
uint64_t dst = reinterpret_cast<uint64_t>(gva) + offset * rank;
smem_shm_copy_gm2gm(dst, src, size);  // AI Core 内 MTE/SDMA 直接搬
```

**TRANS：按需 import + 地址翻译模型（非对称）**

用户传 `remoteAddr`（对端进程的**本地地址**）+ `remoteUniqueId`（"ip:port"）。系统在 `remoteSlices_` 表中查到该 peer 的 local→global 地址映射（`smem_trans_entry.cpp:618`），用 `TransformAddr` 把远端本地地址翻译成本地可见的 GVA（`smem_trans_entry.cpp:569`），再调 `hybm_data_batch_copy` 拷贝：

```cpp
// smem_trans_entry.cpp:624-635
mappedAddress = global段基址 + (remoteAddr - local段基址);
hybm_data_batch_copy(entity_, {src=localAddr, dest=mappedAddress(GVA), ...}, AUTO, stream, flag);
```

| 寻址维度 | BM | SHM | TRANS |
|---------|----|-----|-------|
| **GVA 空间** | 对称连续 | 对称连续 | 散列 per-peer import 映射 |
| **获取对端地址** | `smem_bm_ptr_by_mem_type(peerRankId)` | kernel 内 `gva + symSize * rank` | 用户给 `remoteUniqueId + remoteAddr`，系统翻译成 GVA |
| **地址翻译** | `TransformVa(gva, GVA→DVA/HVA)` | 无需翻译（GVA==LVA，kernel 直接用） | `TransformAddr(remoteAddr → GVA)` |
| **GVA 返回给用户** | 否（仅内部用） | **是**（`smem_shm_create` 输出参数） | 否（内部翻译后用） |

### 4.6 数据操作差异

| 维度 | BM | SHM | TRANS |
|------|----|-----|-------|
| **copy API** | `smem_bm_copy` / `copy_batch` / `copy_batch_partial_succeed` | **无**（AI Core kernel 直接用 GVA） | `smem_trans_write/read` / `batch_write/read` |
| **方向类型** | L2G/G2L/G2H/H2G/G2G/AUTO（`smem_bm_def.h:71`） | 无（kernel 自行决定） | write=L2G，read=G2L |
| **异步机制** | `ASYNC_COPY_FLAG` + `smem_bm_wait` | kernel 自行管理（aclrtSynchronizeStream） | `*_submit` 系列提交到 acl stream |
| **量化拷贝** | 库不暴露 | 无 | `smem_trans_batch_quant_write`（float16/bfloat16→int8） |
| **dataOpType** | SDMA/HOST_RDMA/HOST_TCP/DEVICE_RDMA/HOST_URMA/HOST_SHM/DEVICE_URMA/DEVICE_UBOE（全集） | MTE/SDMA/RDMA（子集，`smem_shm_def.h:28`） | SDMA/DEVICE_RDMA/DEVICE_URMA/DEVICE_UBOE（设备侧，`smem_trans_entry.cpp:84-115`） |
| **transport 层** | SDMA + DEVICE_RDMA + HOST_RDMA + HOST_SHM + URMA + UBOE（最广） | 同 BM 底层 | SDMA + DEVICE_RDMA + DEVICE_URMA + DEVICE_UBOE（仅设备侧） |

### 4.7 控制面差异

| 功能 | BM | SHM | TRANS |
|------|----|-----|-------|
| **barrier** | 内部用（`GroupOpBarrier`），无公开 API | **有公开 API**：`smem_shm_control_barrier`（`smem_shm.cpp:153`） | 无 |
| **allgather** | 内部用 | **有公开 API**：`smem_shm_control_allgather`（`smem_shm.cpp:185`） | 无 |
| **subgroup barrier/allgather** | 无 | **有**：`smem_shm_subgroup_barrier/allgather`（`smem_shm.cpp:169/207`） | 无 |
| **atomic alloc/release** | 无 | **有**：`smem_shm_atomic_alloc_value/release_value`（CAS 位图，`smem_shm.cpp:246/275`） | 无 |
| **global exit** | 无 | **有**：`smem_shm_register_exit/global_exit`（Watch/Set "EXIT" key，`smem_shm.cpp:369/390`） | 无 |
| **topology can_reach** | 无公开 API（内部 `hybm_entity_reach_types`） | **有公开 API**：`smem_shm_topology_can_reach`（`smem_shm.cpp:229`） | 无 |
| **group event 回调** | **有**：`smem_bm_set_group_event_handler`（join/leave 通知，`smem_bm.cpp:411`） | 无 | 无 |
| **peer down 回调** | 无 | 无 | **有**：`smem_trans_set_peer_down_callback`（TCP 链路断开检测，`smem_trans.cpp:351`） |
| **动态扩容** | **有**：`smem_bm_extend_local_mem`（`smem_bm.cpp:374`） | 无 | 无 |
| **注册用户内存** | **有**：`smem_bm_register_user_mem`（`smem_bm.cpp:618`） | 无 | **有**：`smem_trans_register_mem`（核心 API，`smem_trans.cpp:104`） |

### 4.8 register_mem 与 Update 机制（TRANS 独有）

TRANS 模式允许用户在 join 后动态注册新内存段，通过 `Update` 机制通知所有 peer：

```
smem_trans_register_mem(handle, addr, size, flags)  smem_trans.cpp:104
└─ entry->RegisterLocalMemory(addr, size, flags)    smem_trans_entry.cpp:518
   ├─ hybm_register_local_memory(entity, addr, size)  // 注册到 segment + transport MR
   ├─ hybm_export(entity, slice, &info)               // 导出新 slice 的 exchange_info
   └─ Update(0)                                        smem_trans_entry.cpp:557
      └─ GroupUpdate → 触发所有 peer 的 UpdateHandle
         └─ peer 侧: hybm_import + hybm_mmap（动态增量导入）
```

这使得 TRANS 支持"先 join 再注册内存"的灵活工作流——create 时不需要预知所有内存，可按需注册。BM 的 `smem_bm_register_user_mem` 类似但不触发自动 Update。

### 4.9 典型使用流程对比

**BM 模式（KVCache offload）**：

```python
config.rank_id = rank_id; config.world_size = 4
offload.initialize(config)              # smem_bm_init
# 内部: smem_bm_create → alloc HBM/DRAM → export → (隐式 join → import → mmap)
offload.sparse_copy(src_ptrs, dst_ptrs, len_ptrs, size_ptr, device)  # smem_bm_copy
offload.uninitialize()
```

**SHM 模式（AllShift 集合通信）**：

```cpp
smem_shm_init(ipport, rankSize, rankId, deviceId, &config);   // 连 store + hybm_init
smem_shm_t h = smem_shm_create(0, rankSize, rankId, 64*MB, SMEMS_DATA_OP_MTE, 0, &gva);
// 内部: create_entity → reserve → alloc → allgather exchange → import → mmap
// gva 直接传给 AI Core kernel:
shm_all_shift<<<1, nullptr, stream>>>(gva, localInput);
// kernel 内: gva + symSize * peerRank 直接寻址对端
smem_shm_control_barrier(h);           // 控制面 barrier
smem_shm_destroy(h, 0);
```

**TRANS 模式（Prefill→Decode KVCache 推送）**：

```python
engine = TransferEngine()
engine.initialize(store_url, unique_id, role, npu_id)  # smem_trans_init + create + join
engine.register_memory(buffer.data_ptr(), total_bytes)   # smem_trans_register_mem
# 对端 (Decode 角色) 也 register_memory
# Prefill 侧写入:
engine.transfer_sync_write(dst_unique_id, local_ptr, peer_ptr, size)  # smem_trans_write
# Decode 侧读取本地 buffer 即可
```

### 4.10 三种模式关系图

```
                    共享 HyBM 底层
    ┌───────────────────────────────────────────────┐
    │  hybm_create_entity / reserve / alloc / export │
    │  hybm_import / hybm_mmap / hybm_data_copy     │
    │  HybmVaManager / TransportManager / DataOp    │
    └───────────┬───────────┬───────────┬───────────┘
                │           │           │
         ┌──────┴──┐  ┌────┴────┐  ┌───┴──────┐
         │   BM    │  │   SHM   │  │  TRANS   │
         │         │  │         │  │          │
         │ 动态组   │  │ 静态组  │  │ 动态组   │
         │ Host发起 │  │ AICore  │  │ Host发起 │
         │ 对称GVA │  │ 对称GVA │  │ 散列GVA  │
         │ PEER    │  │ PEER    │  │ S/R角色  │
         │ 库分配   │  │ 库分配  │  │ 用户注册 │
         │ copy API│  │ 无copy  │  │ write/read│
         │ HBM+DRAM│  │ 仅HBM  │  │ 仅HBM    │
         └─────────┘  └─────────┘  └──────────┘
```

### 4.11 小结

- **BM** 是最全功能的模式：动态成员、HBM+DRAM 混合、多种 transport、host 发起的 copy，适合需要 host 编排的内存池化/offload 场景。
- **SHM** 是最精简的模式：静态全员组、仅 HBM、AI Core 直接用 GVA，重控制面（barrier/allgather/atomic/exit），适合 NPU 间对称集合通信。
- **TRANS** 是最灵活的点对点模式：uniqueId 标识、sender/receiver 角色过滤、用户自管内存、动态 register_mem + Update，适合跨进程/跨节点的 push-pull 传输（如 KVCache 从 Prefill 推送到 Decode）。
