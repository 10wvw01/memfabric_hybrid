# PyPTO 在 AICPU 上拉起自定义算子的机制（参考档案）

> **文档定位**：说清 pypto 当前构建如何在 AICPU 上跑自定义算子，作为 memfabric
> 部署链（见[部署档案](./部署档案.md)）的对照材料。历次勘误与完整证据链见 git 历史
> （本文 2026-10-13 由 462 行详版精简重写）。
>
> **证据基线**：pypto `trunk_for_a1` @ `bf5df560e`；本机 310P3 + CANN 9.1.0 + 驱动 26.1.0；
> 设备侧结论全部来自 mini_kfc 实机探针（2026-10-10/12/13）。

## 1. 核心模型：库是预装的，程序是载荷

**pypto 不往设备部署任何算子库。** CANN 装机时已在设备预置了一个通用 runtime
（`DynTileFwkKernelServer`，编译在库存组件 `libaicpu_extend_kernels.so` 里）；
用户的自定义算子被 JIT 编译成"程序镜像"，**每次执行时作为参数拷到设备内存**，
由这个 runtime 现场执行与调度。

| | memfabric | pypto |
|---|---|---|
| 设备上的库 | 自己部署（deployer + KFC） | CANN 预装，零部署 |
| 用户程序 | 固定功能 kernel（so 文件上设备） | 每次传入的数据载荷（不上文件系统） |

## 2. 完整链路

### 2.1 编译期（host 侧 JIT）

用户 Python 代码被编译成两样产物（AICPU 视角）：

1. **控制流二进制**——AICPU 上跑的用户代码：JIT 生成 C++ 源码
   （`backend.cpp:525` BuildControlFlow，入口函数 `ControlFlowEntry`）→ CANN 交叉
   工具链 `aarch64-target-linux-gnu-g++` 编成 **AArch64 机器码**（AICPU 即 ARM 核；
   `backend.cpp:1103` 同时编 host 版用于仿真）→ objcopy 抽出 `.pypto` 代码段；
2. **程序镜像（DevAscendProgram）**——控制流二进制 + 异构子图描述 + 任务编码表，
   打包成一个自描述的内存结构。

（AICore 的 CCE kernel 是另一条编译线（bisheng/ccec），与本问题无关，不展开。）

### 2.2 每次执行（host 侧两步，AICPU 视角）

| 步 | 动作 | 源码坐标 |
|---|---|---|
| ① | 程序镜像 + 参数 H2D 拷到设备内存（GM），算子参数 `cfgdata` 指向它——**用户代码以数据身份进 GM** | `device_runner.cpp:76-89`（CopyDataToDevice） |
| ② | KFC 直发 `DynTileFwkKernelServer(Init)` 给 AICPU：进程 Init 直发一次 Init；每次执行直发 1 个 CTRL + N 个 SCHED（同一函数，靠参数 runMode 分角色）——**这是唯一被"拉起"的算子，且是预置 runtime，不是用户代码** | `device_runner.cpp:427-434`（Init）、`:530-550`（三流 ctrl/sched）、`:364-387`（双流） |

（AICore 的 CCE kernel 走正常 launch 通道，与"AICPU 上拉起算子"无关，此处不展开。）

KFC 直发的机制：`RuntimeAicpuKernelLaunchExWithArgs(AICPU_KFC, "AST_DYN_AICPU", ...)`，
真实函数名/库名写在参数里（`machine_ws_intf.h:105-110` 默认
`kernelName="DynTileFwkKernelServer"`、`soName="libaicpu_extend_kernels.so"`），
设备侧 `ae_kernel_lib_aicpu_kfc.cpp` 按名解析——和 mini_kfc 同一套机制。

### 2.3 设备侧：用户代码如何被"拉起"——**call，不是 launch**

预置 runtime 在 CTRL AICPU 上执行到 `ctx.GELaunch(...)`（`device_ctrl.h:348`）时，
从 GM 的程序镜像里取出 `ControlFlowEntry` 的**地址，当函数指针直接调用**：

```cpp
ControlFlowEntry(ctx, symbolTable, runtimeCallList[], startArgs)
```

用户机器码从此在 AICPU 上原生速度执行；需要 runtime 服务（分配内存、生成任务）时
经 `runtimeCallList` 回调表反调 runtime（ROOT_ALLOC / ROOT_STITCH…）。
**CANN 意义上的"算子拉起"只发生一次（预置 runtime）；用户的自定义算子在 AICPU
上是被 runtime 当子程序 call 的机器码。**

