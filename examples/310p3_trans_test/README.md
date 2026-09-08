# 310P3 TRANS Mode Test

## 概述

验证 310P3 (Atlas 300I Duo) 上 TRANS 模式的完整链路：register → write → read → verify → deregister。

## 测试内容

| 测试项 | 说明 |
|--------|------|
| TestSyncWriteRead | 同步 write 到 peer + verify 数据完整性（L2G） |
| TestAsyncSubmit | 异步 write_submit + stream sync + verify（L2G） |
| TestSyncRead | 同步 read 从 peer 读取 + verify 数据完整性（G2L） |
| TestAsyncReadSubmit | 异步 read_submit + stream sync + verify（G2L） |
| TestDeregister | deregister_mem 释放已注册内存 |

## VA 交换

各 rank 通过 `MPI_Allgather` 交换 `hybm_alloc_vmm_buffer` 返回的设备内存虚拟地址，
write/read 的 `remoteAddr` 参数使用对端真实 VA，不依赖同地址空间假设。
交换后通过 `assert` 断言所有 rank 的 VA 相同。

## 运行方法

### 前置条件

- MemFabric 已构建安装：`bash script/build_and_pack_run.sh`
- `ASCEND_HOME_PATH` 指向 CANN 安装目录
- MPI 已安装

### 2-rank（同卡 die 对 dev0/dev1）

```bash
./run.sh 2 127.0.0.1 24000
```

### 4-rank（跨卡 dev0..dev3）

```bash
./run.sh 4 127.0.0.1 24000
```

### 手动构建

```bash
export MEMFABRIC_HYBRID_HOME_PATH=/path/to/output
export ASCEND_HOME_PATH=/path/to/cann
./build.sh
mpirun --allow-run-as-root -np 2 ./310p3_trans_test tcp://127.0.0.1:24000
```

## 预期结果

- 2-rank：ALL TESTS PASS
- 4-rank：ALL TESTS PASS（P2P 槽位 4 ≤ 8，可行）
- 8-rank：smem_trans_create 失败，日志输出 "P2P slot budget exceeded"

## 注意事项

- `smem_trans_create` 内部会调用 `TopologyPreflight`，310P3 上自动检查 P2P 槽位预算
- 测试使用 `SMEM_TRANS_BOTH` 角色，每个 rank 既是 sender 又是 receiver
- `devId = rankId`，MPI rank 0 对应 dev0，rank 1 对应 dev1，以此类推
- 跨卡测试需确保 dev0..dev3 分别在不同卡上（dev0/dev1=card0, dev2/dev3=card1）
