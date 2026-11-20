# smem_mm_iar_310p3 — TP2 MatMul + AllReduce 通算融合算子（硬件归约版）

基于 memfabric_hybrid 的 MemFabric SDMA 对称内存池，在 Ascend C 单算子内完成
**矩阵乘 + TP2 AllReduce**：一次调用替换 vllm-ascend 推理中 RowParallelLinear
（o_proj / O.out）的 `MM + tensor_model_parallel_all_reduce` 两步。

- **归约机制：硬件归约（hardware reduction）**——通信交换用 ADD_FP16 半交换
  （每端把自己的非 owned 半区以 SDMA 途中加法落入对端同偏移，再以 COPY 把已
  终值的 owned 半区回传对端），加法由 SDMA 引擎在传输途中完成，AIV 侧只做
  尾部纯拷出（work→y）。
- 研发出身：本算子是研发期工程 `smem_mm_ar_inline_reduce` 的 **flavor 11 部署档**，
  收尾规整（run8/run11/run12/run13 锁定版本）独立成包并全链改名。协议逻辑与
  锁定版本等价；**2026-11-19 ABI 减参（13→8 属性）**：删除 5 个锁定场景下的
  常量属性，固化为 kernel 编译期常量（见契约节），协议行为零变化；
  **2026-11-19 池布局自治（8→7 属性）**：组完成通知数组并入池内 seats 区
  （删 `ibPtr`），池内区域划分收编到 tiling.h 布局推导段成为唯一事实源，
  建池下限与行数契约经 xops wheel 程序化导出（`pool_floor_bytes()` 等），
  会话封装层（`xops.SmemSession`）随 wheel 发布，外部不再需要掌握池内布局；
  **2026-11-20 分层重构（封版版）**：kernel 单体拆为 tiling.h（布局）/
  protocol.h（协议常量与错误码）/ transport.h（传输原语）/ cpp（三侧编排，
  入口拆 `fused_matmul`/`allreduce`）四层，命名统一（变量小驼峰、函数
  snake_case），契约自检 Check 删除（op_host tiling 期前置校验成为唯一
  契约门），运行期失败码枚举化为 `ProtocolError`（数值不变），协议行为
  零变化（构建 + M2 全矩阵逐位对拍 + op_bench 锚点漂移带内三重背书）。
- 同族姊妹算子：`smem_mm_ar_310p3`（本地归约版，全量 COPY + 本地加法并行处理分支），
  见 `../smem_mm_ar_310p3/`。两者 ABI/接口同构，一个 serve 会话二选一启用。

## 目录结构

| 路径 | 内容 |
|---|---|
| `op_kernel/smem_mm_iar310p3.cpp` | 三侧逻辑编排 + kernel 入口：入口按 `__DAV_CUBE__` 编译期隔离拆分为 `fused_matmul`（Cube 侧：AIC 8 核纯 MM）与 `allreduce`（Vector 侧：三游标调度器 + 4 条尾部拷出并行处理分支 + 1 个完成通知消费者核） |
| `op_kernel/smem_mm_iar_310p3_tiling.h` | tiling 数据结构（7 属性）+ **池内布局推导（唯一事实源）** |
| `op_kernel/smem_mm_iar_310p3_protocol.h` | 协议层：协议常量、阶段号/device_tag、标记位域、轮询预算/退避、运行期错误码（`ProtocolError` 枚举）、诊断 trace 布局 |
| `op_kernel/smem_mm_iar_310p3_transport.h` | 传输层：SDMA 搬运与标记原语（`flush`/`check`/`poll_flag`/`probe_flag`/`probe_seats`/`issue_reduce_leg`/`issue_gather_leg` 等） |
| `op_host/smem_mm_iar_310p3.cpp` | 算子定义/tiling/shape 推导（CANN OpDef；tiling 期校验池下限） |
| `CMakeLists.txt` `CMakePresets.json` `build.sh` | CANN 自定义算子包构建（产物 `.run`） |
| `xops/` | torch 扩展（注册 `torch_npu.npu_smem_mm_iar_310p3`）+ 契约查询导出 + `SmemSession` 会话封装层 |

命名链（坑：CANN GA executor 注册表不接受带下划线的算子类型名，ABI 必须
CamelCase）：

