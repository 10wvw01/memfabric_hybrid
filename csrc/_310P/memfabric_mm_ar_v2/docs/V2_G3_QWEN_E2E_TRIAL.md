# V2 G3：Qwen3.6 指定 M 区间融合、其余 M 走 Stock

> 状态：源码待 310P3 实机编译/验证。不要把静态审视称为实机 PASS。
> 基线：最新 V2 T0/T1/T2 实机报告；V1 核心实现保持不变。

## 目标与路由

针对 Qwen3.6-35B-A3B-w8a8、310P3 TP=2 的 10 个 full-attention
self_attn.o_proj + 30 个 GDN linear_attn.out_proj（两类为未量化 FP16，
K_local=N=2048，NZ 权重），固定规则：

- M < 2048 或 M > 4096：原生 unquantized Linear MM + HCCL AllReduce；
- 2048 <= M <= 4096：V2 T0 MM + MemFabric SDMA + FP16 Add 单 Kernel，chunk_rows=1024，
  MM(k) 可覆盖 SDMA(k-1)；
- 其他模型、层、量化及 TP 形状一律维持原始路径；
- V2 路由默认 OFF，绝不自动使用旧的 V1 模型推理 Runtime；
- 资格选层复用 V1 既有 40 层模型检查，V2 的 Torch Op 和上下文独立。

关键代码：csrc/_310P/memfabric_mm_ar_v2/src/torch_adapter/mf310p_v2_serve.cpp，
vllm_ascend/_310p/ops/memfabric_mm_ar_v2_serve.py，modelslim_config.py。

## 本轮 M 的实际含义：动态行数与 4096 上限

**用户约束**：每条请求输入最多 4096 tokens；本次性能用例的生成输出为 2048 tokens，并发 10。
**调度约束**：服务脚本明确设置 `--max-num-batched-tokens 4096`，
因此本次服务模型单次 Forward 中各请求合并后的有效 token 行数 M
按 **M≤4096** 验收。一个请求的输入长度和 M 不是同一个量：
M 是同一次模型 Forward 进入目标 `o_proj/out_proj` MM 的 token 总行数。

“动态 M”只是说每次 Forward 的 M 会变化，例如 Decode 并发 10 时 M≈10；
一次 Prefill 可以是 M=1024、2048、3000、4096，遵循固定策略：

- M<2048 或 M>4096：Stock = 原生 MM + HCCL AR。
- M=2048、3072、4096：V2 直接执行（chunk_rows=1024 时恰好整块）。
- M=2049、3000、4095：V2 使用预分配的 input stage，向上补至
  3072/3072/4096 行，并只返回真实 2049/3000/4095 行；
  多算的填充行不是有效输出，Staging 的开销计入性能数据。

**本轮不做多段计算**：原先的 M>8192 多次拆分并非当前端到端场景需要，
已经从 G3 Serving 代码中去除。V2 融合仅针对 2048≤M≤4096；
如果运行时意外出现 M>4096，模型路由**明确回退到原生 MM + HCCL AR**，
不触发 V2、也不拆分成多个融合波次。它属于诊断配置异常的安全回退，
可记录日志排查 `max-num-batched-tokens`，但不应中断正常请求。
直接调用底层 V2 Torch Op 仍有 M≤4096 的输入校验，防止绕过模型路由。
T0 独立算子此前 M=8192 的实机正确性及性能数据仍然有效，
只是不属于本轮 Serving 目标范围。


## 已知问题处理及未消除风险

1. 原 T0 每次换 y 都会触发全设备同步（0～15s 随机）。改为 64 地址有界记录：
   已实际触发守护的持久地址，不因循环槽位轮换而重复 Device Sync。
2. 服务启动首次大 M：在 MemFabric 池创建**之前**预分配并固定持有
   4 个（可设 2～16）[4096,2048] FP16 输出，以及 4096-row input stage。
   池创建后执行 IB 探针，并将全部输出槽依次通过真实 MM→SDMA→Add→DONE 预热、
   同步和协议检查；目的是把所有首次 Device Sync 移到初始化窗口。
3. 输出 Tensor/视图持有的 Storage 未释放之前**严禁槽位复用**；
   耗尽立即报错，不能静默覆盖先前结果。
