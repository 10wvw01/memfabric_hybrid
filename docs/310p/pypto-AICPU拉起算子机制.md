# PyPTO 在 AICPU 上拉起自定义算子的机制（参考档案）

> **文档定位**：说清 pypto 当前构建如何在 AICPU 上跑自定义算子，作为 memfabric
> 部署链（见[部署档案](./部署档案.md)）的对照材料。历次勘误与完整证据链见 git 历史。
>
> **证据基线**：pypto `trunk_for_a1` @ `bf5df560e`；本机 310P3 + CANN 9.1.0 + 驱动 26.1.0；
> 设备侧结论来自 mini_kfc 实机探针（2026-10-10/12/13）；架构定性采纳 pypto 官方结论
> （预置 extend 库拉起定制 so），本机证据链佐证（§3）。

## 1. 核心模型：预置引导层拉起定制 so，用户程序是载荷

三层结构（官方结论 + 实测佐证）：

1. **预置引导层**——`libaicpu_extend_kernels.so`（CANN 装机经签名通道预置），内含
   引导入口 `DynTileFwkKernelServer(Init)`（**pypto 仓库里没有这个符号**，是华为
   extend 集成层专有）和部署机器（落盘/dlopen/转发）；
2. **定制 so**——pypto 自建设备侧 runtime（wheel 里的 `libtilefwk_backend_server.so`，
   真身 `DynTileFwkBackendKernelServer(Init)`），**运行时由引导层拉起**：host 把字节
   经 GM 传入 → 引导层写入 **memfd（内存文件）并 dlopen(`/proc/self/fd/N`) 加载**——
   **全程不落盘**（2026-10-15 反汇编勘正，此前"落盘到设备搜索路径"结论错误），
   保证与主机侧版本严格匹配；
3. **用户程序**——控制流二进制（机器码）嵌在程序镜像里，以数据身份进 GM，
   被定制 so 里的 runtime 以函数指针 call。

| | memfabric | pypto |
|---|---|---|
| 设备上预置的 | 无（全部自己部署） | 引导层（extend 库） |
| 运行时部署 | deployer + KFC `cat`（外部工具） | 引导层内置部署机器（GM→落盘→加载） |
| 用户程序 | 固定功能 kernel（so 上设备） | 机器码载荷（不上文件系统） |

## 2. 完整链路

### 2.1 编译期（host 侧 JIT）

用户 Python 代码被编译成两样产物（AICPU 视角）：

1. **控制流二进制**——AICPU 上跑的用户代码：JIT 生成 C++ 源码
   （`backend.cpp:525` BuildControlFlow，入口函数 `ControlFlowEntry`）→ CANN 交叉
   工具链 `aarch64-target-linux-gnu-g++` 编成 **AArch64 机器码**（AICPU 即 ARM 核）
   → objcopy 抽出 `.pypto` 代码段；
2. **程序镜像（DevAscendProgram）**——控制流二进制 + 异构子图描述 + 任务编码表，
   打包成一个自描述的内存结构。

（AICore 的 CCE kernel 是另一条编译线（bisheng/ccec），与本问题无关，不展开。）

### 2.2 每次执行（host 侧，AICPU 视角）

| 步 | 动作 | 源码坐标 |
|---|---|---|
| ① | **两类载荷 H2D 拷到 GM**：a) wheel 的定制 so 字节（`InitAiCpuSoBin`，存入 `DeviceArgs.aicpuSoBin`，进程 Init 时一次）；b) 程序镜像 + 参数（`cfgdata` 指向） | `device_runner.cpp:390-408`（so 字节）、`:76-89`（通用拷贝） |
| ② | KFC 直发**引导层入口** `DynTileFwkKernelServer(Init)`：进程 Init 直发一次 Init（引导层此时消费 ①a：落盘定制 so + dlopen + 缓存真身地址）；每次执行直发 1 个 CTRL + N 个 SCHED（引导层转发到定制 so 真身，靠参数 runMode 分角色） | `device_runner.cpp:427-434`（Init）、`:530-550`（三流）、`:364-387`（双流） |

