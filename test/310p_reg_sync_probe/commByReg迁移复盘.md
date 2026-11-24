# commByReg 从 pypto 迁移到 memfabric:原理梳理、验证边界与问题复盘

> 2026-11-20 初稿;2026-11-23 订正(补代码位置;订正"常驻/握手/精确寻址"的错误因果;
> 补"那段扫描为什么可能轮不到执行")。本文回答一个问题:**pypto 上 commByReg 跑得好好的,
> 探针也穿刺通过了,为什么迁移到 memfabric 就挂死?** 先把三样东西的原理讲清楚(pypto 原始实现、
> 探针验证、memfabric 迁移),再对照挂死现场逐条分析差在哪,最后给出未裁决的假设清单。
> 本文只梳理,不做新实验。

---

## 1. 名词先行(讲人话)

- **AICore**:矩阵核,跑计算 kernel。我们用 bisheng 编译,`<<<...>>>` 语法由 ACL 运行时调度上核。
- **AICPU**:设备上的小 CPU,跑 CANN 的调度进程(CP1),能 dlopen 我们的 so、能用 volatile 直接读写物理寄存器。
- **SPR / MMIO**:AICore 每个物理核有一组私有寄存器(SPR)。驱动开放了一个窗口(halMemCtl,type 2),
  把 10 个 AICore 核的控制寄存器映射成 AICPU 进程里的一段地址——AICPU 往这段地址写,等于直接写
  某个指定核的寄存器;AICore 用 `set_cond` / `MOV DATA_MAIN_BASE` 这类硬件指令读写自己核的寄存器。
- **COND 寄存器(0x4C8)**:AICore 用 `set_cond(value)` 指令写;AICPU 可经 MMIO 窗口 volatile 读。
- **DATA_MAIN_BASE(0xA0)**:AICPU 可经 MMIO 写;AICore 用一条汇编 `MOV %0, DATA_MAIN_BASE` 读到自己核的这个值。
- **commByReg**:用上述两个寄存器在 AICPU 和 AICore 之间传同步信号,代替"往 GM 内存写标志 + 对方刷缓存轮询"的老办法。

---

## 2. pypto 的 commByReg 是怎么工作的

### 2.1 执行模型:这是全部分歧的根源

pypto 里 AICore **不是**我们平时那种"launch 一次、跑完就退出"的 kernel。pypto 的 AICore kernel 是
**常驻的**:

```
AICore 主循环(常驻):
  taskId = 轮询自己的 0xA0 寄存器(等 AICPU 派发)
  取任务 → 执行
  set_cond(完成号)   ← 写自己的 0x4C8
  回到循环开头
```

AICPU 侧则是一个调度器,负责给它管理的每个 AICore block 派任务、收完成。

### 2.2 关键设计:握手上报物理核号,之后一切寻址按映射表

ACL 把 kernel 调度到哪个物理核,**pypto 也不假设**。它的做法是启动握手:

1. AICore 启动后先写 GM 握手缓冲,报 "HELLO",同时**把自己的物理核号 get_coreid() 放在高 32 位**
   —— `framework/src/interface/machine/device/tilefwk/aicore_entry.h:270-281` `HandshakeClient()`;
2. AICPU 读到 HELLO,取出核号,填进映射表 `GetPhyIdByBlockId(blockIdx) = phyId`
   —— `experiments/runtime/src/machine/device/dynamic/aicore_hal.h:514-535` `TryHandShakeByGm()`
   (映射表在 :526 用上报的核号填充);
3. `aicore_manager.h:509-521` 的 `HandShake` / :2025 的 `while (handShakeNum < mngAicoreNum)`
   等全部核报到完毕,映射表才算齐,之后才敢往具体核写。

之后所有寄存器操作都查这张表:

```cpp
// aicore_hal.h —— 所有读写都经 GetPhyIdByBlockId() 寻址,不做等差猜测
WriteReg32(coreIdx, offset, val):   // :152-159
    addr = regAddrs_[GetPhyIdByBlockId(coreIdx)] + offset;
    *(volatile uint32_t*)addr = val;

SetReadyQueue(coreIdx, value):      // :224-236, C2 派发=AICPU 写目标核 0xA0
    *readyRegQueues_[GetPhyIdByBlockId(coreIdx)] = value;
    // readyRegQueues_[i] = regAddrs_[i] + 0xA0,初始化见 :403-418

GetFinishedTask(coreIdx):           // :342-354, C1 收完成=AICPU 读目标核 0x4C8
    return *finishRegQueues_[GetPhyIdByBlockId(coreIdx)];
    // finishRegQueues_[i] = regAddrs_[i] + 0x4C8,初始化见 :403-418
```

