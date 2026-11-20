# commByReg 从 pypto 迁移到 memfabric:原理梳理、验证边界与问题复盘

> 2026-11-20。本文回答一个问题:**pypto 上 commByReg 跑得好好的,探针也穿刺通过了,
> 为什么迁移到 memfabric 就挂死?** 先把三样东西的原理讲清楚(pypto 原始实现、探针验证、
> memfabric 迁移),再对照挂死现场逐条分析差在哪,最后给出未裁决的假设清单。
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

ACL 把 kernel 调度到哪个物理核,**pypto 也不假设**。它的做法是握手(aicore_hal.cpp 的
`TryHandShakeByGm`):

1. AICore 启动后先写 GM 握手缓冲,报 "HELLO",同时**把自己的物理核号 get_coreid() 放在高 32 位**;
2. AICPU 读到 HELLO,取出核号,填进映射表:`GetPhyIdByBlockId(blockIdx) = phyId`;
3. 之后所有寄存器操作都查这张表:

```cpp
// aicore_hal.h —— 所有读写都经 GetPhyIdByBlockId() 寻址,绝不等差数列猜测
WriteReg32(coreIdx, offset, val):
    addr = regAddrs_[GetPhyIdByBlockId(coreIdx)] + offset;   // 按 coreIdx 映射到物理核
    *(volatile uint32_t*)addr = val;

SetReadyQueue(coreIdx, value):        // C2 派发:AICPU 写目标核的 0xA0
    readyRegQueues_[GetPhyIdByBlockId(coreIdx)] = value;     // = regAddrs_[phyId] + 0xA0

GetFinishedTask(coreIdx):             // C1 收完成:AICPU 读目标核的 0x4C8
    return *finishRegQueues_[GetPhyIdByBlockId(coreIdx)];    // = regAddrs_[phyId] + 0x4C8
```

`regAddrs_[]` 是 host 侧经 halMemCtl 查到的 10 核基址数组(base + i×0x200000,每核间隔 2MB)。

### 2.3 两个方向

| 方向 | AICPU | AICore | 语义 |
|---|---|---|---|
| 派发(C2) | 写目标核 `0xA0` = 任务号(映射表寻址) | 裸汇编 `MOV %0, DATA_MAIN_BASE` 轮询读自己核 | AICPU → AICore:下一件活 |
| 完成(C1) | volatile 读目标核 `0x4C8`(映射表寻址) | `set_cond(完成号)` 写自己核 | AICore → AICPU:活干完了 |

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

### 3.2 修掉的假阴性,恰好暴露了关键前提

旧探针曾判定"C2 不可达",真因是**核错配**:ACL 逐次 launch 的 kernel 每次落在不同的物理核
(实测组合 4/5、5/6、7/0、3/4,每轮都不同)。旧探针按 Send kernel 的核号让 AICPU 写 0xA0,
而读方 Recv kernel 在另一个核上读自己的 DATA_MAIN_BASE——**写读异核**,读到的永远是 0。

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

因为 memfabric 没有常驻 AICore、没有握手协议,**pypto 的"精确按核寻址"模式搬不过来**。
于是我做了一个设计降级:寄存器不再承载任何语义,只当**门铃**用:

```
memfabric commByReg=1 的实际设计:

AICore 侧(smem_shm_aicore_base_sdma.h, SdmaSubmitAt 尾部):
    写完邮件、推进 tail 之后:
    if (ctx.commByReg != 0) {
        SdmaSetCond((block << 32) | (tail + 1));   // 按门铃:写自己核的 0x4C8
    }

AICPU 侧(hybm_sdma_orch_kernel.cc 主循环):
    空闲时(idleSpins > 0)不扫 GM,只读 10 个核的 COND MMIO:
        for (c = 0..10): v = volatile read(regBase[c] + 0x4C8)
                          v != lastCond[c] → 有变化 = woken
    woken → 去 GM 扫 24 个请求队列;
    连续 4096 次没变化 → 强制 GM 全扫兜底(kCondForceScan)。
    (兜底的目的:就算门铃全丢,也退化成 commByReg=0 的纯 GM 轮询,不该挂死)
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
| 门铃丢失的兜底 | 无关(它是按核读,不判变化) | 4096 次空转(约零点几秒)后强制 GM 全扫 |

**除最后一行外,每一行都是 pypto 从未运行过的模式。** 迁移保住的是"寄存器通道本身可达"这一点
(通道级兼容),但把通道嵌进去的**使用模式**全是新的。

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

### 5.2 次生灾难:每次挂死都打挂一对卡

模式已经三次重复(dev2/3、dev4/5 两次):**commByReg=1 挂死 → 进程被强杀(abort/SIGKILL,
而非优雅退出)→ 所在卡对 npu-smi Health=Critical(aclrtSetDevice 报 507033)**。
这是流程错误放大了硬件代价,第 7 节单独反思。

### 5.3 悬而未决的矛盾

理论上这个设计**不该挂死**:就算 COND 门铃一个都丢,AICPU 空转 4096 次后必然强制 GM 全扫,
最坏多等零点几秒。r0 侧 quiet 全过还证明 r0 的 AICPU 一直在正常消费。但实测 r0 等 r1 的回执
等了 66 秒。**"门铃丢失最多延迟零点几秒"的推理,和"66 秒等不到回执"的现场,二者必有一个的
前提是错的。** 缺的数据(见第 8 节)还没拿全,这里不下结论。

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

### 6.3 "全核扫描判变化"同样是我发明的,而且有个自以为是的兜底

pypto 从不轮询 COND 等变化——它按握手映射读特定核、消费具体值。memfabric 改成"10 核扫描、
lastCond 比较、4096 次兜底",这个模式在 pypto 里找不到对应物。我当时论证"有兜底所以最坏退
化成 GM 轮询,逻辑上不该挂死"——但现场恰恰挂死了,说明**"兜底必然兜住"的推理链里有一环
在实机上不成立**(比如 AICPU 主循环根本没跑到兜底那一步、或 COND 的 MMIO 读行为与预期
不符)。纸面兜底不是实测兜底。

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
- commByReg=1 挂死点在**应用层 ready 回执协议**(r0 阶段码 0x41),而非提交、quiet、发布提交;
- r0 的 AICPU 消费链和 SDMA 通道全程活着(r0 的 quiet 戳全落地);
- 挂死 + 强杀 → 卡对 Critical,三次重复;

未证实的(下一步的数据需求,按裁决力排序):
1. r1 的阶段码:0x51(r0 的发布没到 r1)/ 0x61(r1 回传卡)/ 0x71 —— 一旦拿到立即二分出
   断链在"r0→r1 发布"还是"r1→r0 回传";
2. r1 的 ready 槽终值(D2H):r0 的 8 字节 publish 在 GM 里的落地实况——quiet 说它落地了,
   内存里到底有没有;
3. r1 的 chunk0 前 16 字节:128KB 数据是否真的到了 r1;
4. r1 的 AICPU orch kernel 工作区诊断(mainRet/consumed/日志环):r1 的消费链死没死。

(按用户指示,本文档完成后暂停实验,不再运行任何会碰设备的程序,直到方案与用户对齐。)

---

## 8. 结论

迁移不是"照抄一个读寄存器的代码",而是**在完全不同的执行模型上重新发明了一套基于寄存器的
调度协议**:pypto 的成功依赖三个 memfabric 不具备的前提(常驻 AICore、握手确定的物理核、
set_cond 后只等派发)。我在保留"通道可达"的同时,替换掉了全部三个前提,却只复用了通道级的
验证。挂死现场与"兜底不该挂死"的推演矛盾,说明还有一环实机行为(候选:set_cond 后续执行
干扰、全核 MMIO 轮询行为、AICPU 消费链在特定时序下的状态)没有被任何已有实验覆盖。

下一步应该怎么做,等对齐后再定——原则先立三条:
1. 任何新实验,失败路径必须优雅退出,先证明"这轮实验无论结果如何都不会再打挂设备";
2. 实验目标从"让它跑起来"改为"逐层裁决第 7 节的未证实清单",一次运行必须拿全一个清单项;
3. 在单元/模拟层面(不碰卡)先复现调度逻辑(比如把 AICPU 轮询、AICore 提交时序写成
   host 侧单测),把"全核扫描判变化 + 4096 兜底"的正确性先在纸面/单测上穷尽。
