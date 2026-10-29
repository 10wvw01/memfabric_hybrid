# hcomm 的 GM 标志消息同步机制（与本项目双环邮箱对照）

> **文档定位**：长寿命参考档案。回答三个问题——hcomm（HCCL 通信基础库）里有没有
> 本项目双环邮箱（GM 标志 + 内存消息语义同步）这样的机制；每层机制**谁是生产者、
> 谁是消费者、什么场景使用**；与我们的做法差在哪。
> **证据基线**：本机 hcomm 仓快照（310P 设备代码位于 `src/legacy/ascend910/`，
> `DEV_TYPE_310P1/310P3` 有专列分支），下文全部行号经 2026-10-29 逐条核实，
> 引用前先复核行号漂移。
> **读者**：要给双环邮箱扩多生产者并行、或想对照官方做法验证"数据 SQE 与标志
> 同 SQ 保 FIFO"这条路线的机制开发者。
> **边界**：本文只记录 hcomm 怎么做，不改本项目行为；本项目行为契约以
> `openspec/specs/smem/310p-sdma-orch/` 为准，wire 配方见[机制档案](./机制档案.md)。

## 0. 结论速览

| # | 结论 |
|---|---|
| 1 | hcomm 有 GM 标志消息同步，且是**四层机制**（第 1 节），但全仓没有任何叫 flag 的类或文件（`FlagMgr/FlagInfo/HcclFlag`、文件名 `*flag*` 均零命中）——机制藏在 SQE 类型、算法模板和 KFC 框架里 |
| 2 | 与本项目双环邮箱最接近的是**第 2 层 MC2/KFC 轮次计数邮箱**：设备上两个执行体（客户端 kernel 与常驻 AICPU 服务进程）用 GM 消息槽 + 轮次计数接力，与我们"AICore↔AICPU 双环"同构、角色互补 |
| 3 | 核心顺序保证与本项目**完全同款**：数据先走、标志后到，且同走一条队列靠 FIFO 保序（Wait→数据→Post 三连）——官方与本项目独立走到了同一条路线，互证正确 |
| 4 | 分歧在认领模型：hcomm **不做跨生产者原子领槽**，全部静态分槽（按 rank/通知类型/队列固定编址）；GM 原子（atomic add）只用于 N:1 到达计数 |
| 5 | 回绕处理是"末轮清零"，不是环形复用；AICore 侧另有 ping-pong 双 bank 与三缓冲 bank |
| 6 | reduce 操作码两套世代互不兼容（V2 枚举 vs V1 拼接），固件只认权威枚举——与本项目 KVER=11 自造 0x71 编码固件不识别的教训同源 |

## 1. 四层机制总览：谁生产、谁消费、什么场景

| 层 | 使用场景 | 生产者（写标志方） | 消费者（等标志方） | 标志载体 |
|---|---|---|---|---|
| 1. AICore 直接信号 | AIV 集合算法的通信步步间同步与到达屏障 | 发送方 rank 的 AIV 核：先拷数据、后往**对端** flag 区写本步 tag | 接收方 AIV 核：轮询**本端** flag 区，tag 匹配才读数据 | 每 rank CCL buffer 头部 3MB flag 区，32B/槽 |
| 2. MC2/KFC 轮次邮箱 | 高层 API 算子把通信操作托付给常驻 AICPU 服务进程接力执行 | 客户端算子 kernel（cann/hccl 仓）：写消息槽 + 推进 commitTurnCnt 轮次计数 | AICPU KFC 服务进程：硬件轮询 commit → 取消息 → 执行数据算子；客户端再等 finishedTurnCnt 回执 | workspace 消息槽数组 + 每槽轮次计数 |
| 3. SQE 类型标志 | host/AICPU 编排的流式通信逐链路握手（如 ring 每步 TxAck/RxAck） | 发送方编排者：在发送流上"数据 SQE 之后"追加 NOTIFY_RECORD/WRITE_VALUE SQE，由队列引擎按序落笔 | 接收方编排者下发的 NOTIFY_WAIT SQE：硬件等待，等到才放行读数据 | MMIO notify 信号量表 / WRITE_VALUE 直写 GM |
| 4. RDMA 三 flag | 服务器间 RoCE 链路收发握手 | 对端 AICPU transport：RDMA WRITE 连数据带 flag | 本端 AICPU transport：自旋轮询 | HBM flag buffer，3×u32 |