`regAddrs_[]` 是 host 侧经 halMemCtl **按核查询**、随 DeviceArgs 下发到 AICPU 的基址数组
(`aicore_hal.h:64` `regAddrs_ = deviceArgs->coreRegAddr`),成员数 = 物理核数(310P3 为 10)。
每个成员是一个物理核寄存器块在 AICPU 地址空间里的起始虚地址。代码从不按"一个基址 + 核号 × 2MB"
现算,原因有三:一是 halMemCtl 返回什么就用什么;二是下标要先经 `GetPhyIdByBlockId` 换成
物理核号,映射可能把下标打散;三是核间间隔属于芯片布局,不跨芯片保证。310P3 上观测到间隔 2MB,
但不能写死。

> 顺带回答一个常见疑问:0xA0 是"寄存器地址"吗?——对 AICore 它是寄存器名
> (`MOV %0, DATA_MAIN_BASE`),对 AICPU 它是"该核寄存器块内的偏移量",两者指同一硬件。
> AICPU 写 `regAddrs_[物理核号] + 0xA0` 这个**地址**,就等于写那个核的 DATA_MAIN_BASE 寄存器。

### 2.3 两个方向

| 方向 | AICPU | AICore | 语义 |
|---|---|---|---|
| 派发(C2) | 写目标核 `0xA0` = 任务号(映射表寻址) | 裸汇编 `MOV %0, DATA_MAIN_BASE` 轮询读自己核 | AICPU → AICore:下一件活 |
| 完成(C1) | volatile 读目标核 `0x4C8`(映射表寻址) | `set_cond(完成号)` 写自己核 | AICore → AICPU:活干完了 |

写入的具体值:
- C2 派发写 `encodeTaskId + 1`(`aicore_manager.h:1131`),AICore 侧读回后
  `(rawRegVal & 0xFFFFFFFF) - 1` 还原任务号(`aicore_entry.h:219`);高 32 位在 GM 备用模式下
  承载 DCCI 元数据。
- C1 完成写 `curTaskIdx | AICORE_FIN_MASK`(`aicore_entry.h:303-309` `SendRegFinish`);
  收到任务时的 ack 写 `taskIdx`(:311-313)。

AICore 侧读寄存器的实现只有一条汇编(`aicore_entry.h:160-165`):

```cpp
INLINE uint64_t GetDataMainBase()
{
    uint64_t coreStatus = 0;
    __asm__ volatile("MOV %0, DATA_MAIN_BASE\n" : "+l"(coreStatus));
    return coreStatus;
}
```

主循环轮询体在 `aicore_entry.h:207-244` `GetNextLeafTask()`:死循环反复读上面的寄存器,
低 32 位值一变就取走任务;每 1000 次查一次超时,超时 `Trap()` 自陷。kernel 入口先
`HandshakeClient`(:726)再进这个循环(:756)。

### 2.4 一个容易忽略但重要的细节:set_cond 之后 AICore 干什么

看 pypto 的 AICore 主循环(aicore_entry.h):`SendCond`(`set_cond`)是**任务收尾动作**,
执行完之后回到循环开头去轮询自己的 0xA0 寄存器——即 **set_cond 之后 AICore 只做"读自己 SPR"
这一件事,不再碰 GM、不做长的数据搬运**。也就是说,pypto 从来没有出现过
"set_cond 之后继续跑长 GM 读写流程"这种执行序列。这一点后文要考。

---

## 3. 探针(reg_sync_e2e)验证了什么、验证到什么程度

### 3.1 探针结构

三个 kernel(host 协调):

- **Send kernel**(AICore):set_cond 写一个值(C1 方向);
- **Recv kernel**(AICore):用 pypto 同款裸汇编轮询 `DATA_MAIN_BASE`,看是否收到 AICPU 写的值(C2 方向);
- **AICPU kernel**:MMIO 写目标核 0xA0 = 0x1234(C2),MMIO 读目标核 0x4C8(C1)。

