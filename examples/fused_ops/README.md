# prd_code — smem MM+AR 融合算子终版工程（310P3 / TP=2）

基于 memfabric_hybrid 的 MemFabric SDMA 池，把 vllm-ascend 推理中 prefill 路径
RowParallelLinear（o_proj / O.out）的 **矩阵乘 + TP AllReduce** 融合为单个
Ascend C 算子。本目录是项目收尾规整的**锁定终版**（对应 run8 / run11 / run12 /
run13 四组实验的代码版本），只含生产代码，不含研发期实验/探针臂。

## 两个融合算子（同族姊妹，接口同构，一个 serve 会话二选一）

| 算子 | 归约机制 | 研发期出身 | 说明 |
|---|---|---|---|
| `smem_mm_ar_310p3` | **本地归约**：全量 COPY 交换 + AIV 车道本地加法 | smem_mm_ar_inline_reduce flavor 10 | 部署默认；实测整体占优（makespan 口径） |
| `smem_mm_iar_310p3` | **硬件归约**：ADD_FP16 半交换（SDMA 途中加法）+ 旗标收敛 | smem_mm_ar_inline_reduce flavor 11 | 通信侧 AIV 更短，矩阵乘侧带宽争用更小 |

适用边界（实测结论，随工程交付如实携带）：算子级 M≥2048 正收益、M=3840 约
+15%；E2E 满载（压测）为负收益**禁部署**，单请求/低载在噪声带内持平——部署
价值定位在 prefill TTFT 低载场景，默认不启用、按需开启。

## 目录介绍

| 目录 | 内容 |
|---|---|
| `smem_mm_ar_310p3/` | 融合算子（本地归约版）：CANN 算子工程（op_host/op_kernel/构建件）+ xops torch 扩展；README 含输入输出规格说明、编译部署教程、memfabric_hybrid 依赖教程 |
| `smem_mm_iar_310p3/` | 融合算子（硬件归约版）：结构同上 |
| `branchmark/` | 功能测验 + 性能测验工程（算子级 / E2E 级），README 含每个实验的跑法、判据与前置依赖 |
| `vllm-ascend/` | 仅装载上述两个融合算子的 vllm-ascend 源码 checkout（接入层） |
| `README.md` | 本文件：工程总览与跑通路径 |

## 前置依赖（版本严格绑定）

| 依赖 | 版本 | 说明 |
|---|---|---|
| CANN | **9.1.0 GA**（`/usr/local/Ascend/cann-9.1.0`） | 任何操作前 `source $CANN/set_env.sh`；beta 版会导致算子装载失败 |
| memfabric_hybrid | **`v28-orch` 分支**（KVER=28） | 构建产物（`output/`）提供 `libmf_smem.so` + orch；血统不匹配会死等编排器。构建教程见各算子 README |
| torch / torch_npu | 2.10 / 2.10（预编译 wheel） | 勿动 |
| vllm | 0.24.0 | editable 安装 |
| vllm-ascend | 本目录 `vllm-ascend/`（0.19.1rc2.dev 系） | editable 安装；C 扩展编译期链 CANN——**换 CANN 必重编** |
| xops wheels | `xops_smem_mm_ar_310p3` / `xops_smem_mm_iar_310p3` | 由各算子目录 `xops/` 构建安装 |
| 模型 | `/home/models/Qwen/Qwen3.6-35B-A3B-w8a8` | `--quantization ascend --dtype float16`；hidden=2048 与算子 K=N=2048 硬编码吻合 |
| 设备 | Ascend 310P3 单卡双 die，**TP=2** | 实验只用物理 **4/5**（38144 卡）；设备 0 布局不对称禁用；禁止 `ASCEND_RT_VISIBLE_DEVICES`（一律 `VLLM_ASCEND_DEVICE_OFFSET=4`） |

## 从零跑通（快速路径）

前置：CANN GA 已装、memfabric v28-orch 已构建（`memfabric_hybrid/output/`）。

```bash
# 1) 编译两个算子包（各自独立 .run）
source /usr/local/Ascend/cann-9.1.0/set_env.sh
for OP in smem_mm_ar_310p3 smem_mm_iar_310p3; do
    (cd $OP && bash build.sh)
done

# 2) 构建并安装两个 xops wheel（torch 入口）
for OP in smem_mm_ar_310p3 smem_mm_iar_310p3; do
    export ASCEND_CUSTOM_OPP_PATH=$PWD/$OP/build_out/_CPack_Packages/Linux/External/custom_opp_ubuntu_aarch64.run/packages/vendors/$OP
    (cd $OP/xops && bash build_and_install.sh)
done

# 3) 安装 vllm-ascend（editable，含 C 扩展；本副本已带预编译产物可直接用）
cd vllm-ascend && pip install -e . --no-build-isolation && cd ..

# 4) 功能验证（M2 双 rank 逐位对拍，全 PASS 才继续）
cd branchmark/functional && bash run_m2.sh && cd ../..

# 5) 算子级性能（stock vs ar vs iar，对照报告看 Δ 与路由证据）
cd branchmark/op_perf && bash run_op_bench.sh && cd ../..

# 6) E2E：起一个融合 serve（SMEM_OP=ar 或 iar）
cd branchmark/e2e_perf
SMEM_OP=ar bash vllm_serve_smem.sh &        # 等待 /health OK
python3 ../functional/e2e_functional_test.py   # 生成正确性冒烟
bash run_e2e_matrix.sh                          # 完整三臂 TTFT 矩阵
```

