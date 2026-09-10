# 310P AICPU SQE 编排模式开发计划（V1，xcclpp aicpu 模式移植）

> **状态：待启动（计划评审中，未开始实施）**
> - 本计划将 MemFabric 310P 数据面从 host 编排（`HostDataOpAclMemcpy`）扩展为可选的
>   **AICPU 编排**（AICPU kernel 内直投 STARS SDMA SQE），复刻 xcclpp `aicpu-310p3`
>   已验证的 v43 配方
> - 已定决策：**开关切换接入**（默认 host 编排，env 开关 opt-in + 探测回退）、
>   **deployer+KFC 部署链先行**（OPP .run 产品化后置）、**一期范围 = 卡内双 die G2G + batch**
> - 上游依据：xcclpp **main 分支** `examples/tutorials/aicpu-310p3/`（PR #27 合入，机制知识档案
>   + 0908 模式矩阵定案）、
>   `memfabric-310p-内存池化方案.md`（D1-D9 决策）、`310p-2rank-vmm-开发计划.md`（前置工程，
>   已验收）、`docs/架构概览.md`（第 5 章数据面插槽机制）

## 一、背景与动机

### 1.1 现状（310p-2-rank 已交付的）

host 编排：`HostDataOpAclMemcpy`（`src/hybm/csrc/data_operation/host/hybm_data_op_acl_memcpy.cpp`）
每笔拷贝由 host 线程直接调 `aclrtMemcpyAsync(D2D) + AclrtSynchronizeStream`。已实机验收
（07 示例三阶段 mismatch=0）。局限：

- 每笔拷贝一次 host 提交 + 一次 stream 同步，批量场景 host 线程是瓶颈；
- async 语义降级为逐跳同步（`hybm_data_op_acl_memcpy.cpp:196-207` 注释明示），`hybm_wait` 为 no-op；
- 数据面完全绑死 host 线程，无法演进设备侧发起 / in-transit 归约（Reduce SQE）等能力。

### 1.2 xcclpp aicpu 模式是什么（已验证的外部依据）

数据面从 host 编排改为 **AICPU 编排**：host 只做资源准备 + `aclrtLaunchKernelWithHostArgs`
一次调用；AICPU kernel（跑在设备 CP1 `aicpu_scheduler` 进程内）自己构造 64B STARS V2 SDMA SQE
直写 SQ 环 + doorbell → TSFW 固件消费 → SDMA 引擎执行搬运。xcclpp 侧关键实测结论：