### 3.2 探针曾误报成失败,恰好暴露了关键前提

旧探针一度判定"C2 不可达",真因是**核错配**:ACL 逐次 launch 的 kernel 每次落在不同的物理核
(实测组合 4/5、5/6、7/0、3/4,每轮都不同)。旧探针按 Send kernel 的核号让 AICPU 写 0xA0,
而读方 Recv kernel 在另一个核上读自己的 DATA_MAIN_BASE——**写读异核**,读到的永远是 0。
通道本身一直是好的,是探针自己把回信写错了核。

修复:Recv kernel 启动时立刻 `get_coreid()` 上报自己的物理核号,host 传给 AICPU kernel,
AICPU **精确写 Recv 所在的那个核**。三轮复测 C1+C2 全过(EXIT=0)。

### 3.3 由此确认的验证边界(诚实清单)

探针证实的:
- C2:AICPU 写"目标核"的 0xA0 → 该核上的 AICore 用裸汇编能读到 ✅
- C1:AICore set_cond → AICPU volatile 读对应核的 0x4C8 能读出值 ✅
- bareasm 读法可行,intrinsic 不可靠(intPollCnt=0,循环体被编译器消掉)✅

探针**没有**验证的(当时也没意识到要验证):
1. **"全核扫描判变化"的唤醒模式**——探针 AICPU 是"精确写一个已知核",从不做"轮询 10 个核的
   COND,任一变化就当作有活动";
2. **set_cond 之后 AICore 继续执行长流程**——探针里 set_cond 接近 kernel 尾部,写完马上收尾;
3. **高频 MMIO 轮询下 COND 窗口的行为**——探针是单次读写,不是每 100 微秒扫 10 个核的持续轮询。

---

## 4. 我是怎么把特性迁到 memfabric 的

### 4.1 memfabric 的执行模型与 pypto 完全不同——所以设计被改了

memfabric 里 AICore kernel 是 **ex08 示例那种普通业务 kernel**:launch、跑完、退出。
物理核不固定(上面实测)。AICore 和 AICPU 的协作是"请求环":AICore 往 GM 保留区写 32 字节请求
邮件(源/目的/长度),推 tail 游标,然后自旋等 quiet 戳;AICPU 的 orch kernel 起码每 15 秒一个
epoch 常驻消费,把邮件翻译成 SDMA SQE 走硬件搬运通道。

> **订正(2026-11-23)**:初稿在这里写"因为没有常驻 AICore、没有握手协议,所以精确按核寻址
> 搬不过来"——这是错的。常驻、握手、精确寻址、AICPU 主动派发是四件互相独立的事,见 4.3 节。
> 常驻只否掉"主动派发"这一项;握手和精确寻址 memfabric 完全可以有,只是当前设计用不上。

于是我做了一个设计降级:寄存器不再承载任何语义,只当**通知信号**用:

```
memfabric commByReg=1 的实际设计:

AICore 侧(smem_shm_aicore_base_sdma.h, SdmaSubmitAt 尾部):
    写完邮件、推进 tail 之后:
    if (ctx.commByReg != 0) {
        SdmaSetCond((block << 32) | (tail + 1));   // 发通知:写自己核的 0x4C8
    }

AICPU 侧(hybm_sdma_orch_kernel.cc 主循环):
    空闲时(idleSpins > 0)不扫 GM,只读 10 个核的 COND MMIO:
        for (c = 0..10): v = volatile read(regBase[c] + 0x4C8)
                          v != lastCond[c] → 有变化 = woken
    woken → 去 GM 扫 24 个请求队列;
    连续 4096 次没变化 → 强制把 GM 全扫一遍(kCondForceScan,备用做法)。
    (这么做的目的:就算通知信号全丢,也退化成 commByReg=0 的纯 GM 轮询,不该挂死)
```

数据面(邮件、tail/head 游标、quiet 戳)与 commByReg=0 时**一字不差**,全走 GM。

### 4.2 与 pypto 的逐条差异(这张表是全文核心)

