# 310P3 特制驱动寄存器开放机制与 memfabric AICPU 方案优化机会

> 调查日期：2026-10-09 / 2026-10-10
> **第一部分**（机制）：证据取自 **pypto 仓库**（`/home/w00930648/pypto`，分支 `trunk_for_a1`，基线 `9f4d16921`）；`mydocs/...`、`runtime_logs/...` 为其 git 历史中已删除的路径（恢复方式见附录 A.3）
> **第二部分**（应用）：分析对象为本仓库（memfabric_hybrid）310P AICPU 编排方案——生产路径 `smem_shm_aicore_base_sdma.h`（AICore 三件套）+ `hybm_overlap_kernel.cc`（AICPU 编排 kernel），机制依据 `docs/310p/机制档案.md`
> **一句话**：pypto 用一台装了特制驱动 `25.3.rc1.b099` 的机器实证了"改驱动开放寄存器"的完整模式；本文第一部分讲清该机制，第二部分把它应用到 memfabric 的 AICPU 方案上
> **阅读提示**：正文用架构图/流程图/时序图讲机制，全部源代码集中在**附录 D**按需查阅

---

# 第一部分：特制驱动寄存器开放机制

## 1. 背景：这个特制驱动开放了什么

同事拿到的是一个**特制驱动版本 `25.3.rc1.b099`**（CANN 9.0.0 配套）。它与普通驱动的唯一关键差异：

| | 普通驱动 | 特制驱动 25.3.rc1.b099 |
|---|---|---|
| `halMemCtl` 的 **type=0（`CTRL_TYPE_ADDR_MAP`）寄存器映射操作** | 310P3 上实测全部 addr_type（0-3）返回 `DRV_ERROR_NOT_SUPPORT (65534)` | **放行**（基线代码一次 map 成功，实测见附录 A） |

> 精确表述：`halMemCtl` 是驱动内部 9 种操作的通用控制入口（真实签名与结构体见附录 D.1），"寄存器映射"只是其中 type=0 这一条操作；特制驱动开放的就是**这条操作在 310P3 上的放行**——使下表两类寄存器区域可被映射进用户态。addr_type（入参 `AddrMapInPara.addr_type`）选择映射哪类资源。

它开放的是两类设备寄存器的**用户态映射权限**：

| 寄存器类 | addr_type | 用途（一句话） | 普通驱动机器上的后果 |
|---|---|---|---|
| **AIC 同步寄存器** | 2 (`ADDR_MAP_TYPE_REG_AIC_CTRL`) | AICPU 调度器 ↔ AICore 的任务分发/完成同步 | 不可用 |
| **AIC PMU 寄存器** | 3 (`ADDR_MAP_TYPE_REG_AIC_PMU_CTRL`) | 采集 AICore 微架构性能计数（流水线利用率、Cache 命中等） | 不可用（无替代路径） |

所以：**两类寄存器的使用都以特制驱动为前提**——只有装特制驱动的机器才能走寄存器路、才能做 PMU 采集；本文只关注寄存器方案。

## 2. 全景：谁在哪儿、怎么碰寄存器

### 2.1 总架构图

```mermaid
flowchart TB
    subgraph HOST["Host 进程（如 PyPTO RuntimeAgent）"]
        HC["① 一次性调用 halMemCtl(type=0)<br/>建立寄存器映射"]
    end
    subgraph DRV["驱动"]
        HAL["libascend_hal.so<br/>dlopen + dlsym 动态加载"]
        KMOD["内核驱动：在进程页表插入映射<br/>虚拟地址 ≡ 寄存器物理区域（uncached）"]
    end
    subgraph DEV["310P3 设备侧"]
        subgraph CP1["AICPU（CP1 进程·调度器）"]
            SCHE["② volatile 指针读写<br/>每次解引用 = MMIO 总线事务"]
        end
        REG["AIC 同步寄存器区域 32MB<br/>每核 2MB × 10 核（开放 8 个）<br/>0xA0 任务信箱 / 0x4C8 完成状态"]
        subgraph AIC["AICore 0..7"]
            CORE["③ 专用指令访问自己的 SPR<br/>MOV DATA_MAIN_BASE / set_cond"]
        end
        GMEM["GM / HBM<br/>（DeviceArgs 等设备内存）"]
    end
    HC --> HAL --> KMOD
    KMOD -.->|"返回 out.ptr 区域基址"| HC
    HC -->|"地址表 CopyDataToDevice"| GMEM
    GMEM --> SCHE
    SCHE <-->|"MMIO 直达，零缓存一致性问题"| REG
    CORE <-->|"核内指令，不走内存总线"| REG
```

三个角色的分工：**Host 只负责"铺路"**（调一次映射拿到基址）；**AICPU 用内存方式**访问（volatile 解引用）；**AICore 用指令方式**访问自己的 SPR。GM 只承担地址表等普通设备内存的传递。

### 2.2 三层访问机制一览

| 层 | 执行者 | 访问方式 | 关键点 | 参考代码 |
|---|---|---|---|---|
| ① 拿地址 | Host 进程 | `halMemCtl(type=0, in={addr_type, devid}) → out={ptr, len}` | dlopen 动态加载 HAL 库；addr_type 选资源；返回**区域基址**（每核基址 = ptr + i×2MB 自算） | 附录 D.1/D.2 |
| ② AICPU 读写 | CP1 进程 | volatile 指针解引用 | 禁止编译器优化/缓存/重排，每次都是真实 MMIO 读写；区域 uncached，读到即硬件当前值 | 附录 D.3 |
| ③ AICore 读写 | 核自己 | CCE 专用指令/intrinsic | SPR 在核内，指令集直接访问（如同 ARM 的 MRS/MSR）；host 仿真有 stub 替身 | 附录 D.4 |

> `halMemCtl` 是驱动**内部接口**（非公开 SDK API、`ASCEND_HAL_WEAK` 弱导出、无兼容承诺），头文件几乎没有文档——布局知识（32MB/2MB stride/偏移）只能靠兄弟仓源码 + 头文件反推 + 实机探针获得，详见附录 D.1 的说明。

### 2.3 映射建立与自动降级（流程）

```mermaid
flowchart TD
    INIT["Host 初始化"] --> PRE["前置检查<br/>CANN runtime 已初始化<br/>驱动版本门禁 ≥ 24.1"]
    PRE --> CALL["halMemCtl(type=0 CTRL_TYPE_ADDR_MAP<br/>in={addr_type 2/3, devid})"]
    CALL --> OK{"映射成功？"}
    OK -->|"特制驱动 25.3.rc1.b099"| REGBASE["out.ptr = 区域基址<br/>每核基址 = ptr + i×2MB<br/>地址表传入设备侧"]
    OK -->|"普通驱动 NOT_SUPPORT"| NA["寄存器方案不可用<br/>需换装特制驱动"]
    REGBASE --> FAST["寄存器快路径"]
```

两个前置坑（pypto 实测教训，附录 C）：调 `halMemCtl` 前必须先初始化 CANN runtime（否则 ret=13）；此前不要做 map→unmap 探测（会破坏驱动内部状态导致真映射失败）。

### 2.4 映射原理（小白版）

1. CPU 眼里只有**地址**；程序访问任何地址都经**页表**翻译：虚拟地址 → 物理地址
2. AICore 的寄存器（SPR）占一段**物理地址空间**（在芯片内部，不在内存条上），这类区域叫 **MMIO 区域**
3. 平时用户进程碰不到它——不是指令不支持，而是**页表里没有这段映射，且普通驱动拒绝替你建**。这就是"原来不可操作的寄存器"的本质；"开放寄存器" = **允许为这段物理区域建立用户态页表映射**
4. 映射是**一次性的铺路**：之后每次读写只是一条普通 load/store（MMIO 总线事务直达硬件），无系统调用、无驱动参与——这是寄存器路快的根本原因

正确性靠两层保证，缺一不可：

| 层面 | 机制 | 作用 |
|---|---|---|
| 编译器层【实证】 | `volatile` | 禁止把轮询读优化成"读一次反复用"、禁止合并/删除/重排写 |
| 硬件层【推断】 | 页表 uncached 属性 | 不进 CPU cache，每次访问直达硬件——寄存器值随时被 AICore 改，缓存即旧值 |

## 3. 用途实证一：任务分发与完成同步

PyPTO 上板执行是 **MPMD 模式**（Multiple Program Multiple Data，区别于 CUDA 的 SPMD）：设备侧 AICPU 跑调度器程序、AICore 跑编译出的算子函数，不同核可同时跑**不同函数片段**。AICPU 把 kernel 拆成的 **leaf-task**（函数 DAG 的叶子节点，最小可调度执行单元）逐个派给核，靠这三个寄存器完成"派活—收工"：

| 偏移 | 名称 | 方向 | 用途 |
|---|---|---|---|
| `0xA0` | `REG_SPR_DATA_MAIN_BASE` | AICPU 写 / AICore 读 | **任务分发**：AICPU 写入 encodeTaskId+1，AICore 轮询取任务 |
| `0x4C8` | `REG_SPR_COND` | AICore 写 / AICPU 读 | **完成上报**：AICore 写完成状态，AICPU 轮询判断任务结束 |
| `0x18` | `REG_SPR_FAST_PATH_ENABLE`（OPEN=0xE/CLOSE=0xF） | AICPU 写 | **快速通路开关**：握手后打开、core 退出前必须关闭 |

### 3.1 寄存器传什么、GM 传什么（单号 vs 账本）

SPR 0xA0 只装得下一个 64 位值，而"任务"包含函数、参数、数据地址——**寄存器传的不是任务内容，是取任务的凭据**。pypto 的拆分（`aicore_manager.h:1479` 位域 + `aicore_entry.h` 消费链）：

