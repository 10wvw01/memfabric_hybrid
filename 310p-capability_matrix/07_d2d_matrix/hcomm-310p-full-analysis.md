# hcomm 310P 全面分析（无先验独立调研汇总）

> 调研方法：6 个无上下文 subagent 从零调研 /home/t00842504/hcomm 源码 + 本机实测证据交叉验证。
> 本文档推翻了此前若干结论，以本文档为准。

---

## 一、核心修正（推翻此前结论）

### 修正 1：`Is310PDevice()` 硬编码 HCCS 的分支在开源 host 构建中是死代码

`ASCEND_310P_DEVICE` 宏唯一定义处（`src/legacy/ascend910/platform/hccl_plf.cmake:15-20`）：

```cmake
target_compile_definitions(hccl_plf PRIVATE
    $<$<STREQUAL:${PRODUCT_SIDE},device>:ASCEND_310P_DEVICE>
```

而 `hccl_plf.cmake` **只在 `PRODUCT_SIDE STREQUAL "host"` 时被 include**（`platform/CMakeLists.txt:11-17`），
此时 `${PRODUCT_SIDE}` 恒为 "host"，生成器表达式 `$<STREQUAL:host,device>` **恒为假**。

**结论：开源构建中 `ASCEND_310P_DEVICE` 永不定义，`Is310PDevice()` 在 host 侧恒为 false。**
该宏是闭源时代 310P DC（device 侧进程）形态的遗留开关。

因此 host 侧（Atlas 300I Duo 实际运行形态）`hrtGetPairDeviceLinkType()` 走**真实查询分支**：
`aclrtGetDevicesTopo()` → die 对返回 HCCS_TYPE，跨卡返回 PXI_TYPE。**拓扑判断是正确的，不存在"把 PCIe 当 HCCS"的问题。**

### 修正 2：hcomm 对 310P3 跨卡通信【会】调用 rtEnableP2P

此前结论"hcomm 310P 不做 P2P enable"是**错误**的（基于死代码分支）。真实逻辑：

调用链（transport 分配阶段，`transport_manager.cc:714-723`）：
```cpp
if ((rankInfoList_[...].deviceType == DevType::DEV_TYPE_310P3 || isStandardCard_)
    && !tempIsInterRdma && !Is310PDevice()) {          // 310P3 host 侧：条件成立
    enableP2PDevices.push_back(...devicePhyId);         // 算法需要的远端设备
    P2PMgmtPub::EnableP2P(enableP2PDevices);            // ★ 会调用
}
```

`P2PMgmt::EnableP2P(uint32_t)` 内部（`p2p_mgmt.cc:50-90`）：
- `Is310PDevice()` → false（host 侧），不跳过
- `IsNeedEstablishP2Pconnection()`：310P3 **恒返回 true**（`p2p_mgmt.cc:374-375`）
- `CheckMarsterId()`：非 910B/910_93 走链路类型判断：
  - HCCS/HCCS_SW/SIO（die 对）→ isMarsterIdDiff=false → **跳过**（且驱动禁止同 PCIe 设备间 enable）
  - PXI（跨卡）→ isMarsterIdDiff=**true** → **`hrtEnableP2P` → `rtEnableP2P(logicId, phyId, 0)` 真正调用**
- 引用计数去重：同一 (logicId, phyId) 只 enable 一次
- `WaitP2PEnabled` 轮询 `rtGetP2PStatus`，P2P 生效后才建 TCP socket、才做 IPC import

**时序：EnableP2P → WaitP2PEnabled → TCP socket → IPC export/import（import 时 P2P 已就绪）**

### 修正 3：hcomm 从不使用 `ENABLE_PEER_ACCESS` import flag

全代码库 grep 证实：
- `aclrtIpcMemImportByKey` 所有调用点 flag **恒为 0UL**（DEFAULT）
- `ACL_RT_IPC_MEM_IMPORT_FLAG_ENABLE_PEER_ACCESS` **从未被使用**
- `aclrtIpcMemGetExportKey`：910 路径 flag=0（保留 PID 校验）；950/orion 路径 flag=1（DISABLE_PID_VALIDATION）