| 维度 | pypto | memfabric 迁移版 |
|---|---|---|
| AICore 生命周期 | 常驻,启动握手 | ACL 逐次 launch,跑完退出 |
| 物理核确定方式 | AICORE 自己上报,get_coreid(),映射表 | **不确定**,所以根本不做按核寻址 |
| AICPU 读 COND 的方式 | 按映射表读**特定核**,读出的值是语义(完成序号) | 每轮扫**全部 10 核**,只判"变化",不消费值 |
| AICPU 写 0xA0(C2) | 每次派发都写 | **完全没用**(无驻留 AICore 可派发) |
| set_cond 之后 AICore 干什么 | 活干完了,回去轮询自己的 SPR | **继续跑**:GM 轮询、quiet 自旋、下一个 chunk、下一个 kernel |
| 通知信号丢失时的备用做法 | 无关(它是按核读,不判变化) | 4096 次空转(约零点几秒)后强制 GM 全扫 |

**除最后一行外,每一行都是 pypto 从未运行过的模式。** 迁移保住的是"寄存器通道本身可达"这一点
(通道级兼容),但把通道嵌进去的**使用模式**全是新的。

### 4.3 订正:常驻 / 握手 / 精确寻址 / 主动派发是四件事

| 能力 | 依赖什么 | memfabric 能不能有 |
|---|---|---|
| AICore 启动握手(报自己的物理核号) | 只要 kernel 入口能执行到那几行;launch-exit kernel 每次 launch 报一次即可 | **能,只要几行** |
| 精确按核寻址(AICPU 写/读指定核的寄存器) | 依赖握手得到的映射表;**不依赖常驻** | **能** |
| AICPU 主动派发任务给 AICore(写 0xA0) | 依赖 AICore 常驻死循环读自己的 0xA0 | **不能,当前设计也不需要**(memfabric 是 AICore 主动提交、AICPU 消费) |
| AICore 回报完成(写自己 0x4C8) | 不需要知道核号;AICPU 扫全部核即可 | **能,迁移版就是这么做的** |

结论:**"没有常驻"只否掉第三行(主动派发),不否掉握手和精确寻址。** 初稿把第三、四行
混成一句,是推理错误。

那迁移版为什么没加握手?诚实说:**当前设计用不上,不是加不了。**
- 迁移版里 AICPU 从不往 AICore 写寄存器(不需要派发),"知道往哪个核写"没有用武之地;
- AICore 写的是自己的 COND,AICPU 扫 10 个核读,这个方向本来就不需要核号;
- 真正需要握手的是 reg_ping 那种"AICPU 回信给 AICore"的方向,那时才必须知道核号
  (探针里是靠对 10 个核广播写绕过去的)。

如果将来要做 AICPU→AICore 的反向通知或主动派发,握手可以照搬 pypto 那两段
(`HandshakeClient` + `TryHandShakeByGm`):AICore 侧约 5-8 行、AICPU 侧约 10 行。
但它解决不了本次挂死(见 6.3)——挂死发生在 r0↔r1 的应用层回执,不是核号错配。

---

## 5. 遇到的问题:挂死现场

### 5.1 现象(dev4/5 仪器化实测,日志 /tmp/ex08_stage_dev45.log)

- 基线(commByReg=0)同代码同环境:双 rank PASS ✅
- commByReg=1:r0 的 AICore kernel **早退**,阶段码 `0x41` = 第 0 个 chunk 的 `SdmaWaitReady`
  (等对端回执)轮询约 1 亿次(~66 秒)后超时;
- **r0 在超时前的所有环节全部成功**:8 个 128KB chunk 的 put_nbi 提交 ✅、quiet(完成戳全落地,
  证明 r0 的 AICPU 消费链活着、SDMA 通道活着)✅、8 次 PublishReady(put_at + quiet)✅;
- r1 的 kernel 76 秒内没有返回,阶段码没拿到(每次 r0 host 失败退出触发 mpirun 杀 r1);
- r1 的角色是:等 r0 的 ready 标志(0x51)→ 数据加 0x100 → 回传(0x61)→ 发布(0x71)。
  它卡在哪一步,现有数据**裁决不了**。

### 5.2 次生灾难:每次挂死都打挂一对卡(归因已订正,见 5.5)

