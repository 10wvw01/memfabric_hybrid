# smem_mm_ar_310p3 — TP2 MatMul + AllReduce 通算融合算子（本地归约版）

基于 memfabric_hybrid 的 MemFabric SDMA 对称内存池，在 Ascend C 单算子内完成
**矩阵乘 + TP2 AllReduce**：一次调用替换 vllm-ascend 推理中 RowParallelLinear
（o_proj / O.out）的 `MM + tensor_model_parallel_all_reduce` 两步。

- **归约机制：本地归约（local reduction）**——通信交换用全量 COPY（每端把
  自己的 MM 结果整组拷进对端 scratch 区），归约加法由本端 AIV 车道本地执行
  （work + scratch → y）。
- 研发出身：本算子是研发期工程 `smem_mm_ar_inline_reduce` 的 **flavor 10 部署档**，
  收尾规整（run8/run11/run12/run13 锁定版本）独立成包并全链改名。协议逻辑与
  锁定版本逐字一致。
- 同族姊妹算子：`smem_mm_iar_310p3`（硬件归约版，ADD_FP16 半交换 + 旗标收敛），
  见 `../smem_mm_iar_310p3/`。两者 ABI/接口同构，一个 serve 会话二选一启用。

## 目录结构

| 路径 | 内容 |
|---|---|
| `op_kernel/smem_mm_ar310p3.cpp` | Ascend C kernel（AIC 8 核纯 MM + AIV 调度器 + 4 条本地归约加法车道） |
| `op_kernel/smem_mm_ar_310p3_tiling.h` | tiling 数据结构（13 属性镜像） |
| `op_host/smem_mm_ar_310p3.cpp` | 算子定义/tiling/shape 推导（CANN OpDef） |
| `CMakeLists.txt` `CMakePresets.json` `build.sh` | CANN 自定义算子包构建（产物 `.run`） |
| `xops/` | torch 扩展（注册 `torch_npu.npu_smem_mm_ar_310p3`） |

命名链（坑：CANN GA executor 注册表不接受带下划线的算子类型名，ABI 必须
CamelCase）：

| 层 | 名字 |
|---|---|
| op 类型（ABI） | `SmemMmAr310p3` |
| aclnn 符号 | `aclnnSmemMmAr310p3` / `aclnnSmemMmAr310p3GetWorkspaceSize` |
| kernel 入口/文件 | `smem_mm_ar310p3`（框架由 ABI 机械派生：大写字母前插下划线、310p3 前不插，故与 ar 连写；文件/入口名不得手改） |
| vendor / 包目录 | `smem_mm_ar_310p3` |
| torch op | `torch_npu.npu_smem_mm_ar_310p3` |

## 算子输入输出规格说明

### 语义

```
y = AllReduce_2rank( x @ w^T )        # fp16，SUM；两 rank 各持 w 的一半（TP2 分片）
```

两 rank 以相同 M/epoch 锁步各发射一次调用，kernel 内部经 MemFabric SDMA 池
交换两侧部分和并归约，**结果直写 y**（无 pad、无 staging 拷贝、无独立 AR 算子）。

### 张量

| 参数 | 形状/dtype/布局 | 说明 |
|---|---|---|
| `x` | `[M, 2048]` fp16 **ND** 连续 | 本 rank 的激活（TP 输入并行分片后的行块） |
| `w` | `[2048, 2048]` fp16 **FRACTAL_NZ 物理**（声明 ND） | 本 rank 的权重分片；`npu_format_cast(w, 29)` 产物。OpDef 原型声明 ND，xops 层手工构造"ND 标记 + NZ 存储"的 aclTensor（详见 xops 源码注释） |
| `y` | `[M, 2048]` fp16 **ND** | 调用方预分配；kernel 直写终值 |

### 契约（host 门 + kernel Check 双重校验，violation 即拒）

- `K == N == 2048`（编译期常量，kernel 硬编码）
- `M ∈ [1024, 4096]` 且 `M % 256 == 0`（去 pad 契约：尾 MM 组/末通信子组可部分行，原生 M 直传）
- `commRows ∈ {1024, 512}`；`aicCount == 8`（BlockDim=15：8 AIC + 7 AIV）
- `packet ∈ [512, 4MiB]` 且 `packet % 512 == 0`（SDMA 单包上限，推荐 1MiB）
- 对称池 `symm ≥ 48MiB + 2048`（池布局见下）

### 13 个 int64 属性（xops/aclnn 形参序）

| # | 属性 | 含义 |
|---|---|---|
| 1 | `gva` | 本 rank 池基址（对称窗口 VA） |
| 2 | `peerGva` | 对端 rank 池基址 |
| 3 | `symm` | 对称窗口大小（字节） |
| 4 | `ibPtr` | 本 rank 通知/座位区设备指针（≥16KiB，位于池外常驻设备缓冲） |
| 5 | `statusPtr` | kernel 状态字设备指针（每次调用前清零，非 0 = 失败码） |
| 6 | `tracePtr` | 诊断 trace 缓冲设备指针（4KiB） |
| 7 | `rank` | 本 rank（0/1，与池成员一致） |
| 8 | `epoch` | 批次号；**每调用 +1**（旗标/座位以 epoch 为真值，跨批次防串扰） |
| 9 | `packet` | SDMA 包大小（字节） |
| 10 | `rows` | M（本波行数） |
| 11 | `commRows` | 通信子组粒度（1024 或 512） |
| 12 | `aicCount` | MM 核数（8） |
| 13 | `enabled` | 0 = 预热波（不执行协议直接返回）；1 = 正式波 |

