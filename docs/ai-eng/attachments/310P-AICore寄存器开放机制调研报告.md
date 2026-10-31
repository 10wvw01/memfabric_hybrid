# 310P AICore 寄存器开放机制调研报告（含 memfabric 六通道寄存器化判定与驱动改动需求）

> **产出方式**：我与 AI（opencode + GLM）协作产出——提示词设计、方向决策与结论把关由我完成，调研执行与验证由 AI 完成，2026-10-31。过程记录见[协作实录](../AI协作实录-寄存器机制调研.md)。
> **调研环境**：310P3 ×8（`npu-smi info` 实测），驱动 26.1.0 / Innerversion V100R001C10SPC008B221（`/usr/local/Ascend/driver/version.info` 实测），CANN 9.1.0。
> **调研对象**：`~/pypto`（framework 副本为主，experiments/runtime 为孪生副本）、`~/driver`（wgm-310p 分支）、`~/memfabric_hybrid`。
> **证据标注**：【实测】= 本机一手验证；【源码】= 源码逐行读到；【推断】= 基于证据推断，未验证。

---

## 一、pypto 仓 AICore 寄存器开放机制（三大用途）

### 1. halMemCtl 映射寄存器区域（addr_type=2 同步寄存器 / addr_type=3 PMU）

**调用链（源码）**：

1. Host 侧 `DeviceRunner::GetAicoreRegs` 按架构取寄存器表：DAV_2002（310P）分别用 `ADDR_MAP_TYPE_REG_AIC_CTRL=2` 和 `ADDR_MAP_TYPE_REG_AIC_PMU_CTRL=3` 各调一次 — `pypto/framework/src/machine/runtime/runner/device_runner.cpp:244-295`（255、269 行）；常量定义 `runtime_agent.h:24-27`。
2. `RuntimeAgent::GetAicoreRegInfoForDAV2002` 组 `AddrMapInPara{addr_type, devid}` 调 `HalMemCtl(0, in, sizeof, &out, nullptr)`，返回 `AddrMapOutPara{ptr, len}` 基址；然后按 **每核 2MB stride（0x200000）、共 10 核（8 核开放给用户）、驱动映射 32MB** 展开成 per-core VA 数组 — `runtime_agent.cpp:164-194`（167-168 行注释原文："A1有10个AICore，但开放给用户的是其中的8个""2MB，驱动总共映射32MB空间给用户"）。
3. `HalMemCtl` 经 AdapterManager dlsym 拿到驱动导出符号 `halMemCtl`（符号名映射 `hal_adapter_types.h:31`；分发 `adapter/api/hal_api.cpp:26-37`）。
4. 寄存器表 VA 拷入设备 GM（`coreRegAddr`/`corePmuRegAddr`，`device_runner.cpp:187-200`；字段定义 `interface/machine/device/tilefwk/aicpu_common.h:151-153`），**AICPU 调度器直接解引用这些 VA 做 MMIO 读写**（跨进程有效性依赖 SVM 共享 VA 窗口 —【源码推断】）。
5. **失败回退**：halMemCtl 返回非 0 → `commByReg=false`，任务派发/完成改走 GM shakeBuffer（`shakeBufferCpuToCore[1]`/`shakeBuffer[4]` + dcci/cvac），AICore 启动时读 `KernelArgs.commByReg` 选路 — `device_runner.cpp:253-265`、`aicore_hal.h:94-98`、`aicpu_common.h:46-51`。

**驱动侧语义（源码）**：`addr_type` 枚举在 `driver/pkg_inc/ascend_hal_define.h:977-984`：**2 = "Used to map aicore synchronous register"，3 = "Used to map aicore pmu synchronous register"**（另有 0=L2BUFF、1=C2C_CTRL、4=CNT_NOTIFY）。调用走 `CTRL_TYPE_ADDR_MAP=0`（:932-956）。

### 2. AICPU↔AICore 任务派发与完成上报（0xA0 ready / 0x4C8 finish / set_cond / MOV）