| # | 事实 | 证据（xcclpp README / 源码） |
|---|------|------------------------------|
| X1 | **v43 SQE 配方全链 PASS**：type=0x09（MEMCPY_ASYNC_SQE_V2）+ kernel_credit=0xff + w4=`00de0e00` + SMMU streamid 按 die（die0=0x7F45，die1=0xBF45）+ ssid 对称=实时查询值 + off8-11 全 0 | `aicpu_kernel/xcclpp_aicpu_sqe.cpp` `buildStarsSdmaSqe`；**已对照 hcomm 源码逐字段证实（见附录 A.1 源码出处列）**；旧值 type=0x01 是 AICPU kernel-launch 类 SQE，固件按启动报文解析垃圾字段 → 静默丢弃假成功 |
| X2 | **提交链**：SQE 直写 sqBase（`dc cvac` 逐 cacheline 刷）→ `halSqCqConfig(SQ_TAIL)` doorbell（CP1 内 HAL devId=**die 号**，host 设备号 %2） | `submitSqe`；v41 实测 tail 落寄存器、v43 后 head 前进（TSFW 消费） |
| X3 | **CP 流配方**：`aclrtCreateStreamWithConfig(ACL_STREAM_DEVICE_USE_ONLY)` + `rtStreamGetSqid`（libruntime.so 内部符号）取 sqId | `aicpu_sqe_mpi.cc:146-156` |
| X4 | **远端内存 IPC Case A**：接收方 P2P policy 分配 + `GetExportKey(flag=0)` + `SetImportPid` + `ImportByKey`（flag 按 `aclrtGetDevicesTopo` 选 HCCS/PHB） | 双拓扑（卡内双 die / 跨卡 PCIe）4MB 全量比对 PASS |
| X5 | **完成检测（main 模式矩阵 0908 定案）**：生产配方 = **completeMode 0（flag-SQE 哨兵轮询）**：kernel 尾部追加 8B SDMA 哨兵搬运 + `dc civac` 轮询，PASS；NOTIFY_RECORD（type=6，mode 1）仅对照用——**310P3 notify 不触发（E3 证伪）**，流内 wait 任务死等不放行且 timeout 不生效（sync/destroy 永久挂死），完成判据退化为数据比对 | main `README.md` 模式矩阵表 + kernel 头注释；wgm-dev4 的 CQE 化（mode 2）实测受阻已回退，未进 main |
| X6 | **部署链（CP1）**：deployer（`CUSTAICPUKernel` json mode=1 落真 .so）→ KFC `cat` 复制到 `/usr/lib64/aicpu_kernels/0/aicpu_kernels_device/`（改名避免 PID 后缀多版本拼接）→ launch 用 `AICPUKernel` json（cpuMode=0） | 全链真机 PASS；CMS 验签墙只拦 common-sink 直推通道 |
| X7 | **部署链已知坑**：并发 deployer 互斥（多设备须串行）；隔夜文件被清（跑前验证门 `grep -q <symbol>`）；KFC 非特权身份、单条短命令稳定（>3 段串联易 507018）；ssid 不可硬编码（随启动变化，kernel 内 `drvMemSmmuQuery` 活查询） | README 部署链档案 |
| X8 | **flag-SQE 兼容模式**：8B SDMA 哨兵搬运 + `dc civac` 轮询（zbal 同款），25s 上限（< AICPU watchdog 30s） | completeMode=0 |
| X9 | **部署文件系统边界（2026-09-10 本机实测，E0a 前置成果）**：**同卡双 die 共享设备 OS 文件系统**（dev0 写 /tmp 探针 → dev1 `test -f` 可见 sync=0）；**跨卡完全独立**（dev2/dev4 均不可见）。推论：①**同卡部署一次即服务双 die**（搜索路径同为 `/usr/lib64/aicpu_kernels/0/`，同一文件系统，两 die 的 CP1 各自 dlopen 同一文件）；②0209 deployer 互斥的作用域推断为**同卡**（共享设备侧迁移服务 CP2 竞态），跨卡目标独立理论上可并行。附带勘正：**KFC sync= 命令退出码**（0→sync=0，非零→507018），xcclpp"通道偶发 507018 须重试"的表述部分实为命令失败的正确信号（确定性失败的命令重试也不会过） | 本机 KFC 探针矩阵：dev0/dev1 同卡（正/阴性对照）+ dev2/dev4 跨卡（`test -d /usr/lib64` 正对照证明通道健康） |

### 1.3 MemFabric 侧可复用资产（重大利好）

| 资产 | 位置 | 复用方式 |
|------|------|---------|
| AICPU kernel 构建体系 | `src/hybm/ops/hybm_kernel/`（hcc 交叉编译 + json + tar） | 追加 `hybm_aicpu_sqe.cc` 源文件即入链 |
| wire 契约先例 | `hybm_batch_transfer.h`（POD 参数 + static_assert） | 照抄模式写 `hybm_aicpu_sqe_contract.h` |
| DataOperator 插槽 | `hybm_data_op_factory.cpp:22` 已有 310P 分支 | 开关路由处扩展 |
| ACL 符号层 | `src/hybm/csrc/under_api/dl_acl_api.{h,cpp}` | 追加新符号弱加载 |
| 310P 池化链路 | `HybmAclVmmSegment`（07 已验收） | SQE 的 src/dst 直接用池内 GVA（待 E0b 验证） |

### 1.4 与现有设备侧 kernel 基础设施的区别（评审必读，防混淆）

仓库现有两套设备侧 kernel 搬运，**只有一套是真 AICPU**，且机制与本计划完全不同：

| 维度 | 现有①：HybmBatchRead/Write（真 AICPU） | 现有②：copy_extend/accoffload（**AICore**，易误认） | 本计划：HostDataOpAicpuSqe |
|------|------|------|------|
| 位置 | `src/hybm/ops/hybm_kernel/`（hcc，OPP .run） | `src/hybm/csrc/copy_extend/`（AscendC AIV） | 新增（见 P1） |
| 执行侧 | CP1 | AICore | CP1 |
| AICPU/AICore 角色 | **hcomm 通道代理调用者**：kernel 内调 hcomm 弱符号 `HcommWriteOnThread` 等（hybm_batch_transfer.cc:17-27），搬运引擎封装在 hcomm 内不可见 | UB 中转 DataCopyPad 拷贝 | **自主编排者**：自构 64B STARS SDMA SQE + doorbell，不经 hcomm |
| 消费方 | `DeviceUrmaTransportManager::LaunchDeviceKernelBatch`（A3 UB，:1933） | `HostDataOpSDMA` COPY_EXTEND_FLAG（dlopen extend lib） | 新 DataOperator 插槽 + 开关 |
| 完成检测 | flag 轮询（含 marker-only） | mask 轮询（AICore） | NOTIFY_RECORD（flag 降级保留） |
| 平台 | A3/910B | A3/910B | 310P |