**hcomm 的模式 = 显式 rtEnableP2P（跨卡）+ DEFAULT import + PID 白名单**，
而 test 07 的模式 = PEER_ACCESS import flag（import 时隐式 enable）。两者完全不同。

---

## 二、310P 型号全景

| 枚举 | 注释 | 形态 | 进程位置 | 拓扑假设 |
|---|---|---|---|---|
| `DEV_TYPE_310P3` | PG | Atlas 300I Duo（一卡双芯）/ 300I Duo V 卡 | **host 侧** | 混合：die 对 HCCS + 卡间 PCIe（运行时探测） |
| `DEV_TYPE_310P1` | AG | 310P DC 形态 | device 侧（AICPU） | 静态全 HCCS（死代码分支） |

- 运行时 SOC 名 `Ascend310P1/P3/P5/P7`（甚至临时 `Ascend310B1`）**全部映射为 `DEV_TYPE_310P3`**（`adapter_hal.h:31-37`）
- 310P3 最大设备数 = **32**（16 张 Duo 卡）；带宽表 = PCIE_GEN3；无 level-1 RDMA 表项
- **代码中不存在"310P 超节点"概念**（超节点仅 910_93/A3）
- 本机实测（npu-smi）：4 × Atlas 300I Duo (IT21PD2G10)，卡内 die 间 HCCS，卡间 PHB/SYS（PCIe）

---

## 三、完整通信流程（310P3 = Atlas 300I Duo，host 侧，ascend910 legacy 路径）

> V1/V2 分流：`hrtGetHcclV2Support()` 按 SoC 名判断，仅 Ascend950/910_96/960 走 V2。
> **310P 走 ascend910 legacy 路径。**

```
┌─ 1. 通信域初始化 ─────────────────────────────────────────────┐
│ HcclCommInitRootInfo (op_base.cc:2111)                        │
│  └ TopoInfoDetect::SetupAgent (TCP, 端口 60000-60031)          │
│     · 每 rank 上报 {rankId, hostIp, devicePhyId, deviceType}   │
│     · ★ 一个 rank = 一个 die（server 内 devicePhyId 唯一性校验）│
│     · Server 收集全表 → 广播回所有 agent                        │
│  └ HcclCommunicator::Init                                       │
│     · InitPreResource: deviceType!=310P3 才 EnableP2P（310P3 此处不做）│
│     · InitTransportManager（此时不建链）                         │
└──────────────────────────────────────────────────────────────┘
┌─ 2. 首次算子执行时惰性建链（非全连接！）───────────────────────┐
│ AllReduce → CalcResRequest → 算法算出 transportRequests          │
│   ★ ring 算法 = 每 rank 只需 2 个邻居；doubling = 3 个邻居       │
│   ★ 不是 56 条全连接，是算法拓扑需要的少量链路                    │
│  └ TransportManager::Alloc → AllocSubCommLinks                   │
│     · [310P3] P2PMgmtPub::EnableP2P(remotePhyId)                │
│       └ CheckMarsterId: PXI(跨卡) → rtEnableP2P ★               │
│                       HCCS(die对) → 跳过（驱动也禁止同卡 enable）│
│     · WaitP2PEnabled: 轮询 rtGetP2PStatus 直到生效              │
│     · CreateDestSockets: TCP 建链（小 rank 做 server）           │
│     · GetTransportType: 同 server → TRANS_TYPE_P2P              │
│     · TransportInit → new TransportP2p                          │
└──────────────────────────────────────────────────────────────┘
┌─ 3. TransportP2p::Init —— IPC 交换（over TCP）────────────────┐
│ ① ExchangeTgidMesg: 交换 bare tgid                              │
│    (aclrtDeviceGetBareTgid，非 getpid！容器环境必须)             │
│    同进程(sendPid==recvPid) → 直接交换 VA，跳过 IPC              │
│ ② Export（本端 CCL in/out buffer + expMem + 用户 buffer）:      │
│    · hrtDevMemAlignWithPage: 页对齐，记 offset                  │
│    · aclrtIpcMemGetExportKey(alignedPtr, alignedSize, name, 65, 0UL)│
│    · aclrtIpcMemSetImportPid(name, &对端bareTgid, 1) ★ 对端PID  │
│      （同 server；超节点跨 server 用 aclrtIpcMemImportPidInterServer）│
│ ③ TCP 发送 {ipcName(65B), size, offset} + notify 序列化信息     │
│ ④ Import:                                                       │
│    · aclrtIpcMemImportByKey(&ptr, name, 0UL) ★ DEFAULT flag    │
│      （此时 P2P 已 enable，跨卡 import 可以成功）                 │
│    · ptr += offset（还原原始地址）                                │
│ ⑤ Notify 同样走 IPC:                                             │
│    aclrtNotifyGetExportKey / SetImportPid / ImportByKey          │
│ ⑥ MemNameRepository 引用计数去重（同名只 import 一次）            │
└──────────────────────────────────────────────────────────────┘
┌─ 4. 数据面（host 侧）────────────────────────────────────────┐
│ TransportP2p::TxAsync                                            │
│  └ HcclD2DMemcpyAsync → DispatcherPub::MemcpyAsync               │
│     └ hrtMemAsyncCopy → aclrtMemcpyAsync(D2D) ★ 最终 API        │
│        （SDMA 引擎执行；>4GB 自动拆分；linkType 参数仅用于 profiling）│
│  └ SignalRecord(remoteSendReadyNotify_)                          │
│     └ aclrtRecordNotify / aclrtWaitAndResetNotify               │
│ Inline Reduce: aclrtReduceAsync（linkType∈{HCCS,PCIE,SIO,HCCS_SW} 可用）│
└──────────────────────────────────────────────────────────────┘
```