四层的标志流向一图流（`──▶` 为写入方向，`◀──` 为轮询等待）：

```
第 1 层  发送方 AIV 核 ──CpGM2GM 数据──▶ 接收方数据区
         发送方 AIV 核 ──Record(tag)──▶ 接收方 flag 区 ◀──Wait(tag) 轮询── 接收方 AIV 核

第 2 层  客户端 kernel ──消息 + commit 轮次++──▶ workspace ◀──COND SQE 硬件轮询── AICPU 服务进程
         AICPU 服务进程 ──执行数据算子──▶ finished 轮次++ ──▶ 客户端 kernel（等回执）

第 3 层  发送方流: [数据 SQE → NOTIFY_RECORD SQE] ──队列引擎 FIFO──▶ 标志按序落笔
         接收方流: [NOTIFY_WAIT SQE] 硬件等待 ──▶ 才放行后续读数据 SQE

第 4 层  对端 AICPU ──RDMA WRITE(数据 + flag1/2/3)──▶ 本端 HBM ◀──WaitAsyncFlag 自旋── 本端 AICPU
```

## 2. 逐层细节

### 2.1 第 1 层：AICore 直接 GM 信号——AIV 集合算法的步步间同步

**场景**：AIV（AICore vector 核）上跑的跨节点/跨 rank 集合算法。每个通信步"数据
先走、标志后到"：发送核把数据 `CpGM2GM`（GM→UB→GM 中转，支持原子累加，
`aiv_crossnode_91093_base.h:658-696`）拷到对端输出区，随后往**对端** flag 区写
本步 tag；接收核轮询**本端** flag 区，tag 匹配才读数据。三种编队形态：

- **1:1 定向**（`Record/Wait`，tag 等值判定）——逐链路步同步；`BatchRecordWait`
  注释即"写所有对端的flag"（`:740-744`），对端地址来自 kernel 参数
  `GM_OUT[i]`（`aiv_communication_base.h:466`）；
- **N:1 到达屏障**（`RecordNv1/WaitNv1`）——多写者往同一聚合槽**原子累加**，
  等待方轮询累加和达标；源码注释原文："其他核去写该flag，做原子累加达到核间
  同步的目的"（`:717-726`）——既做**设备内核间**屏障，也做跨 rank 到达屏障；
- **1vN 广播通知**（`Record1vN/Wait1vN`）——一写多槽，广播完成。

**生产者**：发送方 rank 的 AIV 核（算法 kernel 内联原语；数据与标志都由它写，
标志在数据后）。
**消费者**：接收方 rank 的 AIV 核（轮询本端 flag 区）。

原语四件套（`aiv_interface/sync_interface.h:29-98`）：

```cpp
SetSignalValue(...)    // UB→GM 直写 32B；写前 SyncFunc<S_MTE3>() 管线事件
WaitSignalValue(...)   // GM→UB 轮询循环，读后 SyncFunc<MTE2_S>()，值==期望即返回
WaitSignalGEValue(...) // 同上，值>=期望即返回（轮次语义）
AddSignalValue(...)    // SetAtomicAdd<int32_t>() + PipeBarrier<PIPE_ALL>()，GM 原子加
```

- flag 区布局：`AIV_FLAG_BUFFER_SIZE=3MB`、每标志 `FLAG_SIZE=32B`、三缓冲
  `FLAG_BUF_NUM=3`、原子聚合槽 `ATOMIC_FLAG_SIZE=512B`（`aiv_communication_base.h:155/203/204/238`）；
  CCL buffer 按 `AIV_FLAG_SIZE=4MB + AIV_DATA_SIZE=36MB` 分配（`ccl_buffer_manager.h:21-22`）。