| 层 | 名字 |
|---|---|
| op 类型（ABI） | `SmemMmIar310p3` |
| aclnn 符号 | `aclnnSmemMmIar310p3` / `aclnnSmemMmIar310p3GetWorkspaceSize` |
| kernel 入口/文件 | `smem_mm_iar310p3`（框架由 ABI 机械派生：大写字母前插下划线、310p3 前不插，故与 iar 连写；文件/入口名不得手改） |
| vendor / 包目录 | `smem_mm_iar_310p3` |
| torch op | `torch_npu.npu_smem_mm_iar_310p3` |

## 算子输入输出规格说明

### 语义

```
y = AllReduce_2rank( x @ w^T )        # fp16，SUM；两 rank 各持 w 的一半（TP2 分片）
```

两 rank 以相同 M/epoch 锁步各发射一次调用。通信为 RS+AG 半交换分解（TP=2 退化
形态）：REDUCE 腿把非 owned 半区 ADD 到对端，GATHER 腿把已终值的 owned 半区
回传；收敛后 work 即全量和，AIV 尾部车道纯拷出 **直写 y**（无 pad、无 staging
拷贝、无独立 AR 算子）。

### 张量

| 参数 | 形状/dtype/布局 | 说明 |
|---|---|---|
| `x` | `[M, 2048]` fp16 **ND** 连续 | 本 rank 的激活（TP 输入并行分片后的行块） |
| `w` | `[2048, 2048]` fp16 **FRACTAL_NZ 物理**（声明 ND） | 本 rank 的权重分片；`npu_format_cast(w, 29)` 产物。OpDef 原型声明 ND，xops 层手工构造"ND 标记 + NZ 存储"的 aclTensor（详见 xops 源码注释） |
| `y` | `[M, 2048]` fp16 **ND** | 调用方预分配；kernel 直写终值 |

### 契约（op_host tiling 期前置校验为唯一契约门，violation 即拒发射）

- `K == N == 2048`（编译期常量，kernel 硬编码）
- `M ∈ [1024, 4096]` 且 `M % 256 == 0`（去 pad 契约：尾 MM 组/末通信子组可部分行，原生 M 直传）
- 对称池 `symm ≥ kPoolFloor`（= 32MiB + 8KiB，由算子自身规格推导：最大矩阵
  4096×2048×fp16 → work 16MiB + scratch 16MiB + 控制区 + seats；**程序化获取**：
  `xops_iar_310p3.POOL_FLOOR_BYTES`，与 kernel/op_host 同源 tiling.h，无第二份
  手工数字）
- kernel 编译期常量（原为属性传入，2026-11-19 减参固化）：SDMA 包长 1MiB、
  通信子组 1024 行（与 MM 组同粒度；512 档在锁定基准中从未使用，删除）、
  矩阵乘 8 核（BlockDim=15：8 AIC + 7 AIV）、无预热门（预热职责在会话层，
  见下"调用纪律"第 1 条）
- 对称窗口不变量：两端池基址必须相同（kernel 从 `gva + symm*(1-rank)`
  推算对端地址，不再接收 peerGva；会话封装层负责 `peer_gva == gva` 强校验）

契约校验全部在 op_host tiling 期完成并拒绝发射（kernel 侧契约自检 Check
已随 2026-11-20 分层重构删除——violation 不会到达 kernel）。kernel 运行期
仍有失败路径，错误码写 `statusPtr` 供宿主定位，已枚举化为 `ProtocolError`
（数值与宿主诊断脚本兼容）：101 标记包投递失败 / 102 数据包投递失败 /
121 完成通知状态非 OK / 155 尾部拷出分支死等 / 157 调度器事件循环死等 /
105 排空超时。

### 7 个 int64 属性（xops/aclnn 形参序）

| # | 属性 | 含义 |
|---|---|---|
| 1 | `gva` | 本 rank 池基址（对称窗口 VA） |
| 2 | `symm` | 对称窗口大小（字节） |
| 3 | `statusPtr` | kernel 状态字设备指针（每次调用前清零，非 0 = 失败码） |
| 4 | `tracePtr` | 诊断 trace 缓冲设备指针（4KiB） |
| 5 | `rank` | 本 rank（0/1，与池成员一致） |
| 6 | `epoch` | 批次号；**每调用 +1**（标记/组完成通知以 epoch 为真值，跨批次防串扰） |
| 7 | `rows` | M（本波行数） |

### 对称池布局（唯一事实源：op_kernel/smem_mm_iar_310p3_tiling.h 布局推导段）

