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

## 内存后端

`hybm_alloc_vmm_buffer`在310P上使用HAL大页分配和映射，注册表导出64位NONE共享句柄。
TRANS按本端驱动逻辑设备编号执行HAL导入、映射和访问权限设置；同卡导入保持本地上下文。
注销和缓冲区释放使用对应的HAL接口。ACL用于本地设备设置、stream和数据拷贝。
普通非VMM用户内存继续使用现有IPC兼容分支。

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

## 失败退出与回归验证

任一rank在初始化、创建、分配内存或测试阶段返回失败时，程序会记录rank、阶段和返回码，
然后通过MPI_Abort结束整个任务。正常完成时执行原有退出流程，mpirun返回0。

在已加载CANN和本次构建库的环境中，可以验证错误退出：

```bash
python3 examples/310p3/310p3_trans_test/test_failure_exit.py \
  --binary examples/310p3/310p3_trans_test/310p3_trans_test
```

脚本固定使用设备0/1，执行真实注册及16MiB读写，并通过临时预加载库注入错误。
注册结果不一致的用例先让两端完成真实注册，再仅向选定rank返回失败。
无故障对照使用相同的额外同步，检查正常流程仍能完成。

回归覆盖注册、各类读写、注销及初始化失败。单rank失败应使任务以非零退出，
并在脚本的等待上限内结束（默认30秒）。脚本需要MPI的C++编译器，动态库和头文件默认使用仓库output目录；
可通过--mf-home指定安装目录，通过--log-dir保存日志。

数据校验回归会分别跳过同步写、异步写、同步读、异步读的一个方向，并让接口返回成功。
两个rank分别作为被跳过的调用方，共覆盖八种情况。示例应在数据比较时报错，并以非零退出。
正常用例应完成八次16MiB全量比较，两侧均输出ALL TESTS PASS。

测试数据按rank、测试阶段和元素位置生成。标签先乘以奇数常量，再与元素位置异或；
同一位置上的不同标签生成不同数据，避免高位标签被移位截断。