- **槽位静态编址，无领槽**：`Record` 偏移 =
  `(blockIdxInGroup*rankSize*FLAG_BUF_NUM + notifyType*rankSize + rank_) * FLAG_SIZE`
  （`aiv_crossnode_91093_base.h:755-758`）——每个生产者（block/rank/通知类型组合）
  固定拥有自己在对端 flag 区里的槽，写前不申请、不竞争。
- 可见性纪律：每次 GM 标志访问用管线事件（S_MTE3 / MTE2_S）兜住，原子加前后
  `PipeBarrier<PIPE_ALL>`；ping-pong（tag%2 偏移）与三缓冲 bank 避免读写自干扰。
- 勿混淆：AscendC 的 `SetFlag<event>/WaitFlag<event>` 是核内管线事件，不是 GM 标志。

### 2.2 第 2 层：MC2/KFC 轮次计数邮箱——算子执行托付常驻 AICPU 服务

**场景**：host 发起集合通信后，客户端算子 kernel 不自己搬数据，而是把"要执行
什么通信操作"写成消息投进 workspace 邮箱，由**常驻 AICPU 服务进程（KFC）**接力
执行——host 不再逐流编排，设备上自接力。
**生产者**：客户端算子 kernel（在 cann/hccl 仓，本仓只见工作区布局与消费侧）——
写 `sendMsgs[队列][槽]` 消息，并把 `commitTurnCnt[槽]` 推进到本轮 turn 值。
**消费者**：AICPU KFC 服务进程——每（句柄，重复轮）三连：`HcclLaunchCcoreWait`
（COND SQE 硬件轮询 commitTurnCnt 到本轮）→ 取消息执行数据算子（`HcclLaunchOp`）
→ `HcclLaunchCcorePost`（回写 finishedTurnCnt 回执）（`comm_kfc_aicpu_server.cc:121-123`）；
客户端轮询 finishedTurnCnt 得知完成。注意这是**请求与回执两对生产/消费**：消息
正向（客户端→服务端），回执反向（服务端→客户端）。

细节：

- 消息面（tiling 提供的 GM workspace，512B 对齐）：`HcclMsgArea =
  sendMsgs[队列数][HCCL_MSG_CNT] + commitTurnCnt[].cnt + finishedTurnCnt[].cnt + …`
  （`aicpu_kfc_rpc_serverv2.cc:145-169`）。
- **轮次计数（turn）**：`turnNum = (repeatCnt-1)*rankSize + (step+1)`，单调递增
  （`mc2_handler.cc:41`）；期望值来自预填 0,1,2,… 的等差数组（`:19`）。
  `Mc2WaitValue` 让 COND SQE 盯 `commitAddr` 直到计数达到本轮 turn（`:46`）；
  `Mc2WriteValue` 向 `finishAddr` 写完成 turn（`:60`）。`Mc2Handler` 结构见
  `algorithm/pub_inc/op_context.h:49`。
- **COND SQE 是硬件轮询器**：SQE 内嵌一段小 ISA（`aicpu_hccl_sqcq.cc:199-219`）——
  load 轮值到 r3、load 发送计数到 r2，`r2 < r3` 就回跳重读（BLTU 循环）；
  **最后一轮附一条 store 0 清零发送计数**（回绕 = 末轮清零，非环形复用）。
- **KFC 服务循环消费**：每队列 `msgPos` 游标环回
  `SetMsgPos(i, (currMsgPos+1) % HCCL_MSG_CNT)`（`aicpu_kfc_process.cc:1151`）；
  消费时三重校验——valid 掩码（非 `HCCL_MSG_VALID_MASK` 视为空，消费后复位
  `~MASK`，`aicpu_kfc_rpc_serverv2.cc:370/396`）、xorCheck 校验（`:379-384`）、
  seqNum 期望值（小于期望直接丢弃，`ProcessExpectPrepareMsg :268-283`）；
  读写边界 `dsb ld/st`（`:319/:326/:361` 等）。