```
64 位 encodeTaskId+1（位域压缩的定位字）：
|--16bit--|--16bit--|-1bit-|-1bit----|-1bit--|--3bit--|--10bit--|--16bit--|
| 上下文修改标志 | devtaskid | rsp | pingpong | dcci  | 并行索引 | func id | opindex |
└- 高32位：任务上下文定位 -┘      └--- 低32位：leaf task 序号（执行哪个函数）---┘

AICPU（先记账后按铃）：
  ① SetParallelDevTask：ptrElements[i]=任务描述符地址 → 写 GM 账本（KernelArgs.parallelDevTask）
  ② SetParallelDevTaskSize：发布 front/rear+版本
  ③ SetReadyQueue：SPR 0xA0 = encodeTaskId+1 → 敲门铃
AICore（收单号查账本）：
  ① GetNextLeafTask：MOV DATA_MAIN_BASE → 低 32 位 = leaf task 序号
  ② GetRegHighValue：高 32 位 = devtaskid+修改标志（决定是否 dcci 刷新账本）
  ③ GetCoreFunctionData：读 GM 账本 ptrElements[idx] → 完整任务描述符（函数+参数）
  ④ CallSubFuncTask：执行
```

顺序保证：先写 GM 账本、后敲寄存器门铃——门铃到 = 账本必就绪（与 memfabric SQE 对"标志到即数据到"同构）。派发槽单槽 + 双缓冲派发（running+pending）用 pingpong 位/并行索引区分，一次 64 位读同时捕获任务 ID 与上下文元数据（aicore_entry.h:214 注释：分开读会读到覆写后的值）。**"GM 账本 + 寄存器单号"模式已在 pypto 生产验证——memfabric 场景一的方案与此同构。**

### 3.2 一次完整任务往返（时序图）

```mermaid
sequenceDiagram
    autonumber
    participant S as AICPU 调度器
    participant MB as SPR 0xA0 任务信箱
    participant K as AICore 5号核
    participant CD as SPR 0x4C8 完成状态
    Note over S,K: 握手阶段（经 GM shakeBuffer 协商）
    K->>S: HELLO + phyId（shakeBuffer[0] 高 32 位）
    S->>MB: fast-path 0x18 = OPEN，SetReadyQueue 清零
    Note over S,CD: 任务循环（每个 leaf-task 一轮）
    S->>MB: *readyRegQueues_[5] = taskId+1（volatile 写 = MMIO）
    MB-->>K: 硬件路由，核内 DATA_MAIN_BASE 更新
    K->>K: 轮询 MOV x0, DATA_MAIN_BASE 发现新任务
    K->>K: 执行 leaf-task
    K->>CD: set_cond(status)（指令写 COND）
    CD-->>S: *finishRegQueues_[5] 轮询到新值（volatile 读）
    S->>S: 派发下一个任务
```

### 3.3 关键 API 索引

| 侧 | API | 位置（pypto） | 功能 |
|---|---|---|---|
| AICPU | `MapRegistersForAllCores` | aicore_hal.h:419 | 初始化 ready/finish 队列指针 |
| AICPU | `SetReadyQueue`（单核/区间） | :233/:248 | 派发 leaf-task |
| AICPU | `GetFinishQueue` / `GetFinishedTask` | :301/:352 | 轮询完成 |
| AICPU | `WriteReg32` / `ReadReg32` | :161/:135 | 通用 MMIO 读写 |
| AICPU | `TryHandShakeByGm` / `CloseFastPathReg` | :551/:156 | 握手 / 退核关 0x18 |
| AICore | `GetTaskReg` / `GetNextLeafTask` | aicore_entry.h:178/:207 | 取任务（轮询） |
| AICore | `SendCond` | :190 | 报完成 |

**影响面**：测试代码不直接调这些 API；凡 `run_mode=npu` 上板执行 kernel（特制驱动机器），调度循环自动走这条通道——所有上板用例隐式依赖。

## 4. 用途实证二：PMU 性能计数采集

按 core 采集 AICore 微架构事件（流水线利用率、Cache 命中、L2 带宽等），用于性能分析。

### 4.1 PMU 寄存器布局（addr_type=3 映射）

| 偏移 | 名称 | 用途 |
|---|---|---|
| 0x200 | PMU_CTRL_0 | 全局使能 |
| 0x210~0x248 | PMU_CNT0~7 | **8 个事件计数器**（310P3 只有 8 个硬件计数器） |
| 0x250/0x254 | PMU_CNT_TOTAL0/1 | **总周期数（64 位拼合）**——可当周期计数器用 |
| 0x260~0x298 | PMU_CNT0~7_IDX | 每计数器的事件选择 |
| 0x2A0~0x2AC | START/STOP_CNT_CYC | 计数起止周期控制 |

事件码定义在 `aicore_prof.h:205-235`（VEC/SU/CUBE_BUSY_CYCLE、ICACHE/DCACHE、L2 命中等）。

### 4.2 采集流程

```mermaid
flowchart LR
    A["编译期<br/>PMU_COLLECT=1<br/>PERF_PMU_TEST_SWITCH=1"] --> B["运行期<br/>PROF_PMU_EVENT_TYPE<br/>选事件组 1-8"]
    B --> C["ProfStartPmu（每核编程）<br/>清零 → 写事件码 → 使能 ctrl"]
    C --> D["每任务完成 ProfGetPmu<br/>读 PMU_CNT0~7 + TOTAL"]
    D --> E["经 msprof 上报落盘"]
    E --> F["tilefwk_pmu_to_csv.py<br/>--arch dav_2002 解析成 CSV"]
```

操作命令：

```bash
# ① 重编译（改宏后必须）
# ② export PROF_PMU_EVENT_TYPE=2        # 默认 PIPE_UTILIZATION
# ③ 采集（310P 只支持 l2 级，建议 CANN ≥ 9.1.0）
msprof --task-time=l2 --output=<路径> python xxx.py
# ④ 解析
python tools/profiling/tilefwk_pmu_to_csv.py -p PROF_xxx/device_x/data -pe=2 --arch dav_2002
```

**硬性依赖**：PMU 映射**无 fallback**——普通驱动上寄存器全 0、采集无数据（仅一条告警）。这是特制驱动不可替代的场景。

## 5. 实操速查

### 5.1 判断当前机器走哪条路（看运行日志）

| 日志 | 含义 |
|---|---|
| `Map AICore reg addr (type 2) 0x... for DAV_2002` | 寄存器映射成功，MMIO 生效（特制驱动） |
| `DAV_2002 halMemCtl PMU failed, PMU regs filled with zeros` | PMU 映射未放行 |

查本机驱动版本：`cat /usr/local/Ascend/driver/version.info`。上板硬门禁：驱动 ≥ 24.1（低于则 kernel launch 直接 abort；解析只看前两段数字，`25.3.rc1.b099` 按 25.3 比较）。

### 5.2 相关测试用例（pypto 仓库）

| 用例 | 位置 | 说明 |
|---|---|---|
| 上板 ST/models（隐式依赖） | `python/tests/st/**`、`models/**` | 凡 npu 模式执行 kernel 都走调度通道；"310P 上不被 soc 过滤"约 347 ST + 115 models 个测试函数是**共享通用用例**（soc 标记只区分 950 vs 其他），非 310P 专属 |
| 310P 专属 | `models/qwen3_moe/tests/`（45 个测试函数）、`models/yolov26/`、C++ STest `TestResNetStage` | 明确为 310P3 写/验证的用例 |
| 核数限制 | `python/tests/st/test_control_cores.py`（4 用例） | `set_stream_limit/set_device_limit` 限制核数后跑 matmul+add，解析 `tilefwk_L1_prof_data.json` 统计实际参与核数 |
| PMU | **无专门 pytest**，走 §4.2 手工流程 | — |
| UT（不依赖驱动） | `test_device_launcher_driver_gate.cpp`、`test_adapter_api.cpp`（StubHalMemCtl）、`test_device_runner.cpp` | 门禁解析/HAL adapter/初始化 |

### 5.3 验证命令

```bash
bash mybuild.sh --compile --no-clean      # 1. 构建
bash mybuild.sh --run -d 3 --no-board     # 2. 仿真
bash mybuild.sh --run -d 3                # 3. 上板（-d device id）
pytest python/tests/st/test_control_cores.py -v
msprof --task-time=l2 --output=out python xxx.py   # PMU
```

---

# 第二部分：memfabric AICPU 方案的寄存器优化机会

## 6. 判定方法论（先读）

一条通信通道能否寄存器化，**只看写者是谁**：

| 写者 | 寄存器化 | 原因 |
|---|---|---|
| 本地 AICPU | ✅ | 可直写目标 AICore 的 SPR（MMIO，第一部分 §3 ready 队列配方） |
| 本地 AICore | ✅ | 可 `set_cond()` 写自己 SPR，对端经映射读（第一部分 §3 finish 队列配方） |
| **SDMA 引擎 / TSFW** | ❌ | 只会写 GM（SQE 的 dst 是内存地址），够不到核内 SPR |
| 数据面（大块搬运） | ❌ | 寄存器是通知面机制，不承载带宽 |

由此得到核心结论：**memfabric 方案里最热的循环（`wait` 的到达戳 dcci 轮询）写者是 SDMA，寄存器帮不上**；能优化的集中在 AICore→AICPU 请求通知、AICPU→AICore 背压/ready、doorbell 提交、PMU 计时四处。

## 7. 现状全景

### 7.1 生产路径架构图