**310P 上现有两套全部不可用**（本计划的存在理由）：① libhcom 设备库未部署（07 运行日志
"Unable to open library libhcom.so"），弱符号落空；② paramSpace 依赖 `HalHostRegister`，
310P 不可用；③ 310P host 侧 STARS 直投不可用（`hybm_data_op_factory.cpp:25-29`）——
但 **AICPU 内投可用**（xcclpp v43 实证）。

**可复用部分**：`BinaryLoad + GetFunction + KernelArgsInit/Append + LaunchKernelWithHostArgs`
框架（`load_kernel.cpp`/`DlAclApi` 已封装）、wire 契约 POD 模式、OPP `.run` 部署终态、
flag 完成检测先例。

### 1.5 预期收益与预期管理

- **收益**：batch N 段一次 launch + 攒批 doorbell（一次 AICPU 往返摊薄）；真异步语义
  （notify 兑现 `DataCopyAsync`/`Wait`）；为 Reduce SQE（归约）与设备侧发起铺路。
- **不承诺**：单笔延迟优于 host 编排——多一跳 AICPU 中转，**预期劣化**；性能基线必须在
  P3 量化（单笔延迟 vs 批量吞吐两张表）。

## 二、目标与非目标

**目标（一期）**：
1. 卡内双 die（dev0/dev1）2-rank HBM 池 G2G + batch 数据面走 AICPU SQE；
2. `MF_310P_DATA_OP_AICPU=1` 开关 opt-in，默认 host 编排；探测失败自动回退并记 ERROR 日志；
3. 07 示例开 AICPU 模式三阶段（池间部分）mismatch=0 + 干净退出；
4. UT 覆盖（acl_stub 扩展 + 算子单测 + 回退路径）。

**非目标（一期不做，登记 backlog）**：
- 跨卡 PCIe（PHB）拓扑（xcclpp 配方已有，移植放二期）；
- H2G/G2H 的 SQE 直译（host 内存注册 SMMU，风险最高）——一期保留 `aclrtMemcpy` 混合路径；
- Reduce SQE / CQE 完成检测 / OPP `.run` 产品化部署 / 常驻 kernel（watchdog 30s 会杀，不做）。

## 三、设计决策

| # | 决策 | 内容 | 理由 |
|---|------|------|------|
| D1 | **开关切换接入** | 新 env `MF_310P_DATA_OP_AICPU`（`MF_*` 主命名，遵循 `mf_env_define.h` 惯例），默认 0；`DataOperatorFactory::CreateSdmaDataOperator` 310P 分支按开关选 `HostDataOpAclMemcpy`（默认）/ `HostDataOpAicpuSqe`（新） | 回退安全：SQE 配方固件强相关，07 已验收链路不受影响；PR 可评审性好 |
| D2 | **探测回退** | `HostDataOpAicpuSqe::Initialize` 内冒烟（建流→取 sqId→加载 kernel→提交一笔写读 own HBM 的 SQE→校验），任一步失败记 ERROR（含 deviceId/sqId/根因）置回退标记，数据面退回 AclMemcpy | 已知部署链脆弱（X7），回退是硬性设计 |
| D3 | **部署形态：三段演进** | ① P0 外部脚本复刻 xcclpp 部署链（X6，隔离变量最快验证）；② **P2 起 opt-in 自部署**：`MF_310P_AICPU_SELF_DEPLOY=1` 时 Initialize 内完成部署——验证门先查（已部署跳过；**同卡对端已部署即命中**，X9）→ 按**卡分组**抢 config store 分布式锁（`DistributedLockGuard` 现成设施；**串行化粒度=卡**：同卡互斥、跨卡文件系统独立可并行，X9）→ mode=1 迁移 + KFC 复制 + 验证门 → mode=0 加载；③ OPP `.run` 产品化后自部署整段删除（移安装期）。**生命周期语义**：自部署文件与进程同生命周期（deployer 退出即清理链，天然对齐池生命周期，消除外部脚本的保活时长错配问题——xcclpp 隔夜文件消失实测）。**07 场景简化**：dev0/dev1 同卡 → **单次部署服务双 die** | 自部署是运行时能力（零外部步骤+生命周期正确）；外部脚本是 POC 手段；OPP 是产品终态。KFC（设备侧 shell 执行）进库代码需安全评审明示（风险表 R8） |
| D4 | **每笔 copy 一次 launch，不做常驻** | batch N 段一次 launch + kernel 内攒批 doorbell | CP1 watchdog 30s 杀常驻进程；hcomm/zbal 均按需 launch |
| D5 | **方向分层** | 一期 G2G 三方向（GD2GD 含跨 die）走 SQE；H2G/G2H 委托 `aclrtMemcpy` 混合路径（日志明确标注） | 见非目标；混合路径语义与现有算子一致 |
| D6 | **完成检测** | **flag-SQE 哨兵轮询**（kernel 尾部哨兵 + `dc civac` 轮询，main 生产配方 PASS）；NOTIFY_RECORD 仅实验保留（310P3 notify 不触发 + 流内 wait 挂死，X5）——若实验 NOTIFY，**禁止在流内 enqueue wait 任务** | 对齐 xcclpp main 模式矩阵定案 |
| D7 | **wire 契约单一事实源** | `hybm_aicpu_sqe_contract.h`：纯 POD、static_assert 锁布局、hcc/g++ 双工具链可编 | 照 `aicpu_sqe_contract.h`（xcclpp）与 `hybm_batch_transfer.h`（本仓）先例 |
| D8 | **符号层封装** | `rtStreamGetSqid`（libruntime）等新符号全部进 `DlAclApi` 弱加载，缺失即回退 | 内部符号有 CANN 版本漂移风险，弱加载 + 探测是防线 |

