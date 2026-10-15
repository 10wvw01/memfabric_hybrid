# 310P3 MemFabric mm_ar e2e tests & benchmarks

Ascend 310P3（单卡双 die, TP=2）上 `matmul + MemFabric TP=2 allreduce` 融合算子
（full-attention `self_attn.o_proj` + GDN `linear_attn.out_proj`）的实机
功能测试与性能基准。融合路径无 M 阈值：启动选择融合后所有 M 均走融合。

## 前置条件

1. 硬件：空闲的 310P3 die 对（默认 `ASCEND_RT_VISIBLE_DEVICES=0,1`）。
2. 安装 `memfabric_hybrid:wgm-dev-310p` 运行包（`/usr/local/memfabric_hybrid`），
   且设备侧 aicpu kernel（`libmf_sdma_orch_v10.so`）已部署到
   `/usr/lib64/aicpu_kernels/0/aicpu_kernels_device/`。
3. feature-on 构建 vllm_ascend_C：

```bash
export SOC_VERSION=ascend310p3 ASCEND_HOME_PATH=/usr/local/Ascend/ascend-toolkit/latest
export COMPILE_CUSTOM_KERNELS=1 MAX_JOBS=32 VLLM_ASCEND_310P_ENABLE_MEMFABRIC_MM_AR=1
source /usr/local/memfabric_hybrid/set_env.sh
python3 -m pip install -e . --no-build-isolation --no-deps
```

4. 模型：`/home/models/Qwen/Qwen3.6-35B-A3B-w8a8`。

## 功能测试

```bash
MEMFABRIC_MM_AR_E2E_ENABLED=1 \
pytest -sv tests/e2e/_310p/test_memfabric_mm_ar_functional.py
```

覆盖：

- 路由：fused serve 日志中 10 个 `self_attn.o_proj` + 30 个
  `linear_attn.out_proj` 层使能；
- 计算/通信准确：greedy 输出与 stock（NZ matmul + HCCL all-reduce）逐
  prompt 完全一致（eager 与 ACL Graph FULL_DECODE_ONLY 两种模式）；
- 批量 decode（并发 10，M=10 即 graph capture size）与 stock 一致。

算子级 bit-exact（含全 M 矩阵、边界值、q=1/2/4、diverge-va）：

```bash
ASCEND_RT_VISIBLE_DEVICES=0,1 VLLM_ASCEND_310P_ENABLE_MEMFABRIC_MM_AR=1 \
torchrun --standalone --nproc-per-node=2 \
  benchmarks/scripts/bench_310p_memfabric_mm_ar_layer.py \
  --rows 1 16 32 64 255 256 257 511 512 513 1024 2048 4096 6144 8192 --repeat 20
```

## 性能基准

算子级（stock vs fused，同进程交替计时，M 梯度 + q 扫描）：

```bash
for q in 1 2 4; do
  ASCEND_RT_VISIBLE_DEVICES=0,1 VLLM_ASCEND_310P_ENABLE_MEMFABRIC_MM_AR=1 \
  torchrun --standalone --nproc-per-node=2 \
    tests/e2e/_310p/bench_memfabric_mm_ar_op.py \
    --batch-basem-count $q --rows 1 16 32 64 256 512 1024 2048 4096 \
    --iters 60 --csv /tmp/mm_ar_op_q$q.csv
done
```

端到端（四组合 stock/fused × eager/graph；输入 4096 / 输出 2048 /
50 prompts / 并发 10）：

```bash
bash tests/e2e/_310p/bench_memfabric_mm_ar_serve.sh /tmp/mm_ar_serve_bench
```

结果 JSON 由 `vllm bench serve --save-result` 写入 out_dir；汇总口径见
`csrc/_310P/memfabric_mm_ar/VALIDATION_REPORT.md`。

## 已知上游约束

- 连续融合负载 ≥5min 存在 MemFabric epoch/aicpu 交互的冻结风险
  （serve 下表现为 ~20s 可恢复停顿，拉高 P99；见 VALIDATION_REPORT R6/D3）。
- 仅 `ASCEND_RT_VISIBLE_DEVICES=0,1`（meta GVA 授权按逻辑 device id）。
- serve 进程若存在 pip 安装的 memfabric_hybrid 副本，需显式
  `MF_SDMA_ORCH_JSON` 指向运行包内的 launch json（脚本已内置）。