```mermaid
flowchart TB
    subgraph LOCAL["本端 310P3"]
        KC["AICore kernel<br/>signal / wait / quiet"]
        RING["协同工作区（GM 双环）<br/>REQ 环 / ARR 环 / QUIET 戳"]
        KA["AICPU 编排 kernel"]
        SQE["SQ 环（GM）"]
        DB["SQ doorbell"]
    end
    TSFW["TSFW 固件"]
    SDMA["SDMA 引擎"]
    subgraph PEER["对端设备"]
        PRING["对端 GM（数据 + 到达戳）"]
        PK["对端 AICore"]
    end
    KC -->|"C1 请求邮件 32B + clean"| RING
    RING -->|"裸读轮询（天然一致）"| KA
    KA -.->|"C2 背压时 dcci 轮询 REQ_HEAD"| KC
    KA -->|"C3 SQE 对 64B + cvac 刷行"| SQE
    SQE --> DB
    DB -->|"MMIO 直写 或 ioctl（待验证）"| TSFW
    TSFW --> SDMA
    SDMA -->|"C4 搬数据 + 写 8B 到达戳（只会写 GM）"| PRING
    PRING -->|"dcci 轮询到达戳"| PK
```

### 7.2 一次搬运的完整时序（含可优化点标注）

```mermaid
sequenceDiagram
    autonumber
    participant K as AICore
    participant R as GM 请求环
    participant A as AICPU kernel
    participant T as TSFW/SDMA
    participant P as 对端 GM
    participant W as 对端 AICore
    K->>R: 写 32B 请求邮件 + clean
    Note right of K: 场景一：游标可 set_cond 化省 1 次 clean；<br/>邮件数据 clean 或许可直接省（P0 实测）
    R-->>A: 裸读轮询发现新邮件
    A->>T: SQE 对（数据+标志）+ cvac + doorbell
    Note right of A: 场景三（已验证）：CP 流 SQ 查不到 sqRegBase<br/>（IsSqDbMap=0），实走 halSqCqConfig；<br/>MMIO 化需驱动/固件补门铃两步注册
    T->>P: SDMA 搬运数据 + 写 8B 到达戳
    P-->>W: dcci 轮询到达戳，读到达邮件
    Note left of W: 场景四：SDMA 只写 GM，寄存器物理不可达，<br/>保留 dcci（已是最小正确实现）
```

### 7.3 通道清单

| # | 通道 | 写者→读者 | 现状实现 | 每次成本 |
|---|---|---|---|---|
| C1 | 请求邮件（signal） | AICore→AICPU | 32B 槽 4×volatile 写 + clean；`REQ_TAIL` 写 + clean（`smem_shm_aicore_base_sdma.h:127-153`） | **2 次 clean**/封 |
| C2 | 背压轮询（满环时） | AICPU→AICore | AICore dcci 轮询 `REQ_HEAD`；demo 形态另有 ready flag（cvac + `dsb ish`，机制档案 §5） | 每迭代 dcci+GM 读 |
| C3 | SQE 提交 + doorbell | AICPU→TSFW | 64B SQE 直写 + cvac 逐行刷 + barrier；doorbell 双路径（`hybm_overlap_kernel.cc:319-332`） | MMIO=ns 级 / **ioctl=μs 级** |
| C4 | 到达/完成戳（wait/quiet） | **SDMA**→AICore | flag-SQE 写 8B 戳；AICore dcci + volatile 读每迭代 | 每迭代 dcci+GM 读 |
| C5 | AICore 计时 | — | dav-202 无 cycle 计数器（`GetSystemCycle` 返回 0 且拖慢 8 倍），等待段用 dcci 迭代数近似 | 精度损失 |
| C6 | 大块数据可见性 | AICore→SDMA | 写后 `ENTIRE_DATA_CACHE` clean（SDMA 只认 GM 一致性点） | **全 cache 清**/块 |

## 8. 六大场景逐一分析

### 8.1 场景一：请求邮件的生产通知（C1，AICore→AICPU）⚠️ 部分可行

**"部分可行"的含义**：这条通知由两部分组成——32B 邮件数据 `{src,dst,len,imm}` + 8B 游标 `REQ_TAIL`（"有新邮件"信号）。SPR 单槽只有 64 位：32B 数据物理上进不去（`src/dst` 各占满 64 位；连续 set_cond 单槽互相覆盖不是队列），**数据只能留 GM**（好在定论 #1 AICore→AICPU 天然一致，AICPU 裸读本就便宜，留在 GM 损失不大）；**只有 8B 游标能寄存器化**。收益也随之为部分：每次 signal 的 2 次 clean 只能省 1 次（游标那次）。

- **现状**：每投递一封请求邮件 = ① 写 32B 槽（4×volatile 64 位）+ ② clean 邮件行 + ③ 写游标 `REQ_TAIL` + ④ clean 游标行
- **方案**：①②留 GM 不动；③④改 `set_cond(tail+1)`，AICPU 轮询映射的 SPR 0x4C8（第一部分 §3 finish 队列配方）
- **具体设计（pypto 同构，见 §3.1"单号 vs 账本"）**：握手阶段抄 `TryHandShakeByGm`（AICore `set_cond(HELLO)` + GM 槽写 phyId，AICPU 建 blockIdx→SPR 地址映射）；生产时 AICore 先写 GM 邮件槽（账本）再 `set_cond(tail+1)`（单号）——**单调序号 + AICPU 本地消费位置**天然适配 SPR 单槽（连写覆盖无碍，AICPU 见 tail=N 即去 GM 账本批量取到第 N 封）。当前生产形态 AICore 单 block（`<<<1,0,aiCoreStream>>>`）单生产者↔单消费者正好适配；未来多 block 需 per-core 信箱轮询或代表核 doorbell（zbal core0 模式）
- **收益**：每封邮件省 1 次 clean + 1 次游标 GM 写；AICPU 轮询从 GM 读变寄存器读（边际收益小）
- **风险**：`set_cond` 对 AICPU 的可见时序需实测（pypto 握手场景实证过该通路，memfabric 高频连续通知场景需确认单槽协议）；`tail+1` 递增天然满足防覆盖序号
- **零成本子优化（先做，可能白赚另一半）**：② 的邮件数据 clean 可能本就多余——邮件读者只有 AICPU（不经 SDMA），按定论 #1 天然一致，实测去掉后 mode13 八步闭环 mismatch=0 是否保持
- **验证**：mode13 回归 + mode14 timeline 打点对比

### 8.2 场景二：背压与 ready 通知（C2，AICPU→AICore）✅ 配方现成

- **现状**：生产路径满环时 AICore dcci 轮询 `REQ_HEAD`；demo 形态 role1 AICPU 写 ready flag（cvac + `dsb ish`），AICore dcci + volatile 轮询
- **方案**：**pypto ready 队列配方原样照抄**——AICPU 直写目标 AICore SPR 0xA0（`*(volatile uint64_t*)(regAddrs_[core]+0xA0) = head+1`），AICore 用 `MOV x0, DATA_MAIN_BASE` 读自己的信箱；握手阶段与场景一共用（AICPU 已建 phyId→SPR 地址映射，见 §3.1）
- **收益**：写侧 2 条 cache 指令+屏障 → 1 次 MMIO 写；读侧 AICore 轮询从"dcci + GM 读"（占 DDR 带宽、污染 cache）→ 纯寄存器读
- **风险**：host 需拿到每核 SPR 地址表并经 wire 契约传入 AICPU kernel；多 AICore 需 blockId→phyId 映射（注意奇数卡 phyId 8/9 边界）
- **定位**：demo 形态收益明显；生产路径仅背压时触发，优先级中等

### 8.3 场景三：SQE doorbell 提交路径（C3，AICPU→TSFW）❌ 已验证：查不到 sqRegBase，实走 ioctl

- **验证结论（2026-10-10，本机 310P3 / 驱动 26.1.0）**：CP 流 SQ 的 `sqRegBase` **查不到有效值**——doorbell 实走 `halSqCqConfig(SQ_TAIL)` HAL 路径（xcclpp v43 全链 PASS、memfabric mode13/14 验收全靠它）
- **证据链**（xcclpp aicpu-310p3 实机实验 + tsch 固件源码，无需重跑）：
  1. 设备日志 `[TsDrvNormalSqInfoInit] sqid=18 ... IsSqDbMap=0 IsHwSqMap=1`——CP 流 remote SQ 的 doorbell 寄存器页映射未建立（队列映射有，doorbell 映射无）
  2. zbal 式"直写 sqRegBase+0x8"提交路径实测失败（xcclpp 提交路径矩阵）
  3. tsch 源码：host 普通流门铃需**两步注册**——① 内核 `trs_sq_remap` 把 SQ 寄存器 4 页 remap 进进程 ② CREATE_TASK_CMD_SQCQ 邮箱让固件注册 SQ + 解除门铃中断屏蔽；**CP 流 remote SQ 的闭源注册 handler 两步都跳过了**（用户态无解，hcomm 官方 API `halResourceIdRestore` 本机无符号）
  4. 对照：host 普通流 `IsSqDbMap=1`（doorbell 映射存在，host 编排生产可用）——缺失是 CP 流 remote SQ 特有