## 四、分阶段任务

### P0 可行性关卡（~2-3 天，三个实验，任一失败走 Plan B）

**E0a 部署链复刻（~0.5 天）**：xcclpp v43 kernel 源码（`buildStarsSdmaSqe`/`submitSqe` 等）
移植进 hybm_kernel hcc 构建为独立 .so（`libmf_aicpu_sqe.so`），在本机复刻 deployer+KFC 链，
验收 = kernel 在 dev0 CP1 内执行（result 区 magic 校验）。
已知坑照单全收（X6/X7）：deployer json 必须 `CUSTAICPUKernel`、launch json 必须
`AICPUKernel` cpuMode=0、deployer 单实例保活、复制改名、跑前验证门。

**E0b 关键实验：AclVmm 池 VA 上的 SQE（~1 天）**——本计划最大技术未知点。
07 示例变体：双 die 建池（AclVmm import 完成）后，发起方 launch kernel，SQE 的 src/dst
直接用池内 GVA（own/peer HBM 窗口 VA），SDMA 搬运后逐字校验。
- 通过 → 架构成立（AclVmm 导入 VA 与 xcclpp 的 IPC Case A VA 理论同类，均 shareable handle
  导入，未实测）；
- 失败 → **Plan B**：`HybmAclVmmSegment` 增加 IPC Case A 导入模式（P2P policy + ImportByKey，
  xcclpp 配方现成，改动集中在段层）。

**E0c 完成检测 + 双向（~0.5 天）**：flag-SQE 哨兵轮询为主验证（main 生产配方）；
NOTIFY 仅作对照（已知不触发 + 挂死风险，**不得 enqueue 流内 wait**）；
双方互为发起方各跑一轮。

> P0 全部在 `310p-capability_matrix/` 风格的独立实验目录做（`17_aicpu_sqe/`，不进主线构建），
> 产物为实验报告追加进本文件（仿 F1-F20 格式）。

### P1 数据面骨架（~1 周，4 工作包）

**WP1 kernel**（`src/hybm/ops/hybm_kernel/hybm_aicpu_sqe.cc` + contract 头）：
入口 `HybmAicpuSqeRun`：ssid 活查询（`drvMemSmmuQuery` 弱符号）→ 逐项
`buildStarsSdmaSqe`（v43 逐字节配方，注释锚定 X1 每个字段的出处）→ 攒批直写 SQ 环
（`dc cvac` + `dsb ish`）→ 一次 doorbell（`halSqCqConfig`，CP1 内 devId=die 号）→ 尾部
NOTIFY_RECORD；流控：SQ 环满分批提交。CMakeLists 追加源文件。

**WP2 DlAclApi 扩展**：`rtStreamGetSqid`（注意来自 **libruntime.so**）、
`aclrtCreateStreamWithConfig`/`aclrtStreamGetId`、`aclrtBinaryLoadFromFile`/`GetFunction`/
`LaunchKernelWithHostArgs`、notify 族、`aclrtDeviceGetBareTgid`。全部弱加载 + 探测。