模式已经多次重复(dev2/3、dev4/5、dev0/1、dev6/7):**commByReg=1 挂死 → 所在卡对
npu-smi Health=Critical(aclrtSetDevice 报 507033)**。
最初归因为"进程被强杀(abort/SIGKILL)连带打挂设备"。**这个归因已被 dmesg 订正,见 5.5:
dev6/7 那轮实验全程优雅退出( EXIT=0、无段错误、无强杀),卡对照样被打挂。
真正的机制是 orch kernel 卡在 MMIO 读上永不返回 → 设备 CPU 对主机的消息中断停止响应
→ host 侧驱动报心跳丢失。**

### 5.3 悬而未决的矛盾

理论上这个设计**不该挂死**:就算 COND 通知信号一个都丢,AICPU 空转 4096 次后必然强制 GM 全扫,
最坏多等零点几秒。r0 侧 quiet 全过还证明 r0 的 AICPU 一直在正常消费。但实测 r0 等 r1 的回执
等了 66 秒。**"通知信号丢失最多延迟零点几秒"的推理,和"66 秒等不到回执"的现场,二者必有一个的
前提是错的。**(2026-11-24 已裁决:错的是前者,见 5.5——kernel 在进 GM 全扫之前就被
一次不返回的 MMIO 读卡死,"空转 4096 次后强制全扫"永远没执行到。)

### 5.4 那段扫描只是 orch kernel 主循环里的一个分支,不是后台单独运行的保护进程

那段扫描的代码在 `feat/sdma-regsync` 分支 `src/hybm/ops/hybm_kernel/hybm_sdma_orch_kernel.cc:551-568`,
位于 orch kernel 主循环(`for`,:539)内部:

```cpp
for (uint64_t loops = 0; loops < maxLoops; loops++) {            // :539
    if (!resident && LoadHeartbeat(result) >= budgetTicks) break; // :540-543 时限到点
    if (loadCell(p->stopAddr) != 0) break;                        // :544-547 停止标志
    if (regBaseArr != nullptr && idleSpins > 0) {                 // :551 空闲才扫 COND
        ... 读 10 核 COND ...
        if (!woken) {
            if (idleSpins < kCondForceScan) { idleSpins++; usleep(...); continue; } // :561-565
            idleSpins = 0;                                        // :566 强制全扫(备用做法)
        }
    }
    ... 扫 24 个请求队列 ...
    if (ret != 0) { result->mainRet = ret; errored = true; break; } // :584-589
}
```

要让它执行,必须同时满足三个条件:
1. **kernel 进了主循环**——入口 `cp1LiveQuery` 失败会直接 `return 1`(:507-513),根本没进循环;
2. **循环没被提前 break**——时限到点(:540)、停止标志(:544)、或任意一封邮件消费失败(:584)
   都会 break 出循环,这段扫描再也不会执行;
3. **恰好处于空闲分支**(`idleSpins > 0`,:551)。

任何一条不满足,这段扫描一次都不会执行。**"代码里有这段扫描"和"故障时它真的被执行过"是两回事,
我们只证明了前者。** 挂死现场 r0 的 quiet 全落地,说明 r0 的 orch 活着;r1 的 orch 当时是活着在扫、
还是已经 break 退出、还是卡在别处,现有数据裁决不了——这正是第 7 节第 4 项要取的诊断。

---

## 6. 思考:为什么"不就读个寄存器吗"会出这么多问题

直接回答:**读写寄存器本身确实不难,探针也证明了它可行。难的不是通道,是通道上的调度语义。**
具体拆成四层:

### 6.1 我验证的是"通道可达",迁移的是一套"新的使用模式",两者不是同一个东西

探针验证的粒度:AICPU 精确写一个已知核,单次;AICore set_cond 在 kernel 尾部,单次。
memfabric 实际运行的粒度:10 核 MMIO 持续轮询判变化;set_cond 插在长流程中间,后面跟着
一亿次的 GM 轮询循环、跨 die 的 SDMA 提交、缓存维护。
**通道级验证通过 ≠ 使用模式正确**。这跟"单根网线测通了"不等于"整个机房网络协议栈没问题"
是一个道理。我当时的隐含假设("探针过了,机制就是对的")本身就是不成立的。

### 6.2 set_cond 的执行位置,是我单方面发明的