KFC 直发机制：`RuntimeAicpuKernelLaunchExWithArgs(AICPU_KFC, "AST_DYN_AICPU", ...)`，
函数名/库名写在参数里（`machine_ws_intf.h:105-110` 默认
`kernelName="DynTileFwkKernelServer"`、`soName="libaicpu_extend_kernels.so"`），
设备侧 `ae_kernel_lib_aicpu_kfc.cpp` 按名解析——和 mini_kfc 同一套机制。

### 2.3 设备侧全景

```
host:  ①a so字节→GM(Init)  ①b 程序镜像→GM(每次)  ② KFC直发引导层×(1+N)
   ▼
┌─ 设备 ──────────────────────────────────────────────────┐
│ 【引导层】libaicpu_extend_kernels.so (CANN预装)          │
│   DynTileFwkKernelServerInit: 读GM里so字节→memfd_create │
│     →write→snprintf("/proc/self/fd/N")→dlopen→dlsym     │
│     真身×3 填函数表(key=1/2/3) ←【拉起定制的aicpu so】  │
│     (全程内存, 不落盘 — 反汇编c3ebc/c4538/c51ac/c46bc)   │
│   DynTileFwkKernelServer: 查表key=3转发到定制so真身      │
│ 【定制 so】(memfd内存加载, 与wheel版本严格匹配)           │
│   DynTileFwkBackendKernelServer = runtime本体            │
│     ├─ CTRL AICPU (1核): GELaunch取出ControlFlowEntry    │
│     │    地址【call 用户机器码】→经回调表要服务           │
│     │    →每轮生成任务进SCHED队列                        │
│     ├─ SCHED AICPU (N核): 取任务→依赖就绪→通知AICore     │
│     └─ AICPU小任务: aicpuLeafCode字节码(AicpuTaskManager)│
└──────────────────────────────────────────────────────────┘
```

**call，不是 launch**：CANN 意义上被"拉起"的只有引导层入口；用户的自定义算子
（控制流二进制）是被定制 so 里的 runtime 经 `ctx.GELaunch`（`device_ctrl.h:348`）
取出 `ControlFlowEntry` 地址**当函数指针调用**的机器码；需要服务（分配内存/生成
任务）时经 `runtimeCallList` 回调表反调 runtime（ROOT_ALLOC / ROOT_STITCH…）。

另有一类受限用户任务：`aicpuLeafCode`（如分布式 wait_until），编成 int32 opcode
数组（`codegen_npu.cpp:725`）由 AicpuTaskManager 解码执行——字节码，非任意代码。

### 2.4 三个常见疑问

**Q：直发的两个入口具体干什么？**
`DynTileFwkKernelServerInit`（引导层，Init 一次）：消费 GM 里的定制 so 字节（落盘
+加载+缓存真身地址），并做日志/trace/性能注册、异常信号处理注册；仓库侧同名逻辑
参考 device_sche.cpp:45-62。`DynTileFwkKernelServer`（引导层，每次执行 1+N 次）：
转发到定制 so 真身，按 runMode 分角色走 CTRL/SCHED 流程。

**Q：AICPU 的代码 so 什么时候部署到设备？GM 里的拷贝怎么到文件系统？**
分两份说：**引导层**（extend 库）随 CANN 装机预置在设备 `/usr/lib64/aicpu_kernels/0/
aicpu_kernels_device/`（注意：该目录在设备上是**易失的运行时环境**——2026-10-15 实测
wwl 机器上整个目录曾被清除，见 §6）；**定制 so**（wheel 版 runtime）由 pypto 运行时部署：
`InitAiCpuSoBin` 把字节 H2D 到 GM，进程 Init 时引导层 `DynTileFwkKernelServerInit` 消费它
——**写进 memfd 内存文件后经 `/proc/self/fd/N` 伪路径 dlopen，不经过文件系统**（2026-10-15
拉回 extend 库反汇编钉死：`memfd_create(G+256,MFD_CLOEXEC)`→`write(fd,aicpuSoBin,soLen)`
→`snprintf_s(G+0x80,"/proc/self/fd/%ld")`→`dlopen(G+0x80)`，偏移 cfgdata@40/aicpuSoBin@96/
aicpuSoLen@104 与主机侧参数镜像 static_assert 三重一致）。extend 库内的目录串与 so 名串
只是日志/提示文案（ExecuteFunc 报错时引用 `pyptoServerSoName_`），不代表落盘路径。