**WP3 `HostDataOpAicpuSqe`**（新 DataOperator，对照 AclMemcpy 结构）：

挂载点（为什么在 create2 而非 bm_init）——先看初始化分级地图（全部实测锚点）：

| 级 | 入口 | 内容 | 部署放这？ |
|---|---|---|---|
| L0 安装期 | OPP `.run` / `_provision.py` | kernel 装进 CANN OPP tree | 产品终态（D3-③） |
| L1 进程级 | `smem_init` / `mf.initialize()` | 日志/TLS/last-error | ✗ 过早（不知道要不要建 SDMA 池） |
| L1.5 引擎级 | `smem_bm_init` | store 连接/竞争、AutoRanking、`hybm_init`（SoC 探测/ptracer） | ✗ 进程级过粗（纯 DRAM/SHM 用户被迫部署） |
| **L2 池级** | `MemEntityDefault::Initialize`（hybm_entity_default.cpp:106-133） | ①InitTagManager ②LoadExtendLibrary（dlopen）③InitSegment（不分配内存）④InitTransManager（**URMA 设备 kernel 在此加载**）⑤**InitDataOperator（★挂载点）** | ✓ |
| L2.5 池资源 | smem_bm_entry.cpp:157-208 | reserve 窗口 → **alloc HBM/DRAM（:167，在算子 init 之后）** → export | ✗ 太晚且非资源语义 |
| L3 成员级 | `smem_bm_join` | import/建链/mmap（可多次：rejoin/extend） | ✗ 一 join 一次，副作用叠动态回滚 |

选 L2 的五条理由：①作用域精确（dataOpType 是池参数；HOST_SHM 池/SHM API 均不需要——
AI_CORE_INITIATE 明确跳过 dataOperator，:1267-1270）；②资源准备语义统一（URMA kernel 加载
在隔壁 InitTransManager、扩展库 dlopen 在 LoadExtendLibrary，同族先例）；③前置齐备（store 已连
=分布式锁可用、SoC 已探测、deviceId/rankId 已知）；④回退决策点就在工厂路由（D1/D2），错误不
跨层传播；⑤多池重复初始化由单例+引用计数兜底。调用链：`smem_bm_create2 →
SmemBmEntry::Initialize(smem_bm_entry.cpp:124) → hybm_create_entity → MemEntityDefault::Initialize
(hybm_entity_default.cpp:106) → InitDataOperator(:1264-1280) → HostComposeDataOp::Initialize
(hybm_compose_data_op.cpp:32) → CreateSdmaDataOperator → Initialize()`。
**多池防重复加载：进程级单例 + 引用计数**（binHandle/funcHandle 全局复用，最后一个
UnInitialize 才释放——每 entity 各建一份 compose→sdma 算子）。
部署边界（D3 三段演进）：P0/P1 为外部脚本前置步骤（不进 memfabric 代码）；P2 起 opt-in 自部署
（`MF_310P_AICPU_SELF_DEPLOY=1`，Initialize 内完成，配置存储锁串行，文件生命周期=进程）；
OPP 产品化后自部署删除、移安装期（`_provision.py` / `installation_aicpu_kernel.md` 先例）。

- `Initialize`：读开关 → 建 CP 流 → sqId/streamId → kernel 加载 → 冒烟探测（D2）。
  **时序约束（实测核实）**：算子 Initialize 时池内存尚未分配（`hybm_alloc_local_memory` 在
  entity Initialize 返回后，smem_bm_entry.cpp:167）→ 冒烟探测**不得用池 GVA**，自备 scratch
  （`aclrtMalloc` 小 buffer → SQE 写读校验 → 释放）；池内真实验证留给 P1 验收的 07 链路；
- `DataCopy`(D2D)：param 写 GM → launch →（kernel 内 flag-SQE 轮询）→ `SynchronizeStream` 等待；
- `BatchDataCopy`：N 段一次 launch + 攒批 doorbell（**核心收益点**）；
- H2G/G2H：委托 `aclrtMemcpy`（D5）；
- 错误自恢复：stream 重建照抄 AclMemcpy 的 `SyncStream` 恢复模式。

**WP4 路由与开关**：工厂 310P 分支（D1）+ 回退日志。

**P1 验收**：单 rank 本地 SQE 冒烟（写读 own HBM）；开关关闭时行为与现版本完全一致（回归）。

### P2 语义完整 + 07 验收（~1 周）

