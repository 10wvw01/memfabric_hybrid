# benchmark — smem_mm 融合算子功能与性能验证工程

本目录是 `prd_code` 两个融合算子（`smem_mm_ar_310p3` 本地归约 / `smem_mm_iar_310p3`
硬件归约）的**功能测验 + 性能测验**工程，覆盖两级口径：

| 级别 | 内容 | 位置 |
|---|---|---|
| 算子级功能 | M2 双 rank 逐位对拍（golden 比对，PASS 判据 = 0 mismatch） | `functional/` |
| E2E 功能 | 对运行中的 vllm serve 发真实请求，验证长/短 prompt 生成正确性 | `functional/` |
| 算子级性能 | stock vs ar vs iar 三臂设备侧计时（msprof span P50 主判据） | `op_perf/` |
| E2E 性能 | 三臂 TTFT 矩阵（fresh serve × ttft_bench + profile） | `e2e_perf/` |

## 前置依赖配置（所有实验通用）

1. **设备纪律：用物理 6/7 die 对**（43520 卡，TP=2 双 die；4/5 = 38144 卡
   引擎层故障待复位，恢复后同样可用）。跑前确认空闲：

   ```bash
   npu-smi info -i 43520 -t usages   # 双 die DDR<20% 且大页<500 视为空闲
   ```

   其它设备上可能有他人任务，禁止触碰；`ASCEND_RT_VISIBLE_DEVICES` 一律 unset
   （脚本已内置）；die 对经 `DEVICES`（M2/op_bench）或
   `VLLM_ASCEND_DEVICE_OFFSET`（serve）指定。

2. **算子包与 wheel 已构建安装**（见各算子 README 的编译部署教程）：

   - `smem_mm_ar_310p3` / `smem_mm_iar_310p3` 两包 `build_out/` 下已有展开产物；
   - `xops_smem_mm_ar_310p3` / `xops_smem_mm_iar_310p3` 两个 wheel 已 `pip install`。

3. **CANN 9.1.0 GA**：`source /usr/local/Ascend/cann-9.1.0/set_env.sh`（各启动
   脚本已内置）。

4. **memfabric_hybrid v28-orch 产物**位于
   `<工作区>/memfabric_hybrid/output`（orch 已部署到
   `/usr/lib64/aicpu_kernels/`；若环境刚复位，首次跑会由部署助手自动装载）。

5. **E2E 实验额外**：vllm-ascend 为 editable 安装的 `prd_code/vllm-ascend`（或
   同内容 checkout），C 扩展已编译；模型在 `/home/models/Qwen/Qwen3.6-35B-A3B-w8a8`。

## 子目录与文件能力

### `functional/` — 功能测验

| 文件 | 能力 |
|---|---|
| `m2_xops_test.py` | M2 单 rank 进程体：池引导 → **会话层不校验暖机波**（预热门已随 2026-11-19 ABI 减参删除；首次 kernel 发射存在冷首发数据竞态窗口，正式校验前两端锁步发射一次不校验的暖机调度批次）→ 正式波 → golden 逐位比对（y 张量直读；数据为 one-hot x × 小整数 w，双 rank 和为序无关 → 逐位一致）。`--op ar\|iar` 选算子 |
| `run_m2.sh` | M2 矩阵启动器：`{ar,iar} × M{1280,3840,4096}` 逐格双 rank 拉起，逐格 ASCEND_CUSTOM_OPP_PATH 切包，PASS 判据 = 双 rank exit 0；`OPS/MS/DEVICES` 可覆盖 |
| `e2e_functional_test.py` | 对运行中的 serve 发长 prompt（触发融合路径）+ 短 prompt（触发 fallback 路径），检查答案正确性（巴黎/算术/东京三问） |

**跑法**：

```bash
bash functional/run_m2.sh                       # 全矩阵（约 10 分钟）
OPS=ar MS="4096" bash functional/run_m2.sh      # 单格
# E2E 功能冒烟（先起一个融合 serve，另开终端）：
bash e2e_perf/vllm_serve_smem.sh &              # SMEM_OP=ar|iar
python3 functional/e2e_functional_test.py
```

### `op_perf/` — 算子级性能测验

