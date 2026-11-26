# 310P3 SQ 提交通知寄存器（doorbell）问题交接

- 日期：2026-11-26
- 平台：8×Ascend 310P3，驱动 25.6.rc1.b010，CANN 9.0.0
- 相关代码：`driver`（用户态 HAL 与内核 trs）、`kernel_space`（tsdrv）、`memfabric_hybrid`（SDMA 编排）
- 本文目的：把当前问题、实测结果、代码位置和待查问题一次讲清，交给后续分析

> 术语说明：代码里把 SQ 的提交通知寄存器叫 `doorbell` / `db`。本文统一称“提交通知寄存器”，涉及代码标识符时保留原文。

---

## 0. 一句话现状

310P3 上，CP1（设备侧 `aicpu_scheduler` 进程）里的 AICPU kernel 能分配 SQ、能构造 SQE、能通过 HAL 提交并让 SDMA 真正搬完数据（08 示例端到端跑通），但“AICPU 用户态直接 MMIO 写 SQ 提交通知寄存器”这条路没有打通：内核跳过了该寄存器的页映射（`IsSqDbMap=0`）；自己 mmap 到该物理页后，读 offset 0 会让 AICPU 核停在那条 load 指令上不返回。目前实际可用的是“内核代写提交通知寄存器”的通路。

---

## 1. 目标

memfabric 的 SDMA 编排 kernel 跑在 CP1 里，需要把构造好的 SQE 写进 CP 流的 SQ 环并写提交通知寄存器。希望由 AICPU 直接 MMIO 写，替代每次走 ioctl / HAL 调用的开销。

## 2. 平台事实（已核实）

| 项 | 结论 | 证据 |
|---|---|---|
| 310P3 芯片代号 | MINI_V2（CHIP_DC），不是 MINI_V3 | `driver/src/ascend_hal/inc/devdrv_hardware_version.h:39,76`；`driver/src/sdk_driver/pbl/chip_config/chip_info.c:112`（HISI_MINI_V2(2) → SOC_TYPE_DC） |
| trs hw_type | TRS_HW_TYPE_TSCPU | `driver/src/sdk_driver/trsdrv/trs/inc/trs_chip_def_comm.h:68-76`（MINI_V3/CLOUD_V2/V4/V5 才是 STARS） |
| 有无 STARS 硬件调度器 | 没有。910B（CLOUD_V2）、310B（MINI_V3）、950（CLOUD_V4）、960（CLOUD_V5）才有 | `comm/pcie/dc/host/mini_v2/` 下 grep STARS 0 命中；`cloud_v2/res_drv_cloud_v2.c:103-120`、`mini_v3/res_drv_mini_v3.c:81-86` 有成片 STARS 区 |
| 调度方式 | TSFW 软件调度固件跑在 TS CPU 核上 | trs 的 tscpu 适配层 `trs_tscpu_*`；`dms_node_type.c:71` 写作 “HWTS/Stars-TS” |
| 提交通知寄存器区 | TS_DOORBELL_REG，512 槽 × 4KB；SQ 槽 0..512，CQ 槽 512..864，在线邮箱槽 1006..1007 | `soc_adapt_res_mini_v2.c:24-33`；物理基址来自 `soc_resmng_get_reg_base(..., "TS_DOORBELL_REG")`（`trs_host_db.c:32`） |
| 该寄存器的写/读实现 | 写 `ka_mm_writel(tail, vaddr + stride*sqid)`；读 `ka_mm_readl` | `trs_ts_db.c:274-325` |
| SQ head/tail 独立寄存器页 | 没有，query PADDR 返回 0 | `trs_tscpu_chan_sqcq.c:86-89`（注释 “not has sq head tail addr”） |

## 3. 提交路径（310P3 实际路径）