- **优化路径修正**：不是"映射一个寄存器"就够——需驱动/固件侧补齐 remote SQ 的门铃两步注册（remap + 固件邮箱），比 pypto 式放行 halMemCtl 重
- **查询链路代码级定位（为什么查不到——三道墙，2026-10-10）**：`halSqCqQuery(prop=5)` 本身不用换，是数据来源断了——
  ```
  CP1: halSqCqQuery({SQ_REG_BASE})
   → 用户态 HAL trs_sq_cq_query (ascend_hal/trs/core/trs_sqcq.c:1466)：prop=5 不在
     UIO 白名单（仅 HEAD/TAIL/CQE/BASE/DEPTH/SQE_SIZE/MEM_ATTR）→ ioctl 进设备内核
   → 内核 trs_sqcq_query (sdk_driver/.../trs_hw_sqcq.c:1642)：
     墙① trs_proc_has_res：CP1 进程不拥有该 SQ（host 分配；设备内核 trs 资源池
         TRS_HW_SQ total=0，SQ 只在 CP1 用户态表）→ -EINVAL
     墙② trs_get_sq_reg_vaddr (trs_hw_sqcq.c:1591)：reg_mem.uva=0（remote 未 remap）
         → -EOPNOTSUPP
     墙③ 语义墙：reg_mem.uva 是分配进程地址空间的 VA，对 CP1 进程页表无意义
  ```
- **改哪里（三个位置）**：
  | 位置 | 代码 | 改法 | 仓库 |
  |---|---|---|---|
  | 1. 根因 | CP1 remote SQ 注册 handler（`TsDrvNormalSqInfoInit`/`trsRemoteAllocResId`，xcclpp 定位 `trs_slave_event.c`/`tsdrv_id.c`，**不在开放仓**） | 注册时把 db 页 mmap 进 CP1（队列页已映 IsHwSqMap=1，同机制多映一页）填 `sq_ctrl.db`，`IsSqDbMap=1`——三道墙一次全解 | 闭源 |
  | 2. 查询侧 | `trs_sqcq.c:1482` UIO 白名单 | 加 `SQ_REG_BASE`（前提：位置 1 修好表里才有值） | 开放 |
  | 3. halMemCtl 路线 | `svm/v3/api/master/svm_mem_ctl.c:151` handler 表分发 | 新增 `ADDR_MAP_SQ_DB` handler；硬依赖：按 sqId 找 reg 物理页须内核 trs 管理该 SQ（墙①），仍绕回位置 1 | 开放但依赖闭源 |
- **用户态"漏步骤"假设定论（2026-10-12 CP1 实机实验，已关闭）**：hcomm 查询前确有 `StreamRestore` 前置步骤（`halResourceIdRestore`），xcclpp v43 复现时跳过。**CP1 实机验证**（xcclpp 部署链加载 `XcclppRestoreProbeRun` kernel，dev6）：
  1. **基线 prop 级差异同机确认**：`BASE`(0xdfffd0190000)/`DEPTH`(1024)/`HEAD`/`TAIL` 可查，**`REG_BASE`=65534 NOT_SUPPORT**（门铃断点，一手证实）
  2. **`halResourceIdRestore` 弱符号在 CP1 进程内未解析**（SYM_NULL）——hcomm StreamRestore 路线不可复现
  3. **设备侧运行时库普查**（KFC 退出码探测）：设备上唯一的 `libascend_hal.so` 在 `/usr/lib64/`，**无 restore 符号**；host 侧 CANN devlib 副本有符号但属开发备份，不被设备进程加载（修正此前"9.0.0/9.1.0 有该符号"的判断——仅对 devlib 成立）
  - **最终结论：不是漏步骤，也非 host 侧操作可补——设备侧运行时库不导出该 API，修复必须走固件/驱动更新（场景三位置 1）**。测试载体：xcclpp `aicpu-310p3/restore_probe.cc` + kernel `XcclppRestoreProbeRun`
- **附带发现**：开放仓 `halMemCtl`（svm_mem_ctl.c:155）对未注册 type 返回 NOT_SUPPORT——普通驱动 ADDR_MAP 的 NOT_SUPPORT 很可能就是"handler 未注册"，特制驱动 25.3.rc1.b099 = 注册了 ADDR_MAP handler 的构建（解释 pypto"放行"机制）
  | 路径 | 改动 | 重量 |
  |---|---|---|
  | a. halMemCtl 新增 `ADDR_MAP_TYPE_REG_SQ_DB`（按 sqId 查门铃物理页 remap；内核 `trs_sq_remap` 本有此能力，host 普通流 `IsSqDbMap=1` 为证） | 驱动加代码，调用方式与 pypto 同款 | 中 |
  | b. 补 CP 流 remote SQ 门铃两步注册（`trs_sq_remap` + CREATE 邮箱解除中断屏蔽；hcomm `halResourceIdRestore` 同款，本机无符号） | 闭源 remote handler/固件 | 重 |
  | c. 先在特制驱动机器测 CP 流 `IsSqDbMap` 是否=1 | 零改动，若=1 则直写直接可用 | 零（最优先） |