### 对称池布局（64MiB 池，每 rank 私有窗 32MiB 内）

```
work[0, 16MiB)     本端 MM 结果（AIC 写）
scratch[16, 32MiB) 对端全量 COPY 落点
ctrl 区 32MiB+64   8B 控制信号暂存（对端 8B 收件在 +64）
peerRdy 32MiB+512  对端"组 g 已落入我 scratch"旗标（车道轮询）
ready   32MiB+1024 调度器 ready 确认槽
sum     32MiB+2048 （保留）
```

### 调用纪律（会话侧义务）

1. **预热**：会话首波前以 `enabled=0` 发射 2 次预热波并做双端 barrier（池首写
   语义）；`status` 每次调用前清零。
2. **epoch 单调递增**，两 rank 同值同增（锁步）。
3. **池外缓冲**（ib/status/trace）由会话持有，进程生命周期内不释放。
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
cd prd_code/smem_mm_ar_310p3
bash build.sh
# 产物：build_out/custom_opp_ubuntu_aarch64.run
# 展开：build_out/_CPack_Packages/Linux/External/custom_opp_ubuntu_aarch64.run/
#       packages/vendors/smem_mm_ar_310p3/
```

说明：kernel 引用 memfabric 设备侧头文件（`smem_shm_aicore_base_sdma.h`），
include 路径在 `op_kernel/CMakeLists.txt`。默认指向本工作区已安装位置；SDK
在别处时构建前 `export MEMFABRIC_SDK_INC=<路径>/include/smem/device` 覆盖。

### 2. 构建 xops wheel（torch 入口）

```bash
source /usr/local/Ascend/cann-9.1.0/set_env.sh
export ASCEND_CUSTOM_OPP_PATH=$PWD/build_out/_CPack_Packages/Linux/External/\
custom_opp_ubuntu_aarch64.run/packages/vendors/smem_mm_ar_310p3
cd xops && bash build_and_install.sh    # 装入 xops_smem_mm_ar_310p3 wheel
```

### 3. 运行期环境（调用算子的进程启动前设置）

```bash
export ASCEND_CUSTOM_OPP_PATH=<...>/vendors/smem_mm_ar_310p3
export MF_SDMA_ORCH_DEPLOY_HELPER=<memfabric>/tools/aicpu_kernel_deploy/orch_deploy_helper
export MF_SDMA_ORCH_JSON=<memfabric output>/hybm/aicpu_kernel/libmf_sdma_orch_v28.json
export MEMFABRIC_HYBRID_HOME_PATH=<memfabric output>
export MEMFABRIC_HYBRID_EXTEND_LIB_PATH=<memfabric output>/hybm/lib64
export MIXFABRIC_SDK_OUT=<memfabric output>
export LD_LIBRARY_PATH=<memfabric output>/smem/lib64:<memfabric output>/hybm/lib64:\
<mfabric output>/acc_links/lib64:$ASCEND_CUSTOM_OPP_PATH/op_api/lib:$LD_LIBRARY_PATH
```

### 4. 使用（torch 侧）

```python
import xops_ar_310p3  # 注册 torch_npu.npu_smem_mm_ar_310p3（池建立后 import）
y = torch.zeros(M, 2048, dtype=torch.float16, device="npu:X")
torch_npu.npu_smem_mm_ar_310p3(
    x, w_nz, y,                    # x [M,2048] fp16 ND；w_nz NZ；y 预分配
    gva, peer_gva, symm,           # 池几何（smem_shm_create/allgather 产物）
    ib.data_ptr(), status.data_ptr(), trace.data_ptr(),
    rank, epoch, 1024*1024,        # packet=1MiB
    M, 1024, 8, 1)                 # rows=M, commRows=1024, aicCount=8, enabled=1
```

E2E 消费路径（vllm-ascend）：`enable_smem_mm_ar_310p3` 配置项，见
`../vllm-ascend/vllm_ascend/ops/smem_mm_310p3.py`（会话引导/接管/fallback 全
在 wrapper 内）。

### 与 smem_mm_iar_310p3 共存

两算子是独立 vendor 包，**一次进程激活一个**（`ASCEND_CUSTOM_OPP_PATH` 指向
对应 vendor；aclnn 符号经 libcust_opapi.so 按激活包解析）。vllm serve 用
`SMEM_OP=ar|iar` 切换（见 `../branchmark/e2e_perf/vllm_serve_smem.sh`）。

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

功能与性能验证件在 `../branchmark/`：

- 功能（M2 双 rank 逐位对拍）：`branchmark/functional/run_m2.sh`
- 算子级性能（stock vs ar vs iar）：`branchmark/op_perf/run_op_bench.sh`