**Q：用户真的从不向 AICPU 下发算子吗？**
**当前构建：是**——AICPU 下发点只有引导层入口两个；用户代码进 GM 走的是数据通道。
仓库侧自定义算子下发机制（`LaunchCustomOp` 等，load_aicpu_op.cpp:77/106/127）函数体
全在 `#ifdef BUILD_WITH_NEW_CANN` 内，产物实测为空；另有一条未启用的 opt-in 路径
（`TileFwkAiCpuCompile`：用户函数→独立 so→CANN 自定义算子，mode=1 上传）。

## 3. 事实清单（实机探针 + nm/strings，2026-10-10/12/13）

| # | 事实 | 结果 |
|---|---|---|
| 1 | 设备搜索路径有 `libaicpu_extend_kernels.so` | ✅ 存在，>3MB（wheel 内同源库约 1MB） |
| 2 | **KFC 直发目标名 `DynTileFwkKernelServer(Init)`（不带 Backend）在 pypto 仓库无定义、wheel so 无此符号**（`nm -D` 只有 `DynTileFwkBackendKernelServer(Init)`；仓库内仅 machine_ws_intf.h:107 / device_runner.cpp:417 的下发字符串） | ✅ → 该入口是华为 extend 集成层专有 = 引导层 |
| 3 | extend 库含部署机器证据：`DynTileFwkKernelServerInit`、"Begin to create server.so"（SaveSoFile 日志）、"has been loaded"（dlsym 缓存日志）、目录串 `aicpu_kernels_device`、so 名串 `libtilefwk_backend` | ✅ 全命中 |
| 4 | extend 库含 runtime 本体代码串（`DynTileFwkBackendKernelServer`、device_sche 日志串"Ctrl enter round" 等）——内置 base 版或引导层自带 | ✅（类名 AiCoreManager 等不在，发布构建无 RTTI，正常） |
| 5 | extend 库不含 `libpypto` / `DynPyptoKernelServer*`（json 路径命名） | ✅ 引导层变体命名与仓库 json 路径不同 |
| 6 | 设备上无 `libtilefwk_backend_server.so` 预置、无运行时落盘副本 | ✅ 属预期：本机从未跑过 pypto，落盘只发生在运行时（引导层消费 GM 载荷后） |
| 7 | 仓库 json 三件套（PyptoNull 落盘 `libpypto_server{设备号}.so` + 转发桩）被 `BUILD_WITH_NEW_CANN`（runtime/CMakeLists.txt:23 被注释）整体关闭，wheel/本地构建产物该分支日志串均为 0；复刻下发 PyptoNull → 507018 | ✅ 死代码路径，与现行机制无关 |
| 8 | 环境版本 | CANN 9.1.0（310P 包，内置 pypto 0.2.1 wheel）+ 驱动 26.1.0；FAQ 要求驱动 ≥25.2.0、CANN ≥8.5.0；官方环境脚本仅 a2/a3（HDK 25.3.RC1、固件 7.8.0.2.212） |

**遗留未知**：本机（9.1.0/26.1.0）Init 桩执行（sync=0）但函数表未填上——断点在
dlopen(fdlink)/dlsym/填表环节，嫌疑 seccomp 拦 `openat("/proc/self/fd/N")` 或 9.1.0
extend 行为变化（对照 §6：wwl 机器 2026-05-11 曾出现同款表空错误，5→9 月间自愈，
解法未明——需向 wwl 求证）。