pypto 里 set_cond 之后 AICore 只做一件事:轮读自己的 SPR(0xA0),等下一个任务。它**从不**在
set_cond 之后继续跑 GM 流程。memfabric 里 set_cond 被插在提交流程尾部,后面还要跑
quiet 自旋(1e8 次 DCCI 循环)、下一轮提交、甚至下一个 kernel。
**set_cond 与后续 GM 访问序列之间是否有干扰(流水线、缓存、异常语义),pypto 的成功经验
没有任何覆盖,我们也没有单独验证过。** 挂死现场 r0"数据面全过、唯独 r0↔r1 的应用层回执
等不到"恰好发生 set_cond 大量执行之后——这不构成证据,但构成嫌疑。

### 6.3 "全核扫描判变化"同样是我发明的,而且有个自以为是的备用做法

pypto 从不轮询 COND 等变化——它按握手映射读特定核、消费具体值。memfabric 改成"10 核扫描、
lastCond 比较、4096 次空转后强制全扫",这个模式在 pypto 里找不到对应物。我当时论证"有这段扫描所以最坏退
化成 GM 轮询,逻辑上不该挂死"——但现场恰恰挂死了,说明**"这段扫描必然被执行到"的推理链里有一环
在实机上不成立**。(2026-11-24 裁决:被推翻的环节不是"扫描没被执行",而是"执行了就能走完"——
扫描确实进了,却卡死在读第 2 个核 COND 的那次 MMIO 读上,见 5.5。"空转 4096 次后强制全扫"
不是后台单独运行的保护进程,只是主循环里的几行判断;kernel 被卡死在它前面的任何一行,
它就永远执行不到。写在代码里不等于故障时真的执行过。)

### 5.5 裁决实验结果(dev6/7,2026-11-23,日志 /tmp/ex08_diag_reg_dev67.log)

本轮实验做了两处修复后进行:(a) r1 退出时 host 进程段错误的根因已修——
`hybm_vmm_based_segment.cpp:142` 的日志用 ostream 打印 `uint8_t*` 类型的
`globalVirtualAddress_`,ostream 会按 C 字符串处理、先对这个指针做 strlen,而这个指针存的
是设备侧地址,主机进程一读就段错误;改成转 `void*` 打印后,本轮全程无段错误、退出码 0。
(b) orch kernel 加了 markStage 阶段码,卡死时能从 result 区读出最后执行到哪一行。

**裁决结果(第 7 节四项未证实清单全部拿到数据,详见第 7 节):**

- r1 的 AICore kernel 阶段码 = **0x60**,即卡在 put_at 回传(r1→r0 方向的发布);
- r1 的 AICPU orch kernel 阶段码 = **0x3102**,即卡在
  `AicpuReadReg64(regBaseArr[2] + REG_SPR_COND)` 这一行——读第 0、1 个核的 COND
  都成功返回,读第 2 个核时不返回,此后 kernel 永久阻塞
  (诊断区:consumed=0, exitReason=RUNNING, 心跳计数持续增加 60 秒不变);
- r1 侧 ready 槽 c0..c7 全 =1,chunk0 前 4 字节 =0x8b000100(原值+0x100):
  **r0→r1 方向的数据面和发布全部送达**;
- r0 侧 ready 槽全 0、chunk0 无增量:**r1→r0 方向的回传从未发出**——与 r1 卡在
  put_at/回传一致;
- r0 的 orch kernel 完全正常:consumed=16,按时限正常退出。

**矛盾解答(5.3 节的悬案):**"AICPU 空转 4096 次后必然强制 GM 全扫,最坏多等零点几秒"
这个推理不成立的原因是:**orch kernel 在进入 GM 全扫之前,要先逐一读 10 个核的 COND,
而第 2 个核的这次 MMIO 读本身就不返回。** kernel 不是在"扫描后发现没变化",而是被
一次寄存器读卡死在扫描过程中,后面的 GM 全扫代码一次都没执行到。

### 5.6 挂死如何打挂设备:dmesg 证据(订正 5.2/6.4 的归因)

dmesg 里两组时间戳(主机运行秒数):

