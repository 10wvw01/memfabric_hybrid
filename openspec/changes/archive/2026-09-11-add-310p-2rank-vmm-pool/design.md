## Context

310P（Ascend 310P3，Atlas 300I Duo）单卡双 die（dev0/dev1 同 NPU）。实测关键事实（编号沿用开发计划 F1-F20）：

- 驱动 Innerversion C10SPC008（GVA V4 窗口），HAL 24 个硬性符号齐全 → `hybm_init` 链零修改（F2/F3）
- libascendcl.so 导出全部所需 `aclrt*` VMM 公开符号（F4）
- `MallocPhysical` 的 handleType 必须 NONE（POSIX 全失败 100000），memAttr P2P_HUGE 可用，granularity 2MB（F10）
- 双进程 `ReserveMemAddress` base 一致（对称 GVA 成立，无需 expectPtr）（F11）
- export 必须带 `ACL_RT_VMM_EXPORT_FLAG_DISABLE_PID_VALIDATION(0x1)`，否则对端 import 被内核拒 -EACCES→507899（F12）
- die 对 import 需显式 SetContext 五步；本进程 ctx 直接 import 被 P2P 前置检查拒（F13）
- VMM VA 是设备地址，CPU 不可直接解引用（F14）
- 310P 不支持 SDID/SERVER_ID/SUPER_POD_ID 查询（rtGetDeviceInfo 返回 207000）（F16）

## Goals / Non-Goals

**Goals:**

- 单卡双 die 2-rank HBM 池（ACL VMM）+ DRAM 池（HOST_SHM 复用）双池端到端可用
- 910B/910C/950 机器零行为变化（符号 OPTIONAL、SOC 分派隔离）
- 全部走 `DlAclApi` aclrt* 符号（用户指定，不新增 HAL 内部符号依赖）

**Non-Goals:**

- PID 白名单安全语义（V1 关闭，V2 经 `aclrtDeviceGetBareTgid` + `aclrtMemSetPidToShareableHandle` 恢复，见开发计划 TODO-1）
- 算子真异步（V1 全方向等效同步，V2 兑现 ASYNC_COPY_FLAG/外部 stream/hybm_wait 契约，见 TODO-3）
- 8-rank HBM 池（受 P2P 槽位预算限制）与 8-rank DRAM 大池实机验证（见 TODO-4）
- 用户自注册裸 VMM 地址的 `GetLocalMemoryType` 正确分类（V1 已知限制）

## Decisions

| # | 决策 | 依据与备选 |
|---|------|-----------|
| D1 | 全部走 `DlAclApi` aclrt* 符号（14 个，`DL_LOAD_SYM_OPTIONAL` 加载） | OPTIONAL 保证 910B/950 零影响（符号缺失仅 WARN，指针 nullptr）；备选直接链接 libascendcl——被否，破坏可选性 |
| D2 | 新增 `HybmAclVmmSegment`，不复用 `HybmVmmBasedSegment` | 910C/950 VMM 段走 HAL halMem*，310P 走 ACL 层，仅接口名相似，语义不同；塞入同文件会污染既有路径 |
| D3 | 新算子 `HostDataOpAclMemcpy` 占 `sdmaDataOperator_` 槽位（compose Initialize 加 310P 分支） | `HostComposeDataOp::Wait` 硬绑 `sdmaDataOperator_->Wait()`，不占此槽 `hybm_wait` 恒失败；备选改 compose 结构——被否，影响面大 |
| D4 | exchange 新 magic `0xAABB1234FFFFEE05` + 扩展 `IsHbmSlice` | `ImportForSegment` 对未知 magic 静默跳过 → 192B 块留在 reader → transportKey 读偏断链；复用旧 magic 会与 910C 路径混淆 |
| D5 | VA：`aclrtReserveMemAddress(rankCnt×maxSize, NULL)` 整窗，slot = base + p×maxSize；`AddVaInfo{gva=dva=hva}`；不走 VaManager 窗口分配 | VMM VA 0x12c140000000 在现有窗口（SVM 8T/GVM 40T+）之外；池内地址全经 map 登记则分类安全 |
| D6 | die 对 import 显式 SetContext 五步；跨卡本进程 ctx 直接 import | 本进程 ctx 直接 import 被 P2P 前置检查拒 507899；前置条件（本端 ctx）由 InitDeviceInfo 的 AclrtSetDevice 保证 |
| D7 | export 固定带 DISABLE_PID_VALIDATION，不设 pid 白名单 | F12 实测裁决；备选默认 flag+白名单——被否，V1 引入 pid 交换时序耦合，V2 再恢复 |
| D8 | D2D 必须 `aclrtMemcpyAsync` + SynchronizeStream；H2D/D2H 用同步 `aclrtMemcpy` | 同步 D2D 跨 die 507899（F15 实测） |
| D9 | HBM 池 opType=SDMA 位；DRAM 池 opType=HOST_SHM | 用户侧语义不变；HOST_SHM 池 hybm_wait 恒失败是既有行为，example 不调用 |
| D10 | DRAM 池与 HBM 池为两个独立池（两个 entity） | HOST_SHM 要求纯 DRAM 池且禁与其他 opType 混用（smem_bm.cpp 既有约束） |
| D11 | `ClassifyAddressMask` 扩展：allocatedMap 命中且非 imported → LOCAL\|GLOBAL 双位 | AclVmm 段不登记 reservedMap，原逻辑只回单 LOCAL → G2H 方向校验失败；双位与 reservedMap 分支语义一致，对 910B 等价 |
| D12 | `QueryDeviceInfoSocTolerant`：310P 拓扑查询失败降级为无效值+WARN | F16：207000 硬失败会阻断建池；可达性改由段映射状态判定 |

## Risks / Trade-offs

- [双进程 reserve base 不一致] → 实测一致（F11）；失败预案：`expectPtr` 指定同址，再失败回退 IPC+查表（方案文档备案）
- [关闭 PID 白名单带来越权访问风险] → V1 接受（单机独占场景）；V2 恢复白名单（TODO-1）
- [依赖 rt 内部符号 rtGetDeviceInfo/rtDeviceGetBareTgid，CANN 升级无兼容承诺] → V2 改公开 API `aclrtGetDeviceInfo`（TODO-2）
- [异步契约未兑现（用户传 ASYNC_COPY_FLAG 得到阻塞行为）] → V1 有意简化，数据正确性不受影响；V2 兑现（TODO-3）
- [ClassifyAddressMask 公共路径修改影响 910B] → 双位语义与 reservedMap 分支一致；全量 UT 回归（HybmVaManagerTest 105/105）证实等价

## Migration Plan

已直接合入（efea25d1..a4db8996），无部署迁移。回滚即 revert 对应提交区间；910B/950 路径不受影响，回滚仅需恢复 310P 分派相关文件。

## Open Questions

无——实现已完成并实机验收（3/3 轮稳定复跑），残余改进项已全部登记为 V2 TODO（见 Non-Goals）。