1. Host：`AclrtCreateStreamWithConfig(…, HYBM_ACL_STREAM_DEVICE_USE_ONLY)`（`memfabric_hybrid/src/hybm/csrc/entity/hybm_sdma_orchestrator.cpp:290-305`），runtime 把 `RT_STREAM_CP_PROCESS_USE`(0x800) 映射成 `TSDRV_FLAG_REMOTE_ID`（`runtime/.../npu_driver_res.cc:1003`）。
2. Host 把 devId/sqId/streamId 放进 `HybmSdmaOrchParam` 下发给 AICPU kernel。
3. AICPU kernel（CP1）用 `halSqCqQuery` 查 SQ_BASE/DEPTH/HEAD/TAIL（`hybm_sdma_orch_kernel.cc:259-312`）。
4. `buildStarsSdmaSqe` 构造 64 字节 SQE（STARS V2 格式）：off0=0x09、off16 opcode、off20/24 SMMU streamid（0x7F45/0xBF45）、off22/26 substreamid（`drvMemSmmuQuery` 实时查）、off28 长度、off32/40 源/目的地址（`hybm_sdma_orch_kernel.cc:153-186`）。
5. `submitSqe` 把 SQE 写到 `sqBase + tail*64`（普通设备内存，不是寄存器），逐缓存行执行 `dc cvac`。
6. `flushBatchDoorbell` → `doorbellViaHal` → `halSqCqConfig(devId, {prop=SQ_TAIL, value=tail})`（`hybm_sdma_orch_kernel.cc:133-146, 483-491`）。
7. HAL 用户态 `trs_sq_cq_config`（`driver/src/ascend_hal/trs/core/trs_sqcq.c:1197-1238`）：
   - 先走 UIO：`trs_sq_cq_config_uio`，只有 `sq_ctrl.head/tail/db` 非空才 MMIO 写（`:1044-1060`），否则返回 NOT_SUPPORT；
   - 再走 ioctl `TRS_SQCQ_CONFIG`（`:1231`）。注意：ioctl 返回 NOT_SUPPORT 时，函数忽略该错误并返回 DRV_ERROR_NONE（`:1232-1237`）。
8. 内核：`CTRL_CMD_SQ_TAIL_UPDATE` → `trs_tscpu_sq_tail_update` → `trs_ring_ts_db(TRS_DB_TASK_SQ, sqid, tail)` → `ka_mm_writel` 写提交通知寄存器槽（`trs_tscpu_sq.c:33-36`、`trs_ts_db.c:291`）。
9. TSFW 收到通知 → 从 SQ 环读 SQE → 派给 SDMA → SDMA 用 SQE 里的 SMMU streamid/substreamid 过 SMMU 翻译搬数据 → 写 CQE、更新 SQ head。

## 4. 已确认走不通的三层位置

### A. 内核 tsdrv 层：寄存器页映射被跳过（IsSqDbMap=0）

- 当前工作副本 `kernel_space/src/tsdrv/ts_drv/ts_drv_common/devdrv_cqsq.c:1048-1085`（`tsdrv_sq_db_map`）：
  - `sqcq_alloc->sqDbVaddr == 0` 时直接 `return 0`，不做映射（注释写明“调用者未预设目标用户态地址时跳过”）；
  - 只有调用者预置了 `sqDbVaddr`，才执行 `tsdrv_sq_reg_map(db_paddr + sqid*4K, sqDbVaddr)` 并把 `IsSqDbMap` 置 1。
- 旧版驱动（24.1.rc3.b999）里的写法是 `if (flag & TSDRV_REMOTE_FLAG) return 0;`，直接返回。
- 当前工作副本已改成按 `sqDbVaddr` 判断，改动已提 MR，待审。
- 另一个相关限制：`tsdrv_sq_head_tail_map` 依赖 `resid_query`（`devdrv_cqsq.c:1121-1158`），310P 没有注册该回调，返回 `-EOPNOTSUPP`，于是 remote SQ 分配会在 `devdrv_cqsq.c:2368` 处跳到 ERROR。另外 remote 分配的前置条件是 `devdrv_is_support_uio_d()`（`:2255-2262`）。

### B. trs 用户态层：`db` 指针为空

- `sq_ctrl.db` 来自 `uio_info->sq_ctrl_addr[TRS_UIO_DB]`（`trs_sqcq.c:640`）。
- `trs_sq_remap` 在 `sq_phy_addr==0 || db_addr==0 || sq_que_addr==0` 时跳过映射（`trs_sqcq_map.c:331-334`）。
- `db_addr` 由 `chan_init.c:240` 经 `QUERY_CMD_SQ_DB_PADDR` 从 TS 侧查询；TSCPU 路径的实现是 `trs_tscpu_get_sq_db_paddr` → `trs_get_ts_db_paddr(TRS_DB_TASK_SQ, sqid)`（`trs_tscpu_sq.c:38-41`）。
- 结果：`db == NULL`，`halSqCqConfig(SQ_TAIL)` 的 UIO 分支返回 NOT_SUPPORT，只能走 ioctl。

### C. 硬件/固件层：读该寄存器会让 AICPU 核停住

- 实测（v4 验证程序）：该寄存器页 mmap 成功，但读 offset 0 的 u32 load 让 AICPU 核永久停在那条指令上（阶段标记 slot14 停在 7，host 等 60 秒后 EXIT=124）。
- 推断：该寄存器槽只实现写通路，读没有硬件应答；AICPU 总线不超时，CPU 停在 load 指令上。
- 写通路是否单独可用尚未测。风险：写错值可能触发 TS 异常或设备停住。