| 时间点 | 事件 | 对应实验 |
|---|---|---|
| 290697s | dev0、dev1 心跳丢失, `ts is down, devid(0)` | dev0/1 那轮 |
| 292500s | dev6、dev7 开始报 `Device irq not resp`(设备 CPU 不响应主机消息中断) | dev6/7 那轮 |
| 292557s | dev6、dev7 心跳丢失, `ts is down, devid(6)` | dev6/7 那轮 |

dev6/7 那轮是**全程优雅退出**的(退出码 0、两侧走完清理、无段错误、无强杀),但 dmesg
显示 dev6 和 dev7 的设备 CPU 在实验过程中就停止响应中断了。所以:

- **打挂设备的不是"强杀",是挂死本身**:orch kernel 卡死在 MMIO 读 → 设备 CPU 停止
  响应主机消息中断 → 驱动报心跳丢失 → 设备进入 Critical。6.4 节"强杀正在打挂设备"
  的归因需要订正为:"kernel 永久阻塞"与"设备 CPU 停止响应"同时发生;强杀只是让
  主机进程退出,并不影响设备侧已经卡死的事实;
- **每次 commByReg=1 挂死都同时打挂成对的两台设备**(dev6 和 dev7 同时报
  `Device irq not resp`),不是只打挂卡死的那一台;
- 截至 2026-11-24,本机 dev0–7 全部不可用(aclrtSetDevice 全部 507033,
  npu-smi 报 -8010),需要设备热复位(`device_hot_reset.sh`)或整机重启恢复。

### 6.4 复盘里最该检讨的:失败路径从来不是实验设计的一部分

三次打挂卡对,不是 commByReg 机制本身打挂的,而是**"挂死之后怎么退出"从来没被我设计过**:
- kernel 里所有轮询都是 1e8 次上限(~60 秒量级),host 层没有任何超时;
- 任何一 rank 失败都走 `exit(1)`,连带 mpirun 杀掉另一 rank;
- 于是每轮实验都注定以"abort/强杀"收场,而强杀正在打挂设备。

基线实验(优雅退出)从来没打过挂任何卡——这不是巧合。**实验设计从第一天起就该保证:
任何失败路径都必须走优雅退出(MPI_Finalize、正常析构),否则就别消耗这对设备。**
我在已经损失 dev2/3 之后,第二次实验仍然没修这一点,又损失了 dev4/5。这是流程上最不该
犯的错。

---

## 7. 事实清单(供下一步实验设计引用)

已证实的:
- C1/C2 寄存器通道在 310P3 可达(探针三轮 EXIT=0);
- memfabric 基线 commByReg=0 ex08 全通过(数据面/SDMA/AICPU 消费链健康);
- commByReg=1 挂死点在**应用层 ready 回执协议**,而非提交、quiet、发布提交;
- r0 的 AICPU 消费链和 SDMA 通道全程活着(r0 的 quiet 戳全落地);
- **dev6/7 裁决实验(2026-11-23)已裁决原第 7.1-7.4 节全部四项未证实项:**
  1. r1 的 AICore kernel 阶段码 =**0x60**(卡在 put_at 回传)→ 断链在 **r1→r0 回传**方向,
     r0→r1 方向(数据 + ready 发布)全通;
  2. r1 的 ready 槽 c0..c7 全 =1: r0 的 8 字节 publish 在 GM 里真实落地;
  3. r1 的 chunk0 =0x8b000100(原值+0x100): 128KB 数据真实到达 r1;
  4. r1 的 orch kernel 工作区诊断: **consumed=0、阶段码 0x3102、kernel 永久卡在
     `AicpuReadReg64(regBaseArr[2] + REG_SPR_COND)`(读第 2 个核的 COND, 偏移 0x4C8)——
     第 0、1 个核读成功, 第 2 个核这次 MMIO 读不返回**。r1 的消费链不是"死了",
     是被这次读卡住, 永远没执行到后面的 GM 全扫;
- **挂死本身打挂设备**(不是强杀): 优雅退出(EXIT=0)的 dev6/7 实验照样使两台设备
  心跳丢失、设备 CPU 停止响应主机消息中断(dmesg `Device irq not resp` +
  `Heartbeat lost`, 见 5.6)。每次 commByReg=1 挂死同时打挂**成对两台**设备;