AICPU 上另有一类受限用户任务：`aicpuLeafCode`（如分布式 wait_until），编成 int32
opcode 数组（`codegen_npu.cpp:725`）由 AicpuTaskManager 解码执行——字节码，非任意代码。

```
host 每次 launch
   │  ①程序镜像(含用户机器码)→GM   ②KFC直发runtime×(1+N)
   ▼
┌─ 设备 ─────────────────────────────────────────────────┐
│ 【extend 库】(CANN 预装, >3MB, 含 server 真身代码)      │
│   DynTileFwkKernelServer                                │
│     ├─ CTRL AICPU (1核): GELaunch 取出 ControlFlowEntry │
│     │    地址【call 用户机器码】(原生速度跑用户循环/调度)│
│     │    → 用户码经回调表要服务 → 每轮生成任务进 SCHED  │
│     ├─ SCHED AICPU (N核): 取任务→依赖就绪→              │
│     │    写寄存器/GM 通知 AICore                        │
│     └─ AICPU 自己也干小活（aicpuLeafCode 字节码任务）    │
└──────────────────────────────────────────────────────────┘
```

### 2.4 三个常见疑问

**Q：直发的两个入口具体干什么？**
`DynTileFwkKernelServerInit`（进程 Init 一次）：日志开关、DeviceTrace、性能统计注册、
**注册异常信号处理** `SignalReg(SigAct)`——出错时复位 AICore 寄存器
（device_sche.cpp:45-62）。`DynTileFwkKernelServer`（每次执行 1+N 次）：runtime
本体，按 runMode 分角色走 CTRL/SCHED 流程（device_sche.cpp:53-60 → device_sche.h:563）。

**Q：AICPU 的代码 so 什么时候部署到设备？GM 里的拷贝怎么到文件系统？**
**当前构建不部署、也没有"后面"**——GM 里那份 so 拷贝（`InitAiCpuSoBin`）是死重，
唯一能把它写成设备文件的 `SaveSoFile`（PyptoNull 算子内）被编译开关关闭。实测设备
上无 `libpypto_server*.so`：pypto 在设备文件系统里一个字节都没放过。设备上的
extend 库是 **CANN/驱动装机升级时**经签名固件通道放的，与 pypto 运行无关（本机
从未跑过 pypto，extend 库在；host 无其非签名副本）。"GM → 落盘 → 文件系统"是
未启用的 json 三件套流程，勿与现行机制混淆。

**Q：用户真的从不向 AICPU 下发算子吗？**
**当前构建：是**——AICPU 下发点只有预置 runtime 两个入口；自定义算子下发机制
（`LaunchCustomOp`/`CustomAiCpuSoLoad`/`AicpuKernelLaunch`，load_aicpu_op.cpp:77/106/127）
函数体全在 `#ifdef BUILD_WITH_NEW_CANN` 内，产物实测为空。但**存在一条未启用的
opt-in 路径**：`TileFwkAiCpuCompile`（compile_control_bin.cpp，环境变量
`ENABLE_CTRLFLOW_COMPILE` 控制）把单个用户函数的控制流编成独立 so
（`lib{函数名}_control.so`），注册为 CANN 自定义 AICPU 算子（`{func}Init/{func}Run`），
mode=1 连 so 内容上传后 launch——该路径运行时同样被 NEW_CANN 开关关闭。


## 3. 事实清单（实机探针，2026-10-10/12/13）

| # | 事实 | 结果 |
|---|---|---|
| 1 | 设备搜索路径 `/usr/lib64/aicpu_kernels/0/aicpu_kernels_device/` 有 `libaicpu_extend_kernels.so` | ✅ 存在，**>3MB**（wheel 里那份同源库才 1MB） |
| 2 | 该库含 pypto server 代码：`DynTileFwkBackendKernelServer` 入口、device_sche 日志串（"Ctrl enter round" 等）均在 | ✅（类名如 AiCoreManager 不在——发布构建无 RTTI，正常） |
| 3 | 该库不含 json 路径转发桩导出名（`DynPyptoKernelServerNull`） | ✅ 不含（与三件套未启用一致） |
| 4 | 设备上无 `libtilefwk_backend_server.so`（json 路径预置库）、无 `libpypto_server*.so`（落盘副本） | ✅ 都不存在；全机无 pypto 运行痕迹 → extend 店是装机预置，非 pypto 放置 |
| 5 | `InitAiCpuSoBin`（把 wheel 里 so 字节 H2D 到 GM）仍无条件执行，但无人消费 | ✅ 死重（json 路径未启用的残影） |
| 6 | 环境版本 | CANN 9.1.0（310P 包，内置 pypto 0.2.1 wheel）+ 驱动 26.1.0；FAQ 要求驱动 ≥25.2.0、CANN ≥8.5.0；官方环境脚本仅支持 a2/a3（HDK 25.3.RC1、固件 7.8.0.2.212） |

