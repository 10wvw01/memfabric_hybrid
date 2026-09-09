# 310P AICPU SQE 编排模式开发计划（V1，xcclpp aicpu 模式移植）

> **状态：待启动（计划评审中，未开始实施）**
> - 本计划将 MemFabric 310P 数据面从 host 编排（`HostDataOpAclMemcpy`）扩展为可选的
>   **AICPU 编排**（AICPU kernel 内直投 STARS SDMA SQE），复刻 xcclpp `aicpu-310p3`
>   已验证的 v43 配方
> - 已定决策：**开关切换接入**（默认 host 编排，env 开关 opt-in + 探测回退）、
>   **deployer+KFC 部署链先行**（OPP .run 产品化后置）、**一期范围 = 卡内双 die G2G + batch**
> - 上游依据：xcclpp `examples/tutorials/aicpu-310p3/README.md`（机制知识档案，v43 双拓扑 PASS）、
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
| X1 | **v43 SQE 配方全链 PASS**：type=0x09（MEMCPY_ASYNC_SQE_V2）+ kernel_credit=0xff + w4=`00de0e00` + SMMU streamid 按 die（die0=0x7F45，die1=0xBF45）+ ssid 对称=实时查询值 + off8-11 全 0 | `aicpu_kernel/xcclpp_aicpu_sqe.cpp` `buildStarsSdmaSqe`；旧值 type=0x01 是 950 代值，310P3 固件不识别 → 静默丢弃假成功 |
| X2 | **提交链**：SQE 直写 sqBase（`dc cvac` 逐 cacheline 刷）→ `halSqCqConfig(SQ_TAIL)` doorbell（CP1 内 HAL devId=**die 号**，host 设备号 %2） | `submitSqe`；v41 实测 tail 落寄存器、v43 后 head 前进（TSFW 消费） |
| X3 | **CP 流配方**：`aclrtCreateStreamWithConfig(ACL_STREAM_DEVICE_USE_ONLY)` + `rtStreamGetSqid`（libruntime.so 内部符号）取 sqId | `aicpu_sqe_mpi.cc:146-156` |
| X4 | **远端内存 IPC Case A**：接收方 P2P policy 分配 + `GetExportKey(flag=0)` + `SetImportPid` + `ImportByKey`（flag 按 `aclrtGetDevicesTopo` 选 HCCS/PHB） | 双拓扑（卡内双 die / 跨卡 PCIe）4MB 全量比对 PASS |
| X5 | **完成检测**：NOTIFY_RECORD SQE（type=6）+ host `aclrtWaitAndResetNotify` 实测 PASS；等待后必须补 `SynchronizeStream`（notify 状态残留误触发） | completeMode=1 |
| X6 | **部署链（CP1）**：deployer（`CUSTAICPUKernel` json mode=1 落真 .so）→ KFC `cat` 复制到 `/usr/lib64/aicpu_kernels/0/aicpu_kernels_device/`（改名避免 PID 后缀多版本拼接）→ launch 用 `AICPUKernel` json（cpuMode=0） | 全链真机 PASS；CMS 验签墙只拦 common-sink 直推通道 |
| X7 | **部署链已知坑**：并发 deployer 互斥（多设备须串行）；隔夜文件被清（跑前验证门 `grep -q <symbol>`）；KFC 非特权身份、单条短命令稳定（>3 段串联易 507018）；ssid 不可硬编码（随启动变化，kernel 内 `drvMemSmmuQuery` 活查询） | README 部署链档案 |
| X8 | **flag-SQE 兼容模式**：8B SDMA 哨兵搬运 + `dc civac` 轮询（zbal 同款），25s 上限（< AICPU watchdog 30s） | completeMode=0 |

### 1.3 MemFabric 侧可复用资产（重大利好）

| 资产 | 位置 | 复用方式 |
|------|------|---------|
| AICPU kernel 构建体系 | `src/hybm/ops/hybm_kernel/`（hcc 交叉编译 + json + tar） | 追加 `hybm_aicpu_sqe.cc` 源文件即入链 |
| wire 契约先例 | `hybm_batch_transfer.h`（POD 参数 + static_assert） | 照抄模式写 `hybm_aicpu_sqe_contract.h` |
| DataOperator 插槽 | `hybm_data_op_factory.cpp:22` 已有 310P 分支 | 开关路由处扩展 |
| ACL 符号层 | `src/hybm/csrc/under_api/dl_acl_api.{h,cpp}` | 追加新符号弱加载 |
| 310P 池化链路 | `HybmAclVmmSegment`（07 已验收） | SQE 的 src/dst 直接用池内 GVA（待 E0b 验证） |

### 1.4 预期收益与预期管理

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
| D3 | **部署形态：deployer+KFC 先行** | POC 期复刻 xcclpp 部署链（X6）；OPP `.run`（复用 `script/kernel/build_ops_run.sh`）后置 backlog | 最快见效；OPP 在 310P3 的分发兼容性未验证过，不阻塞主线 |
| D4 | **每笔 copy 一次 launch，不做常驻** | batch N 段一次 launch + kernel 内攒批 doorbell | CP1 watchdog 30s 杀常驻进程；hcomm/zbal 均按需 launch |
| D5 | **方向分层** | 一期 G2G 三方向（GD2GD 含跨 die）走 SQE；H2G/G2H 委托 `aclrtMemcpy` 混合路径（日志明确标注） | 见非目标；混合路径语义与现有算子一致 |
| D6 | **完成检测** | NOTIFY_RECORD + `aclrtWaitAndResetNotify`（后补 `SynchronizeStream`，X5）；flag-SQE 轮询作降级模式保留 | 对齐 xcclpp 已验证配方 |
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