- **多队列静态分片**：`GetLocalQueueRange` 把消息队列按 AICPU block 切片
  （`aicpu_kfc_rpc_serverv2.cc:65`）；初始化时全部槽复位（valid=~MASK、cnt=0，
  `:467-475`）。

### 2.3 第 3 层：SQE 类型标志——传输层逐链路握手

**场景**：host/AICPU 编排的流式通信（典型 ring 算法）：每条链路每个通信步一次
握手——发送方"数据 SQE 之后"追加 record 标志 SQE，接收方 wait 标志 SQE 硬件
等待，等到才放行读数据。跨服务器 HCCS 链路上，notify record 改走 SDMA（搬一片
预填 1 的 buffer 到对端 notify 地址，链路异常可被 STARS 检出触发重执行）。
**生产者**：发送方的编排者——host 调度器 `DispatcherPub::SignalRecord`（走
runtime `hrtNotifyRecord`，`dispatcher.cc:258-262`）或 AICPU 服务进程
`DispatcherAiCpu::SignalRecord`（构造 SQE，`dispatcher_aicpu.cc:173`）；物理
落笔者是队列引擎按序执行 SQE（写 MMIO notify 表或 GM）。AICPU 下发的 p2p
record 前还有 cache-write 占位 SQE 保数据可见（`dispatcher_aicpu.cc:199-200` 注释）。
**消费者**：接收方编排者下发的 NOTIFY_WAIT SQE（硬件等待）。

细节：

- 310P 用 V2 类型表（`aicpu_hccl_sqcqv2.h:19-31`）：`EVENT_RECORD/WAIT=4/5`、
  `NOTIFY_RECORD/WAIT=6/7`、`WRITE_VALUE=8`、`SDMA=9`。310P1/310P3 在调度器里有
  专列分支选 V2 构造器（`dispatcher_aicpu.cc:74/319/330`）；V2 的 notify 结构注释
  "内部替换为 event 的 notify"（`sqcqv2.h:50`），EVENT_RESET 任务由 WRITE_VALUE
  SQE 承载（`:28` 注释）。
- 硬件 notify 表是内存映射寄存器不是 DRAM（`STARS_NOTIFY_BASE_ADDR=0x100000`，
  `aicpu_hccl_sqcq.h:109`）；环算法每步 `TxAck/RxAck` 走 notify
  （`reduce_ring.cc:81-152`）；每远端 rank 一套 stream+notify 四元组
  （`ipcPre/PostRecord/WaitNotify`，`aicpu_kfc_utils.cc:227-239`）。
- 跨服务器 notify 改走 SDMA：`useSdmaToSignalRecord_`（节点间 HCCS 时置位，
  `transport_p2p.cc:182-188`）+ `CreateNotifyValueBuffer` 预填 1 的 HBM buffer
  （`:246-272`）——本项目"SDMA inline 标志"模式的官方版。
- SQ 环：`SqeRingBuffer` + `GetNextSqeBufferAddr`（`stream_pub.h:42/115`），
  数据 SQE 与标志 SQE 同环追加——FIFO 保序的物理基础。
- **无"flag 操作码"**：标志是 SQE *类型*（WRITE_VALUE/COND/NOTIFY），不是 SDMA
  数据操作码。reduce 操作码权威枚举：`NOT_ATOMIC=0 / INT_16=1 / FLOAT_32=2 /
  FLOAT_16=3`（`aicpu_hccl_sqcqv2.cc:15-20`）；V1（910B 世代）另一套拼接式编码
  （数据类型 FP16=0x60/FP32=0x70，`aicpu_hccl_sqcq.h:235-236`），两套互不兼容。

### 2.4 第 4 层：RDMA 三 flag——服务器间 RoCE 握手