- **自部署模式（D3-②）**：`MF_310P_AICPU_SELF_DEPLOY=1` 时 Initialize 内部署——验证门先查
  （grep 符号，已部署跳过）→ 抢 config store 分布式锁（`DistributedLockGuard`）→ mode=1
  CUSTAICPUKernel 加载+GetFunction 迁移 → KFC `cat` 复制到本设备 CP1 搜索路径 → 验证门 →
  mode=0 加载；多 rank 各自设备按锁串行（deployer 并发互斥，X7）。**文件生命周期=进程生命周期，
  进程退出自动清理**。默认关闭（外部队列预部署仍为主路径）
- 双 rank 双向并发（**同卡单次部署服务双 die**，X9；07 双进程模型 dev0/dev1 同卡，
  rank0 部署、rank1 验证门直接命中；每 die 的 CP1 各自 dlopen 同一文件）；
- `DataCopyAsync` 真异步 + `Wait` 兑现（一期可先同步化，完成检测按 D6 flag 轮询）；
- 资源生命周期：池 destroy 时 CP 流/kernel/notify 清理顺序、deployer 保活进程的拉起与退出管理；
- **验收：07 示例 AICPU 模式三阶段 mismatch=0 + 干净退出**（复用
  `310p-2rank-vmm-开发计划.md` 第八节命令，`/tmp` 二进制放 `/tmp` 之外——07 的 WORK_DIR
  会 `rm -rf /tmp/p5_e2e` 自删，已踩坑）。

### P3 加固与基线（~1 周）

- UT：`test/ut/mock/acl_stub.cpp` 扩展新符号 mock；算子单测（探测失败回退、batch 边界、
  notify 超时、SQE 环满分批）；
- 故障注入：kernel 未部署→回退、SQE 不消费→超时上报（fault_injection 机制现成）；
- 性能基线报告：单笔延迟 + batch 吞吐 vs host 编排（预期单笔劣化、批量占优，两张表量化）；
- 文档：`docs/` 增 310P AICPU 模式说明（部署步骤、开关、已知限制）；`docs/架构概览.md`
  第 5/8 章补一行新算子。

## 五、代码规范

沿用 `310p-2rank-vmm-开发计划.md` 第五节与 `AGENTS.md` 全量规范：函数≤50 行、嵌套≤4 层、
错误返回记 ERROR 日志（关键参数：deviceId/sqId/rankId/kernel 名/ret）、行宽≤120。
kernel 侧（hcc）代码遵循 `hybm_batch_transfer.cc` 现有风格。每个 P 完成即跑改动覆盖路径 UT。

## 六、风险与对策

| 风险 | 等级 | 对策 |
|------|------|------|
| E0b：AclVmm VA 不被 SDMA ssid 翻译 | **高（唯一可推翻架构项）** | P0 前置验证；Plan B 段层 IPC Case A |
| deployer+KFC 是 POC 形态（保活/易失/非特权） | 高 | 一期验收不承诺产品可用；OPP 路线在 backlog；探测回退兜底 |
| R8：自部署把 KFC（设备侧 shell 执行）引入库代码 | 中 | 默认关闭（opt-in env）+ 部署路径全链日志留痕；上线前过安全评审（PR 模板安全项）；OPP 终态删除该路径 |
| R9：自部署并发（多 rank/多进程同时 mode=1）| 中 | config store 分布式锁串行（`DistributedLockGuard` 现成）；锁等待超时回退外部队列 |
| `rtStreamGetSqid` 为 libruntime 内部符号，CANN 升级漂移 | 中 | D8 弱加载 + 探测；声明锁定 CANN 9.1 |
| SQE 配方固件强相关（X1 教训：type 错=静默假成功） | 中 | D2 冒烟探测；配方逐字段注释出处；失败回退 |
| AICPU 单笔延迟劣化引发"退步"质疑 | 确定 | 1.4 预期管理 + P3 双表量化 |
| 部署链运维复杂（串行/验证门/文件易失） | 中 | P3 文档化运维手册；验证门脚本化 |

## 七、里程碑

| 里程碑 | 内容 | 退出条件 |
|--------|------|---------|
| M0 | P0 三实验 | E0a/b/c 报告追加进本文件；E0b 结论明确（通过或 Plan B 立项） |
| M1 | P1 骨架 | 单 rank 冒烟 PASS + 开关关闭回归全绿 |
| M2 | P2 语义 | 07 AICPU 模式三阶段 mismatch=0 |
| M3 | P3 加固 | UT 全绿 + 性能基线报告 + 文档合入 |

## 八、验证执行命令（占位，M1 后补全实测版）