## vllm-ascend 编译部署及使用说明

### 接入形态

`vllm-ascend/` 是 vllm-ascend 源码 checkout，融合算子接入为**最小挂点**改动
（共 6 文件）：

| 文件 | 改动 |
|---|---|
| `vllm_ascend/ops/smem_mm_310p3.py` | **新增**：全部融合逻辑（池会话引导 ctypes、层接管、首调逐位门禁、fallback 日志、算子选择 ar/iar） |
| `vllm_ascend/_310p/worker_310p.py` | 设备偏移 + **early bootstrap 挂点**（进程第一个 NPU 分配 = 池窗口预留，310P 对称窗口 VA 基址约束） |
| `vllm_ascend/worker/worker.py` | 通用 worker 同款挂点（310P 部署用 _310p worker，此为对等保留） |
| `vllm_ascend/worker/model_runner_v1.py` | load_model 尾部 `maybe_install` 挂点（权重加载后包一层 forward） |
| `vllm_ascend/envs.py` / `ascend_config.py` | 使能开关注册（`enable_smem_mm_ar_310p3` / `enable_smem_mm_iar_310p3`） |

接管机制：实例级 forward 替换（`RowParallelLinear` 命中 `_layer_qualifies` →
`module.forward = wrapper.apply`），不改框架类一行；不满足契约（M<1280、M%256≠0、
M>4096、非 fp16、有 bias 等）自动 fallback 原生路径并记录原因——**fallback 日志
是部署核验的一部分**。

### 编译

```bash
source /usr/local/Ascend/cann-9.1.0/set_env.sh
cd vllm-ascend
pip install -e . --no-build-isolation    # 编译 C 扩展（链 CANN；换 CANN 必重编）
```

本副本已含预编译产物 `vllm_ascend/vllm_ascend_C.cpython-312-aarch64-linux-gnu.so`
（当前 CANN GA 环境构建），同环境可直接 editable 安装使用。

### 使用

**配置使能**（二选一，互斥；同时开两个会启动时拒止）：

```bash
vllm serve /home/models/Qwen/Qwen3.6-35B-A3B-w8a8 \
  --tensor-parallel-size 2 --quantization ascend --dtype float16 \
  --max-model-len 8192 --max-num-seqs 16 --max-num-batched-tokens 4096 \
  --gpu-memory-utilization 0.90 \
  --enable-prefix-caching --mamba-ssm-cache-dtype float16 \
  --mamba-cache-mode align --enable-chunked-prefill --async-scheduling \
  --enforce-eager \
  --additional-config '{"enable_smem_mm_ar_310p3":true, ...}'   # 或 enable_smem_mm_iar_310p3
```

**运行期环境必须**（完整脚本见 `branchmark/e2e_perf/vllm_serve_smem.sh`，含全
部环境变量）：

```bash
export VLLM_ASCEND_DEVICE_OFFSET=4          # 落物理 4/5；禁止 VISIBLE_DEVICES
export ASCEND_CUSTOM_OPP_PATH=<...>/vendors/smem_mm_ar_310p3    # 或 iar 包
export MF_SDMA_ORCH_JSON=<memfabric output>/hybm/aicpu_kernel/libmf_sdma_orch_v28.json
export MF_SDMA_ORCH_DEPLOY_HELPER=<memfabric>/tools/aicpu_kernel_deploy/orch_deploy_helper
export MEMFABRIC_HYBRID_HOME_PATH=<memfabric output>
export LD_LIBRARY_PATH=<memfabric output>/smem/lib64:...:$ASCEND_CUSTOM_OPP_PATH/op_api/lib:...
```

**接管核验（必查，只看 /health 会漏静默降级）**：serve 日志必须出现 6 标记
（`session up` / `installed on 40 ...` / `first fused call verified bit-exact`
各 × 2 worker），且 `FALLBACK to native` 原因只允许小块/非契约 M。

### 性能口径（引用数字时注意）

- 算子级：msprof 设备侧 span P50（慢侧 rank）；池驻留时 host 墙钟无效。
- E2E：TTFT 单请求配对（同 seed 两轮正反序）；±1% 为噪声带。
- 详见 `branchmark/README.md` 判据节。

## 已知边界（如实携带，勿越线部署）

1. 满载压测（并发 10 × 50 请求）融合为负收益——**满载禁部署**；收益定位在
   prefill TTFT 低载。
2. M<2048 算子级劣化；wrapper 默认 M≥1280 接管（部署块型实测档），可经
   `SMEM_MM_310P3_MIN_ROWS` 调整。
3. 图模式（torch.compile/npugraph_ex）与融合路径不兼容——serve 须
   `--enforce-eager`。
4. 两算子一次进程只激活一个（`ASCEND_CUSTOM_OPP_PATH` 二选一）。