| 文件 | 能力 |
|---|---|
| `op_bench.py` | 单格单 rank 进程体：stock = `F.linear(NZ-w)+HCCL AR`（serve 同构配方）；ar/iar = 池会话 + 融合波（原生 M）。**主判据 = torch_npu.profiler 设备侧 span P50**（池驻留时 host 墙钟无效：逐 op sync 会把 SDMA 完成退化成 ~500ms 看门狗）。含路由门（stock 无 MatMul 核 / 融合臂无融合核 = 数字无效，干净失败） |
| `run_op_bench.sh` | 矩阵启动器：`{stock,ar,iar} × M`，单格看门狗 1200s，逐臂切包；跑完自动出报告 |
| `analyze_trace.py` | chrome trace 窗口切分/聚类/统计（锚点分段，剔前 2 窗口） |
| `make_report.py` | 汇总 summary_rank{0,1}.json → 慢侧 span P50 对比表 + Δ vs stock + 路由证据；`--anchors` 可挂历史锚点 JSON 看漂移 |

**跑法**：

```bash
bash op_perf/run_op_bench.sh                       # 默认 M="1280 3840 4096"，全三臂
MS="1280 3840" bash op_perf/run_op_bench.sh        # 自定义档位
PATHS="stock ar" bash op_perf/run_op_bench.sh      # 自定义臂
DEVICES=6,7 REPS=3 ITERS=30 bash op_perf/run_op_bench.sh   # 自定义设备/重复/迭代
# 结果落 op_perf/results/<时间戳>/{arm}_m<M>/；跑完自动打印报告
```

**判读**：报告给出每档三臂慢侧 span P50 与 Δ%；跨会话噪声带约 ±3%，带内差异不作
胜负判（项目既有口径）。

### `e2e_perf/` — E2E 级性能测验

| 文件 | 能力 |
|---|---|
| `run_e2e_matrix.sh` | 三臂 TTFT 矩阵编排：S1（stock）/ F10（ar）/ F11（iar），每臂 fresh serve → 6 标记核验（接管证据）→ 逐长度 ttft_bench（1 cold + 2 warmup + 10 测量；默认 LENS="4096"——E2E 口径只测 4096）→ round 1 附加每长度 1 个 profile 请求；round 2 反序确认轮。`DFLASH=1/0` 切部署块型（on=1280×3+256 / off=3840+256），两轮同 seed 配对分析 |
| `vllm_serve_stock.sh` | stock 基线 serve（无融合；additional_config 全关） |
| `vllm_serve_smem.sh` | 融合 serve：`SMEM_OP=ar\|iar` 选算子（切 ASCEND_CUSTOM_OPP_PATH + 使能配置）；`DFLASH=0` 切 dflash-off |
| `ttft_bench.py` | 单臂 TTFT 计时（HTTP 发出 → 首 SSE 块，perf_counter 墙钟；1 cold + 2 warmup + 10 测量，`--seed-base` 保证跨臂同 prompt 配对） |
| `profile_e2e.py` | 每臂每长度 1 个 profile 请求（`/start_profile` → 1 请求 → `/stop_profile`，msprof 设备侧窗口，接管证据 + 算子级耗时） |

**跑法**：

```bash
bash e2e_perf/run_e2e_matrix.sh                    # DFLASH=1（部署真实块型）
DFLASH=0 bash e2e_perf/run_e2e_matrix.sh           # dflash-off（3840 主块型）
ARMS="F10" ROUNDS=1 LENS="4096" bash e2e_perf/run_e2e_matrix.sh   # 单臂冒烟
```

**接管核验纪律（必查，只看 /health 会漏静默降级）**：每臂 serve 日志中
`session up` + `installed on 40` + `verified bit-exact` 三个标记 × 2 worker =
6 个；不足 6 = 接管不完整，数字无效。S1（stock）臂必须 0 个融合标记。
`FALLBACK to native on ...` 行逐条看原因——合法原因只有小块/非契约 M。

**结果目录**：`e2e_perf/results/e2e_dflash{0,1}_<时间戳>/`
（`ttft_*.json`、`serve_*.log`、`profile_*/`）。TTFT 对比：同臂同长度跨轮 P50
配对；±1% 以内属单请求噪声带，不作结论。

## 实验组合建议

- **改动算子后回归**：`run_m2.sh`（全 PASS）→ `run_op_bench.sh` → 对照历史锚点
  （慢侧 P50 漂移 <±3% 为带内）。
- **验证 vllm 接入**：`e2e_matrix.sh` 单臂冒烟（ARMS=F10 ROUNDS=1）看 6 标记 +
  首调逐位 + fallback 原因；再跑完整两轮矩阵出 TTFT 配对结论。