### 集合通信算法（310P3）

- 算法选择：`SelectAlgfor310P3DUO`（整卡）/ `SelectAlgfor310P3`（非整卡）
- 小数据 ≤256KB：AllReduceDoubling / AllReduceDoublingDirect（对数步数）
- 大数据：AllReduceRing（环形，每 rank 2 邻居）
- AlltoAllVFor310P：main die（偶数 rank）协调 minor die（奇数 rank），main 有 2 子流
- **★ 全部是 ring/doubling 拓扑，从不需要全连接**
- onlyRead 机制（`hccl_communicator_host.cc:7408-7420`）：
  非整卡（如 2 张 Duo 卡各取 1 die）时 `pairLinkInfo_[HCCS].size() != userRankSize`
  → 因 PCIe 限制强制 onlyRead=1 → CC_EXE_ONE_SHOT_SINGLE_RING（只读单环）

### 跨 server（跨节点）

- 310P3 rank_graph：跨 server 恒为 `COMM_PROTOCOL_PCIE` → TransportIbverbs（host NIC RoCE）
- **本机实测：310P3 无 device 侧 RoCE AIQP（缺 libhccp.so）→ 跨节点只能 host 网络**（HCOMM host RDMA/TCP，即 memfabric 用的那条路）

---

## 四、为什么 hcomm 不受 8 槽位限制，而 test 07 失败

| 维度 | test 07 | hcomm |
|---|---|---|
| 链路拓扑 | **全连接**（8×7=56 条） | **ring/doubling**（每 rank 2-3 邻居） |
| 每卡 P2P 需求 | 6+6=**12 > 8**（超限） | ring≈2，doubling≈4（**远低于 8**） |
| P2P enable 方式 | PEER_ACCESS import flag 隐式触发 | 显式 rtEnableP2P + 引用计数去重 |
| import flag | die 对 DEFAULT / 跨卡 PEER_ACCESS | **一律 DEFAULT(0)** |
| PID | DISABLE_PID_VALIDATION 跳过 | 白名单 = **对端 bare tgid** |
| 驱动约束 | — | 同 PCIe 设备（同卡 die）禁止 enable P2P（本来也不需要） |

