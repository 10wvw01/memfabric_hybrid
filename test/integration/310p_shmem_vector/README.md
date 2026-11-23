# 310P Vector 请求队列验证

在`smem_shm_init`前调用`smem_shm_config_init`获取默认配置，再按需设置生产者掩码：

```cpp
smem_shm_config_t config{};
smem_shm_config_init(&config); // sdmaProducerMask默认为0x00FF
config.sdmaProducerMask = 0x7FFF;
smem_shm_init(storeUrl, worldSize, rankId, deviceId, &config);
```

Python使用`memfabric_hybrid.shm.ShmConfig.sdma_producer_mask`。掩码在本次初始化到uninit期间固定，
随后创建的通信池继承配置。掩码按两个8位段划分：

| 位段 | 位段掩码 | 用途 |
|---|---|---|
| bit0～7 | 0x00FF | AI Core |
| bit8～15 | 0xFF00 | Vector，其中bit15预留且必须为0 |

bit15对应的队列槽继续预留，初始化和设备提交都拒绝使用该位。
有效掩码支持非零的0x7FFF子集；0x7F00允许当前7个Vector使用队列8～14，
0x7FFF允许AI Core与Vector共同提交。核的启动方式与实际工作分配由调用kernel决定。
每条队列由唯一执行主体生产；多个并发kernel须由调用方保证相同block队列不并发写入。
扩大掩码只改变提交权限和扫描集合，不会自动启动更多核。开放高编号队列时校验设备核数。

配置使用原结构体的对齐空间，大小仍为20字节，flags仍位于偏移16。
Host/AICPU参数增加生产者掩码后为88字节，协议标识为0x5D0A000A。
Host库、AICPU内核与设备头文件使用同一次构建产物。加载CANN环境后构建：

```bash
cmake -S test/integration/310p_shmem_vector -B build-vector-queues \
  -DCMAKE_BUILD_TYPE=Release -DMEMFABRIC_PREFIX="$MEMFABRIC_HYBRID_HOME_PATH"
cmake --build build-vector-queues -j4
export MF_SDMA_ORCH_DIR="$MEMFABRIC_HYBRID_HOME_PATH/hybm/aicpu_kernel"
export LD_LIBRARY_PATH="$MEMFABRIC_HYBRID_HOME_PATH/lib:$LD_LIBRARY_PATH"
export TEST_DEVICE_BASE=4
mpirun --allow-run-as-root --stdin none -np 2 build-vector-queues/vector_queue_test
mpirun --allow-run-as-root --stdin none -np 2 build-vector-queues/vector_queue_test 0x7F00 600
mpirun --allow-run-as-root --stdin none -np 2 build-vector-queues/vector_queue_test 0x7FFF 600
```

`TEST_DEVICE_BASE`选择连续两张卡，`STORE_URL`可指定配置存储地址。
不传掩码时验证初始化默认值0x00FF，第二个参数为重复次数，默认600。

混合kernel启动15个逻辑block，记录每个block是否运行及AI Core/Vector编译分支。
rank0的允许队列重复提交put、get、携带通知的put和独立通知，然后quiet；rank1提供源数据并接收写入。
检查完整目标片段、通知、每条队列的head/tail及最后完成序号，覆盖512次暂存槽复用。
未开放的block及编号15、UINT32_MAX验证各提交路径和quiet返回失败，队列保持未提交状态。

需要检查epoch重启时，使用1秒epoch预算并启用测试侧的观察：

```bash
export MF_SDMA_ORCH_EPOCH_BUDGET_S=1 CHECK_EPOCH=1
mpirun --allow-run-as-root --stdin none -np 2 build-vector-queues/vector_queue_test 0x7FFF 30000
```

测试在Event完成前采样工作区心跳，检查生产请求期间观察到epoch重启，
随后仍检查所有队列的最终游标、完成序号和数据。
