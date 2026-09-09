# 310P3 Bare ACL P2P VMM Benchmark (MPI Cross-Process)

裸 ACL VMM P2P 带宽基准测试，MPI 双进程跨进程模式，对标 MemFabric GVA 框架的跨 rank 场景。

## 测试内容

MPI 双进程：rank0（exporter/devA）分配 VMM P2P 内存并 export，rank1（importer/devB）import 后执行带宽测试。

- **D2D**：devA exported VA → devB local VA（Pull 模式，跨进程），`ACL_MEMCPY_DEVICE_TO_DEVICE`
- **H2D**：host → devB local VA，`ACL_MEMCPY_HOST_TO_DEVICE`
- **D2H**：devB local VA → host，`ACL_MEMCPY_DEVICE_TO_HOST`

每种 kind 对比 single-sync（每次 async 后立即 sync）与 batch-sync（100 次提交后统一 sync）。D2D 每轮使用不同偏移地址避免 device cache 命中。

## 前置条件

- 310P3 平台，CANN 9.1.0+
- `ASCEND_HOME_PATH` 指向 CANN 安装目录
- MPI 已安装（mpirun）

## 编译运行

```bash
bash build.sh

# 同卡跨 die（dev0 -> dev1）
bash run.sh

# 跨卡（dev0 -> dev2）
bash run.sh 0 2
```

## Block Size 规格

32KB, 128KB, 256KB, 512KB, 1MB, 2MB, 4MB, 16MB, 32MB, 64MB, 128MB