- 310P 驱动(kernel_space svm_agent_addr_map.c)对 ADDR_MAP_TYPE_REG_AIC_CTRL=2 走
  整段连续 remap TS_AICORE_REG(固定 VA 偏移 512MB), host 侧配比 10 核 × 2MB stride
  与 pypto 参考实现(GetAicoreRegInfoForDAV2002)完全一致——host 侧映射查询配置
  **不是**挂死原因;
- HYBM 日志默认 WARN_LEVEL(mf_out_logger.h), BM_LOG_INFO 全被过滤——日志里看不到
  "commByReg: enabled"不代表功能没开。

未证实的(下一步的数据需求):
1. 为什么 r1 侧读第 2 个核的 COND 不返回而 r0 侧同样的 10 核扫描正常——设备间映射差异
   假设**已排除**(2026-11-24 主机重启设备全恢复后, hal_memctl_probe 对 dev0–7 逐一
   实测: 全部 ret=0、len=32MB(AIC CTRL 与 AIC PMU 都是), 偶数设备 VA=0xfee20000000、
   奇数设备 VA=0xff020000000(每卡两 die 各一个固定 VA 段), dev6 与 dev7 结构完全对称,
   10 核 × 2MB = 20MB 全部落在 32MB 映射窗口内; 排除"dev7 映射窗口偏小/基址异常");
   剩余待裁决: 读不返回的微观机制(需在 COND 读前后加单次读探针 + 设备侧日志配合);
2. 挂死的精确微观机制: MMIO 读不返回是设备 CPU 上的什么行为(总线挂起?核 2 的
   COND 寄存器所在页有问题?)——需要设备侧日志(device log/plog)配合;
3. 修复方向的裁决: "COND 读加退出上限/超时"能否避免打挂设备, 还是必须放弃
   全核 COND 扫描改回 GM 轮询 + 按握手映射读特定核(pypto 原模式)。

(按用户指示,本文档完成后暂停实验,不再运行任何会碰设备的程序,直到方案与用户对齐。设备恢复需先做热复位或整机重启。)

---

## 8. 结论

迁移不是"照抄一个读寄存器的代码",而是**在完全不同的执行模型上重新发明了一套基于寄存器的
调度协议**。订正一处因果:pypto 的成功依赖的"常驻 AICore"只影响"主动派发"这一项能力,
握手和精确寻址 memfabric 其实可以具备(见 4.3),只是当前设计没有用上;真正被替换掉的
是**使用模式**——"set_cond 后只等派发"变成了"set_cond 后继续跑长 GM 流程","按核读语义值"
变成了"全核扫判变化 + 4096 次空转后强制全扫"。我在保留"通道可达"的同时,替换掉了全部使用模式,
却只复用了通道级的验证。

**(2026-11-24 订正)本结论最初推测"挂死最可能是主循环提前退出,扫描没被执行到"。这个推测
已被 dev6/7 裁决实验推翻:扫描确实被执行了,而且恰恰是卡死在扫描内部——r1 的 orch kernel
在读第 2 个核的 COND 寄存器(`AicpuReadReg64(regBaseArr[2] + REG_SPR_COND)`)时这次
MMIO 读不返回,kernel 从此永久阻塞,后面的"4096 次空转后强制 GM 全扫"永远没执行到
(见 5.5)。"全核 MMIO 轮询"这一自发明的使用模式本身就是挂死的直接现场,
不是"没被执行的备用逻辑该执行而没执行"。**

下一步应该怎么做,等对齐后再定——原则先立四条:
1. 任何新实验,失败路径必须优雅退出,先证明"这轮实验无论结果如何都不会再打挂设备";
2. 实验目标从"让它跑起来"改为"逐层裁决第 7 节的未证实清单",一次运行必须拿全一个清单项;
3. 在单元/模拟层面(不碰卡)先复现调度逻辑(比如把 AICPU 轮询、AICore 提交时序写成
   host 侧单测),把调度逻辑的正确性先在纸面/单测上穷尽;
4. **任何会被 kernel 无限阻塞的路径都必须有退出上限**——orch kernel 的 COND 扫描读
   目前是裸的 volatile MMIO 读,一次读不返回就把 kernel 和设备一起挂死;
   在"读不返回"的微观机制查明并修复之前,不得在实机上再跑全核 COND 扫描。
