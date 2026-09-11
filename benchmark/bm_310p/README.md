# bm_perf_benchmark_310p — 310P 单卡双 die 2-rank BM 双池带宽测试

基于 `benchmark/bm`（通用版）复制改造，**仅适用于 Atlas 300I Duo（Ascend310P3）单卡双 die 2-rank 场景**：

- **双实体双池**（对齐 `examples/memory_pool/01_basic/07_310p_die_pair_pool`，310P 上单实体双池不可建）：
  - HBM 池：`SMEMB_DATA_OP_SDMA` op 位 → `HybmAclVmmSegment`（ACL VMM shareable handle + die 对导入）+ `HostDataOpAclMemcpy` 算子
  - DRAM 池：`SMEMB_DATA_OP_HOST_SHM` → `HybmHostShmSegment`（同节点 POSIX 共享内存）
- 10 个方向按 **目的池类型** 路由实体：HBM 池方向（H2D/H2RD/D2RD/RD2D/RD2H）走 HBM 实体，
  DRAM 池方向（D2H/H2RH/D2RH/RH2D/RH2H）走 DRAM 实体
- HBM 物理分配支持 **P2P_HUGE → HBM_HUGE 自动回退**（310P P2P 分区上限 ~4088MB，见
  `310p-2rank-bm-bw-测试计划.md` F1/F2）

## 前置条件

- 已安装 CANN（`source /usr/local/Ascend/cann-xxx/set_env.sh`）
- 已安装 memfabric_hybrid（`source /usr/local/memfabric_hybrid/set_env.sh`），或按下方"单独编译"用源码 output 产物
- 空闲 die 对（`npu-smi info` 确认，如 dev4/dev5）
- `/dev/shm` 可用空间 ≥ `dram-size × world-size`（DRAM 池为 host 共享内存）

## 编译

单独编译（需设置 `MEMFABRIC_HYBRID_HOME_PATH` 指向含 `include/` 与 `aarch64-linux/lib64/` 的安装根）：

```bash
cd benchmark/bm_310p
mkdir build && cmake . -B build && make -C build
```

或执行 `bash build_bm_perf_benchmark_310p.sh`；或随 run 包编译
（`bash script/build_and_pack_run.sh --build_test ON ...`）。

## 运行

```bash
./bm_perf_benchmark_310p -bw -t [TYPE] -s [BLOCK_SIZE] -cc [COPY_COUNT] -bs [BATCH_SIZE]
       -d [DEVICE_START] -ws [WORLD_SIZE] -lrs [LOCAL_RANK_SIZE] -ip tcp://<ip>:<port>
       -hbm-size [BYTES] -dram-size [BYTES] -hm [pinned|pageable]
```

- TYPE：`h2d/d2h/h2rd/h2rh/d2rd/d2rh/rh2d/rh2h/rd2d/rd2h/all`（`all` 依次测全部 10 方向）
- 池大小默认各 2GB（2M 对齐必填校验）；`-d` 指定起始 die（rank i 用 dev `-d+i`）

示例（dev4/dev5 die 对，2MB 块 × 100 次）：

```bash
./bm_perf_benchmark_310p -bw -t all -s 2097152 -cc 100 -d 4 -ws 2 -lrs 2 -ip tcp://127.0.0.1:28570
```

正确性冒烟（1MB × 10 次，WrongBytes(B) 列应全为 0）：

```bash
./bm_perf_benchmark_310p -bw -t all -s 1048576 -cc 10 -d 4 -ws 2 -lrs 2 -ip tcp://127.0.0.1:28571
```

## 输出

结果表含 `Pool` 列标注该方向执行的实体（HBM/DRAM）：

```text
Type      Pool  NPU        Rank      ...
H2D       HBM   4          0         ...  BW(GB/s) ...  WrongBytes(B) 0
D2H       DRAM  4          0         ...
```

## host 缓冲两种口径（`-hm pinned|pageable`，默认 pinned）

本地 host 缓冲（H2D/D2H 系方向的源/目的）支持两种分配方式，对齐 01 探针的口径矩阵：