## 5. 验证程序 sq_db_probe 的实测结果（v6，已跑完，设备健康）

源码：`memfabric_hybrid/test/310p_reg_sync_probe/sq_db_probe/{sq_db_probe_kernel.cc, sq_db_probe_main.cpp}`

- 部署方式：extend loader 进 CP1（三个同名符号 + `DynTileFwkKernelServer` 分发器，timeout=0xFFFF + flag 0x100）。
- 观测方式：kernel 每步写 slot14 递增值并 flush cache；host 每 2 秒 D2H 轮询 32 个槽，不依赖 `aclrtSynchronizeStream`。
- 结果槽布局：0=入口标记 0xD00B0005；1=符号位图；2=openRet；3=fd；4=dbVA；5/6=读写值；7/8=allocRet/errno；9=sqId；10/11=query/config；13=free；14=阶段；16=streamRet|streamId<<32。
- v6 实测：
  - `halResourceIdAlloc(type=DRV_STREAM_ID=0)` 建 stream，streamId=48；
  - `_halSqCqAllocate(flag=0, sqeDepth=1024, cqeDepth=1024, info[0]=streamId)` 成功，sqId=65；
  - 对该 SQ 调 `halSqCqQuery` / `halSqCqConfig(SQ_TAIL)`，返回 0xfffe = NOT_SUPPORT；
  - free 成功；EXIT=0。
- 关键前置条件：分配前必须先建 stream，否则 errno=14（EFAULT）。真因是设备内核 `devdrv_ioctl_sqcq_alloc` 的 streamId 归属检查（`devdrv_cqsq.c:2276-2279` → `tsdrv_id_is_belong_to_proc`，`devdrv_id.c:897-936` 借用 EFAULT 返回）。
- 设备状态：8×310P3，驱动 25.6.rc1.b010；验证程序跑完后全部 Health OK，dmesg 无 Heartbeat lost。

## 6. 待查问题（按优先级）

1. 为什么 runtime 建的 CP 流 SQ 在 CP1 里 `halSqCqConfig(TAIL)` 看起来成功，而验证程序自建的 SQ 返回 NOT_SUPPORT？
   - 08 示例端到端搬数成功，说明 SQE 确实被 TSFW 消费；但 `trs_sq_cq_config` 在 ioctl 返回 NOT_SUPPORT 时会忽略错误并返回成功（`trs_sqcq.c:1231-1237`）。需要确认 08 示例的提交通知到底是谁写的：UIO 直写、ioctl 内核代写，还是软队列（写 sq_id 触发内核工作队列）。
   - 差异候选：runtime SQ 有完整 trs chan 对象，验证程序的 SQ 只有 tsdrv 分配记录；`trs_get_sq_info` 能否找到；软队列标志 `trs_is_sq_use_soft_que`。
2. 写语义：310P 的 TSFW 期待写 tail 值还是 sq_id？
   - `trs_sq_task_send_set_db`：软队列模式写 sq_id（`trs_sqcq.c:1527`），普通模式写 tail（`:1530`）。
   - memfabric 代码注释提到“固件把 doorbell 尾值 0 当作空环，插入自拷贝占位使尾值非零”（`hybm_sdma_orch_kernel.cc:446`），说明 TSFW 直接解析尾值。
3. 寄存器槽与 sqId 的对应关系：`trs_get_ts_db_paddr` 用 `paddr + sqid*4K`；CP 流 SQ 的 sqId（如 65）是否落在 TASK_SQ 槽 0..512 内、是否与 TS 侧槽号一致？验证程序拿到的 sqId=65 在范围内，但未验证该槽是否就是该 SQ 的。
4. 读停住的硬件原因与安全写测试方案：该寄存器槽是否只写不读？能否用“写当前 tail 原值”这类无害值验证写通路？需要风险控制（单设备、可热复位）。
5. 内核映射放开后的完整路径：`tsdrv_sq_db_map` 需要调用者预置 `sqDbVaddr`，这个地址由谁分配（runtime / 闭源 HAL 的 `TsDrvNormalSqCqMap`）？闭源 HAL 是否愿意在 CP1 场景传非 0 地址？如果不传，内核改了也永远不会映射。
6. remote SQ 分配在 310P 是否真的走 REMOTE 标志：`devdrv_support_remote_sqcq` 要求 `devdrv_is_support_uio_d()`；若 310P 不支持 UIO_D，CP 流 SQ 可能是普通分配。这决定 `TSDRV_REMOTE_FLAG` 那段直接返回是否真是 310P 的问题（旧结论可能需要修正）。
7. `resid_query` 缺失：310P 没有注册 `resid_query`（只在 ascend610 注册），导致 `tsdrv_sq_head_tail_map` 返回 -EOPNOTSUPP。remote 分配路径是否因此失败？是否需要为 310P 补注册（参考 `ascend610/tsdrv_hwts.c:105-113`）？

