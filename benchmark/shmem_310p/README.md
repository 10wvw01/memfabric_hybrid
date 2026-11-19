# TP=2 SHMEM与HCCL统一AllReduce基准

构建项目并加载CANN环境后执行：

```bash
DEVICE_BASE=2 BACKEND=both LOG_DIR=/path/to/logs bash run.sh
```

同一程序提供MF/HCCL后端，两进程、FP32原地SUM，测试256、512、1024、2048、4096方阵。BACKEND=both依次运行mf-signal和hccl，也可指定空格分隔的后端列表：

```bash
DEVICE_BASE=2 BACKEND="mf-sync mf-nbi mf-signal mf-signal-nbi hccl" \
 BLOCKS=8 CHUNKS=8 WARMUP=10 ROUNDS=100 LOG_DIR=/path/to/logs bash run.sh
```

| 后端 | 数据与通知组织 |
|---|---|
| mf-sync | 逐块同步put，独立SET通知 |
| mf-nbi | 分块NBI提交，统一quiet，独立SET通知 |
| mf-signal | 最后一块同步put_signal |
| mf-signal-nbi | 最后一块NBI put_signal，再统一quiet |
| hccl | HcclAllReduce，同样的输入与原地输出 |

BLOCKS取1/2/4/8，每block处理独立片段；CHUNKS是每block每阶段分块数，可取1～64的2次幂。HCCL处理整个矩阵。默认预热10轮、正式100轮；支持TIMEOUT、STORE_URL、MF_SDMA_ORCH_DIR、HCCL_BUFFSIZE，HCCL默认64MiB缓冲、启用PCIe并关闭RoCE。

每轮恢复输入并执行相同barrier后计时，完成后逐元素检查。Host总耗时从记录开始Event前到stream同步返回；Event耗时为两个Event之间的间隔；enqueue为提交调用耗时。初始化、输入恢复、barrier和检查在计时外，这三个计时不能相加。

`summarize.py LOG_DIR`按同轮两个rank的较大值聚合，输出平均、P50/P95/P99及最大值，写入日志目录summary.json。运行前确认设备可用，测试程序不会清理其他进程。基准不调整库内部等待或调度策略。