寄存器偏移常量：`aicore_hal.h:51-54`（DAV_2002：`REG_SPR_DATA_MAIN_BASE=0xA0`、`REG_SPR_COND=0x4C8`；DAV_2201 同值 :56-59；A5/3510 为 0xD0/0x5108 :61-64）。AIC CTRL 映射区即 per-core SPR 的 MMIO 窗口【源码推断】。

**派发（AICPU 写 → AICore 读）**：
- AICPU `SendTaskToAiCore` → `SetReadyQueue(coreIdx, encodeTaskId+1)` → `*readyRegQueues_[phyId] = value`，即 MMIO 写 `regAddrs_[phyId] + 0xA0`（64 位）— `aicore_hal.h:233-246`（237 行）、映射 `MapRegistersForAllCores` :419-456（429-431 行）；调度侧调用点 `aicore_manager.h:1554`（`SendTaskToAiCore` :1528-1590）、STOP 派发 :646/:3067-3079。
- 编码：低 32 位 = taskId+1（1 基，0 保留），高 32 位 = devtask 元数据（`REG_HIGH_DTASKID_SHIFT=32`，`aicpu_common.h:197-199`；AICore 侧 `RefreshParallelDevTask` 消费，`aicore_entry.h:622-693`）。
- AICore 读：`GetDataMainBase()` 用内联汇编 **`MOV %0, DATA_MAIN_BASE`** 读 SPR — `aicore_entry.h:159-165`（163 行）；主循环 `GetNextLeafTask` 轮询 :207-244，`GetRegHighValue` 等高位 :246-257。

**完成上报（AICore 写 → AICPU 读）**：
- AICore 执行前 `SendRegAck`：**`set_cond(taskIdx)`**；执行完 `SendRegFinish`：**`set_cond(taskIdx | AICORE_FIN_MASK)`**（0x80000000）— `aicore_entry.h:303-313`，KernelEntry 中调用 :808/:818。`set_cond` 是 CCE 编译器内建 intrinsic（仓内只有 host 仿真定义：`simulation/host_aicore_entry_adapter.h:66-70`；本机 CANN 头文件中无定义，实现在 ccec 编译器内 —【源码+实测搜索】）。
- AICPU 轮询 `GetFinishedTask` → `*finishRegQueues_[phyId]`，即 MMIO 读 `+0x4C8` — `aicore_hal.h:352-370`（356 行）；批量 8 路展开 `GetFinishQueue` :301-350。解码：bit31=状态（1=FIN、0=ACK，`aicore_manager.h:54-57`），低 31 位=taskId；A1 双缓冲槽位状态机 `ReleaseCoreByRegValForA1`（:1937-1999，注释明确"单槽覆写完成通道，pending 完成蕴含 running 已完成"）。

**握手**：AICore 上电后 `HandshakeClient` 先 `set_cond(AICORE_TASK_INIT)`，再把 `phyId<<32 | AICORE_SAY_HELLO` 写 GM shakeBuffer[0] 并 dcci 刷出；AICPU `TryHandShakeByGm` 校验 phyId 越界后建 blockId→phyId 映射（`aicore_entry.h:270-281`、`aicore_hal.h:551-593`）。此外 0x18 偏移有 fast-path 开关寄存器（`WriteReg32(REG_SPR_FAST_PATH_ENABLE)`，`aicore_hal.h:583-585`）。

### 3. PMU 性能计数器采集