4. 此方法只是缓解已知 MTE3 首写问题的待测设计；不能声称已经修复内存一致性
   的硅级根因。首启有 0～15s×槽位数量随机开销和跨 Rank 看门狗风险。
   任意首写丢失、错结果或 AICore 507014，均为 P0 失败，不允许继续性能测试。
5. KFC 控制面正常初始化仍未经本轮验证；必须用健康设备或已正确外部部署的
   MemFabric Run 包验证，禁止把 assume-predeployed 绕过称为正常部署。
6. 单 MemFabric Context、单 eager stream、TP 两 Rank 相同调用顺序；
   协议错误 Fail-stop 并要求两 Rank 一起重启，不做单侧降级。
7. ACL Graph 限定 FULL_DECODE_ONLY 且所有 capture size <2048：
   Decode 的原生 HCCL 可被捕获；大 M V2 prefill 保持 Eager。
   捕获大 M V2 会明确报错（不能声称支持 V2 Graph replay）。
8. V2 与 HCCL 同一 TP2 进程交替、缓冲池与输入 stage 的跨 Kernel 可见性、
   Memory allocator 复用以及长时间连续负载，仍需专项设备验证。

## 开关和构建

在服务器安装 CANN 9.1、MemFabric wgm-dev-310p 当前 run 包，并且：

    git checkout feat/310p-memfabric-public-api && git pull
    source /usr/local/memfabric_hybrid/set_env.sh
    export SOC_VERSION=ascend310p3
    export ASCEND_RT_VISIBLE_DEVICES=0,1
    export VLLM_ASCEND_310P_ENABLE_MEMFABRIC_MM_AR=1
    export VLLM_ASCEND_310P_ENABLE_MEMFABRIC_MM_AR_V2=1
    python setup.py build_ext --inplace

运行时**只有 fused serve 进程**开启 V2：

    export VLLM_ASCEND_310P_MEMFABRIC_MM_AR_V2_SERVE=1
    export VLLM_ASCEND_310P_MEMFABRIC_V2_CHUNK_ROWS=1024
    export VLLM_ASCEND_310P_MEMFABRIC_V2_OUTPUT_SLOTS=4
    export VLLM_ASCEND_310P_MEMFABRIC_STORE_URL=tcp://127.0.0.1:8581

Stock serve 须关闭 V1/V2 runtime 使能。V2 route 仅在 V2 Serve flag=1 时选择。
若需调整输出池槽数，双 rank 参数必须一致。4 槽将额外常驻约 64 MiB
输出加 16 MiB input stage / rank，独立于 MemFabric 池。

## 分阶段 310P3 验收

### Gate A：链接、真实预热与初始窗口

- 两个 build 开关启用，检查 torch.ops._C_ascend 暴露
  memfabric_mm_ar_v2_allreduce、memfabric_mm_ar_v2_debug_status、memfabric_mm_ar_v2_shutdown。
- 真实 MM/SDMA/Add 的初始化预热，不允许仅 enable=0/no-op 充当首次真实预热。
- 两 Rank 初始化各槽耗时/最慢 Rank 偏斜、status=0、首次结果位精确。
- 检查第二次及后续同槽使用不会再运行 aclrtSynchronizeDevice。

### Gate B：TP=2 混合通信 / shape 正确性

    torchrun --standalone --nproc-per-node=2 \
      csrc/_310P/memfabric_mm_ar_v2/test/e2e/tp2_v2_mixed_route_correctness.py \
      --rows 2047 2048 2049 3000 3073 4095 4096 4097 5000 \
      --chunk-rows 1024 --reps 3

- 2047 / 1 / 10 / 4097 / 5000 = Stock HCCL（MemFabric 池存活后仍然使用 HCCL）。
- 2048 / 2049 / 3000 / 3073 / 4095 / 4096 = V2；与
  F.linear(x,w).float() → HCCL AllReduce → FP16 的参考位精确。
- stream sync 后 V2 status=0；补测 256/512/1024 chunk，多数据 seed、
  持续引用所有输出槽导致正确拒绝复用、动态地址和 TP 偏斜。