## 4. 未启用的储备机制：json 三件套（一段话）

源码里另有一套设计完备的版本解耦机制：json 登记三个内置算子 → PyptoNull 把
wheel 版本 so 经 GM 落盘成 `libpypto_server{设备号}.so` → 转发桩查表转发真身。
**该路径被编译开关整体关闭**（`BUILD_WITH_NEW_CANN` 定义在
`runtime/CMakeLists.txt:23` 被注释；wheel 与本地构建产物中该分支独有日志串均为 0）。
启用它的前提是设备预置 `libtilefwk_backend_server.so`——哪个环境提供它目前无任何
直接证据。复刻实验：手工按该路径下发 PyptoNull，launch=0 / sync=507018（库缺失，
执行失败）——验证的是死代码路径的失败形态，不影响实际运行。完整解析见 git 历史
（2026-10-12 版 §3）。

## 5. 与 memfabric 部署链对照

| 维度 | memfabric | pypto（实际） |
|---|---|---|
| "第一次"上设备 | deployer（CUST 模式 1 迁移）+ KFC `cat` | 真身随 CANN 签名通道预置于 extend 库 |
| 日常 | 安装期一次，运行期不再传 | 零部署，程序镜像每次随参数传 |
| 版本管理 | 改名 + KVER 递增 | 随 CANN 同源打包（CANN 内置 pypto wheel） |
| 生命周期风险 | deployer 清理链删设备文件 | 无 |
| 本质 | 第三方 kernel 挤进设备 | 华为自家组件搭签名通道便车 |

**对 memfabric 的可借鉴点**：pypto 的"设备内存中转 + 落盘算子"设计（未启用那套）
可替代 KFC `cat` 做无 shell 自部署——不受单命令约束、无 `sync=` 歧义，前提是落盘
算子先上设备（"第一次"仍靠 deployer/OPP）。

## 附录：关键坐标与证据

- KFC 直发三调用点：`device_runner.cpp:427-434`（Init）/ `:530-550`（三流，无条件）/ `:364-387`（双流 else）；参数默认值 `machine_ws_intf.h:105-110`
- NEW_CANN 开关（注释状态）：`framework/src/machine/runtime/CMakeLists.txt:23`
- 真身源码：pypto 仓库 `framework/src/machine/device/`（构建目标 `tile_fwk_server`）——入口 `dynamic/device_sche.cpp:45,53`；分派 `device_sche.h:563`；CTRL `device_ctrl.h/.cpp`；SCHED `aicore_manager.h`；数据结构 `utils/dynamic/`。这是 pypto 自有的**设备侧 runtime**（host 侧在 `framework/src/machine/runtime/`），非 CANN 代码。**同一源码两份构建**：pypto 自建 → wheel 内 `libtilefwk_backend_server.so`（当前不部署不用）；华为随 CANN 构建 → 编进 `libaicpu_extend_kernels.so` 预置到设备（设备上实际跑的这份；华为所用 pypto 基线不可考，签名包不可解包，仅符号/日志串与仓库源码一致）
- json 三件套源码：`load_aicpu_op.cpp`（json 生成/加载）、`pypto_aicpu_interface.{h,cpp}`（落盘+转发）
- extend 库承载归因：CANN `opp/Ascend310P/aicpu/Ascend310P-aicpu_extend_syskernels.tar.gz`（`aa55aa` 签名格式，实测存在，未解包直证）——命名与库名对应
- 方法论备注（误判复盘）：判断代码行为先看构建配置，再看构建产物 strings（分支独有日志串），最后才读源码分支——"代码存在且精致"不等于"代码在运行"（本档案曾因此误判，详见 git 历史 2026-10-12 修正记录）