- **区域**：addr_type=3 映射的 per-core PMU 寄存器页。DAV_2002 偏移：`PMU_CTRL_0=0x200`、`PMU_CNT0..7=0x210..0x248`（8B stride）、`PMU_CNT_TOTAL0/1=0x250/0x254`（64 位总周期）、`PMU_CNTx_IDX=0x260..0x298`（事件选择）、`START/STOP_CNT_CYC=0x2A0-0x2AC` — `aicore_prof_dav2002_pmu.h:27-49`。
- **编程与读取全在 AICPU 侧**（MMIO）：`ProfStartPmu` 对每核页对齐 mapBase → `InitPmuRegAddrsForCore` 算好各计数器地址 → `ProgramPmuStartForCore`：先读一遍清零 → `SetPmuEvents` 把 8 个事件号写 `CNTx_IDX` → 设 start/stop 周期窗口、写 ctrl0 使能（ctrl0Val=GLB+(USER<<1)+(SAMPLE<<2)=7，`aicore_prof.h:186`）— `aicore_prof.cpp:326-339、341-374、376-400、402-412`。
- **按任务采样**：AICPU 在任务边界 `FillPmuData` 直接 MMIO 读 8 个计数器 + 64 位 totalCyc，经 `MsprofReportAdditionalInfo` 上报 msprof — `aicore_prof.cpp:435-456、470-516`。事件表（VEC_BUSY/MTE2/L2 命中等）`aicore_prof.h:205-235`。
- AICore 侧也有 PMU 使能开关 intrinsic（`set_ctrl/get_ctrl`，`aicore_entry.h:445-464`），但 `PERF_PMU_TEST_SWITCH=0` 默认关闭（:28）— AICore 只贡献计数，不读数。

---

## 二、特制驱动 25.3.rc1.b099 与普通驱动的行为差异

**【实测】本机"普通"驱动（26.1.0，SPC008B221）上一手探测结果**（探针模式：`aclrtSetDevice(N)` → dlopen `libascend_hal.so` → 调 halMemCtl）：

| 探测项 | 结果 | 结论 |
|---|---|---|
| 未 open 设备直接调 | ret=4（`DRV_ERROR_INVALID_HANDLE`，`ascend_hal_error.h:38`） | 与源码前置条件吻合（`devmm_map_dev_reserve.c:216-219`）|
| open 设备后 addr_type=0/1/2/3/4 | **全部 ret=65534（`DRV_ERROR_NOT_SUPPORT`）**，ptr=0 | 整个 MAP_DEV_RESERVE 通路不可用 |
| `/proc/svm/dev0..7/feature/aic_reg_map` | =1 | 内核能力开关是开的（非能力拦截）|
| `halGetDeviceSplitMode(0)` | ret=0, mode=0 | 非 vNPU 切分（非 split 拦截）|
| `nm -D libascend_hal.so` | 导出 `halMemCtl(W)`、`devmm_ctrl_map_addr(T)` | 本机装的是 **v2（ascend910B）链** |
| `dmesg` | **91 条** `devmm_send_map_addr_msg: Map address failed. (ret=-95; addr_type=…)`，函数名与 `svm_master_addr_map.c` 吻合 | 失败点在**设备侧链路返回 -EOPNOTSUPP**（内核能力检查在 :60-65 会直接返回而不进 send）→ **设备侧 AICPU agent 固件不实现 MAP_DEV_RESERVE** |
| dmesg 进程名 | 大量为 python（pypto 运行） | **本机 pypto 一直走 GM fallback（commByReg=false）** |

**【源码】普通 vs 特制的结构性差异**（`~/driver` 仓，wgm-310p 分支）：

1. **v2/v3 双链**：halMemCtl 的 ADDR_MAP 只在 v2（ascend910B 产品线，310P 属此线）有实现（9 项 handler 表，`src/ascend_hal/svm/v2/devmm/devmm_svm.c:3182-3210`）；v3（ascend950）表里只有 3 项，ADDR_MAP handler 为 NULL → 必返 65534（`v3/api/master/svm_mem_ctl.c:144-165`）。编译期二选一：`src/ascend_hal/svm/CMakeLists.txt:11-14`。
2. **特制（custom）打包**：custom 驱动版本命名为 `主版本[.rcN].b构建号`（ascend910B custom 线现为 `26.1.0.b061`，上游原版为 `8.5.T7.0.B053`；见 `scripts/package/custom/driver/ascend910B/scripts/sys_version/sys_version.conf`）。`version_cap.map:1-8` 显示 25.3.rc1 对应硬件基线 240。
3. **wgm-310p 分支相对 master 只多一个提交** `7b4467a4`（2026-10-15）：补齐 stars_v2 的 remote SQ 门铃映射（详见第四节）。