**场景**：服务器间 RoCE 链路：对端先写就绪 flag、RDMA WRITE 数据、再写"数据
已写"flag，尾部同步 flag 收尾。
**生产者**：对端 AICPU transport 线程（RDMA WRITE 连数据带 flag）。
**消费者**：本端 AICPU transport 线程（`WaitAsyncFlag` 自旋，
`framework/device/framework/aicpu_hccl_process.cc:596`）。
3 个 u32 flag（`POST_SEND_FLAG_COUNT=3`，`FLAG_OFFSET=1/FLAG_INTERVAL=2`，
`aicpu_kfc_def.h:53-55`）。服务器间专用，310P 单机场景用不到。

## 3. 与本项目双环邮箱对照

先答我方三件事（证据：wire 契约 `src/hybm/ops/hybm_kernel/hybm_sdma_contract.h`，
KVER=28 快照；布局单一事实源 `src/smem/include/device/smem_shm_sdma_layout.h`）：

**场景**：用户 AICore 算子在设备上直接发起跨 rank 搬运/归约（signal/wait/quiet
设备侧自驱），host 只建池与起停 epoch kernel，不进数据面（contract.h:18-19）。
**请求环（AICore→AICPU）**：生产者 = 本端调 signal 的 AICore（写请求邮件 +
`RS_REQ_TAIL` 生产游标，layout.h:49-51）；消费者 = 本端 AICPU epoch kernel
（按 `RS_REQ_HEAD` 消费游标取件、构造 SDMA SQE，`hybm_sdma_orch_kernel.cc:38`）。
**到达环（AICPU→AICore）**：生产者 = 数据接收方的 AICPU kernel（SQE 完成后往
该端 `RS_ARR_MAIL` 写到达邮件）；消费者 = 接收方调 wait 的 AICore
（`RS_ARR_HEAD` 游标推进、按 imm 匹配）。

与 hcomm 第 2 层同构：都是"设备上两个执行体用 GM 邮箱接力"，角色互补——hcomm
是客户端（AICore 算子）请求、常驻 AICPU 服务执行；我方请求环同向
（AICore→AICPU），到达环反向（AICPU→AICore），构成双向闭环。

| 本项目双环邮箱 | hcomm 对应物（层） | 备注 |
|---|---|---|
| 请求环 `RS_REQ_RING` 64 槽，邮件 `HybmSdmaReqMail` 32B（contract.h:37-42，layout.h:49-50） | `sendMsgs[队列][槽]`（第 2 层） | 同为 GM 槽数组，方向同为"发起方→执行方" |
| 生产游标 `RS_REQ_TAIL`（AICore 写）/消费游标 `RS_REQ_HEAD`（AICPU 写），独占缓存行（layout.h:51-52） | 每队列 `msgPos` 游标环回（第 2 层） | 都是单消费者按序推进；hcomm 槽本身不带生产游标，生产进度由 commitTurnCnt 表达、COND SQE 硬件轮询 |
| 到达环 `RS_ARR_MAIL` 1024 深度 + 消费游标 `RS_ARR_HEAD`（AICore 写，layout.h:57-58） | `finishedTurnCnt` 完成计数 + 客户端轮询（第 2 层） | 回执方向一致；hcomm 用"值>=turn"比较，单调值本身就是状态，无需独立消费游标 |
| imm 标签路由 `[63:48]magic/[47:40]opcode/[39:0]tag`（contract.h:44-62） | tag 值标志（值==tag 判定）+ 等差 turnNums 数组（第 1/2 层） | 同为"值即语义"，免额外元数据握手 |
| epoch 分轮消费（epochBudgetS 15s + host 心跳报时，contract.h:76/104-112） | turnNum 逻辑轮次（按通信步单调） | 时间轮 vs 逻辑轮；hcomm 轮次与通信步一一对应 |
| 数据 SQE 与标志同 SQ 保 FIFO（orch_kernel.cc:190 满环活查询注释自注"hcomm LaunchTask 同款"） | Wait→数据→Post 同 SQ 环（第 2 层，comm_kfc_aicpu_server.cc:121-123） | **完全一致，互证正确** |
| reduce 构造 FP32=0x02/FP16=0x03 SQE（contract.h:51-52，注释直引 hcomm 权威源） | `SdmaReduceOpcode` 枚举（第 3 层，sqcqv2.cc:15-20） | 同一权威源 |
| 完成戳 `RS_QUIET` 64 槽（SQE 写，layout.h:53-54） | `finishedTurnCnt` 回写（第 2 层） | quiet（静默/全部落地）语义两边都有 |
| 暂存区 `RS_STAGE_SLOTS=512`（到达戳数据源本端暂存，layout.h:65） | —（AICPU 内存天然一致，无需暂存） | 我方为 AICore DCache 双向陷阱的对策（机制档案定论 2） |

