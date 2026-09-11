## ADDED Requirements

### Requirement: 双实体双池建池

`bm_perf_benchmark_310p` SHALL 以两个独立 smem_bm 实体建池：entity0 = HBM 池（SDMA 位 opType，310P 走 AclVmm 段）、entity1 = DRAM 池（HOST_SHM）。HBM 池大小 SHALL 经 `-hbm-size`、DRAM 池经 `-dram-size` 指定（默认各 2GB），且 SHALL 校验 2MB 对齐（AclVmm 段 granularity）。池大小非法（非 2M 对齐、建池失败）SHALL 记录 ERROR 并失败退出。

#### Scenario: 双池默认建池

- **WHEN** 2-rank 以默认参数（各 2GB）启动带宽测试
- **THEN** 两个实体分别建池成功，十方向按目的池类型路由执行

#### Scenario: 大池 CLI 指定

- **WHEN** 以 `-hbm-size 8589934592` 启动
- **THEN** HBM 池 8GB 建池成功（触发核心 P2P→HBM_HUGE 回退），测试正常执行

### Requirement: 十方向按目的池路由

测试矩阵 SHALL 覆盖十个方向（h2d/d2h/h2rd/h2rh/d2rd/d2rh/rh2d/rh2h/rd2d/rd2h，`all` 依次全部）：H2D/H2RD/D2RD/RD2D/RD2H 落 HBM 池，D2H/H2RH/D2RH/RH2D/RH2H 落 DRAM 池。每方向 SHALL 按"目的池类型"选择对应实体的 handle 执行 `smem_bm_copy_batch`，输出 SHALL 含 Pool 列标识落位。

#### Scenario: 十方向全量执行

- **WHEN** 以 `-t all` 执行
- **THEN** 十方向依次执行，每方向两 rank 各输出一行（Type/Pool/NPU/Rank/BW/WrongBytes）

### Requirement: host 缓冲 pinned/pageable 双口径

本地 host 缓冲 SHALL 支持 `-hm pinned|pageable` 两种口径（默认 pinned）：pinned 经 `aclrtMallocHost` 分配，pageable 经 `posix_memalign`（4096 对齐）。pinned 缓冲 VA 落 SVM 统一地址窗口、被 hybm 地址分类器判为 LOCAL_DEVICE，其 host 侧方向（H2D 系/RD2H 系）SHALL 自动切换为 device 侧 copyType（Put 用 L2G、Get 用 G2L）；pageable 保持 host 侧 copyType（Put 用 H2G、Get 用 G2H）。输出 SHALL 打印 `host_mem:pinned|pageable` 标识当前口径。

#### Scenario: pinned 默认口径走 device 侧类型

- **WHEN** 默认（pinned）执行 H2D 方向
- **THEN** host 缓冲经 `aclrtMallocHost` 分配，copyType 取 L2G，数据校验通过

#### Scenario: pageable 口径保持 host 侧类型

- **WHEN** 以 `-hm pageable` 执行 H2D 方向
- **THEN** host 缓冲经 `posix_memalign` 分配，copyType 取 H2G，数据校验通过

### Requirement: 数据完整性与缓冲生命周期

每方向每 rank SHALL 统计并输出 WrongBytes（逐字 pattern 校验不匹配字节数），非 0 即失败。pinned host 缓冲与本地 HBM 缓冲 SHALL 在 `aclrtResetDevice` 之前释放（pinned host 依赖 device context）。

#### Scenario: 校验通过输出

- **WHEN** 任一方向执行完成
- **THEN** 输出行携带 WrongBytes 值，正常路径为 0

### Requirement: 与通用版基准的共享头复用

`benchmark/bm_310p` SHALL 复用 `benchmark/bm` 的 `barrier_util.h` 与 `common_def.h`（以通用版目录为唯一来源，经 CMake include 路径 `../bm` 解析），不得维护逐字副本；`benchmark/bm` SHALL 保持零改动。

#### Scenario: 通用版零改动

- **WHEN** 本能力引入后查看 `benchmark/bm` 目录
- **THEN** 无任何文件被本基准修改，通用版构建与行为不变

#### Scenario: 头文件解析到通用版

- **WHEN** `bm_perf_benchmark_310p` 构建时编译 `band_width_calculator.h`
- **THEN** `barrier_util.h`/`common_def.h` 经 include 路径解析至 `benchmark/bm/` 下的副本