**【推断，验证缺口】`25.3.rc1.b099` 字面量在全部本地仓（pypto/memfabric/driver/docs/OpenSpec/hccl/hcomm/shmem/xcclpp/runtime/tsch/megakernel-mega/cli/skills/workflows + wgm_verify.tar.gz）搜索 0 命中**。按 custom 命名法推断它是 25.3.rc1 代码线的第 99 次 custom 构建。结合 pypto 的 fallback 注释（"halMemCtl unsupported on this CANN/driver version"）和本机实测，**特制驱动与普通驱动的关键行为差异应为**：(a) 设备侧 SVM agent 固件实现 `DEVMM_CHAN_MAP_DEV_RESERVE`（msg id 13，`svm_kernel_msg.h:37、454-459`），使 addr_type=2/3 映射成功（返回 ptr/len=32MB）→ pypto 寄存器模式可用；(b) 携带 stars_v2 `sqcq_reg_map` 补丁 → remote SQ 门铃可查；(c) 版本串换成 custom 命名。其中 (a) 无法在本机核实，需在装有 b099 的机器上跑探针验证（方法见 4.C）。

---

## 三、memfabric 六条通道寄存器化可行性（判定标准：写者是谁）

现状载体：六条通道全部在 GM 段尾保留区（48KB 布局图 `src/smem/include/device/smem_shm_sdma_layout.h:26-35、47-65`），AICore/AICPU 双侧软件维护缓存（dcci/cvac）轮询，无中断、notify 已弃用（`docs/310p/机制档案.md:23`）。

| # | 通道 | 写者→读者 | 判定 | 结论 |
|---|---|---|---|---|
| a | 请求邮件通知 | **本端 AICore** → 本端 AICPU | 核内写 | **可行**（通知走寄存器，载荷留 GM）|
| b | 背压/ready | **本端 AICPU** → 本端 AICore | 核内写 | **可行**（与 pypto 0xA0 方向 1:1）|
| c | SQE doorbell 提交 | **本端 AICPU** → TSFW 固件 | 核内写 | **可行**（走 SQ 自己的门铃寄存器页，非 0xA0）|
| d | 到达/完成戳 | **SDMA 引擎**（戳 SQE ②③）→ 对端/本端 AICore | SDMA 写 | **不可行**（物理够不到核内寄存器）|
| e | AICore 计时 | **host CPU**（心跳）→ AICPU kernel | host 写 | **不可行**（非本地核写；且 310P AICore 无 cycle 计数器）|
| f | 大块数据 cache clean | AICore 产出、**SDMA 消费** | 数据面 | **不可行/无收益**（数据本体在 GM，寄存器装不下）|

### (a) 请求邮件通知 — 可行
- **现状**：AICore `smem_shm_sdma_signal_at` 写 32B 槽（src/dst/len/imm）+ `DataCacheCleanAndInvalid` 刷出 + 推进 REQ_TAIL（`smem_shm_aicore_base_sdma.h:151-185`，182-183 行）；AICPU epoch kernel `loadCell(REQ_TAIL)`（civac）轮询 + 读邮件（`hybm_sdma_orch_kernel.cc:482、399-405`）。
- **方案**：镜像 pypto COND 方向——AICore `set_cond(seq)`，AICPU 轮询该核 `+0x4C8`；32B 邮件体留 GM 环，寄存器只传序号（pypto 正是"任务 ID 走 0xA0/COND、任务体走 GM"的同构设计）。
- **收益**：AICore 每封邮件省一次 dcci + tail 单元写刷；AICPU 省每次轮询的 civac（MMIO 读无缓存一致性问题）；**消灭共行互踩 bug 类**（v23 布局纪律整类问题对该通道不复存在，见 507014 根因 1，`smem_shm_sdma_layout.h:21-25`）。
- **风险**：① COND 单 64 位槽/核 → 64 槽环语义需重构为 seq+ACK（pypto 用 running/pending 双缓冲 + ACK/FIN 状态机解决，`aicore_manager.h:1937-1999`，可直接抄）；② 请求环是 rank 内多 AICore 共享、AICPU 单点消费，寄存器是 per-core → AICPU 每轮要轮询 N 个核的 0x4C8（pypto `GetFinishQueue` 已有 8 路展开范式，`aicore_hal.h:301-350`）；③ 依赖特制驱动（本机实测普通驱动映射失败）；④ VA 需在 CP1 进程可解引用（pypto 模式：host 调 halMemCtl，VA 经 DeviceArgs 下传 —【源码推断】）。