```
work[0, 16MiB)     本端 MM 结果（AIC 写）；REDUCE 的源 / GATHER 的落点
scratch[16, 32MiB) （本算子不使用；半交换无 scratch 落点）
ctrl 区 32MiB+64   8B 控制信号暂存（对端 8B 收件在 +64）
peerRdy 32MiB+512  对端 RDONE 就绪标记（调度器 C 游标探测）
ready   32MiB+1024 对端 GDONE 就绪标记（尾部拷出分支单门）
        +512 偏移起 对端 seatUp 通知槽（调度器 A 游标发布、B 游标探测）
seats   32MiB+4096 组完成通知数组（原池外 ib 缓冲，2026-11-19 并入池内；
                   4 组 × 512B 需求配 4KiB，2 倍余量）
建池下限 kPoolFloor = 32MiB + 8KiB（sum 预留区已删，属旧协议遗留）
```

### 调用纪律（会话侧义务）

1. **预热职责在会话层**（原 enabled=0 预热门已随 2026-11-19 ABI 减参删除）：
   进程/池新建后的**第一次 kernel 发射存在冷首发数据竞态窗口**——算子
   二进制装载与首次真实调度批次的 SDMA 数据竞争，在部分设备对的时序上
   咬合（2026-11-19 设备 2/3 实证；ar 受影响、iar 单包腿在实测中未受
   影响，但同契约约束）。会话侧义务：正式校验前两端锁步发射**一次不
   校验的暖机调度批次**（m2 测试即此实现）；vllm 会话以首调用逐位校验门
   兜底（冷窗腐化会响亮失败并要求双端重启，比静默哑预热更安全）；
   `status` 每次调用前清零。
2. **epoch 单调递增**，两 rank 同值同增（锁步）。经 `SmemSession` 使用时由
   封装层内管，外部不接触。
3. **通道缓冲**（status/trace）由会话封装层持有，进程生命周期内不释放；
   组完成通知数组已并入池内 seats 区，会话不再分配池外通知缓冲。
4. 池驻留期间**禁止逐 op `synchronize()`**（SDMA 完成退化为 ~500ms 看门狗）。

## 编译部署及使用教程

### 前置依赖

| 依赖 | 版本/位置 | 说明 |
|---|---|---|
| CANN | **9.1.0 GA**（`/usr/local/Ascend/cann-9.1.0`） | 任何操作前 `source $CANN/set_env.sh`；beta 版禁用 |
| memfabric_hybrid SDK | `v28-orch` 分支（KVER=28）构建产物 | 见下节；kernel 编译需要其设备侧头文件，运行期需要 `libmf_smem.so` + orch |
| torch / torch_npu | 2.10 / 2.10 预编译 wheel | xops 扩展编译依赖 |
| 设备 | Ascend 310P3 双 die，TP=2 | 对称池要求两 rank 同卡双 die |

### 1. 编译算子包（.run）

```bash
source /usr/local/Ascend/cann-9.1.0/set_env.sh
cd prd_code/smem_mm_iar_310p3
bash build.sh
# 产物：build_out/custom_opp_ubuntu_aarch64.run
# 展开：build_out/_CPack_Packages/Linux/External/custom_opp_ubuntu_aarch64.run/
#       packages/vendors/smem_mm_iar_310p3/
```

说明：kernel 引用 memfabric 设备侧头文件（`smem_shm_aicore_base_sdma.h`），
include 路径在 `op_kernel/CMakeLists.txt`。默认指向本工作区已安装位置；SDK
在别处时构建前 `export MEMFABRIC_SDK_INC=<路径>/include/smem/device` 覆盖。

### 2. 构建 xops wheel（torch 入口）

```bash
source /usr/local/Ascend/cann-9.1.0/set_env.sh
export ASCEND_CUSTOM_OPP_PATH=$PWD/build_out/_CPack_Packages/Linux/External/\
custom_opp_ubuntu_aarch64.run/packages/vendors/smem_mm_iar_310p3
cd xops && bash build_and_install.sh    # 装入 xops_smem_mm_iar_310p3 wheel
```

### 3. 运行期环境（调用算子的进程启动前设置）

```bash
export ASCEND_CUSTOM_OPP_PATH=<...>/vendors/smem_mm_iar_310p3
export MF_SDMA_ORCH_DEPLOY_HELPER=<memfabric>/tools/aicpu_kernel_deploy/orch_deploy_helper
export MF_SDMA_ORCH_JSON=<memfabric output>/hybm/aicpu_kernel/libmf_sdma_orch_v28.json
export MEMFABRIC_HYBRID_HOME_PATH=<memfabric output>
export MEMFABRIC_HYBRID_EXTEND_LIB_PATH=<memfabric output>/hybm/lib64
export MIXFABRIC_SDK_OUT=<memfabric output>
export LD_LIBRARY_PATH=<memfabric output>/smem/lib64:<memfabric output>/hybm/lib64:\
<memfabric output>/acc_links/lib64:$ASCEND_CUSTOM_OPP_PATH/op_api/lib:$LD_LIBRARY_PATH
```