## 7. 环境与复现

- 机器：8×310P3，驱动 25.6.rc1.b010。
- CANN：`source /usr/local/Ascend9.0.0/ascend-toolkit/set_env.sh`（只 source `set_env.sh` 不够，会找不到 libascendcl.so）。
- host 编译：`/opt/rh/gcc-toolset-13`；设备侧 so 用 CANN 9.0.0 里的 hcc 交叉编译器。
- 运行：`D=/home/w00930648/memfabric_hybrid/test/310p_reg_sync_probe/sq_db_probe; cd $D && ./sq_db_probe`（Bash 工作目录每次重置，必须在单条命令内 cd）。
- 验证程序直读该寄存器页默认关闭，只有 `tsId` 最高位置 1 才执行（`if ((p->tsId & 0x80000000U) != 0)`），避免再次停住。

## 8. 相关文件清单

| 文件 | 内容 |
|---|---|
| `driver/src/ascend_hal/trs/core/trs_sqcq.c` | 用户态 HAL：db 指针来源(640)、config 路径(1044-1060,1197-1238)、send 路径(1500-1589) |
| `driver/src/sdk_driver/trsdrv/trs/trs_core/trs_sqcq_map.c:317-415` | `trs_sq_remap`：db_addr==0 跳过映射 |
| `driver/src/sdk_driver/trsdrv/trs/lba/comm/adapt/trs_ts_db.c` | 提交通知寄存器区的 ioremap、读写、paddr 查询 |
| `driver/src/sdk_driver/trsdrv/trs/lba/comm/adapt/trs_host_db.c:24-51` | 寄存器区物理基址来自资源管理 TS_DOORBELL_REG |
| `driver/src/sdk_driver/trsdrv/trs/lba/comm/adapt/tscpu/trs_tscpu_sq.c` | 310P 路径：tail 更新、读 tail、查 db paddr |
| `driver/src/sdk_driver/trsdrv/trs/lba/comm/adapt/tscpu/trs_tscpu_chan_sqcq.c` | 310P query/ctrl 实现（head/tail PADDR 返回 0） |
| `driver/src/sdk_driver/trsdrv/trs/lba/near/comm/adapt/soc_adapt/mini_v2/soc_adapt_res_mini_v2.c` | 310P 寄存器槽布局 |
| `driver/src/sdk_driver/trsdrv/trs/inc/trs_chip_def_comm.h:68-76` | hw_type 判定（MINI_V2→TSCPU） |
| `kernel_space/src/tsdrv/ts_drv/ts_drv_common/devdrv_cqsq.c` | 内核：sqcq alloc(2267)、streamId 归属检查(2276)、db map(1048)、head/tail map(1121) |
| `kernel_space/src/tsdrv/ts_drv/ts_drv_common/devdrv_id.c:897-936` | `tsdrv_id_is_belong_to_proc`（errno 14 的来源） |
| `memfabric_hybrid/src/hybm/ops/hybm_kernel/hybm_sdma_orch_kernel.cc` | SQE 构造(153-186)、提交(206-256)、提交通知(133-146,483-491) |
| `memfabric_hybrid/src/hybm/ops/hybm_kernel/hybm_sdma_contract.h` | `HybmSdmaOrchParam`、commByReg 常量 |
| `memfabric_hybrid/src/hybm/csrc/entity/hybm_sdma_orchestrator.cpp:290-305` | host 建 CP 流 |
| `memfabric_hybrid/test/310p_reg_sync_probe/sq_db_probe/` | 验证程序源码 |

## 9. 实测与推断边界

**实测**：
- 310P3 没有 STARS 资源区；
- 提交通知寄存器区 512 槽 × 4KB 存在，且内核可写；
- CP1 内 mmap 该寄存器页成功，读 offset 0 停住不返回；
- 自建 SQ 分配前必须先建 stream；
- 自建 SQ 的 query/config 返回 NOT_SUPPORT；
- 08 示例 SDMA 数据面端到端正确；
- 验证程序跑完后设备全部健康。

**推断（未实机验证）**：
- 该寄存器槽只写不读；
- TSFW 内部解析 SQE 的结构；
- `flushBatchDoorbell` 返回 0 时提交通知确实被写；
- CP 流 SQ 在 310P 上的实际分配标志与 chan 对象状态。