### (b) 背压/ready — 可行
- **现状**：AICPU 消费后 `storeCell(REQ_HEAD, seq+1)`（cvac，`orch_kernel.cc:430`）；AICore signal 时轮询 REQ_HEAD（每次 `SdmaReadCell` 都 dcci，`sdma.h:155-167`）。
- **方案**：与 pypto 派发方向完全同构——AICPU MMIO 写消费游标到核的 `+0xA0`，AICore `MOV DATA_MAIN_BASE` 直读 SPR（`aicore_entry.h:159-165`），零缓存维护。
- **收益**：背压自旋环里每次迭代都是 dcci（310P AICore DCache 双向陷阱下最贵的操作，`机制档案.md` 定论 2）；换 SPR 读后轮询开销和 GM 流量双降。
- **风险**：同 (a) 的 ①③④；另外 REQ_HEAD 是整 rank 一份、寄存器 per-core——要么 AICPU 广播写各信号核的 0xA0，要么改 per-core 消费游标（协议改动）。

### (c) SQE doorbell 提交 — 可行（价值最高、最直接）
- **现状**：AICPU `submitSqe` 写 SQE 入环 + cvac（`orch_kernel.cc:193-229`），批末 `flushBatchDoorbell`（kDoorbellBatch=8）经 **HAL 弱符号 `halSqCqConfig`（ioctl 路径）敲铃**（:121-134、384-395）。
- **方案**：**不走 0xA0/0x4C8（那是 AICore 同步寄存器，TSFW 不监听）**，走 SQ 自己的门铃寄存器页：`halSqCqQuery(DRV_SQCQ_PROP_SQ_REG_BASE)`（prop=5）拿 `sqRegBase`，直写 `*(u32*)(sqRegBase+0x8)=newTail`。历史/旁证：能力矩阵 kernel 已实现该双路径（`310p-capability_matrix/17_aicore_aicpu_overlap/aicpu_kernel/hybm_overlap_kernel.cc:38-39、325-343、423-433`），A3/zbal 在用（`app/zbal/.../zbal_kernel_sdma_data_op.h:292/392/452`）。
- **收益**：每批一次 ioctl/通道消息 → 一次 MMIO 写；doorbell 延迟从 syscall 级降到访存级；这是 AICPU epoch kernel 主循环里最大的一笔固定开销。
- **风险**：① 现状查不到 sqRegBase（65534）的根因是驱动 stars_v2 适配层 `.sqcq_reg_map=NULL`（见第四节，需特制驱动/补丁）；② TRS 有 `SQ_SEND_HIGH_SECURITY/HIGH_PERFORMANCE` 模式门（`trs_host_mode_config.c:36-44`），直写可能被安全模式约束；③ 设备侧固件必须应答 `TRS_MSG_SQ_REG_MAP`（补丁设计为失败即回退原行为，安全）；④ 占位 SQE、满环活查询等既有纪律不变。