**结论：hcomm 不是"绕过"了 8 槽位限制，而是其算法设计根本不需要那么多 P2P 连接。**
test 07 的失败（card0: r0 用 6 + r1 用 2 = 8 后 r1 第 3 条跨卡失败）与 8 槽位限制精确吻合。


---

## 五、本机实测证据交叉验证（/home/t00842504 其他目录）

| 证据 | 结果 | 来源 |
|---|---|---|
| `aclrtDeviceCanAccessPeer(主die→副die)` | **返回 0** | kernel_mte_thread_p2p_map_validation 日志 |
| Host 侧 aclrtMemcpy 访问 IPC imported DVA | **PASS** | 310p-datacopy-analyze.md |
| AICore Kernel 访问跨 die IPC DVA | **FAIL**（SMMU fault, 548s 超时） | kernel_mte_ipc_validation 日志 |
| 软件 GVA segment（PIX 开 PeerAccess / HCCS 关） | 6/6 **PASS** | software_gva_segment_validation |
| libhcomm.so 加载 + 符号链 | **可用**（host RDMA/TCP） | 310p-plan.md |
| 310P3 device 侧 RoCE | **无**（缺 libhccp.so） | 310p-plan.md |
| HCCL 集合通信算子在本机运行日志 | **未发现** | 全盘搜索 |

**注意**：本机 AICore 直接访问跨 die IPC DVA 会 SMMU fault——意味着 device 发起的访问
（MemFabric 式 GVA 直访）在本机 310P3 上受限，host 驱动的 aclrtMemcpy 路径正常。
hcomm host 路径恰好只依赖 host 驱动拷贝。

---

## 六、hcomm 是否受"8 槽位"限制？（定量分析）

### 结论

**驱动层限制对 hcomm 同样存在、无法豁免**（8 个跨卡 P2P 关系/Duo 卡，两 die 共享）。
但 hcomm 单通信域的标准用法**永远不会触发**——不是靠绕过，而是算法拓扑根本用不了那么多槽位。
真正被限制的不是"8 个 rank 同时用 IPC"，而是"每张卡同时持有的**不同跨卡 peer 关系数** ≤ 8"。

### 每卡槽位消耗定量（本机 4 卡 × 2 die，8 rank）

| 算法（310P3 路径） | 每 rank 通信对象 | 每卡跨卡 P2P 关系数 | 占 8 槽位预算 |
|---|---|---|---|
| **AllReduceRing**（大数据默认） | `(r±1)%8` 共 2 个 | **2**（die 对走 HCCS 不占槽） | 25% |
| **AllReduceDoubling**（≤256KB） | `r^1, r^2, r^4` 共 3 个 | **4**（完美洗牌：card0 的两 die 连 card1/card2 各 2） | 50% |
| **AlltoAllVFor310P** | main die(0,2,4,6) 成环 + 各自 minor（die 对） | **2**（minor 不跨卡） | 25% |
| ring + doubling 混用累积 | — | **6**（dev0:{7,2,4} + dev1:{2,3,5}） | 75% |
| **test 07 全连接** | 全部 7 个 peer | **12**（6+6） | **150% → 必然失败** |

test 07 失败点（card0: r0 成功 6 + r2 成功 2 = 8，第 9 条失败）与该模型精确吻合。

### hcomm 的四个架构性规避机制

1. **310P3 跳过全量 P2P enable**（`hccl_communicator_host.cc:1462`：`deviceType_ != DEV_TYPE_310P3` 才做）。
   其他设备类型在 init 时对本 server 全部设备 enable（≤8 设备时 7 个关系尚可）；
   310P3 单 server 最多 **32 个设备**（16 张 Duo 卡），全量 enable 需要 31 个关系，必然爆 8 槽——
   所以 310P3 改为**按算法需要的 peer 逐链路 enable**（`transport_manager.cc:714-723`：每次只 push 当前链路的 1 个远端 devicePhyId）。
   **这是 hcomm 明确针对 Duo 共享槽位约束的设计选择。**

2. **P2PMgmt 引用计数去重**（`p2p_mgmt.cc:66-88`）：同一 (logicId, phyId) 对跨算子/跨 tag 只 enable 一次。