- **有利条件**：kernel 侧 MMIO 直写分支与 prop=5 查询代码已就位（`submitSqe`/`cp1QueryProp`）；v43 勘正确认门铃硬件链路一直通（假成功根因=SQE type 字段），拿到有效 VA 后直写应能响；门铃物理位置明确（tail 寄存器 4K 槽+0x8 即硬件 `SQ_DB`，tsch 源码证实）
- **遗留待验证**：zbal 式直写在 type 修正后未重测（矩阵数据是 type 错误时期测的）；特制驱动机器 `IsSqDbMap`
- **生产参照（2026-10-10 核查 shmem 仓 master@`0ab6ccc`，VERSION 1.7.0）**：CANN SHMEM **v1.3.0（2026-04-08 tag）已落地"AIV 直驱 STARS 驱动 SDMA"**——SDMA 特性 commit 2026-02-05、CMO 特性 2026-03-03，均早于 v1.3.0。三步流程代码属实：① Host 建 **CP 流**（`aclrtCreateStreamWithConfig(ACL_STREAM_DEVICE_USE_ONLY)` + `RtStreamGetSqid`，通道数上限 `ACLSHMEM_MAX_AIV_PER_NPU=72`，每 AIV 独立通道，`device_sdma_transport_manager.cpp:92-130`）② `AclnnShmemSdmaStarsQuery` 内置算子把 `stars_channel_info_t{sq_base, sq_reg_base, sq_depth, sq_id...}` 写入 GM workspace（`:251-259`，dlopen opapi 二段式调用）③ AIV 组装 64B SQE + dcci 刷 + **直写 doorbell `sq_reg_base+0x8`**（`shmem_device_sdma.hpp:331`；950/dav-3510 用 STARS v2 布局 offset 0x0）。CMO 预取同链路：`aclshmemx_cmo_nbi` + `stars_sdma_cmo_sqe_t`（当前仅支持 PREFETCH 类型），样例 `examples/cmo/` 三场景测试（不预取/Host 预取/Device 预取）与文章一致
- **交叉印证（2026-10-12 一手实测，双层证据）**：shmem 这条链路与本文场景三**完全同构**。A2/A3 上 CP 流 remote 注册完整故"仅依赖现有接口"成立；**310P3 断点一手实测**（测试载体 `/tmp/opencode/stars_test/`，CANN 自带 `pto.bak/comm/async/sdma/sdma_workspace_manager.hpp` 原样复刻 shmem 三步流程，仅通道数 48→4）：
  - **Host API 层**：CP 流创建成功、`aclnnShmemSdmaStarsQuery` 调用返回 0、流同步失败（507018 aicpu exception）
  - **设备日志层（决定性）**：`[shmem_sdma_stars_query.cc:97] halSqCqQuery base addr failed. ret = 65534 sqid:340`——算子内部 HAL 查询返回 **NOT_SUPPORT**，经 AICPU 标准错误通道（kernel failed → 流异常）上报，**非崩溃**
  - 稳健性：设备 0/6 × CANN 9.0.0/9.1.0 四组一致；算子正确解析输入后才走到查询步（sqid=340），排除测试侧传参问题
  - **与 xcclpp 归档记录完全一致**（他们记录的 65534 即此内部返回码；"aicpu exception"是 AICPU 算子报错的标准外部表现）
  - **prop 级细节（解释 xcclpp 直查成功与算子失败并存）**：算子要查 `SQ_REG_BASE`(prop=5)——不在用户态 UIO 白名单（`trs_sqcq.c:1482`），走 ioctl 进内核 trs 撞三道墙 → 65534；而 xcclpp 直查的 `SQ_BASE`(prop=6) 在白名单内、直接读 CP1 用户态 trs 表（remote 注册建了表项）故成功——**断的恰好是门铃地址查询（场景三本尊）**
  - **SQ_BASE vs SQ_REG_BASE（信箱 vs 门铃）**：前者是 SQE 环内存地址（数据面，"往哪写 SQE"），后者是寄存器页基地址（控制面，head/tail/**门铃 +0x8**）；内核 `trs_sq_remap` 本该映射 4 页（队列+head+tail+门铃），310P3 remote 注册只做了队列页（`IsHwSqMap=1` / `IsSqDbMap=0`）——**只有 SQ_BASE 时提交者必须能调 ioctl（→AICPU 编排，memfabric 现行）；有 SQ_REG_BASE 才能 kernel 内 MMIO 直敲门铃（→AICore 直驱，shmem 前提）——即"AICPU 中转 vs AICore 直驱"的分界线**
  - 另测 **host 侧 `aclrtCmoAsync` 返回 207000**（API 存在于 9.1.0 但 310P3 不可用）
  - **结论：310P3 上 StarsQuery 链路不通，断点 = HAL 查询 NOT_SUPPORT（与场景三"三道墙"中的①②吻合）**。310P3 驱动修复 remote 注册后，shmem 这套三步流程可直接作为 memfabric 的实现参照

### 8.4 场景四：到达/完成检测（C4，SDMA→AICore）❌ 不可寄存器化

- **现状**：`wait_at` 热循环每迭代 dcci + volatile 读 ARR_STAMP；`quiet_at` 同构轮询 QUIET 戳；戳由 SDMA flag-SQE 写入
- **为什么不行**：写者是 SDMA 引擎，SQE 的 dst 只能是内存地址，物理上够不到核内 SPR。这是方案里**占比最大的等待成本**（mode13 单 chunk 预算 24000 迭代），但寄存器无解
- **保留结论**：dcci 单行轮询已是 E0c 定论的最小正确实现；固件级特性（TSFW 完成写 doorbell 寄存器）远超改驱动范围，不投入。**此场景的优化方向是降低等待本身**（通信掩盖、流水化——mode14 8.02x 已在做）

### 8.5 场景五：AICore 精确计时（C5，PMU 周期计数器）✅ 独特收益

- **现状**：dav-202 无 cycle 计数器，timeline 等待段只能用 dcci 迭代数近似——mode14 加速比、通信掩盖量化都建立在这个近似上
- **方案**：PMU 映射里的 **64 位周期计数器 `PMU_CNT_TOTAL0/1`（0x250/0x254）**，AICPU 按核读取（第一部分 §4 配方整链可抄：事件编程 → 计数器读取 → AICore 侧 `set_ctrl` 采样开关）
- **收益**：等待段/执行段从"迭代数×假设周期"变精确周期数——不改运行时性能，但**所有性能结论的置信度升级**（8.02x 加速比构成、掩盖窗口测量、后续调优标尺）
- **前置**：特制驱动 PMU 映射（无 fallback）；计数值读取时机需与 timeline 打点对齐

### 8.6 场景六：大块数据 cache clean（C6，AICore→SDMA）❌ 语义必需，可调研替代

- **现状**：AICore 大块 GM 写后必须 `ENTIRE_DATA_CACHE` clean（SDMA 从 GM 一致性点读，定论 #3）——全 cache 清，波及同 kernel 其他数据的 cache 驻留
- **为什么不能寄存器化**：数据面跨引擎可见性要求，不是通知面问题
- **可调研（非寄存器）**：CCE 是否有 write-through / non-temporal store（写时直通 GM、不驻留 cache）——若有则"事后全清"变"写时直通"，双重收益。独立调研项，与驱动改动无关

## 9. 汇总：优先级与行动清单

| 优先级 | 行动 | 类型 | 前置 |
|---|---|---|---|
| ~~P0~~ **已完成** | doorbell 现状确认 → **结论：CP 流 SQ 查不到 sqRegBase（IsSqDbMap=0），实走 halSqCqConfig**（§8.3 证据链：设备日志 + zbal 式实测失败 + tsch 源码两步注册分析） | 已验证 | — |
| ~~P0~~ **已完成** | restore"漏步骤"假设 → **结论：设备侧运行时库（/usr/lib64/libascend_hal.so）不导出 halResourceIdRestore，CP1 内 SYM_NULL，不可复现；修复必须走固件/驱动**（§8.3 CP1 实机实验：REG_BASE=65534 / restore 符号未解析 / 设备侧库普查） | 已验证（CP1 实机） | — |
| **P0** | 请求邮件数据 clean 必要性实测（定论 #1 反推） | 零成本实验 | 无 |
| **P1** | 在特制驱动 25.3.rc1.b099 机器上验证 CP 流 `IsSqDbMap` 是否=1（若=1 则 zbal 式直写直接可用） | 实机验证 | 特制驱动机器 |
| **P1** | 若需本机 MMIO doorbell → 改驱动/固件补齐 remote SQ 门铃两步注册（trs_sq_remap + CREATE 邮箱，闭源 handler 现跳过） | **改驱动/固件** | 驱动仓 |
| **P1** | ready/背压通知 SPR 化（场景二配方） | kernel + wire 契约扩展 | 特制驱动 + host 传 regBase |
| **P2** | PMU 周期计数器接入 timeline（场景五配方） | kernel 侧 | 特制驱动（无 fallback） |
| **P2** | `REQ_TAIL` 游标 `set_cond` 化（场景一） | kernel 侧 | 同上 |
| **不做** | SDMA 到达戳寄存器化（物理不可达）、大块 clean 寄存器化（语义必需） | — | — |
| **调研** | CCE write-through store（场景六替代路线） | 独立调研 | 工具链文档/实验 |

## 10. 公共前置条件与风险

1. **特制驱动**：P1/P2 全部依赖 25.3.rc1.b099 模式（`halMemCtl` 的 type=0（ADDR_MAP）操作放行 addr_type=2/3），本方案以特制驱动为前提
2. **地址传递**：host 拿映射地址（`base+i*2MB`）后须经 wire 契约传入 AICPU kernel（param 扩展）；pypto 已实证 host 拿的 VA 在 AICPU 进程有效
3. **CCE kernel 改动**：`MOV x0, DATA_MAIN_BASE` / `set_cond` / `set_ctrl` 指令加进 AICore 设备头（pypto `aicore_entry.h:160-201` 现成代码，注意 host 仿真需 stub）
4. **SPR 单槽协议**：所有寄存器化通道需单调递增序号防覆盖（现有 `tail+1`/`seq+1` 游标语义天然兼容）
5. **安全 review**：寄存器直写绕过驱动校验，fast-path 0x18 等控制位误写有挂核风险——只碰 0xA0/0x4C8/PMU 区域，复用 pypto 的偏移常量定义
6. **固件相关性**：SQE 配方固件强相关（机制档案 §2 纪律），doorbell 寄存器偏移（+0x8）同样需实机回归

---

# 附录

## 11. 与 CANN SHMEM 直驱直通方案的对照与启发（2026-10-12）

### 11.1 文章核查与一手实测

CANN SHMEM 直驱直通文章（AIV 直驱 STARS 驱动 SDMA + CMO 缓存预取）代码级核查**属实**：v1.3.0（2026-04-08 tag）落地、当前 1.7.0；三步流程/CMO 接口/样例齐全（§8.3 生产参照）；性能数字（小消息 80%+、CMO 3.3us、预取 2~4 倍）为内部实测，仓库无数据不可核实。310P3 侧一手实测（测试载体 `/tmp/opencode/stars_test/`）：StarsQuery 算子 aicpu exception、host `aclrtCmoAsync` 返回 207000——**文章能力在 310P3 当前不可用，堵点与场景三重合**。

### 11.2 对 memfabric 310P 通信优化的启发

1. **AICPU 中转的性能天花板有了官方数据背书**：文章三方案对比明确 AICPU 中转"小消息带宽低、P99 大、调度不稳定（最大 6 核）"——memfabric 现行 AICPU 编排方案的小消息上限即此形态；本文场景一/二（通知链寄存器化，砍 AICPU 调度环节的缓存维护开销）正对该瓶颈
2. **AICore 直驱有完整蓝图，310P3 堵点收敛为一个**：shmem 三步流程（CP 流→StarsQuery→AIV 直写 doorbell）是现成参照；310P3 的 StarsQuery/doorbell/CMO 堵点全部指向场景三的 SQ 通道信息获取 + doorbell 映射。**修好场景三解锁的不只是 AICPU 的 ioctl→MMIO 优化，而是整套 AICore 直驱（小消息 80%+ 的量级收益）**
3. **混合架构选项（不修驱动可试）**：xcclpp v43 已证 AICPU kernel 内直查 `halSqCqQuery(SQ_BASE)` 成功——AICPU 编排 kernel 初始化时把 `sq_base` 查好写进协同工作区 GM，AICore kernel 从 GM 读后**自己组装 SQE**（不经 AICPU 逐笔中转）；唯一剩余堵点是 doorbell 直写（AICore 无法调 ioctl）
4. **CMO 预取的下一步实验（可能立即可做）**：host 侧 `aclrtCmoAsync` 不可用（207000），但 **AICPU 可用现有 ioctl doorbell 提交 CMO SQE**（CMO 与数据 SQE 走同一 SQ 环）——值得实验：AICPU 编排 kernel 构造 CMO SQE（type=6 PREFETCH）提交，验证 310P3 固件是否执行。若通，"AICore 发预取请求邮件 → AICPU 构造 CMO SQE → SDMA 预取到 L2"立即可落地
5. **池化场景放大预取收益**：文章 2~4 倍是本地 GM→L2（L2 命中 ~7TB/s vs GM ~1.6TB/s）；memfabric 的远端池数据访问延迟更高，预取掩盖收益理论上更大——修好直驱后值得做"远端池数据 L2 预取"专题
6. **通道模型**：每核独立 CP 流（48/72 通道上限）避免多核共享 SQ 锁竞争——直驱修好后可借鉴

---

## 附录 A 特制驱动 25.3.rc1.b099 证据链

### A.1 行为差异实测

| 环境 | 驱动 | `halMemCtl` type=0（`CTRL_TYPE_ADDR_MAP`）行为 |
|---|---|---|
| 远端 bz_310p3 (123.60.231.33, Docker env_wwl_docker_310p_20260521) | 普通（版本未记录） | 全部 addr_type（0-3）返回 `DRV_ERROR_NOT_SUPPORT (65534)`；`CTRL_TYPE_SUPPORT_FEATURE` bit2=0；`halGetDeviceInfoByBuff` 全部 NOT_SUPPORT |
| 本地 A1 机器（host deviceId=3） | **25.3.rc1.b099**（CANN 9.0.0） | 基线分支 `pypto_base_resnet18_0522` "HAL 一次 map 成功" |

即 25.3.rc1.b099 是开放寄存器映射的驱动构建；文档未明说其内部改动（那是驱动仓的事），仅记录两台机器的功能差异。

### A.2 时间线

| 日期 | 事件 | 证据 |
|---|---|---|
| 2026-06-16 | DAV_2002 全链路适配计划（含寄存器映射设计） | commit `022455012` |
| 2026-06-16 | `GetAicoreRegInfoForDAV2002` 落地 | commit `fef306e18` |
| 2026-06-17 | 普通驱动 halMemCtl 整体 NOT_SUPPORT → 零地址 fallback | commit `02c5bcec1`；`mydocs/tests/issue_dav2002_map_reg_addr_failed.md` |
| 2026-06-18~21 | 25.3.rc1.b099 机器调试：诊断 probe 破坏驱动状态；基线一次 map 成功 | `mydocs/tests/log/log_bugfixed_0621.md` 等（标注"驱动版本：25.3.rc1.b099"） |
| 2026-06-30 | ret=13 根因：缺 `aclrtCreateEventExWithFlag` | `mydocs/reviews/R11_dav2002_root_cause_analysis.md` §2 |
| 2026-07-14 | 驱动门禁 25.5 → 25.3，放行 25.3.rc1.b099 | commit `4c5dba721`（1 行改动，无正文） |
| 2026-07-15~16 | A1 PMU 机制分析 + 使能计划 | `mydocs/reqs/req_a1_pmu_mechanism_analysis.md`、`mydocs/plan/plan_a1_pmu_enable_dav2002.md` |
| 2026-07-25 | mydocs/ 与 runtime_logs/ 从仓库删除 | commit `50be3081d`、`d76569f74` |
| 2026-09-01 | 门禁再降 25.3 → 24.1（奇数卡机器驱动 24.1） | commit `4df2cfdf9` |
| 2026-09-14 | 310P PMU 采集落地 | commit `c11315094` |

### A.3 历史文档与日志的恢复方式

mydocs 已从 HEAD 删除但保留在 git 历史中：

```bash
git show 50be3081d^:mydocs/reqs/req_dav2002_aic_register_mapping.md   # 寄存器映射需求文档
git show 50be3081d^:mydocs/tests/issue_dav2002_map_reg_addr_failed.md # NOT_SUPPORT 完整诊断
git show 50be3081d^:mydocs/tests/log/log_bugfixed_0621.md             # 25.3.rc1.b099 机器日志分析
git show 50be3081d^:mydocs/reqs/req_a1_pmu_mechanism_analysis.md      # A1 PMU 机制分析
```

备份运行时日志（含 25.3.rc1.b099 版本串 `ParseVersionInfo: ... [Version=25.3.rc1.b099]`）：
`git show e7af3379a:runtime_logs/log_07061137_baseline_add_11_success/log/debug/plog/plog-16170_20260706114044628.log`

## 附录 B 环境要求汇总

| 项 | 要求 | 来源 |
|---|---|---|
| 驱动（上板门禁） | ≥ 24.1（硬编码） | `device_launcher_driver_gate.h:24-25` |
| 驱动（寄存器/PMU 快路径） | **25.3.rc1.b099（特制）** | 本文 §1、附录 A |
| 驱动（文档推荐） | Ascend HDK 25.5.0 | `docs/zh/install/prepare_environment.md:110` |
| FAQ 口径 | 驱动 ≥ 25.2.0、CANN ≥ 8.5.0 | `docs/zh/tutorials/appendix/faq.md:122` |
| CANN | mybuild.sh 支持 cann8.5.1/cann9.0；PMU l2 采集建议 ≥ 9.1.0 | `mybuild.sh`、performance.md:151 |
| 本机现状 | 驱动 26.1.0（非特制版） | `/usr/local/Ascend/driver/version.info` |

## 附录 C 历史已知问题（均已修复，留档）

| 问题 | 根因 | 修复 |
|---|---|---|
| MAP_REG_ADDR_FAILED (ret=65534) | 普通驱动 halMemCtl 整体 NOT_SUPPORT | 零地址 fallback（`02c5bcec1`）→ GM fallback |
| halMemCtl ret=13 | 缺 `aclrtCreateEventExWithFlag` 初始化 | 补 event 创建（R11 §2） |
| handshake SIGSEGV phyId=8 | 奇数卡 phyId 8/9 越界寄存器表 | 边界校验快速失败 + 驱动 24.1 奇数卡支持（`8663ef9cb`） |
| DAV2002_DIAG probe 破坏驱动状态 | map→unmap 探测后真实 map 失败 | 删除诊断代码（`d7e78584`） |

## 附录 D 参考代码（正文机制的源码依据）

### D.1 halMemCtl 真实签名与结构体

本机驱动头文件 `driver/kernel/inc/driver/ascend_hal.h:2822` / `ascend_hal_define.h:873-941`：

```c
// halMemCtl 是 5 参通用控制入口（type 分发 9 种操作：ADDR_MAP/UNMAP/FEATURE 查询/内存修复/
// CP_MMAP 等），寄存器映射只是其中 type=0 的一条
drvError_t halMemCtl(int type, void *param_value, size_t param_value_size,
                     void *out_value, size_t *out_size_ret);

struct AddrMapInPara  { unsigned int addr_type; unsigned int devid; };        // in：哪类资源+哪个设备
struct AddrMapOutPara { unsigned long long ptr; unsigned long long len; };   // out：映射基址+长度
struct AddrUnmapInPara { unsigned int addr_type; unsigned int devid;         // 解除映射（type=1）用
                         unsigned long long ptr; unsigned long long len; };
```

> **为什么官方文档几乎为零**：`halMemCtl` 是驱动**内部接口**（非公开 SDK API，`ASCEND_HAL_WEAK` 弱导出、无兼容承诺），头文件全部文档只有一句泛泛的 "This command is used to control memory"；`addrMapType` 枚举每项一行注释（如 "Used to map aicore synchronous register"）是仅有的语义说明。32MB 区域、每核 2MB stride、0xA0/0x4C8 偏移等**布局知识不在任何头文件里**，来自三条腿：① 兄弟仓源码（hcomm/pypto 基线）② 头文件反推 ③ 实机探针实验（pypto mydocs 诊断、xcclpp `comm/` probe 工具）。特制驱动能"开放寄存器"也正因它是内部接口——驱动团队改自己的门禁。

### D.2 pypto 的真实调用与地址计算

`runtime_agent.cpp:164-194`（经 adapter 封装落在 `HalMemCtl`）：

```cpp
struct AddrMapInPara inMapPara;
struct AddrMapOutPara outMapPara;
inMapPara.devid = deviceId;
inMapPara.addr_type = addrType;              // 2=AIC_CTRL / 3=AIC_PMU_CTRL
auto ret = HalMemCtl(0,                      // type=0 (CTRL_TYPE_ADDR_MAP)
    reinterpret_cast<void*>(&inMapPara),     // param_value
    sizeof(struct AddrMapInPara),            // param_value_size
    reinterpret_cast<void*>(&outMapPara),    // out_value
    nullptr);                                // out_size_ret 不用，传 null

// 地址计算（:167-168 注释）：驱动共映射 32MB，每核 2MB stride，A1 有 10 核开放 8 个
constexpr int nrCore = 10;
constexpr uint64_t coreStride = 0x00200000ULL;  // 2MB
for (int i = 0; i < nrCore; i++) {
    aic.push_back(baseAddr + i * coreStride);   // 每核基址
}
```

### D.3 AICPU 侧 volatile 读写（aicore_hal.h）

```cpp
// 初始化预计算队列指针 (MapRegistersForAllCores, :419-438)
readyRegQueues_[i]  = (volatile uint64_t*)(regAddrs_[i] + 0xA0);   // 派任务
finishRegQueues_[i] = (volatile uint64_t*)(regAddrs_[i] + 0x4C8);  // 收完成

// 通用读写 (:161-217)，批量广播有 8 路展开版本
*(volatile uint32_t*)(regAddrs_[phyId] + offset) = val;   // WriteReg32
return *(volatile uint32_t*)(regAddrs_[phyId] + offset);  // ReadReg32
```

PMU 同一套，多一步页对齐：`ProfStartPmu`（`aicore_prof.cpp:402`）先 `addr & ~(pageSize-1)`，再按 `kArchPmuConfigs` 偏移表把各寄存器换算成 `volatile uint32_t*`。

### D.4 AICore 侧 CCE 指令（aicore_entry.h）

```cpp
__asm__ volatile("MOV %0, DATA_MAIN_BASE\n" : "+l"(x));  // :163 读任务寄存器（内联汇编，SPR 名）
set_cond(value);                                          // :193 写完成寄存器（CCE intrinsic）
set_ctrl((uint64_t)get_ctrl() | 0x1);                     // :446 PMU 采样开关（intrinsic）
```

| 类别 | 例子 | 说明 |
|---|---|---|
| intrinsic（内置函数） | `set_cond`、`set_ctrl/get_ctrl`、`wait_flag/set_flag` | 编译器直接翻译成 AICore 指令，类比 CUDA `__syncthreads()` |
| 内联汇编 | `MOV %0, DATA_MAIN_BASE`（`+l` 是 64 位通用寄存器约束） | `DATA_MAIN_BASE` 是汇编器认识的 SPR 名 |
| 语言扩展 | `__gm__` 指针 | 全局内存地址空间限定符，类比 CUDA `__global__` |

同构例子：AICPU 读自己定时器 `asm volatile("mrs %0, cntvct_el0")`（`aicore_prof.cpp:520`）——核内寄存器用指令访问的通用模式。host 仿真 stub：`host_aicore_entry_adapter.h:66-77`（`set_cond`/`GetDataMainBase` 替换为模拟器模型读写）。

### D.5 PMU 代码链（谁在什么时候操作寄存器）

```
PmuCommon::InitPmuEventType (pmu_common.cpp:115)   读环境变量 → 按架构生成 8 个事件码
AiCoreProf::ProfInit (aicore_prof.cpp:103)         profLevel 达标且 PMU_COLLECT=1 → ProfInitPmu(corePmuRegAddr, pmuEventAddr)
AiCoreProf::ProfStartPmu (:402)                    AICPU 握手后调用：清零计数器 → SetPmuEvents 写事件码到 PMU_CNTx_IDX
                                                   → 写 START=0/STOP=0xFFFFFFFF → 置 PMU_CTRL_0 使能
DfxProcAfterFinishTask (aicore_manager.h:3131)     每个任务完成时 ProfGetPmu 读 PMU_CNT0~7+TOTAL → 上报 msprof
AiCoreProf::ProfStopPmu                             恢复 ctrl、上报剩余数据
```

### D.6 memfabric submitSqe 的 doorbell 双路径（hybm_overlap_kernel.cc:319-332）

```cpp
if (c->ch->sqRegBase != 0) {                    // 来自 cp1QueryProp(prop=5, DRV_SQCQ_PROP_SQ_REG_BASE)
    *(volatile uint32_t*)(sqRegBase + 0x8) = newTail;   // MMIO 直写（shmem 同款）
} else {
    doorbellViaHal(devId, sqId, newTail);        // halSqCqConfig → ioctl
}
```

kernel 头注释（:17）写"doorbell 走 halSqCqConfig (ioctl)"——**已验证属实**（§8.3：CP 流 SQ `IsSqDbMap=0`，sqRegBase 查不到有效值）。

## 附录 E 相关文件索引

**pypto 仓库（机制证据与配方）**：

| 文件 | 角色 |
|---|---|
| `framework/src/machine/runtime/runner/runtime_agent.cpp:164-194` | Host 侧 halMemCtl 映射 |
| `framework/src/machine/runtime/runner/runtime_agent.h:24-27` | ADDR_MAP_TYPE 常量（4/5 为非官方 AIV 类型） |
| `framework/src/machine/runtime/runner/device_runner.cpp:243-285` | GetAicoreRegs 寄存器地址获取 |
| `framework/src/machine/runtime/runner/device_launcher_driver_gate.{h,cpp}` | 驱动版本门禁 |
| `framework/src/machine/device/dynamic/aicore_hal.h` | AICPU 侧寄存器读写/队列/握手/fast-path |
| `framework/src/machine/device/dynamic/aicore_constants.h:48-50` | REG_SPR_FAST_PATH (0x18) |
| `framework/src/machine/device/dynamic/aicore_prof.{h,cpp}` | PMU 配置表与采集实现 |
| `framework/src/machine/device/dynamic/aicore_prof_dav2002_pmu.h` | 310P3 PMU 寄存器偏移 |
| `framework/src/machine/runtime/runner/pmu_common.cpp` | PROF_PMU_EVENT_TYPE → 事件码 |
| `framework/src/machine/utils/device_switch.h:24-30` | PMU_COLLECT / PERF_PMU_TEST_SWITCH 编译宏 |
| `framework/src/interface/machine/device/tilefwk/aicore_entry.h:178-205,445-465` | AICore 侧取任务/上报/PMU ctrl |
| `framework/src/interface/machine/device/tilefwk/aicpu_common.h:44-51,151-179` | DeviceArgs 寄存器字段、shakeBuffer 索引 |
| `framework/src/machine/device/dynamic/aicore_manager.h:737,1554,3131` | ProfStart / SetReadyQueue / ProfGetPmu 调用点 |
| `framework/src/machine/simulation/host_aicore_entry_adapter.h:66-77` | host 仿真 intrinsic stub |
| `tools/profiling/tilefwk_pmu_to_csv.py` | PMU 二进制 → CSV |
| `docs/zh/tutorials/debug/performance.md:100-158` | 310P PMU 采集使用文档 |
| `docs/zh/tutorials/appendix/faq.md:95-154` | CANN 不兼容 FAQ（含 25.3.rc1 version.info 样例） |

**memfabric_hybrid 仓库（应用对象）**：

| 文件 | 角色 |
|---|---|
| `src/smem/include/device/smem_shm_aicore_base_sdma.h` | AICore 三件套（signal/wait/quiet），C1/C2/C4 热路径 |
| `src/smem/include/device/smem_shm_sdma_layout.h` | 双环工作区布局（REQ_TAIL/REQ_HEAD/QUIET/ARR_STAMP/ARR_MAIL 偏移） |
| `310p-capability_matrix/17_aicore_aicpu_overlap/aicpu_kernel/hybm_overlap_kernel.cc` | AICPU 编排 kernel（submitSqe/pollSlot/cp1QueryProp） |
| `docs/310p/机制档案.md` | SQE 配方、通知链三件套与 cache 纪律、八个定论 |
| `openspec/specs/smem/310p-sdma-orch/spec.md` | 协同模式行为契约 |
| `examples/memory_pool/01_basic/08_310p_aicore_aicpu_sdma/` | 生产示例（三件套用法） |

---

# 第三部分：驱动改动需求规格（2026-10-15 行号级核实）

> 本章为"向驱动团队提需求"的落地清单：每项需求给出**改哪个仓哪个文件哪一行、开放哪段寄存器、
> 怎么验证**。全部坐标已在本机 driver 源码仓逐行核实（`/home/w00930648/driver`）。

## 12. 机制新发现：halMemCtl 有 v2/v3 双链，构建产品线决定成败

**本轮核实修正了 §8.3 对"特制驱动"机制的推断**（原推断"注册了 ADDR_MAP handler 的构建"——方向对，但真相更具体）：

| | v2 链（ascend910B 产品线） | v3 链（ascend950 产品线） |
|---|---|---|
| halMemCtl 实现 | `ascend_hal/svm/v2/devmm/devmm_svm.c:3195` | `ascend_hal/svm/v3/api/master/svm_mem_ctl.c:150` |
| handler 表 | `devmm_svm.c:3182`：**`[CTRL_TYPE_ADDR_MAP]=devmm_ctrl_map_addr` 已注册**（:3184） | `svm_mem_ctl.c:144`：**ADDR_MAP 槽位为空** → 直接 NOT_SUPPORT |
| addr_type 二级表 | `devmm_map_dev_reserve.c:182-188`：**五种 addr_type 全注册**（含 AIC_CTRL=2 / AIC_PMU_CTRL=3） | — |
| 构建选择 | `ascend_hal/svm/CMakeLists.txt:18-22`：`PRODUCT == ascend910B → v2`；`ascend950 → v3` | 同左 |

**结论**：310P3 构建驱动时 PRODUCT 走哪条产品线，决定 halMemCtl(type=0) 是通还是 NOT_SUPPORT。
**特制驱动 25.3.rc1.b099 的最可能本质 = 310P3 走 v2 链（或 v3 补注册）的构建**。

## 13. 需求清单（三项，按依赖关系排序）

### R1+R2（一起提）：开放 AIC 同步/PMU 寄存器映射——场景一/二/五

**开放哪些寄存器**（memfabric 实际使用的子集，均可先整区域开放再收紧）：

| addr_type | 区域 | 大小/布局 | memfabric 用途与偏移 |
|---|---|---|---|
| 2 (`ADDR_MAP_TYPE_REG_AIC_CTRL`) | AIC 同步寄存器区 | 32MB = 每核 2MB × 10 核（开放 8） | 场景一 AICore 写 `0x4C8`(finish/游标)、场景二 AICPU 写 `0xA0`(ready/背压)、握手 `0x18`(fast-path 开关) |
| 3 (`ADDR_MAP_TYPE_REG_AIC_PMU_CTRL`) | AIC PMU 寄存器区 | 每核一页 | 场景五周期计数 `0x250/0x254`(PMU_CNT_TOTAL0/1) + 编程偏移 `0x200-0x2AC` |

**改动位置（三选一，由轻到重；先确认本机构建走哪条链再选）**：

| 方案 | 改动 | 仓 | 重量 |
|---|---|---|---|
| A. **零代码**（仅当本机驱动已是 v2 链构建） | proc 运行时开关：`echo 1 > /proc/<设备节点>/feature/aic_reg_map`（写接口 `svm_master_feature_proc_fs.c:50-76`，节点由 `devmm_dev_feature_proc_fs_create` 每设备创建） | — | 零 |
| B. **一行接线**（v3 链构建时） | `svm_mem_ctl.c:144` 的 handler 表加 `[CTRL_TYPE_ADDR_MAP] = <addr_map handler>`——移植 v2 的 `devmm_ctrl_map_addr`（`devmm_map_dev_reserve.c:194`）及其下游 | 开放 | 轻（表接线+链移植） |
| C. **产品线归位** | 310P3 的 PRODUCT 构建从 v3 切回 v2 线（或在 CMakeLists `:18-22` 增加 310P3→v2 分支）——整个 v2 链（含 unmap）原样生效 | 开放 | 中（构建配置，需回归） |

**内核侧两道闸（无论 A/B/C 都要过，均已核实）**：
1. 能力位 `feature_aic_reg_map`：**硬编码 true**（`svm_master_dev_capability.c:186`，不依赖设备上报）✓ 无需改
2. PF 禁用开关：`AIC_REG_MAP = {"aic_reg_map", ..., is_support_disable=true}`（`:38`）→ `devmm_dev_feature_capability_disable`（`:49`）**PF 设备默认禁用**——需 proc 写 1 打开（方案 A）或驱动团队把此项默认改为 enable
3. 用户态 split mode 闸：`devmm_map_dev_reserve.c:209-213`（AIC_CTRL/PMU 在 `devmm_is_split_mode()`（`devmm_svm.c:168`，查 `halGetDeviceSplitMode`）为真时 NOT_SUPPORT）——物理卡非 split 部署应不受影响，构建时确认

**验证方法**（照抄 pypto 模式）：`halMemCtl(0, {addr_type=2, devid}, &out)` 返回 `out.ptr` 非 0；pypto 侧日志 `Map AICore reg addr (type 2) 0x... for DAV_2002`（§5.1）。

### R3：CP 流 remote SQ 门铃注册——场景三（AICore 直驱解锁项，最重）

**这是三道墙的根因修复**（§8.3 已分析，此处给行号级落点）：

| 位置 | 文件/函数 | 改动 | 仓 |
|---|---|---|---|
| 1. 根因（必改） | CP 流 remote SQ 注册 handler：`TsDrvNormalSqInfoInit` / `trsRemoteAllocResId`（**grep 实锤不在开放仓**） | 注册 remote SQ 时补 doorbell 寄存器页 remap（**队列页已映 `IsHwSqMap=1`，同机制多映一页 db**），填 `sq_ctrl.db`，使 `IsSqDbMap=1`——参照物：host 普通流注册在 `trs_hw_sqcq.c:608/:969` 调 `trs_sq_remap`（`trs_sqcq_map.c:295`，内核本有此能力） | **闭源** |
| 2. 查询白名单 | `ascend_hal/trs/core/trs_sqcq.c:1481-1485` UIO 直读白名单：现含 HEAD/TAIL/CQE_STATUS/BASE/DEPTH/SQE_SIZE/MEM_ATTR——**缺 `DRV_SQCQ_PROP_SQ_REG_BASE`** | 加一项（前提：位置 1 修好，表里才有值） | 开放 |
| 3. 内核查询 | `trsdrv/trs/trs_core/trs_hw_sqcq.c:1642 trs_sqcq_query`：墙① `trs_proc_has_res(TRS_HW_SQ)`（CP1 进程不拥有，:1670 附近 -EINVAL）；墙② `trs_get_sq_reg_vaddr` reg_mem.uva=0（remote 未 remap）→ -EOPNOTSUPP | 位置 1 修复后墙②自解；墙①是资源归属语义，remote SQ 注册进内核 trs 资源池即可（随位置 1） | 开放（随位置1） |

**验证链**：设备日志 `IsSqDbMap=1` → CP1 `halSqCqQuery(prop=5)` 返回有效 VA → AICPU/AIV `*(volatile u32*)(sqRegBase+0x8)=tail` MMIO 直写 doorbell 生效（v43 通道回归）。

**不改动时的替代路径**（§11.2-3 已列）：AICPU ioctl doorbell（现行）+ 混合架构（AICore 自组 SQE、AICPU 代敲铃）。

## 14. 提需求时的三个关键问题（给驱动团队）

1. **310P3 的驱动构建 PRODUCT 归哪条线**？（决定 halMemCtl 是 v2 通链还是 v3 断链——25.3.rc1.b099 是怎么编的）
2. **PF 上 aic_reg_map 能否默认 enable**（或提供官方 proc 开关口径）？——`is_support_disable=true` 的设计初衷是什么（安全/多租户？）
3. **remote SQ 注册 handler 为什么跳过 db 页映射**（IsHwSqMap=1 而 IsSqDbMap=0）——是遗漏还是刻意（安全边界）？

## 15. 本轮新增核实坐标速查

| 坐标 | 内容 |
|---|---|
| `ascend_hal/svm/CMakeLists.txt:18-22` | v2(910B)/v3(950) 产品线构建选择 |
| `ascend_hal/svm/v3/api/master/svm_mem_ctl.c:144-149,150-165` | v3 handler 表（无 ADDR_MAP）+ halMemCtl |
| `ascend_hal/svm/v2/devmm/devmm_svm.c:3182-3190,3195,168` | v2 handler 表（有 ADDR_MAP）+ halMemCtl + split_mode 查询 |
| `ascend_hal/svm/v2/devmm/devmm_map_dev_reserve.c:182-193,194-215` | addr_type 二级表（全注册）+ 入口（split 闸 :209） |
| `sdk_driver/svm/v2/master/comm/svm_master_addr_map.c:49-76,78` | 内核 map（能力闸 :60）+ ioctl 入口 |
| `sdk_driver/svm/v2/master/comm/svm_master_dev_capability.c:38,49,186` | AIC_REG_MAP 项 / PF 禁用 / feature 硬编码 true |
| `sdk_driver/svm/v2/master/comm/svm_master_feature_proc_fs.c:50-76` | /proc feature 运行时开关（1=enable） |
| `ascend_hal/trs/core/trs_sqcq.c:1481-1485` | 用户态 UIO 查询白名单（缺 SQ_REG_BASE） |
| `sdk_driver/trsdrv/trs/trs_core/trs_hw_sqcq.c:1642,1670` | 内核查询（墙① proc_has_res） |
| `sdk_driver/trsdrv/trs/trs_core/trs_sqcq_map.c:295` | trs_sq_remap（host 普通流 db 注册能力，参照物） |
| `pkg_inc/ascend_hal_define.h:933,977-983` | CTRL_TYPE_ADDR_MAP=0 / addrMapType 枚举 |

## 16. 附：halSqCqConfig(SQ_TAIL) 的完整九跳链路（2026-10-15 行号核实，回答"最终怎么写上寄存器"）

```
用户态: doorbellViaHal(hybm:199) → halSqCqConfig(:211)
  → trs_sq_cq_config(trs_sqcq.c:1301; UIO直写:1313 因 db/tail=NULL 失败 → ioctl:1327)
  → trs_dev_ioctl(trs_dev_drv.c:623)  ══ 系统调用 ══
内核: ioctl分发表(trs_fops.c:1172) → ioctl_trs_sqcq_config(:319, copy_from_user)
  → type分发(:315, DRV_NORMAL_TYPE→trs_hw_sqcq_config)
  → trs_proc_has_res(trs_hw_sqcq.c:1518 — CP1 能过! 见下) 
  → case SQ_TAIL(:1537) → trs_chan_ctrl(chan_rxtx.c:1016) → trs_chan_set_sq_tail(:413)
  → ts_inst->ops.sqcq_ctrl(:418 函数指针, stars_v2_ops.c:342/:157)
  → trs_stars_v2_chan_sqcq.c:249/:238 → trs_stars_set_sq_tail(trs_stars.c:187)
  → ★ ka_mm_writel(tail, stars->base + sqid*stride + reg_offset)  ← 内核 iomem 直写 doorbell
```

**精化"三道墙"结论（重要）**：config 链的 has_res（:1518）实测能过（v43 全链 PASS）——
**CP1 内核资源池里其实有这条 remote SQ**。故 prop=5 查询 65534 的真正断点是**墙②**
（`trs_get_sq_reg_vaddr` 读 `reg_mem.uva`——remote SQ 注册未做 db 页 remap，uva 恒 0 →
-EOPNOTSUPP），非墙①。与 IsHwSqMap=1/IsSqDbMap=0 自洽：队列页映了（BASE 可查），db 页
没映（REG_BASE 不可查、kernel 不能直写、halSqCqConfig 的 UIO 快路径也是 NULL）。
**R3 修复（补 db 页 remap）一改三收的机理由此完全闭环**：uva 被填 → prop=5 返回有效
VA → kernel 直写 + halSqCqConfig UIO 化 + AICore 直驱全解锁；终点寄存器从来没变
（`stars->base` ≡ 查不到的 `sqRegBase`），变的只是"谁能直达它"。

## 17. R3 根因精化与开放仓补丁（2026-10-15 第二轮核实，重大修正）

**根因修正**：remote SQ 门铃查不到（prop=5 → 65534）的根因**不是**（只是）闭源注册
handler 跳过——是 **stars_v2 适配层把整块寄存器相关 ops 全部置 NULL**
（`trs_core_stars_v2_ops.c:119-124`：`sqcq_reg_map/reg_unmap/get_res_reg_offset/
get_res_reg_total_size…` 全 NULL），v1 有完整实现（`trs_core_stars_v1_ops.c:72-115`）
未被迁移到 v2。**→ R3 在开放仓即可尝试修复**（补丁见
`patches/R3-stars-v2-sqcq-reg-map.patch.md`）。

**关键机制链（行号核实）**：
- 注册: `trs_hw_sqcq.c:606/:965` → `trs_res_mng.c:1077` ops 空检查（v2=NULL 跳过）
- v1 实现: 发 `TRS_MSG_SQ_REG_MAP` 同步邮箱（`trs_host_msg.c:170
  devdrv_sync_msg_send`，带 remote_tgid → 设备侧处理回填 sq_map.va）→
  `trs_set_sq_reg_vaddr`（`trs_hw_sqcq.c:1566`，导出符号）填 `sq_ctx[sqid].reg_mem.uva`
- 查询: 用户态白名单不含 REG_BASE → ioctl 进内核 → `case` 已存在（`:1668`）→
  `trs_query_sq_reg_vaddr`（`:1626`）读 uva=0 → -EOPNOTSUPP

**两处修正旧结论**：① §8.3 位置 2"白名单加 SQ_REG_BASE"**不要做**（真值在内核，
必须走 ioctl；进白名单反而 UIO 读用户态表必失败）；② 墙①（has_res）实测能过
（config 同检查通过），唯一断点是 uva=0。

**补丁安全性**：失败=消息发送返回错误、uva 保持 0=现状不变；成功=prop=5 查询/
halSqCqConfig UIO 化/kernel 直写三链全通。剩余前置风险：设备侧消息处理器（闭源）
是否支持 v2 芯片——失败时退回位置 1（闭源 handler 补 db 页 mmap）。