### (d) 到达/完成戳 — 不可行
- **现状**：戳 SQE ②（对端 ARR_STAMP）③（本端 QUIET）由 **SDMA 引擎**执行 8B 单号搬运落地（`orch_kernel.cc:49-53、370-380`；单号源为本端 STAGE 暂存，`smem_shm_sdma_layout.h:59-64`）；对端 AICore `wait_at` / 本端 `quiet_at` dcci 轮询（`sdma.h:188-244`）。
- **判定**：SQE 的 dst 只能是经 SMMU 翻译的 GM/一致性域地址，**SDMA 物理上无法写核内同步寄存器区**；且到达戳读者是对端 die 的 AICore，跨 die 触达对端核内寄存器的通路不存在。pypto 的 0x4C8 之所以成立，是因为写者是 AICore 自己（set_cond）——方向恰好相反。**保持 GM 戳 + FIFO 保序设计不变**。
- 附带：到达邮件 ARR_MAIL（32B，写者=本端 AICPU 跨 die 直写对端 GM，`orch_kernel.cc:410-417`）写者虽是 AICPU，但读者是对端 AICore、载荷超 64 位、且需要跨 die 寄存器寻址通路（未验证存在）——**不建议寄存器化**。

### (e) AICore 计时 — 不可行
- **现状**：310P AICore 无 cycle 计数器（`GetSystemCycle` 返回 0 且拖慢 8 倍，`机制档案.md:25` 定论 7；pypto 侧同证：`__DAV_V200` 下 `GET_SYS_CNT()` 恒 0，`aicore_entry.h:63-64`）；type-5/KFC 环境下 AICPU `cntvct_el0` 也失效 → 现行 host 监督线程 100ms 心跳报时（`hybm_sdma_contract.h:104-112`、`orchestrator.cpp:675-701`、`orch_kernel.cc:289-295`）。
- **判定**：写者是 host CPU，不满足"本地 AICPU/AICore 写"；核内同步寄存器机制里没有时钟源寄存器。唯一寄存器邻接选项是 PMU 总周期计数器（0x250/0x254，addr_type=3 区），但它由 AICPU 侧 MMIO 读写（`aicore_prof.cpp:441-442`），AICore 无 SPR 读通路证据，且"PMU 使能后周期"作壁钟的暂停/溢出语义未验证【推断】。**保持 host 心跳 + 迭代数近似**。

### (f) 大块数据 cache clean — 不可行/无收益
- **现状**：AICore 产出大块数据后必须 `ENTIRE_DATA_CACHE` clean（或逐 64B 行 `SINGLE_CACHE_LINE`，推荐姿势 `examples/memory_pool/01_basic/08_310p_aicore_aicpu_sdma/08_310p_aicore_aicpu_sdma.asc:55-64`；zbal 全缓存 clean `zbal_kernel_sdma_data_op.h:288-289/388-389/448-449`），否则 SDMA 读到陈旧数据（`机制档案.md:21` 定论 3）。
- **判定**：这不是信令通道而是数据可见性维护——数据本体在池 GVA、SDMA 经 SMMU 消费；寄存器装不下大块数据，用 MMIO 写数据绕 DCache 每次只 4/8B，带宽上不可行。**保持 cache clean**（若 (a) 寄存器化，邮件体的 clean 义务同步消失，但数据载荷的 clean 保留）。

---

## 四、给驱动团队的改动需求

### A. halMemCtl 的 v2/v3 双链机制

