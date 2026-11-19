# 310P SHMEM通知功能测试

先构建项目并加载CANN环境，再执行：

```bash
TEST_DEVICE_BASE=2 BLOCKS=8 ROUNDS=600 bash run.sh
```

需要同机两张可用310P卡和MPI。TEST_DEVICE_BASE指定连续设备起始编号；BLOCKS为1～8，ROUNDS是每用例轮数，默认8个block、600轮。MPI_HOME可指定MPI目录，STORE_URL可指定配置存储地址。`bash run.sh --build-only`仅编译。

覆盖同步/NBI联合通知、COPY、FP32/FP16归约、独立SET、零长度通知、全部比较条件、等待失败、非法地址、8个block并发和连续轮次。NBI突发用例提交64个数据请求后再发布通知，统一quiet，覆盖请求及暂存槽回绕。

接收方看到signal后检查完整数据，随后交换消费完成通知。API或数据检查失败时通过MPI终止任务。日志由调用方重定向至实验记录目录。
