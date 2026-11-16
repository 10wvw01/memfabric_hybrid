# BM mock-server 测试

安装 MemFabric Python 包、Torch、torch_npu 和 CANN。通过 `MMC_LOCAL_CONFIG_PATH` 指定配置文件，
配置项包括配置存储地址、world_size、protocol、HBM/DRAM 大小和 hcom_url。

双 rank 场景使用两个服务器进程；初始化请求需要在两个 rank 上同时发起。
从本目录执行，设备编号按空闲设备调整：

```bash
python3 smem_bm_server.py 127.0.0.1 33001 0 0 &
python3 smem_bm_server.py 127.0.0.1 33002 1 1 &
# 服务器就绪后，为 peer 发起初始化，再运行原客户端用例。
python3 -c 'from smem_test_common import TestClient; print(TestClient("127.0.0.1", 33002).init_smem_bm())' &
python3 test_smem_bm_g2g.py
```

`init_smem_bm` 返回整数状态码；用例检查初始化、搬运返回值和数据求和结果。

返回值回归从仓库根目录执行：

```bash
python3 -m unittest discover -s test/python/mock_server/smem_bm -p test_client_status.py -v
```