**E0c 完成检测 + 双向（~0.5 天）**：NOTIFY_RECORD + `WaitAndResetNotify`（补 SynchronizeStream）；
双方互为发起方各跑一轮；flag 模式作对照。

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
- `Initialize`：读开关 → 建 CP 流 → sqId/streamId → kernel 加载 → 冒烟探测（D2）；
- `DataCopy`(D2D)：param 写 GM → launch → notify 等待；
- `BatchDataCopy`：N 段一次 launch + 攒批 doorbell（**核心收益点**）；
- H2G/G2H：委托 `aclrtMemcpy`（D5）；
- 错误自恢复：stream 重建照抄 AclMemcpy 的 `SyncStream` 恢复模式。

**WP4 路由与开关**：工厂 310P 分支（D1）+ 回退日志。

**P1 验收**：单 rank 本地 SQE 冒烟（写读 own HBM）；开关关闭时行为与现版本完全一致（回归）。

### P2 语义完整 + 07 验收（~1 周）

- 双 rank 双向并发（每 die CP1 独立部署，07 双进程模型；deployer 串行部署规则见 X7）；
- `DataCopyAsync` 真异步 + `Wait` 兑现（D6）；
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
  优于 notify 只能判"搬完了"）

## 附录 A：xcclpp 移植配方速查（实施时直接对照）

### A.1 SQE 64B 布局（v43 破壁版，逐字段出处）

| 偏移 | 值 | 说明 |
|------|-----|------|
| 0 | `0x09` | MEMCPY_ASYNC_SQE_V2 真实 type（**0x01 是 950 代错误值**） |
| 4-5 | streamId | CP 流 rt_stream_id |
| 6-7 | taskId | |
| 8-11 | 全 0 | 0903 fresh dump 勘正（残留数据曾误导为 f1/f0） |
| 12-15 | `res3:16 \| credit=0xff:8 \| res4:8` | kernel_credit=0xff |
| 16-19 | `00 de 0e 00` | opcode=0 NOT_ATOMIC + 位域（ie_dma/sssv/dssv/sns/dns/qos/sro/dro） |
| 20-21 / 24-25 | SMMU streamid | die0=0x7F45，die1=0xBF45（hcomm `aicpu_hccl_sqcqv2.cc:101-106`） |
| 22-23 / 26-27 | ssid | **对称**=实时查询值（`drvMemSmmuQuery`，不可硬编码） |
| 28-31 | length | |
| 32-39 / 40-47 | src / dst（32+32 拆分） | |
| 48-51 | `00 00 20 00` | 模板原样 |
| 52-59 | overflow_addr | |

### A.2 提交序列与流控

读 shadow tail（先 `dc civac` invalidate）→ 环满判断（used = tail-head）→ SQE 直写
slot → `dc cvac` 逐 cacheline 刷 → doorbell（GM 写 `sqRegBase+0x8` 或 `halSqCqConfig`）→
回写 shadow tail（刷 cache）。攒批模式：只推进 shadow tail，整批完成后一次 doorbell。

### A.3 CP 流与 kernel 加载（host 侧）

`aclrtCreateStreamWithConfig(&cpStream, 0, ACL_STREAM_DEVICE_USE_ONLY)` →
`rtStreamGetSqid` → `aclrtBinaryLoadFromFile(json, cpuKernelMode=0)` →
`aclrtBinaryGetFunction(binHandle, "HybmAicpuSqeRun", &funcHandle)` →
`aclrtLaunchKernelWithHostArgs(funcHandle, ..., stream, &param, sizeof(param))`。
**CP1 内 HAL devId = die 号**（host 设备号 %2：0/2/4/6→0，1/3/5/7→1）。

### A.4 部署链运维规则（X6/X7 全量）

1. 同一时刻只跑一个 deployer（多设备串行：杀尽旧的→单起→验证门→下一台）；
2. deployer json 必须 `CUSTAICPUKernel`（mode=1 才落真 ELF；`AICPUKernel`+mode=1 落假文件）；
3. launch json 必须 `AICPUKernel` + cpuMode=0（走 `/usr/lib64/aicpu_kernels/0/aicpu_kernels_device/` 搜索路径）；
4. 复制时改名（文件名带 PID 后缀，glob 多版本拼接 = 损坏 ELF）；
5. 正式验证前跑验证门（grep 符号）；deployer 保活时长按实验窗口设；
6. KFC：单条短命令；多段串联（>3 段 / >2000 字符）拆单条 + 重试；精简 rootfs 无 `ls`/`od`/`base64`；
7. `aclrtWaitAndResetNotify` 后必须补 `SynchronizeStream`（状态残留误触发）；
8. result/ovf buffer 必须 `ACL_MEM_MALLOC_HUGE_FIRST`（NORMAL_ONLY 落特殊段 SDMA 不可翻译）。