| 口径 | 分配方式 | 数据通路 | copyType |
|------|---------|---------|----------|
| `pinned`（默认） | `aclrtMallocHost` | DMA 直达（pinned→HBM 池实际走 `AclrtMemcpyAsync(D2D)`+Sync） | device 侧：Put `L2G` / Get `G2L` |
| `pageable` | `posix_memalign` | 驱动内部 staging 中转（host→staging memcpy + staging→dev DMA 两次拷贝） | host 侧：Put `H2G` / Get `G2H` |

- **pinned VA 落在 SVM 统一地址窗口内（~0x12c0_xxxx_xxxx），hybm 地址分类器按范围判为
  LOCAL_DEVICE**（hybm_va_manager.cpp:256），故 pinned 模式 host 侧方向须使用 device 侧
  copyType；池类型（dst/src 的 HOST/DEVICE）由地址分类自动落位
- pageable 模式保持原 host 侧 copyType 语义（SVM 窗口外的普通 malloc 地址判为 LOCAL_HOST）
- 实测对比（128MB 块，x16 卡 dev6/dev7，2-rank）：pinned H2D 11.8~12.7 vs pageable 8.8 GB/s；
  x8 卡两种口径均触顶 ~7（链路上限，见下表）

## pinned host 缓冲与 copyType 语义（310P SVM 特性）

- 本地 host 缓冲使用 `aclrtMallocHost`（pinned，对齐 01 探针口径）。pageable（普通 malloc）
  走驱动内部 staging 两次拷贝，x16 卡实测仅 5.6~8.5 GB/s
- **pinned VA 落在 SVM 统一地址窗口内（~0x12c0_xxxx_xxxx），hybm 地址分类器按范围判为
  LOCAL_DEVICE**（hybm_va_manager.cpp:256），故 host 侧方向须使用 device 侧 copyType：
  Put 用 `L2G`（H2D/H2RD/H2RH），Get 用 `G2L`（RD2H/RH2H）；dst/src 池类型由地址分类自动落位
- pinned → HBM 池实际走 `AclrtMemcpyAsync(D2D)` + SyncStream（单流 x16 实测 23.8 GB/s）
- pageable 模式（H2G/G2H/GH2H host 侧类型）已被 pinned 模式取代，语义差异见测试计划附录

## 带宽参考值（128MB 块 × 30 次，2-rank 双 die 同时压测）

| 卡型 | PCIe | H2D/H2D系(每rank) | D2RD/RD2D(die间) | host memcpy(H2RH/RH2H) |
|------|------|------------------|------------------|------------------------|
| x16（dev6/dev7） | Gen4 x16 | 10.6 ~ 12.7 GB/s | 16.5 / 17.6 GB/s | 10.8 ~ 13.8 GB/s |
| x8（dev4/dev5） | Gen4 x8 | 6.4 ~ 7.1 GB/s | 16.4 / 17.6 GB/s | 9.5 ~ 11.5 GB/s |

> H2D 系上限 = 单卡 PCIe 链路 ÷ 2（双 die 共享一个端点）：x16 单流 pinned sync 实测 27.1 GB/s，
> 双 rank 各得 ~12；x8 单流 13.8，双 rank 各得 ~7。die 间 D2D 与 host memcpy 不走 PCIe，不受此限。
> 单 die 裸 API 参考值（01 探针口径 128MB pinned sync）：x16 卡 26.96 GB/s。

## 与通用版 bm_perf_benchmark 的差异

| 项 | 通用版 | 310P 版 |
|----|--------|---------|
| 实体 | 单实体双池（HBM+DRAM） | 双实体（HBM/DRAM 各一） |
| op 类型 | `-ot` 必填（sdma/rdma/urma/...） | 固定：HBM=SDMA 位，DRAM=HOST_SHM |
| 池大小 | 固定 4GB/4GB | `-hbm-size/-dram-size` 可配（默认 2GB，2M 对齐） |
| 适用 | 910B 等多卡多机 | 仅 310P 单卡双 die 2-rank |
