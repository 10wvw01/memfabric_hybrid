## Purpose

定义 Ascend 310P（单卡双 die）上通过 ACL VMM 接口创建与管理 2-rank HBM 内存池的行为契约：SOC 识别与段分派、段生命周期、跨进程/跨 die 导入导出、池内地址分类与可选符号容忍语义。

## Requirements

### Requirement: 310P SOC 识别与 HBM 段分派

系统 SHALL 识别 soc 字符串包含 "Ascend310P" 的设备为 ASCEND_310P 类型。在 310P 设备上创建 HBM 内存池（SDMA 位 opType）时，系统 SHALL 使用 ACL 层 VMM 路径构建段，且该分派 SHALL 优先于 GVA 版本判断（310P 驱动版本被映射为 GVA V4 窗口时也不得落入 HAL 路径）。在 910B/910C/950 设备上，段分派与建池行为 SHALL 保持与引入本能力之前完全一致。

#### Scenario: 310P 上创建 HBM 池

- **WHEN** 在 soc 字符串为 "Ascend310P3" 的设备上以 SDMA 位 opType 创建 HBM 内存池
- **THEN** 建池成功，段经由 ACL VMM 路径构建（ReserveMemAddress/MallocPhysical/MapMem 链路）

#### Scenario: 非 310P 设备行为不变

- **WHEN** 在 910B 或 910C/950 设备上创建 HBM 内存池
- **THEN** 段分派结果与建池行为和引入本能力之前一致，无任何行为变化

### Requirement: ACL VMM 可选符号缺失容忍

系统 SHALL 以 OPTIONAL 方式动态加载 ACL VMM 相关的 `aclrt*` 符号。当宿主机器的 libascendcl.so 不导出这些符号（如 910B/950 环境）时，`hybm_init` SHALL 正常完成，仅记录 WARN 日志，对应函数指针保持空值；不得因符号缺失导致初始化失败或崩溃。

#### Scenario: 符号缺失的机器上初始化

- **WHEN** 在不导出 ACL VMM 符号的 CANN 环境上执行 `hybm_init`
- **THEN** 初始化成功返回，仅出现符号加载 WARN 日志，进程不崩溃

### Requirement: 段地址空间布局

在 310P 上创建 HBM 池时，系统 SHALL 以一次整窗预留（`rankCnt × maxSize`）方式获得虚拟地址窗口，并按 `base + rankIndex × maxSize` 的槽位布局安放各 rank 分片。池内地址的 GVA、DVA、HVA SHALL 为同一 VMM VA 值（地址直接可用，无需变换）。各 rank 分片的物理内存分配 SHALL 按 VMM granularity（实测 2MB）对齐。

#### Scenario: 整窗预留与槽位布局

- **WHEN** 2-rank 各 8MB 建立 HBM 池
- **THEN** 系统一次预留 16MB 窗口，rank0/rank1 分片分别位于 base 与 base+8MB，GVA==DVA==HVA

### Requirement: 跨进程导出与导入

HBM 分片导出信息 SHALL 使用 magic `0xAABB1234FFFFEE05`（ACL VMM 段专用），序列化布局 192 字节；导出 SHALL 携带 gva、slotIndex、devId、pid、rankId、size 与 shareableHandle。导出 shareable handle 时 SHALL 固定携带 `ACL_RT_VMM_EXPORT_FLAG_DISABLE_PID_VALIDATION` 标志（关闭驱动级 PID 白名单，免去跨进程 pid 交换时序耦合）。

对端导入时：同卡 die 对（devId 整除 2 相同）SHALL 采用显式上下文切换五步序列（在对端 die 上 CreateContext → SetCurrentContext → Import(本端 die) → SetDevice(本端 die) → DestroyContext）；跨卡场景 SHALL 在本进程上下文直接导入。导入地址 SHALL 取导出信息中的 gva（对称 GVA，双进程 ReserveMemAddress base 一致）。

#### Scenario: 跨 die 导入

- **WHEN** rank0（dev0）导出分片，rank1（dev1，同卡 die 对）执行 join 导入
- **THEN** 五步上下文序列全部成功，导入地址等于 rank0 的 gva，随后跨 die D2D 拷贝逐字校验通过

#### Scenario: 未知 magic 的导出块被正确识别

- **WHEN** 传输层收到 magic 为 `0xAABB1234FFFFEE05` 的交换块
- **THEN** 该块被识别为 HBM 分片并进入导入流程，不被静默跳过（避免后续读取错位断链）

### Requirement: 导入幂等性

同一 (rankId, sliceIndex) 的 shareable handle 重复导入时，系统 SHALL 幂等去重并返回成功，不得报错或产生重复映射。join 重试（对端尚未就绪导致的重复 join）SHALL 不因重复 import 而失败。

#### Scenario: join 重试重复导入

- **WHEN** 某 rank 因对端未就绪多次 join，导致对同一 shareableHandle 重复触发导入
- **THEN** 后续导入被幂等去重，join 最终成功，无错误日志刷屏