- 用户态唯一符号 `halMemCtl`（DLLEXPORT 弱符号，`pkg_inc/ascend_hal_base.h:3221-3222`；上层 dlsym 按名解析）。
- **v2 链（ascend910B 产品线，310P 属此线）**：`src/ascend_hal/svm/v2/devmm/devmm_svm.c:3195-3210` 按 `type` 查 9 项 handler 表 → `CTRL_TYPE_ADDR_MAP(0)` → `devmm_ctrl_map_addr`（`devmm_map_dev_reserve.c:197-230`：split-mode 拦截 AIC CTRL/PMU :209-214；结果按 (devid,addr_type) 进程内缓存永不释放 :111-116）→ ioctl `DEVMM_SVM_MAP_DEV_RESERVE`（`svm_ioctl.h:705`）→ 内核分发表 `devmm_proc_info.c:2847-2848` → `svm_master_addr_map.c:49-74`（aic_reg_map 能力检查 :60-65，默认 true `svm_master_dev_capability.c:186`，procfs 可关 `svm_master_feature_proc_fs.c:47-76`）→ **同步邮箱 `DEVMM_CHAN_MAP_DEV_RESERVE_H2D_ID=13` 发设备侧 agent**（`svm_master_addr_map.c:20-47`、`svm_kernel_msg.h:37、454-459`）→ 设备侧完成映射并回填 va/len。
- **v3 链（ascend950）**：`v3/api/master/svm_mem_ctl.c:144-165` 表内仅 3 项，`CTRL_TYPE_ADDR_MAP` handler 为 NULL → 直接返回 `DRV_ERROR_NOT_SUPPORT`；v3 全树无任何 ADDR_MAP/MAP_DEV_RESERVE 代码。
- 编译分叉：`src/ascend_hal/svm/CMakeLists.txt:11-14`、`src/sdk_driver/svm/CMakeLists.txt:10-13`（产品级二选一，非运行期版本判断）。
- **给驱动团队的需求**：310P 特制线必须 ① 走 v2 链编译；② **设备侧 SVM agent 固件实现 msg 13（MAP_DEV_RESERVE）对 addr_type=2/3 的处理**——本机普通 26.1.0 驱动实测 ret=-95 出在 `devmm_send_map_addr_msg`（dmesg 91 条），即设备侧不支持；这是普通驱动与特制驱动的核心行为差异。

### B. CP 流 remote SQ 门铃注册为什么查不到 sqRegBase

**查询链**：`halSqCqQuery`（声明 `pkg_inc/ascend_hal_base.h:4606`，实现 `src/ascend_hal/trs/core/trs_interface.c:324-370`）→ 属性号 `DRV_SQCQ_PROP_SQ_REG_BASE`（`pkg_inc/hal_pkg/trs_pkg.h:74-93`）→ remote SQ 走设备侧同步事件（`trs_master_event.c:457-490`）→ 内核 ioctl `TRS_SQCQ_QUERY`（`trs_ioctl.h:167`，fops `trs_fops.c:350-387`）→ `trs_sqcq_query` 的 `case DRV_SQCQ_PROP_SQ_REG_BASE`（`trs_hw_sqcq.c:1668-1670`）→ `trs_query_sq_reg_vaddr`（:1626-1640）→ `trs_get_sq_reg_vaddr`（:1591-1624）。

**根因（源码+git diff 实证）**：注册函数 `trs_set_sq_reg_vaddr` 写 `sq_ctx[sqid].reg_mem.uva`（:1566-1589），唯一的填值入口是适配层 ops 槽 `.sqcq_reg_map`（`trs_core.h:141-142`），在 SQ 分配末尾自动调用（pair 模式 `trs_hw_sqcq.c:602-608`、non-pair :961-965、释放反向 :767-770）。**上游 master 的 stars_v2 适配层把 `.sqcq_reg_map/.sqcq_reg_unmap` 置 NULL** → `trs_res_mng.c:1074-1084` 的 NULL 检查不通过 → `reg_mem.uva` 恒 0 → `trs_get_sq_reg_vaddr:1610-1613` 返回 `-EOPNOTSUPP` → 用户态看到 65534。memfabric 侧实测记录与之闭环：`hybm_overlap_kernel.cc:372` 注释"65534=NOT_SUPPORT"、:425-430 查询失败置 0 走 ioctl fallback；`hybm_entity_default.cpp:1303` "AclnnShmemSdmaStarsQuery 实测 65534"。