### 4. 使用（推荐：xops 会话封装层）

```python
import xops_iar_310p3                    # 注册 torch_npu.npu_smem_mm_iar_310p3
                                        # 并提供 SmemSession 封装层
sess = xops_iar_310p3.SmemSession(       # 建池尺寸 = max(64MiB, 布局下限)，
    device=dev, rank=rank,              # 布局下限程序化导出，无需读文档
    store_url="tcp://127.0.0.1:8730")  # 或环境变量 SMEM_MM_310P3_STORE_URL
y = sess.launch(x, w_nz, rows)          # epoch 内管；out= 可传预分配输出
...
sess.teardown()                         # 退出前必调（否则进程退出时 core dump）
```

封装层内建不可违反的约束：建池尺寸下限校验、`peer_gva == gva` 强校验、
行数契约校验（`[1024, 4096]`、256 对齐）、epoch 递增。裸调用
`torch_npu.npu_smem_mm_iar_310p3(x, w_nz, y, gva, symm, status_ptr, trace_ptr,
rank, epoch, rows)` 仍可用（7 个 int64 属性，见契约节），但需自行保证上述
约束与 status/trace 通道（64B/4KiB）分配。

E2E 消费路径（vllm-ascend）：`enable_smem_mm_iar_310p3` 配置项，见
`../vllm-ascend/vllm_ascend/ops/smem_mm_310p3.py`（会话引导/接管/fallback 全
在 wrapper 内）。

### 与 smem_mm_ar_310p3 共存

两算子是独立 vendor 包，**一次进程激活一个**（`ASCEND_CUSTOM_OPP_PATH` 指向
对应 vendor；aclnn 符号经 libcust_opapi.so 按激活包解析）。vllm serve 用
`SMEM_OP=ar|iar` 切换（见 `../benchmark/e2e_perf/vllm_serve_smem.sh`）。

## memfabric_hybrid 依赖编译部署教程

算子对 memfabric_hybrid 的依赖分两层：

1. **编译期**：设备侧 SDK 头文件（`smem_shm_aicore_base_sdma.h` 等）——kernel
   `#include` 引用，构建算子包时经 `-I` 引入（上节 MEMFABRIC_SDK_INC）。
2. **运行期**：`libmf_smem.so`（池会话/SDMA 编排 API）+ orch（AI CPU 侧 SDMA
   编排器内核，由部署助手自动装载）。

### 构建

```bash
git clone <memfabric_hybrid 仓地址> && cd memfabric_hybrid
git checkout v28-orch          # 血统红线：必须 v28（KVER=28）；其它分支会死等 orch
bash script/build.sh           # 默认 RELEASE + cmake；产物落 output/
# 产物：output/smem/lib64/libmf_smem.so
#       output/hybm/aicpu_kernel/libmf_sdma_orch_v28.json   # orch 内核清单
#       output/hybm/lib64/、output/acc_links/lib64/ 等
```

orch 部署助手（常规使用随产品安装包分发，无需手动构建；若需重建）：

```bash
cd memfabric_hybrid/tools/aicpu_kernel_deploy
make ASCEND_HOME_PATH=/usr/local/Ascend/cann-9.1.0
# 产物：orch_deploy_helper（运行期 MF_SDMA_ORCH_DEPLOY_HELPER 指向它）
```

### 部署形态

- 运行期通过环境变量注入（见上节"运行期环境"）；orch 内核由库内置自动部署链
  （fork+exec orch_deploy_helper）按 `MF_SDMA_ORCH_JSON` 清单装载，无手工步骤。
- 版本纪律：**v28-orch ↔ CANN 9.1 GA ↔ 本算子包** 三者绑定；换 CANN 须重建
  memfabric 与算子包并重装 vllm-ascend C 扩展。

## 验证

功能与性能验证件在 `../benchmark/`：

- 功能（M2 双 rank 逐位对拍）：`benchmark/functional/run_m2.sh`
- 算子级性能（stock vs ar vs iar）：`benchmark/op_perf/run_op_bench.sh`