```bash
# 部署（POC 链，串行，跑前验证门）
./deployer 0 aicpu_kernel/libmf_aicpu_sqe.json 3600 &   # 保活
sleep 15 && ./mini_kfc 0 "cat /home/CustAiCpuUser/lib/libmf_aicpu_sqe.so.* > \
  /usr/lib64/aicpu_kernels/0/aicpu_kernels_device/libmf_aicpu_sqe.so"
./mini_kfc 0 "grep -q HybmAicpuSqeRun /usr/lib64/aicpu_kernels/0/aicpu_kernels_device/libmf_aicpu_sqe.so"

# 运行（07 示例 + 开关）
export MF_310P_DATA_OP_AICPU=1
export ASCEND_HOME_PATH=/usr/local/Ascend/cann-9.1.0
# （构建与运行命令同 310p-2rank-vmm-开发计划.md 第八节，二进制勿放 /tmp/p5_e2e）
```

## 九、后续 TODO（backlog，一期不做）

- **TODO-1**：OPP `.run` 产品化部署（复用 `script/kernel/build_ops_run.sh`，需验证 310P3
  OPP vendors 分发兼容性）
- **TODO-2**：跨卡 PCIe（PHB）拓扑支持（xcclpp `make run_mpi DEVS="0 2"` 配方移植）
- **TODO-3**：H2G/G2H 的 SQE 直译（host 内存注册 SMMU 实验）
- **TODO-4**：Reduce SQE（xcclpp 已拿到 slot7 模板：type=0x09 opcode=2 fp32 SUM，AllReduce
  in-transit 归约）
- **TODO-5**：完成检测 CQE 化（`halCqReportRecv` 设备侧正路，携带 errorCode 可判"搬错了"，
  优于 notify 只能判"搬完了"）。**hcomm 生产配方已源码证实**（aicpu_hccl_sqcq.cc:289-315
  `CqReportRecv`）：`timeout=0` 非阻塞 + `task_id=0xFFFF` 全收 + 非 DRV_LOGIC_TYPE 时
  `cqe_num=MAX_REPORT_CNT`；`AC_SQE_REV_MAX_CNT=32`（aicpu_hccl_sqcq.h:126）。
  **风险预告**：xcclpp wgm-dev4 的 B1 实验（completeMode=2）在 310P3 实测 CQE 受阻后回退
  （保持 mode 0，未进 main）——实施前先复核其受阻根因（wgm-dev4 提交 82224c1/8f0d684）

## 附录 A：xcclpp 移植配方速查（实施时直接对照）

### A.1 SQE 64B 布局（v43 破壁版，逐字段出处）

> **源码级证据（2026-09-09 已对照 `/home/w00930648/hcomm` 逐字段证实）**：
> 构造函数 `AddOneMemcpySqeV2`（`src/legacy/ascend910/platform/task/rtsq_interact/aicpu_hccl_sqcqv2.cc:92-152`）、
> SQE type 枚举（`pub_inc/aicpu/aicpu_hccl_sqcqv2.h:20-30`）、credit 常量
> （`ascend950/unified_platform/resource/stream/aicpu/sqe.h:24` = 255U）。
> v43 的配方不再是"环 dump 逆向"，每个字段均有 hcomm 生产源码出处：

| 偏移 | 值 | 说明 | hcomm 源码出处 |
|------|-----|------|----------------|
| 0 | `0x09` | `RT_HW_STARS_SQE_TYPE_SDMA = 9`（**0x01 是 AICPU kernel-launch 类 SQE**，固件按启动报文解析 → 静默丢弃假成功） | aicpu_hccl_sqcqv2.h:29 |
| 4-5 | streamId | CP 流 rt_stream_id | `sqe->rt_stream_id = streamId` |
| 6-7 | taskId | | `sqe->task_id = taskId` |
| 8-11 | 全 0 | 0903 fresh dump 勘正（残留数据曾误导为 f1/f0） | — |
| 12-15 | `res3:16 \| credit=0xff:8 \| res4:8` | `RT_STARS_NEVER_TIMEOUT_KERNEL_CREDIT = 255` | sqe.h:24 |
| 16-19 | `00 de 0e 00` | opcode=NOT_ATOMIC + **sro/dro/sns/dns/sssv/dssv 六位全 1** + overflow_en + qos | aicpu_hccl_sqcqv2.cc:116-121,131-132 |
| 20-21 / 24-25 | SMMU streamid | die0=0x7F45，die1=0xBF45（`devId==0 ? 0x7F45 : 0xBF45`） | aicpu_hccl_sqcqv2.cc:101-106 |
| 22-23 / 26-27 | ssid | **对称**=实时查询值（`drvMemSmmuQuery`，不可硬编码） | `sqe->src/dst_substreamid = ssid` |
| 28-31 | length | | `sqe->length = length` |
| 32-39 / 40-47 | src / dst（32+32 拆分） | | aicpu_hccl_sqcqv2.cc:123-127 |
| 48-51 | `00 00 20 00` | 模板原样（含 overflow_en 位） | `sqe->overflow_en = 1U` :130 |
| 52-59 | overflow_addr | | aicpu_hccl_sqcqv2.cc:130-134 |