**需要补什么（文件/行）**——本仓 wgm-310p 分支已有现成补丁（commit `7b4467a4`，2026-10-15，仅改 1 个文件 +60/-2），需向 25.3.rc1.b099 线合入：
- `src/sdk_driver/trsdrv/trs/lba/near/comm/adapt/trs_host_core/stars_v2/trs_core_stars_v2_ops.c`：
  - **:117-139 新增 `trs_core_stars_v2_ops_sqcq_reg_map`**：构造 `TRS_MSG_SQ_REG_MAP` 同步邮箱（带 `remote_tgid`）→ 设备侧完成门铃页映射并回填 `sq_map.va/size` → `trs_set_sq_reg_vaddr` 写入内核 `sq_ctx[sqid].reg_mem.uva`；
  - **:141-163 新增 `trs_core_stars_v2_ops_sqcq_reg_unmap`**；
  - **:177-178 ops 表挂接** `.sqcq_reg_map/.sqcq_reg_unmap`（master 上为 NULL）；
  - 参考实现（v1 原版）：`stars_v1/trs_core_stars_v1_ops.c:73-94`（挂接 :136）。
- 配套（已在树上、无需改）：消息定义 `trs_h2d_msg.h:53-54、133-144`；查询出口 `trs_fops.c:380-383` 对 SQ_REG_BASE 额外 put_user value[1]/[2]。
- 注意：APM 资源映射回调只注册了 NOTIFY/CNT_NOTIFY 两类（`trs_host_res_map.c:61-86`），STARS_RTSQ 未走 APM——remote SQ 门铃就是走上面 TRS 邮箱链，别改错地方。

### C. 验证方法

1. **halMemCtl（可在任何目标机一键验证）**：编译运行探针（完整源码见[过程实录](../AI协作实录-寄存器机制调研.md) §4.3；模式：`aclrtSetDevice(N)` → dlopen `libascend_hal.so` → `halMemCtl(0, {addr_type=2/3, devid=N}, ...)`）：
   - 特制驱动预期：ret=0、ptr≠0、len=0x2000000（32MB/10 核×2MB，对照 `runtime_agent.cpp:167-168`）；
   - 普通驱动实测：ret=65534 + dmesg `Map address failed. (ret=-95...)`；
   - 辅助：`cat /proc/svm/devN/feature/aic_reg_map`（=1 才可能过内核检查）、`halGetDeviceSplitMode`（≠split）。
   - 端到端：跑 pypto 负载看日志 `Map AICore reg addr (type 2) 0x… for DAV_2002`（`runtime_agent.cpp:191`）且无 "falling back to GM-based communication"（`device_runner.cpp:258`）、`Dev args :cor … commByReg 1`（:201-202）。
2. **remote SQ 门铃**：CP1 常驻 kernel 内 `halSqCqQuery(prop=5)`（样板 `hybm_overlap_kernel.cc:423-433`）：未打补丁 65534；打补丁后 ret=0 且 value[0]/[1] 拼出非零 sqRegBase → 提交 SQE 后直写 `sqRegBase+0x8`，验证 SDMA 执行（数据落地+戳写入）；回归项：sqRegBase==0 时 `halSqCqConfig` 旧路径不受影响（双路径代码已有，`hybm_overlap_kernel.cc:326-338`）。设备侧固件若不应答 `TRS_MSG_SQ_REG_MAP`，发送报错、uva 保持 0，行为与未打补丁一致（补丁注释 `trs_core_stars_v2_ops.c:115` 自述）。
3. **0xA0/0x4C8 语义**（仅特制驱动机器）：最小 AICPU kernel MMIO 写核 N 的 `+0xA0`，配 AICore kernel `MOV DATA_MAIN_BASE` 读（样板 `aicore_entry.h:159-165`）；PMU 则读 `+0x250` 看活动核计数值递增（编程样板 `aicore_prof.cpp:376-412`）。

**验证缺口声明**：`25.3.rc1.b099` 未装本机且字面量在全部本地仓 0 命中，其行为差异中"addr_type=2/3 映射成功"一项为推断，需按上述方法 1 在装有该驱动的机器上实测闭环；本机所有 0xA0/0x4C8/PMU 行为结论均为源码推断（本机映射失败无法实测）。
