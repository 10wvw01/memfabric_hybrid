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
       -hbm-size [BYTES] -dram-size [BYTES]
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

## 与通用版 bm_perf_benchmark 的差异

| 项 | 通用版 | 310P 版 |
|----|--------|---------|
| 实体 | 单实体双池（HBM+DRAM） | 双实体（HBM/DRAM 各一） |
| op 类型 | `-ot` 必填（sdma/rdma/urma/...） | 固定：HBM=SDMA 位，DRAM=HOST_SHM |
| 池大小 | 固定 4GB/4GB | `-hbm-size/-dram-size` 可配（默认 2GB，2M 对齐） |
| 适用 | 910B 等多卡多机 | 仅 310P 单卡双 die 2-rank |
