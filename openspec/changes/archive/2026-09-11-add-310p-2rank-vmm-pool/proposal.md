## Why

Atlas 300I Duo（Ascend 310P3，单卡双 die）此前无法通过 hybm 建 HBM 内存池：现有 VMM 段实现仅支持 910C/950（走 HAL `halMem*` 路径），310P 需要走 ACL 层 `aclrt*` VMM 公开接口。本变更使单卡双 die 2-rank 建池端到端可用（HBM 池走 ACL VMM shareable handle 跨 die 导入，DRAM 池复用 HOST_SHM），并已实机验收通过（双池三阶段 8MB 逐字校验 mismatch=0，3/3 轮稳定复跑）。

> **补录说明**：本变更为补录（retroactive）——实现已在 efea25d1..a4db8996 完成并合入。本 change 依据《310p-2rank-vmm-开发计划.md》（仓库根）与 git 历史补齐规划工件并归档，用于建立 OpenSpec 规格基线；tasks 全部标记为已完成。

## What Changes

- 新增 `ASCEND_310P` SOC 类型识别（soc 字符串匹配 "Ascend310P"）；HBM 段工厂分派中 SOC 判断优先于 GVA 版本判断（310P 驱动被映射为 GVA V4 也不能落入 halMem* 路径）
- `dl_acl_api` 新增 14 个 `aclrt*` VMM 符号（MallocPhysical/ReserveMemAddress/MapMem/SetAccess/Export/Import 等）的 typedef / OPTIONAL 动态加载 / inline wrapper（符号缺失仅 WARN、指针保持 nullptr，910B/950 机器零影响）
- 新增 `HybmAclVmmSegment` 内存段：`aclrtReserveMemAddress` 整窗预留 + slot 布局，GVA==DVA==HVA 同值；export/import 序列化 magic `0xAABB1234FFFFEE05`
- 跨 die import 采用显式 SetContext 五步（CreateContext(对端 die)→SetCurrentContext→Import(sh, 本端 die)→SetDevice(本端 die)→DestroyContext）；export 固定带 `ACL_RT_VMM_EXPORT_FLAG_DISABLE_PID_VALIDATION`
- import 幂等去重（按 rankId+sliceIndex），join 重试重复 import 不报错
- `ClassifyAddressMask`：已登记的本地池内地址返回 LOCAL|GLOBAL 双位（对齐 reservedMap_ 分支语义，对 910B 行为等价）
- `MemSegment::InitDeviceInfo` 对 310P 不支持的 SDID/SERVER_ID 查询容忍降级（无效值 + WARN，可达性改由段映射状态判定）
- 新增 `HostDataOpAclMemcpy` 拷贝算子（占 compose `sdmaDataOperator_` 槽位）：D2D 走 `aclrtMemcpyAsync` + SynchronizeStream，H2D/D2H 走同步 `aclrtMemcpy`，不支持方向返回 BM_INVALID_PARAM
- 新增 example `examples/memory_pool/01_basic/07_310p_die_pair_pool/`（C++/Python 双语言，双进程双 die 双池端到端校验）
- 修复 `ReleaseSliceMemory` use-after-free（ASAN 检出：入参为 map 内 shared_ptr 引用，erase 后悬空）

## Capabilities

### New Capabilities

- `hybm/acl-vmm-hbm-pool`: 310P 单卡双 die 2-rank HBM 内存池的 ACL VMM 段能力——SOC 识别与段分派、段生命周期（预留/分配/导出/导入/映射/回收）、跨进程跨 die 语义、池内地址分类、设备信息查询容忍降级
- `hybm/acl-memcpy-data-op`: 310P 内存池数据拷贝算子能力——HostDataOpAclMemcpy 的方向支持矩阵、同步语义、compose 候选链接入与 hybm_wait 契约

### Modified Capabilities

（无——`openspec/specs/` 当前为空，本变更建立项目初始规格基线）

## Impact

- **代码**：`src/hybm/csrc/common/hybm_define.h`、`src/hybm/csrc/mm/`（hybm_acl_vmm_segment 新文件、hybm_mem_segment、hybm_va_manager）、`src/hybm/csrc/under_api/dl_acl_api.h/.cpp`、`src/hybm/csrc/data_operation/host/`（hybm_data_op_acl_memcpy 新文件、compose/factory 接入）、`test/ut/`（4 个新/改测试套件 + acl_stub）、`examples/memory_pool/01_basic/07_310p_die_pair_pool/`
- **兼容性**：910B/910C/950 行为零变化（符号 OPTIONAL 加载、SOC 分派隔离、ClassifyAddressMask 双位与 reservedMap_ 分支语义一致、QueryDeviceInfoSocTolerant 仅 310P 触发降级）
- **依赖**：libascendcl.so 导出的 `aclrt*` VMM 公开符号；驱动 Innerversion C10+（GVA V4 窗口）
- **已知限制**（登记为 V2 TODO，见开发计划第九节）：export 关闭 PID 白名单校验（V2 恢复）、设备信息查询依赖 rt 内部符号（V2 改公开 API）、算子全方向等效同步（V2 真异步）