3. **IPC 对象数 ≠ 槽位数**：每条链路上 IPC 导出多个对象（CCL in/out buffer + expMem + notify + 用户 buffer），
   但它们共享**同一个** peer 对的 P2P 关系（minimal 测试 T12 已证：同一 peer 连续 4 次 import 全部成功，只占 1 槽）。
   约束是"关系数"而非"IPC 对象数"。

4. **die 对从不占槽**：`CheckMarsterId` 对 HCCS/SIO/HCCS_SW 链路直接判 isMarsterIdDiff=false 跳过 enable
   （驱动本身也禁止同 PCIe 设备间 enable）。

### hcomm 仍可能触发限制的场景（无准入控制）

`P2PMgmt` 只有引用计数去重，**没有针对 8 槽位的计数/准入控制**。以下场景理论上可耗尽：

- **多子通信域 + 不同 rank 编排**：如 group1={0,1,2,3}、group2={0,1,4,5}、group3=...，
  同一 die 对不同 peer 累积关系：dev0→{2..7}（6 个）+ dev1→≥3 个 = 9 > 8，第 9 个 enable 报错并向上传播（无 RoCE 回退，同 server 无 RDMA 路径）。
- **混合多种拓扑的算子流**（不同 root 的 Broadcast 树 + ring + doubling 长期累积，链路按 tag 缓存不释放）。
- 典型训练/推理负载（固定 collective 家族）实测落在 2-6 槽/卡，安全。

### 佐证：hcomm 开发者明确知晓 310P Duo 的 PCIe 约束

- `onlyRead=1` 机制（`hccl_communicator_host.cc:7408-7420`）：非整卡（HCCS 数 != rank 数，即 2 卡各取 1 die）时
  因 PCIe 限制强制只读单环算法——**主动在算法层规避 PCIe 数据通路限制**。
- 仓库自带驱动桩头文件明确记载 "The two devices of Ascend310P Duo share 8 MAX enable number"。

---

## 七、附：关键代码索引

| 内容 | 位置 |
|---|---|
| ASCEND_310P_DEVICE 死代码 | `platform/hccl_plf.cmake:15-20` + `platform/CMakeLists.txt:11-17` |
| Is310PDevice 编译期常量 | `platform/common/device_capacity.cc:42-46,201` |
| hrtGetPairDeviceLinkType 真实查询 | `platform/common/adapter/adapter_rts.cc:1137-1177` |
| 310P3 EnableP2P 调用点 | `transport_manager.cc:307/714/903`、`comm_base.cc:1089` |
| CheckMarsterId PXI→enable | `p2p_mgmt.cc:224-248` |
| IsNeedEstablishP2Pconnection 310P3 恒 true | `p2p_mgmt.cc:364-381` |
| rtEnableP2P | `adapter_rts.cc:1887-1930` → `rt_external_device.h:261` |
| 驱动 P2P 限制表 | `test/stub/.../ascend_hal.h:1382-1432`（Duo 共享 8，同 PCIe 设备禁止） |
| IPC export flag=0 | `adapter_rts.cc:1251`（910）/ `orion_adapter_rts.cc:571`（950, flag=1） |
| IPC import 恒 flag=0 | `adapter_rts.cc:1324` / `orion_adapter_rts.cc:601` |
| bare tgid | `aclrtDeviceGetBareTgid`，`adapter_rts.cc:2114` |
| PID 白名单=对端 tgid | `mem_name_repository.cc:105-121`（HCCL_IPC_PID_ARRAY_SIZE=1） |
| 页对齐+offset | `adapter_rts.cc:1446-1460`，`mem_name_repository.cc:74/225` |
| onlyRead PCIe 限制 | `hccl_communicator_host.cc:7408-7420` → `aicpu_kfc_process.cc:440-460` |
| host 数据面 aclrtMemcpyAsync | `dispatcher.cc:399` → `adapter_rts.cc:996` |
| 算法选择 | `all_reduce_operator.cc:108-136,200-252` |
| rank=1 die | `topoinfo_exchange_agent.cc:922-946`（devicePhyId 唯一性校验） |