## 4. 关键差异与取舍

| # | 维度 | hcomm | 本项目 | 取舍理由 |
|---|---|---|---|---|
| 1 | 槽位认领 | 静态分槽（rank/通知类型/队列固定编址），零原子零竞争 | 单生产者单消费者双环 + 独占行游标 | 生产者模型不同：hcomm 的生产者集合（rank/平面/队列）静态已知；本项目单流水线内环+游标最简 |
| 2 | 多生产者扩展 | 加槽/加队列（多 SQ 分片），从不在共享槽上做原子领槽 | 同路线（imm 路由 + 多 SQ 分片） | GM 原子只在 N:1 汇聚值得付（hcomm 的 AddSignalValue 到达计数） |
| 3 | 回绕 | 末轮清零 + ping-pong/三缓冲 bank | 环形复用（模运算） | 两侧都以单调值为语义锚（turn/seq）；清零实现更简，环形复用吞吐更稳 |
| 4 | 可见性 | AICore 管线事件 + AICPU dsb/dmb，无 cache 维护 | dcci 纪律（机制档案定论 2/3） | 同一问题两套解：hcomm 靠一致性与屏障，我方显式维护缓存行 |
| 5 | 硬件 notify | 与 GM 标志并存（传输层 notify、算法层 GM） | 全内存方案（定论 5：310P3 notify/事件对全弃用） | hcomm 自己在 310P V2 上也把 notify 内部替换成 event——佐证我方弃用决策 |

## 5. 对本项目的启示

1. **"数据 SQE 与标志同 SQ FIFO"路线定案**：官方 Wait→数据→Post 与我方完全同构，
   且我方满环活查询重算本就自注"hcomm LaunchTask 同款"（orch_kernel.cc:190）——
   这条核心保证无需再动摇。
2. **多生产者并行的官方答案**：静态分槽（按 rank 固定槽/多 SQ 分片），不是共享环上
   原子领槽；GM 原子只做 N:1 到达计数（AddSignalValue + sum==期望值）——与我方
   imm 路由的定位一致，扩展时照此加槽/加队列即可。
3. **防陈旧/损坏消息三重校验**（valid 掩码 + seqNum 期望值 + xorCheck）值得借鉴：
   我方排 507014 丢件时自建的逐单日志（contract.h:113-132）是同思路的诊断面；
   若未来要把"消费侧拒收"做成契约，可参考这套组合。
4. **操作码教训普适**：V1/V2 两套编码互不兼容、固件只认权威枚举——我方 KVER=11
   自造 0x71/0x61 固件静默丢弃事故（已修正为 0x02/0x03，contract.h:48-49 注释
   留档）与此同源；跨平台移植务必重查目标固件的权威枚举。
5. **值即语义（tag/turn/等差数组）**：用比较值代替清零握手，等待侧 `>=` 判定天然
   免疫乱序到达——与我方 seq/imm 设计同宗，可继续沿用。

## 6. hcomm 侧引用位置清单

全部位于 `/home/w00930648/hcomm/src/legacy/ascend910/` 下，行号 2026-10-29 快照：