## 6. wwl 机器交叉验证（2026-10-15，CANN 8.5.1/9.0.0 并存 + 驱动 24.1.rc3）

在该机器（docker_claude_310p，6 NPU = 3 芯片×2die，dev0/1 Critical、其余 OK）上跑
wgm-310p 分支探针套件（fork: gitcode.com/gcw_w5BPnxkC/pypto，工具 wgm_verify/）的结论：

| # | 事实 | 证据 |
|---|---|---|
| 1 | **memfd 部署链在 wwl 机器批量成功过**：2026-09-16/17 dev2 上 `LoadTileFwkKernelFunc` 三函数（Static/DynInit/Dyn）"has been loaded" 共 **873 次**，extend 库 OpenSo 成功 344 次；完整时序 `KFC(kernelType=5)→OpenSo extend→14ms 内三函数 loaded→4 执行桩并行→AICore 开工`（plog device-2/device-116138_20260916095202551.log 等） | plog 铁证 |
| 2 | **该机器 2026-05-11 出现过与本机现在同款的 Function[3] 表空错误**（device-0 plog，`ExecuteFunc: kernel func[3] is invalid`）——5→9 月间被解决（转折原因未明，最大嫌疑 CANN 9.0.0 环境部署了新版 extend） | plog |
| 3 | **设备 aicpu_kernels 环境是易失的**：dev2 9 月还在用，2026-10-15 实测 dev2/3/4/5 的 `/usr/lib64/aicpu_kernels/` 整目录不存在（`/usr/lib64/libaicpu_processer.so` 仍在）→ 探针在所有可用卡上 undefined symbol。9-18~10-15 之间环境被整体清除（清除事件未明，dev0/1 同时 Critical） | check_all_dev 扫描 |
| 4 | **两机 CP1 框架库不同版本**：wwl 机 processer 110,472B（md5 14f9bf…）vs 本机 79,848B（md5 0a8479…）——驱动 24.1.rc3 vs 26.1.0 差异的直接体现 | file_pull 拉取对比 |
| 5 | wwl 机器 extend 库（9-16 实际使用的版本）**未能取回**：dev0 环境 Critical + 其他卡环境已清除；设备侧 extend 库随 aicpu_kernels 目录一同消失 | — |

**对 memfabric 的补充启示**：pypto 的 memfd 部署证明**无文件系统权限也能让 CP1 执行
任意 so**（GM 字节→内存文件→dlopen），比落盘方案更轻；但该链对 extend 库版本/环境
敏感（两机都出现过"表空"阶段），且环境本身会消失——memfabric 的自管部署（deployer）
在这两点上更可控。

## 4. 仓库侧未启用的同类机制：json 三件套（一段话）

仓库里有一套设计完备的同类版本解耦机制：json 登记三个内置算子 → PyptoNull 把
wheel 版 so 经 GM 落盘成 `libpypto_server{设备号}.so` → 转发桩查表转发真身。
**该路径被编译开关整体关闭**（事实清单 #7）。它与 extend 引导层是**同一思想的
两种实现**（预置引导 + 运行时落盘激活定制 so + 转发）：仓库版走 json 算子通道，
extend 版由华为编译进 CANN——实际生效的是后者。完整解析见 git 历史（2026-10-12 版）。

## 5. 与 memfabric 部署链对照