**Reduce SQE（TODO-4）同函数证实**：`rtReduceOp == ACL_RT_MEMCPY_SDMA_AUTOMATIC_SUM` 时
opcode 取 `ReduceOpcode(runtimeDataType)`（aicpu_hccl_sqcqv2.cc:137-139）——hcomm 生产
在用 SDMA in-transit 归约，移植时照抄该分支即可。

### A.2 提交序列与流控

读 shadow tail（先 `dc civac` invalidate）→ 环满判断（used = tail-head）→ SQE 直写
slot → `dc cvac` 逐 cacheline 刷 → doorbell（GM 写 `sqRegBase+0x8` 或 `halSqCqConfig`）→
回写 shadow tail（刷 cache）。攒批模式：只推进 shadow tail，整批完成后一次 doorbell。

> hcomm 生产同款（源码证实）：`MemcpySqeArray` 批量直写后**仅末尾一次**
> `ConfigSqStatusByType(devId, sqId, DRV_SQCQ_PROP_SQ_TAIL, newTail)`（dispatcher_aicpu.cc:926-932）；
> `ConfigSqStatusByType` 内部即 `halSqCqConfig`（aicpu_hccl_sqcq.cc:96-117，
> `configInfo.cqId = 0; type = DRV_NORMAL_TYPE`）——**攒批 doorbell 是 hcomm 生产用法**，
> 非本计划发明。

### A.3 CP 流与 kernel 加载（host 侧）

`aclrtCreateStreamWithConfig(&cpStream, 0, ACL_STREAM_DEVICE_USE_ONLY)` →
`rtStreamGetSqid` → `aclrtBinaryLoadFromFile(json, cpuKernelMode=0)` →
`aclrtBinaryGetFunction(binHandle, "HybmAicpuSqeRun", &funcHandle)` →
`aclrtLaunchKernelWithHostArgs(funcHandle, ..., stream, &param, sizeof(param))`。
**CP1 内 HAL devId = die 号**（host 设备号 %2：0/2/4/6→0，1/3/5/7→1）。

### A.4 部署链运维规则（X6/X7/X9 全量）

1. deployer 并发互斥作用域 = **卡**（X9 实测：同卡双 die 共享设备 OS 文件系统，跨卡独立）：
   同卡同一时刻只跑一个 deployer（杀尽旧的→单起→验证门）；跨卡可并行（保守起见可仍全局串行）；
   **部署粒度 = 每卡一次**（同卡对端 die 的 CP1 共享 `/0/` 目录，各自 dlopen 同一文件）；
2. deployer json 必须 `CUSTAICPUKernel`（mode=1 才落真 ELF；`AICPUKernel`+mode=1 落假文件）；
3. launch json 必须 `AICPUKernel` + cpuMode=0（走 `/usr/lib64/aicpu_kernels/0/aicpu_kernels_device/` 搜索路径）；
4. 复制时改名（文件名带 PID 后缀，glob 多版本拼接 = 损坏 ELF）；
5. 正式验证前跑验证门（grep 符号）；deployer 保活时长按实验窗口设；
6. KFC：单条短命令；多段串联（>3 段 / >2000 字符）拆单条；精简 rootfs 无 `ls`/`od`/`base64`；
   **sync 语义勘正（X9 实测）：sync= 命令退出码**（0→sync=0，非零→507018）——确定性失败的命令
   重试不会过，重试纪律只用于真偶发失败；判"文件不存在"用 `test -f`（非零→507018）即可判定；
7. NOTIFY 通道在 310P3 已证伪（notify 不触发 E3 + 流内 wait 挂死）：生产完成检测用
   flag-SQE 哨兵轮询（main 定案）；若实验 NOTIFY，勿在流内 enqueue wait 任务；
8. result/ovf buffer 必须 `ACL_MEM_MALLOC_HUGE_FIRST`（NORMAL_ONLY 落特殊段 SDMA 不可翻译）。