| 主题 | 位置 |
|---|---|
| AICore GM 信号原语 | algorithm/base/alg_aiv_template/aiv_interface/sync_interface.h:29-98 |
| flag 区常量 | algorithm/base/alg_aiv_template/aiv_communication_base.h:155/203-204/238-241 |
| 对端输出区地址（GM_OUT） | algorithm/base/alg_aiv_template/aiv_communication_base.h:466 |
| GM→GM 数据中转（CpGM2GM） | algorithm/base/alg_aiv_template/aiv_crossnode_91093_base.h:658-696 |
| 三种编队形态（1:1 / N:1 / 1vN） | algorithm/base/alg_aiv_template/aiv_crossnode_91093_base.h:707-744 |
| 槽位公式 / N:1 原子计数 | algorithm/base/alg_aiv_template/aiv_crossnode_91093_base.h:755-758/795-806/831-841 |
| CCL buffer 尺寸 | algorithm/impl/resource_manager/ccl_buffer_manager.h:21-22 |
| turnNum 公式与 wait/write | algorithm/base/mc2_handler/mc2_handler.cc:19/41/46/60 |
| Mc2Handler 结构 | algorithm/pub_inc/op_context.h:49 |
| COND SQE 内嵌 ISA / 写值 SQE | platform/task/rtsq_interact/aicpu_hccl_sqcq.cc:182-261 |
| 服务端 Wait→数据→Post 编排 | framework/device/aicpu_kfc/decoupler/comm_kfc_aicpu_server.cc:121-123 |
| 邮箱消费/校验/复位 | framework/device/aicpu_kfc/framework/aicpu_kfc_rpc_serverv2.cc:145-169/268-283/319-396/467-475 |
| msgPos 游标环回 | framework/device/aicpu_kfc/framework/aicpu_kfc_process.cc:1151 |
| 队列分片 | framework/device/aicpu_kfc/framework/aicpu_kfc_rpc_serverv2.cc:65 |
| V2 SQE 类型表 / notify 内部替换为 event | pub_inc/aicpu/aicpu_hccl_sqcqv2.h:19-31/28/50 |
| reduce 操作码权威枚举 | platform/task/rtsq_interact/aicpu_hccl_sqcqv2.cc:15-20 |
| V1 拼接编码（910B） | pub_inc/aicpu/aicpu_hccl_sqcq.h:235-236 |
| notify 表基址（内存映射寄存器） | pub_inc/aicpu/aicpu_hccl_sqcq.h:109 |
| host 侧 notify 收发 | platform/task/dispatcher.cc:258-262/304 |
| AICPU 侧 notify 收发（含 p2p cache 占位） | platform/task/dispatcher_aicpu.cc:173/199-200 |
| 310P 专列分支 | platform/task/dispatcher_aicpu.cc:74/319/330 |
| 环算法 notify 收发 | algorithm/base/alg_template/temp_reduce/reduce_ring.cc:81-152 |
| 跨服务器 notify 走 SDMA | platform/resource/transport/host/transport_p2p.cc:182-188/246-272 |
| SQ 环原语 | pub_inc/stream_pub.h:42/115 |
| RDMA 三 flag | framework/device/aicpu_kfc/common/aicpu_kfc_def.h:53-55；framework/device/framework/aicpu_hccl_process.cc:596 |

## 术语注释表

| 术语 | 含义 |
|---|---|
| SQ / SQE | 提交队列 / 提交队列描述符（投给队列引擎的 64B 指令） |
| COND SQE | 条件类型 SQE，内嵌一段小指令程序，由队列引擎在 GM 上循环轮询/写值 |
| notify | 硬件信号量（内存映射寄存器表），RECORD 记录、WAIT 等待 |
| turn / 轮次 | hcomm 的单调轮次计数，等价于按通信步递增的轮次号 |
| KFC | kernel framework core，hcomm 的设备侧框架进程（AICPU 上常驻） |
| AIV | AICore vector 核 |
| 双环邮箱 | 本项目 SDMA mail 机制：请求环（AICore→AICPU）+ 到达环（AICPU→AICore），单生产者单消费者，游标独占缓存行 |
| imm | 立即数标签，本项目请求邮件第 4 字：magic/opcode/用户标签编码 |