| 维度 | memfabric | pypto（实际） |
|---|---|---|
| 设备预置 | 无 | 引导层（extend 库，随 CANN/驱动预置但**环境易失**，§6#3） |
| "第一次"/每次 | deployer（CUST 模式 1 迁移）+ KFC `cat`，外部工具 | 引导层内置部署机器：GM 中转 → **memfd 内存文件** → dlopen，无需 shell、无需文件系统权限 |
| 版本匹配 | 改名 + KVER 递增纪律 | memfd 加载的定制 so 与 wheel 严格同版本 |
| 用户程序形态 | 固定功能 kernel（so 上设备） | 机器码载荷（程序镜像进 GM，不上文件系统） |
| 生命周期风险 | deployer 清理链删设备文件 | memfd 随进程生命周期，无文件残留；但依赖 extend 环境存活 |
| 本质 | 第三方 kernel 挤进设备 | 华为自家组件：引导层随 CANN 预置 + 定制 so 运行时激活 |

**对 memfabric 的可借鉴点**：pypto 的"GM 中转 + 预置引导层 + memfd 内存加载"是**已生效**
的无 shell、无文件系统权限自部署范式——比 memfabric 的 KFC `cat` 方案不受单命令约束、
无 `sync=` 歧义，也不需要 aicpu_kernels 目录写权限；memfabric 若借鉴，需先解决"引导层
怎么预置"（你们的等价物仍是 deployer/OPP 通道，或做成 CANN 自定义算子首次安装）。

## 附录：关键坐标与证据

- KFC 直发三调用点：`device_runner.cpp:427-434`（Init）/ `:530-550`（三流，无条件）/ `:364-387`（双流 else）；参数默认值 `machine_ws_intf.h:105-110`（注意 kernelName 指向引导层专有符号）
- 定制 so 载荷：`device_runner.cpp:390-408`（InitAiCpuSoBin → `DeviceArgs.aicpuSoBin`）；真身源码：pypto 仓库 `framework/src/machine/device/`（构建目标 `tile_fwk_server`，入口 `device_sche.cpp:45,53` 带 Backend 的符号；CTRL `device_ctrl.h/.cpp`；SCHED `aicore_manager.h`）
- 引导层：**仓库无源码，但 2026-10-15 已拉回本机设备上的实体并反汇编**（/tmp/opencode/pull/libaicpu_extend_kernels.so，8,063,424B，拉取工具 wgm_verify/file_pull*）——Init 桩关键坐标：`c3e3c`(cfgdata@40)/`c3e4c`(aicpuSoBin@96)/`c3e7c`(SoLen@104)/`c3ebc`(memfd_create)/`c4538`(write)/`c51ac`(snprintf "/proc/self/fd/%ld"，.rodata@0x576270 实测)/`c46bc`(dlopen)/`c51c0`(设已加载标志G[112])/`c40c8`(查表key=2)；孪生实现（机制镜像，源码在手）：pypto `pypto_aicpu_interface.h` BackendServerHandleManager（key: 2=Init/3=Exec，"has been loaded"=h:134、"Function[N] is null"=h:145）
- 设备侧加载框架坐标（plog 实名）：KFC 解析 `ae_kernel_lib_aicpu_kfc.cpp:79 GetApiGlobal/:168 CallKernelApi`；so 管理 `ae_so_manager.cc:193/202 OpenSo`；宿主库 `/usr/lib64/libaicpu_processer.so`（两机版本不同，§6#4）
- wwl 机器验证产物存档：pypto fork `wgm-310p` 分支 `wgm_verify/results/1015_150633/`（plog 铁证 + processer_wwl.so + 全部 results 日志）
- 仓库 json 三件套（未启用）：`load_aicpu_op.cpp`、`pypto_aicpu_interface.{h,cpp}`；开关 `runtime/CMakeLists.txt:23`（注释状态）
- 方法论备注（三次误判复盘）：①"代码存在且精致"≠"代码在运行"——先看构建配置，再看产物 strings，最后读源码分支；②**符号在哪个构建产物里定义，比源码里写了什么更接近真相**（2026-10-13 纠正"真身直接跑"误判）；③**验收标准必须与被测机制同构**——查文件系统验证一个不落盘的机制（memfd），"零部署"假阴性三周才纠正（2026-10-15 反汇编+wwl 机器 873 次成功加载日志双证）
