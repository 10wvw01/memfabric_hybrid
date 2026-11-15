# BM 全路径测试

从仓库根目录执行，需要安装本次构建的 MemFabric Python 包、Torch、torch_npu 和 CANN。

脚本固定两个 rank，每个 rank 使用 1 GiB HBM、8 GiB DRAM，沿有效搬运路径串联复制，
最后比较首尾数据的校验值。初始化、搬运、校验或子进程失败时返回非零退出码。

默认协议为 device_rdma，需要可用的设备 RDMA 网卡。310P 同卡两个 die 可选择 SDMA：

```bash
python3 test/python/hybm/data_op_all_path_test.py --devices 0 1 --data-op-type sdma --error stop --repeat 1
```

参数：

- `--devices`：两个设备编号。
- `--data-op-type`：`device_rdma` 或 `sdma`，默认 `device_rdma`。
- `--error`：`stop` 遇错停止，`bypass` 记录错误并继续；两者都会将失败反映到退出码。
- `--repeat`：整条路径的重复次数，默认 1。

入口与退出状态回归：

```bash
python3 -m unittest discover -s test/python/hybm -p test_data_op_entrypoint.py -v
```
