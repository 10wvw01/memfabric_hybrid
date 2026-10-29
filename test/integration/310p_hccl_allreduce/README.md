# 310p_hccl_allreduce：卡内双芯 HCCL AllReduce 验证（真机集成测试）

## 场景

Atlas 300I Duo（Ascend310P3）**同卡两芯片**（dev N 与 N+1，同一 PCIe 总线，如
dev0/dev1 同为 0000:18:00.0）上，用官方 `HcclCommInitAll`（单进程多设备通信域，
免 MPI）执行**一次** AllReduce（INT32 SUM，1M 元素/rank），逐元素精确校验：
rank r 输入 `r*1000000+i`，两 rank 之和期望 `1000000+2i`。

## 验证什么

1. **数据正确性**：同卡双芯 2-rank 单次 AllReduce 逐元素全对。两个变体：
   默认展开模式、`HCCL_OP_EXPANSION_MODE=AI_CPU`（点名 AICPU 展开）。
2. **设备侧执行链**（机制验证）：开
   `ASCEND_GLOBAL_LOG_LEVEL=0 ASCEND_SLOG_PRINT_TO_STDOUT=1` 可见证据链
   `launch_aicpu.cc → LoadBinaryFromFile(ccl_kernel.json) →
   RegisterCpuKernel[HcclKernelEntrance, libccl_kernel.so]`（每 rank 一次），
   另有 `TbeVectorReduce Init`（AICore 归约算子）与 ring 算法选择——即 hcomm
   设备库 `libccl_kernel.so` 的 AICPU 入口经 KFC 通道执行
   （`ccl_kernel.json` 的 `opKernelLib=KFCKernel`，与本项目 epoch kernel 同通道）。
   详见 `docs/310p/hcomm-GM标志消息同步机制.md`。

## 必要条件

- Atlas 300I Duo（Ascend310P3）+ CANN ≥ 9.1
- 同卡设备对空闲（默认 dev0/dev1；`TEST_DEVICE_BASE` 可换对）

## 运行

```bash
bash run.sh                       # 编译并运行两个变体
bash run.sh --build-only          # 仅编译（产物 /tmp/mf_310p_hccl_allreduce）
bash run.sh --run-only            # 仅运行
TEST_DEVICE_BASE=4 bash run.sh    # 用 dev4/dev5（避开占用设备）
```

期望输出：两变体均 `== PASS: 卡内 devN+devN+1 一次 AllReduce(INT32 SUM) 结果正确 ==`。

## 实测纪律（踩坑记录，2026-10-29）

1. `HcclCommInitAll` 前必须先 `aclrtSetDevice`——否则 HCCL_E_RUNTIME(15)，
   日志根因 `rtGet device fail, please make sure that device is set`。
2. 集合通信的**提交会阻塞等对端参与**——rank 必须各自线程并发出；单线程顺序
   下发 ~2 分钟后 HCCL_E_TIMEOUT(9)。
3. rank 粒度=芯片：本测试覆盖"同卡双芯"通信；单芯片内部 die0/die1 跨 Die 路径
   由 [310p_sdma_stability](../310p_sdma_stability/README.md)（SDMA mail，SQE 按
   die 分 streamid）覆盖。