### Requirement: 池内地址分类

对于已登记在本地分配表中的池内地址：本端分配的地址 SHALL 分类为 LOCAL 与 GLOBAL 双位（对称池内地址既可作拷贝源也可作目标）；导入的对端地址 SHALL 分类为单一 GLOBAL 位。该分类 SHALL 与既有 reservedMap 路径的语义保持一致，且对 910B 场景行为等价。

#### Scenario: 本地池地址作拷贝源与目标

- **WHEN** 用户对本端分配的 HBM 池地址发起 G2H 或 H2G 方向拷贝
- **THEN** 地址被分类为 LOCAL|GLOBAL 双位，方向判定通过，拷贝正常执行

### Requirement: 310P 设备信息查询容忍降级

`hybm_init` 阶段的设备拓扑信息查询（SDID/SERVER_ID/SUPER_POD_ID）在 310P 上不被驱动支持（返回 207000）时，系统 SHALL 容忍降级：记录 WARN 日志并将对应字段置为无效值，初始化 SHALL 继续成功；跨 rank 可达性判定 SHALL 改由段导入映射状态决定，而非拓扑坐标。

#### Scenario: 310P 初始化不因拓扑查询失败而失败

- **WHEN** 在 310P 上执行 `hybm_init`，内部拓扑查询返回 207000
- **THEN** 初始化成功，仅记录 WARN，后续建池与跨 die 导入可达性由段映射状态判定

### Requirement: 段回收不产生内存错误

段销毁与分片回收路径 SHALL 无 use-after-free：回收接口在移除容器内分片对象后不得再解引用该对象的成员。

#### Scenario: 析构路径回收分片

- **WHEN** 段析构时回收全部分片（容器为对象最后所有者）且以 ASAN 构建
- **THEN** 无 use-after-free 报告，进程干净退出

### Requirement: aclrtMemAttr 枚举与 CANN 严格对齐

`dl_acl_api.h` 中本地声明的 `aclrtMemAttr` 枚举 SHALL 与构建目标 CANN 版本（9.1.0）`acl/acl_rt.h` 的编号严格一致（全量 18 项，0~17）。枚举值经 `aclrtMallocPhysical` 的 `aclrtPhysicalMemProp.memAttr` 直传驱动，本地自编号 SHALL 被禁止（错位会导致语义漂移：名为 P2P_HUGE 实发 HBM_HUGE）。枚举定义处 SHALL 以注释标明错位风险与实测教训。

#### Scenario: 枚举值直传驱动语义正确

- **WHEN** 以 `ACL_MEM_P2P_HUGE`（值 16）构造 memProp 调用 `AclrtMallocPhysical`
- **THEN** 驱动收到值 16，从 P2P 分区分配；池 ≤4GB 时分配成功，>4GB（分区上限 ~4088MB）返回 207001

#### Scenario: 非 310P 环境零影响

- **WHEN** 在 910B/910C/950 环境加载 libascendcl.so
- **THEN** 枚举仅为本头文件类型声明，不影响宿主 CANN 行为

### Requirement: 物理分配 P2P_HUGE 到 HBM_HUGE 回退

HBM 池物理分配（`MallocPhysicalWithFallback`）SHALL 先以 `ACL_MEM_P2P_HUGE` 申请；当且仅当返回码精确等于 207001（RT_ERROR_DRV_OUT_MEMORY，P2P 分区 OOM）时，SHALL 记录含 rankId、size、ret 的 WARN 日志并以 `ACL_HBM_MEM_HUGE`（AI 主池，~42GB）重试一次。其他错误码（如 100000 参数错、207000）SHALL 不回退、直接失败并在根因处记录 ERROR。回退按单次分配调用（每 slice）判定，`smem_bm` 建池为一次性整池分配，一个池的物理来源 SHALL 全 P2P 或全 HBM_HUGE，不混合。

#### Scenario: 大池触发回退

- **WHEN** 2-rank 各建 8GB/32GB HBM 池（超过 P2P 分区 ~4088MB 上限）
- **THEN** 双 rank 各打印一条 P2P partition exhausted WARN 后 HBM_HUGE 整池分配成功，建池/join/十方向拷贝校验全部通过

#### Scenario: 非分区 OOM 错误不回退

- **WHEN** `AclrtMallocPhysical(P2P_HUGE)` 返回 207001 以外的错误码
- **THEN** 不触发回退，错误逐层上抛（hybm_acl_vmm_segment 根因处已记录 ERROR）

#### Scenario: HBM_HUGE 也失败时最终失败

- **WHEN** 回退后 `AclrtMallocPhysical(HBM_HUGE)` 仍失败（如 AI 主池余量不足）
- **THEN** `MallocPhysicalWithFallback` 记录含 rankId/size/ret 的 ERROR 并返回失败，建池失败链路各层均记录日志