- 进一步完成至少 1000 次混合调用、20 次新进程创建/退出。

### Gate C：真实 40 层路由、功能、ACL Graph

    MEMFABRIC_MM_AR_E2E_ENABLED=1 MEMFABRIC_MM_AR_E2E_V2=1 \
      pytest -sv tests/e2e/_310p/test_memfabric_mm_ar_functional.py

必须记录 10 + 30 个目标层，首次 [mf310p-v2] 小 M Stock / 大 M V2 日志。
Eager 与 FULL_DECODE_ONLY Graph、并发 10、确定性 prompt 输出与 Stock 一致。
发生运行时 MTE3、507014、KFC 问题应立即停止本 Gate，而非增大超时掩盖。

### Gate D：与 Stock 同口径端到端性能

    MEMFABRIC_MM_AR_E2E_V2=1 MEMFABRIC_MM_AR_E2E_REPEAT=3 \
      bash tests/e2e/_310p/bench_memfabric_mm_ar_serve.sh \
      /tmp/mf_v2_g3_serve \
      "stock graph" "fused graph" "stock eager" "fused eager"

默认 input=4096、output=2048、50 requests、concurrency=10；先 Graph 主验收，
再 Eager 功能诊断。两组除了融合开关外使用相同模型、设备、采样形状、
token budget、runtime 参数。记录成功率、吞吐 output tok/s、median/P95/P99
TTFT、TPOT、峰值显存、冷启动耗时。至少 3 次独立会话，避免将波动当收益。

### Gate E：长稳

- Graph 模式真实推理连续 10 分钟以上，最好 30 分钟；
- 500+ 请求、1000+ V2 wave、不同 M/输出池复用、TP skew；
- 0 死锁、0 507014、0 静默数值错误、0 协议非零、无 KFC 旁路；
- 增加 DFlash/普通 Qwen 组合 A/B（以项目目标实际部署配置为准）。

## 三次独立 Stock/V2 会话与自动性能判定

已有 serve 脚本中的 REPEAT=3 是**同一 server 的三次测量**，不等于
三个独立启动会话。为避免误判，推荐每次重启 Stock/V2 且保存独立目录：

    for run in 1 2 3; do
      MEMFABRIC_MM_AR_E2E_V2=1 MEMFABRIC_MM_AR_E2E_REPEAT=1 \
        bash tests/e2e/_310p/bench_memfabric_mm_ar_serve.sh \
        /tmp/mf_v2_g3_session_$run "stock graph" "fused graph"
    done

    python csrc/_310P/memfabric_mm_ar_v2/test/e2e/evaluate_v2_g3.py \
      /tmp/mf_v2_g3_session_1 /tmp/mf_v2_g3_session_2 /tmp/mf_v2_g3_session_3

自动验收要求至少三套独立 Stock/V2 配对结果，且每次请求数全数完成；
还会检查 fused 服务日志是否有大 M V2 真正执行的证据。三个指标均要求
三会话中位改善不少于 2% 且至少 2/3 会话方向正确：
吞吐越高越好，TTFT/TPOT 越低越好；P99 不得有超过 5% 的明显退化。
2% 为验收用预置防噪阈值，不是由当前 V2 实机数据证明的合理性下限，
可以在实验前基于 Stock 自身跨会话分布调整，**不能事后调门槛使结果 PASS**。
任一 FAIL 意味着尚未证明端到端超越 Stock。单独的 Eager/Graph 功能、长稳与
健康 KFC Gate 仍需独立通过。

## 验收标准

Stock 是唯一性能收益基线。V2 必须在相同工作负载中同时表现为：
**吞吐量更高、TTFT 更低、TPOT 更低**，且改善超过 Stock 本身
会话波动；不能只靠 T0 单层的 M=4096/8192 性能收益判通过。尾延迟不能显著退化。

报告：csrc/_310P/memfabric_mm_ar_v2/V2_G3_QWEN_E2E_REPORT.md，
包括完整命令/环境、两 Rank 首次预热延迟、功能与错误日志、三会话性能矩阵、
新增 BUG 修复与全套 T0/T1/T2/V1 回归。当前没有实机结论。
