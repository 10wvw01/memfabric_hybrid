# 310P3 MemFabric o_proj 融合算子（ABI v7）实机测试报告

日期: 2026-09-22 | 联调: USER (USER)

> **历史记录**：本报告按当期名称 `custom_memfabric_o_proj` 记录，目录/符号/
> 环境变量现已统一更名为 `memfabric_mm_ar`（o_proj 语义不变），文内旧名
> 保留原始语境。后续轮次（GDN out_proj 融合 + 全 M 融合）验证见追加章节。

## 1. 环境与版本

| 项 | 值 |
|---|---|
| 硬件 | Ascend 310P3 ×2 die（卡5664, dev0/dev1, bus 18:00.0），TP=2 |
| CANN | 9.1.0 (/usr/local/Ascend/cann-9.1.0) |
| 框架 | torch 2.10.0 + torch_npu 2.10.0.post4, vllm 0.27.1 (source /vllm-workspace/vllm), python 3.12.13 |
| vllm-ascend | feat/310p-memfabric-public-api @ **[sha]**（本轮提交；基线 [sha]） |
| memfabric_hybrid | wgm-dev-310p @ **[sha]**（本轮提交；自 [sha] 重建，v9 内核 HYBM_SDMA_ORCH_KVER=9） |
| 设备侧内核 | libmf_sdma_orch_v9.so 经 deployer+mini_kfc 预部署于 /usr/lib64/aicpu_kernels/0/aicpu_kernels_device/ |
| 模型 | Qwen3.6-35B-A3B-w8a8（qwen3_5_moe_text, 40 层, full_attention_interval=4 → 10 个 full-attn o_proj; K_global=4096/local=2048, N=2048, FP16） |
| 关键约束 | MemFabric 仅支持 ASCEND_RT_VISIBLE_DEVICES=0,1（meta GVA 授权按逻辑 device id，其余 die 对 507899 实测失败） |
| 构建 | bisheng --npu-arch=dav-2002；feature-on/off 双构建均验证通过 |

## 2. BUG 清单、根因与修复（均已修复并回归）

### BUG#1 feature-off 构建导入崩溃（vllm-ascend）

- 现象: `ImportError: undefined symbol _ZN11vllm_ascend31memfabric_o_proj_debug_snapshotEv`
- 根因: binding 无条件注册 debug op，但 `memfabric_o_proj_debug_snapshot()` 仅在 feature-on `#ifdef` 分支定义。
- 修复: `#else` 分支补 stub（返回 `zeros({24}, kLong)`，镜像 feature-on 无上下文结果）。
- 回归: feature-off 构建 import/注册/fused op 清晰报错/snapshot 全零 → PASS。

### BUG#2 层基准 q=1/4 双侧 MAIL_MISMATCH trap（bench 脚本）

- 现象: q=1/q=4 首次运行 `mf310pWaitBatchKernel` aicore trap。
- 根因: `VLLM_ASCEND_310P_MEMFABRIC_O_PROJ_BATCH_BASEM_COUNT` 只在 rank0 设置（rank1 用默认 q=2）→ batch_bytes/mail imm-len 两侧期望不一致。
- 修复: env 在 dist init 前对全部 rank 设置。
- 回归: q=1、q=4 各 9 个 M 全部 PASS（修复前 100% trap）。

### BUG#3 融合 MM 效率（核心，.asc 设备内核）

- 现象: producer 1024µs/批 vs stock ~246µs/批；fused 17.8ms@M=8192 vs stock ~6.93ms。
- 根因: AscendC 经典 `Matmul` 是**单核语义**（每个 block 从 tensor 基址消费同一 tiling），原代码把整批形状填入 `MatmulShapeParams` singleCore 字段 → 8 核全量冗余计算整批（正确但 ~8×工作量）。
- 修复: 8 核协作改为显式 per-block M 分区——A/C 连续行切片视图（`aGm = xGm + block·(BATCH_M/8)·K` 等），B(NZ) 全量共享；tiling `{MB/8, 2048, 2048}` basic `{MB/8, 256, 64}`，**CONFIG_NORM**（CONFIG_MDL 在该 per-block M 下实测片上 MTE 越界 aicore 507015 崩溃）；cache clean 收窄为各核只清自己写的连续 C 行。
- 结果: producer 273µs/批；fused@8192 **5.65ms（-18% vs stock 6.93ms）**；tiling 扫描 {basicN=512:283µs, 256:273µs, basicK=128:290µs} → 取 256/64。
- 回归: 全部正确性矩阵 bit-exact。

### BUG#4 运行包缺 310P 编排内核三件套（memfabric_hybrid）

- 现象: .run 安装后 orchestrator 加载失败（除非手设 MF_SDMA_ORCH_JSON）。
- 根因: `make_run.sh` 未打包 `output/hybm/aicpu_kernel`（launch json + CUST json + so）；运行时按 `<prefix>/<ARCH_OS>/lib64/../hybm/aicpu_kernel` 自动发现。
- 修复: 三件套随包打入 `${PKG_DIR}/${ARCH_OS}/hybm/`。已验证自动发现生效。

### BUG#5 安装器在 310P3 上中断（memfabric_hybrid）

- 现象: `install.sh` 芯片检测失败 exit 1 → set_env.sh 不生成。
- 根因: npu-smi 正则仅匹配 Ascend9xx，310P3 报 "310P3"；copy_extend 仅为 A2/A5 构建。
- 修复: 检测不到时 WARNING 跳过可选 extend lib 并继续安装。已验证 set_env.sh 正常生成。

### 环境问题（非代码缺陷，已规避/工具化）

1. **库内自动部署在 torch 进程内必败**: KFC probe miss（`test -f` 退出码 1）经流 sync 以 507018 返回后，torch_npu 注册的 ACL 错误回调将进程内 ACL 运行时闩锁（后续建流/launch 立即 507018）；独立进程无此现象。→ 必须外部预部署（`tools/aicpu_kernel_deploy/deployer`+`mini_kfc` P0 链），已在 orchestrator 代码中注释记录。
2. **设备侧 kernel 文件被延迟 janitor 删除** → 每次运行前 `ensure_kernel.sh` 重新探测/补拷。
3. **pip 残留 memfabric_hybrid 包抢先加载 site-packages 的 libmf_smem.so** → serve 内 json 自动发现失败（`mf310p_create ret=-7`）。处理: 卸载 pip 残留 + serve 脚本显式 `MF_SDMA_ORCH_JSON`。

## 3. 正确性（atol=rtol=0，全部 bit-exact）

| 项 | 结果 |
|---|---|
| q=2 全矩阵 11 个 M（255,256,257,511,512,513,1024,2048,4096,6144,8192） | **ALL PASS** |
| q=1 / q=4 各 9 个 M | **ALL PASS** |
| diverge-va（rank1 预占 4GB 使 GVA 基址分离）511/2048/8192 | **PASS** |
| UT 源码守卫 tests/ut/_310p/test_memfabric_o_proj_source.py | **19/19 PASS** |
| feature-off 构建（不含 MemFabric 依赖） | PASS |
| ACL Graph（gate7）: M=2048×32 replay、M=8192×16 replay、replay 后 eager 连续性 | **双 rank 全 bit-exact** |
| serve 级输出（Qwen eager, TP=2, greedy） | 统计等价（见下） |

serve 级说明: 基线栈自身对大 prompt 跨 run 非确定（stock 服务器 3 次自比, p0/p1/p3 均发散）。负对照: n=512 prompt 在 fused 服务器走纯 stock 路径（M<4096 不融合）仍与 stock 服务器输出不一致 → 证明输出发散 ≠ 融合路径因果。前缀一致长度中位数: p0 S~F 0.34 落在 S~S 0.18 / F~F 0.42 包络内；p1 三组均 ~0.01；确定性 prompt（4095/64）双端完全一致；融合路径 prompt（4100）存在共享变体。结论: 融合输出分布与 stock 统计等价，未见融合特异性发散。

## 4. 性能

### 4.1 算子层头对头对比（核心指标）：stock（o_proj matmul + HCCL allreduce 总耗时）为基准

同层同权重、同进程交替计时（3 轮取中位，/tmp/opencode/v8_headtohead.log；含 §4.2 修复后的 v8 终态与修复前对比）：

| M (q=2) | stock 基准 | fused v8 | v8 相对基准 | 修复前(v7) |
|---|---|---|---|---|
| 512 | 0.481ms | 0.627ms | 慢 30.4% | 慢 79.7% |
| 1024 | 0.789ms | 0.815ms | 慢 3.2% | 慢 32.5% |
| 2048 | 1.472ms | 1.471ms | **持平 (+0.1%)** | 慢 16.9% |
| 4096 | 3.284ms | 2.726ms | **快 17.0%** | 快 6.7% |
| 6144 | 5.273ms | 3.998ms | **快 24.2%** | 快 15.4% |
| 8192 | 6.760ms | 5.258ms | **快 22.2%（吞吐 +28.6%）** | 快 17.3% |

**盈亏交叉点从 ~4096 降到 ~2048**；MIN_M=4096 保持保守（其下仍全部走 stock 路径，零损失），下调至 ~2560 可让 (2560,4096) 行区间兑现 5%~17% 收益（列为后续选项，需 serve 级复验）。

### 4.2 固定协议开销：根因实测与修复（v8）

**根因确认（profiler 逐任务时间线 + 消融实验）**: 每波固定 ~0.52ms 中约 85% 是 `prepare_wave` 的 **32 KiB ready-cell memset**——它在 GVA 池内存上以引擎任务慢速路径执行（~260µs，与负载无关），且同流串行于下一波 gate 之前（时间线: `GATE→P→W→A→QUIET→ACK→[gap150]→COPY70µs→COPY258µs→下波GATE`）。消融验证：删除该 memset 后时间线中 258µs 任务消失、M=512 周期 903→616µs（-32%）。

**修复（v8, commit [sha]）**: eager 波改用单调递增 ready-cell generation（`0x40000000` 起，与 graph 捕获烘焙的 `batch+1` 小值域不相交）→ 陈旧 cell 永不等于当前波 → 32 KiB 清零不再必要；graph 捕获路径保持 wave-invariant generation + 图内清零；64 B 错误通道清零保留（~56µs）。

修复后剩余固定开销: 64B memset ~56µs + 间隙 ~140µs + 单批暴露等待 ~170µs（≥2 批时 wait 降至 1.5-2.2µs）≈ **~0.2ms/波**（原 ~0.52ms）。AICore 协议内核（gate/quiet/ack）合计 <10µs 可忽略；launch 流水良好（链内间隙 1-2µs）。

尾批惩罚（未变，M=768 实测）: scratch memset(2MB)+copy(1MB)=623µs（引擎 ~4.9GB/s）+ 尾批 W 暴露 ~102µs + quiet 等待 ~60µs——513 比 512 慢 2× 的全部构成；后续可同样消除（尾批 scratch 改为按 valid_rows 缩小或复用不清零方案）。

### 4.3 serve 级 TTFT 验证（稀释效应，o_proj M = prompt tokens）

单请求并发 1、max_tokens=1（TTFT≈纯 prefill），每点 5 次取中位（/tmp/opencode/sweep_{stock,fused,forced}.json）：

| M | stock (ms) | fused 生产路由 (ms) | Δ | fused 强制全融合 MIN_M=1 (ms) | Δ |
|---|---|---|---|---|---|
| 512 | 560 | 579 | +3.3% | 577 | +3.0% |
| 1024 | 914 | 932 | +1.9% | 928 | +1.5% |
| 2048 | 1696 | 1697 | +0.1% | 1706 | +0.6% |
| 4096 | 3371 | 3388 | +0.5% | 3394 | +0.7% |
| 6144 | 5111 | 5126 | +0.3% | 5121 | +0.2% |
| 8192 | 6799 | 6817 | +0.3% | 6827 | +0.4% |

三组配置 serve 级全部持平（差异 ≤3%，其中 512/1024 的 +1.5~3.3% 为服务器实例间噪声——生产路由下 M<4096 本走 stock 路径，与强制融合几乎同值可证）。**稀释效应**: o_proj 仅 10 层 × 毫秒级 = prefill TTFT 的 0.1~1%（如 M=8192 层级省 1.18ms×10=11.8ms，占 6799ms 的 0.17%），算子层 ±17% 效应映射到 serve 为 ±0.2~0.7%，低于实例噪声。另有并发负载 A/B（32req×4096-token, conc=4）: TTFT med 7.749 vs 7.773s、e2e med 22.83 vs 23.34s、decode 4.24 vs 4.12 tok/s——中位持平略优，尾部见 §5。

### 4.4 其他性能数据

- Producer 内核中位 **273µs/批**（8 核 M-split）vs stock per-批等价 ~246µs（+11%，超出 P0 门 ~5%，端到端反超见 4.1）。
- Overlap 证据（M=8192）: wait 中位 1.8µs vs 串行 SDMA 下界 104.9µs（2MiB@20GB/s）→ SDMA 完全被后续 MM 掩盖；add 40.8µs 与下一批 SDMA 重叠；`Block Num=8`。
- 诊断开关 `VLLM_ASCEND_310P_MEMFABRIC_O_PROJ_TRACE=1` 有真实开销（decode -27%），默认关闭。
- 未采纳路线（勘明未实现）: N-split per-core B/C（诊断实测 producer 147µs/批潜力），需权重按核 NZ 重排 + gather-add，列为后续优化。

## 5. 稳定性与已知上游问题

| 项 | 结果 |
|---|---|
| repeat=1000 @8192 | PASS（med 5.65ms, max 6.7ms） |
| 60s soak（34k 次 @2048） | PASS（max 33ms） |
| mixed-M 交错（10 序列×100） | PASS |
| 5~12min 连续负载 soak | **3/3 wedge**（r1 ~10-12min, r2 ~10min, r3 ≤5min 内） |
| 进程生命周期 | 每次 torchrun 完整 create→use→destroy ~20+ 次全部干净退出；graph 使用后保守跳过 destroy（设计行为） |

**Wedge 签名**（3 次一致）: 双 rank 同时 `mf310pWaitBatchKernel`/`mf310pQuietKernel` aicore 超时（507014），orchestrator 已加载、aicpu 无异常日志、kernel 文件完好、设备利用率归零、SIGTERM 不可杀需 SIGKILL，数分钟~10min 自愈。
**归属**: 上游 MemFabric epoch 续命（18s 网格）与 aicpu 调度/运行时交互（R6/D3 已知类别；发生率约每 6-20 分钟连续负载 1 次，与 serve 下 ~20s 可恢复冻结同源——serve 引擎内其他活动使其可恢复，纯融合 bench 下退化为永久 wedge）。**非本集成代码缺陷**；上游已反馈（D3 报告 + 本轮 3 次复现证据）。
**serve 影响**: 尾部延迟被冻结事件拉高（fused ttft_p95 59.3s vs stock 14.3s，1-2 次/4min 运行），中位数不受影响。

## 6. HCCL 共存（受控微基准）

MemFabric 池存活前后 HCCL allreduce（fp16 [1/8/64,2048]）中位延迟 0.209→0.205 / 0.217→0.218 / 0.232→0.232 ms → **无退化**；stock o_proj fallback（matmul+allreduce）建池后 0.32-0.35ms 健康；serve 级 decode（全 HCCL 路径）速率与 stock 持平。

## 7. 剩余风险与验收结论

风险（按严重度）:

1. **上游 D3/R6 wedge/冻结**: 连续融合负载 ≥5min 有 wedge 风险，serve 下为 ~20s 冻结拉高尾部延迟——生产化阻塞项，待上游修复。
2. **部署链**: torch 进程内自动部署不可用（ACL 闩锁）+ kernel 文件 janitor 删除 → 依赖外部 deployer 工具链，需服务化固化。
3. 纯 MM +11%（端到端已 -18%）；N-split 优化路线已勘明。
4. graph 使用后进程退出 SIGABRT（上游缺"只停线程不销毁池"API，已反馈）。
5. 多副本 libmf_smem.so 环境陷阱（pip 残留/opt 旧装）→ 部署需唯一安装源或显式 MF_SDMA_ORCH_JSON。

**验收结论（实机）**:

- 正确性（层级 bit-exact 全矩阵 + diverge-va + graph replay + serve 统计等价）: **PASS**
- 性能（M=8192 端到端 -18%；serve 中位持平略优；overlap 有 profiler 证据）: **PASS**
- 稳定性（60s/mixed/repeat1000/进程生命周期/HCCL 共存）: **PASS**；≥5min 连续负载受上游 D3 限制: **条件通过**（<5min 负载窗口内可靠，或待上游修复）

## 8. 交付物

- vllm-ascend `feat/310p-memfabric-public-api` @ [sha]（signed-off: USER）
- memfabric_hybrid `wgm-dev-310p` @ [sha]（signed-off: USER）
- 工具: /tmp/opencode/{ensure_kernel.sh, v7_env.sh, run_layer.sh, v7_profile.py, stock_profile.py, gate7_graph.py, hccl_coexist.py, ab_headtohead.py, ab_serve.sh, ab_capture.py, ab_compare.py, bench_ab.py, patch_tiling.py}
- 证据: /tmp/opencode/{v7_correct_q*.log, v7_final_*.log, v7_soak_*.log, v7_prof_*.log/csv, gate7_m*.log, hccl_coexist.log, ab_*.csv, ab_*out*.json, ab_*serve*.log}

---

## 第二轮（2026-09-29）：memfabric_mm_ar 更名 + GDN out_proj 融合 + 全 M 融合

日期: 2026-09-29 | 联调: USER (USER) | 提交: [sha] → [sha] → [sha] → [sha]

## R1. 本轮变更与环境

| 项 | 值 |
|---|---|
| 变更 | ① 彻底更名 custom_memfabric_o_proj → memfabric_mm_ar（目录/文件/torch ops/Python 模块/环境变量/编译宏/文档/UT）；② GDN `linear_attn.out_proj`（30 层）纳入融合（形状合同与 o_proj 一致: K_local=2048, N=2048, TP=2, 未量化 FLOAT，内核零改动复用）；③ 移除 MIN_M 阈值——选融合即全 M 融合；④ 尾批优化: producer scratch 分配时一次性清零，尾批只拷贝有效行（消除每尾批 ~0.6ms 的 2MiB 引擎 memset） |
| 环境 | CANN 9.1.0 / torch 2.10.0+npu / vllm 0.27.1 (source /vllm-workspace/vllm)；卡 5664 dev0/dev1，ASCEND_RT_VISIBLE_DEVICES=0,1 |
| MemFabric | 运行包 1.2.0（wgm-dev-310p @ [sha]，**KVER=10**，设备侧 v10 内核已部署）；serve 显式 MF_SDMA_ORCH_JSON |
| 模型 | Qwen3.6-35B-A3B-w8a8（40 层 = 10 full-attn o_proj + 30 GDN out_proj，均 FLOAT 未量化） |

## R2. 正确性（全部 bit-exact，atol=rtol=0）

| 项 | 结果 |
|---|---|
| 层级全 M 矩阵（q=2: 1/16/32/64/255/256/257/511/512/513/1024/2048/4096/6144/8192；q=1/4: 1..4096） | **ALL PASS**（M=1 稳态 0.53ms，尾批优化生效） |
| serve 路由 | fused 日志使能 **10 o_proj + 30 out_proj**（跨 rank 去重），`reduce_results` 关闭正确 |
| serve greedy（eager + FULL_DECODE_ONLY graph） | 确定性 prompt（4 短 + [64]×4095 长）+ 并发 10 批量 decode 全部与 stock **完全一致**（2/2 PASSED） |
| 大 prompt 基线非确定性 | 自然语言 ~4k prompt 在 stock 服务器自身跨运行即发散（两个 attractor 互换，eager/graph 交叉复现）→ 精确对比必须用重复 token-id prompt；负对照结论沿用首轮 |

## R3. 算子级性能（同进程交替计时取中位；stock = NZ matmul + HCCL allreduce）

fused 相对 stock 耗时变化（负 = fused 更慢；M≤64 为 decode 区、4096 为 prefill 区）:

| M | q=1 (256) | q=2 (512) | q=4 (1024) |
|---|---|---|---|
| 1 | -63% | -92% | -135% |
| 10/16 | -59%/-52% | — | — |
| 32 | -45% | -74~-82% | -153% |
| 64 | -57% | -84~-93% | -154% |
| 256 | -24% | -56% | -107% |
| 512 | -31% | -29% | -62% |
| 1024 | -30% | -4% | +1% |
| 2048 | -23% | **+3.3%** | **+13%** |
| 4096 | -0.9% | **+17~20%** | **+29~41%** |

- 绝对值: stock M=1~64 稳定 0.29-0.33ms；fused 小 M 平坦（q=1 ~0.47 / q=2 ~0.55-0.60 / q=4 ~0.78ms，= padding 到 batch_m 的 MM + ~0.2ms 协议）；M=4096: stock 3.28ms vs fused q=1 3.31 / q=2 2.73 / q=4 2.36ms。
- 结论: 小 M fused 恒亏（padding 代价），大 M fused 恒赚且 q 越大赚越多（批数少、协议开销摊薄）；e2e 主导因素是 decode 小 M。
- **默认 q 决策: 保持 q=2（batch_m=512）**——e2e 实测 q=1 与 q=2 均与 stock 持平（见 R4），q=2 保留 prefill 层 +17~20% 收益且与首轮验证连续。

## R4. 端到端性能（vllm bench serve: 随机 4096 入 / 2048 出 / 50 prompts / 并发 10；输出吞吐 output tok/s）

| 组合 | 完成 | out tok/s | TTFT med | ITL med | 备注 |
|---|---|---|---|---|---|
| stock_eager | 50/50 | 66.49 | 16.36s | 129.11ms | 基线 |
| **fused_eager (q=1)** | **10/50** | — | — | **120.10ms** | **2/2 复现致命崩溃**（见 R5）；崩溃前 ITL 反而优于 stock -7% |
| stock_graph | 50/50 | 87.73 / 86.40（均值 87.07） | 16.30s | 94.87/96.27ms | graph 较 eager +32% |
| **fused_graph (q=1)** | 50/50 ×2 | 85.33 / 87.41（均值 86.37，**-0.8%**） | 16.31s | 96.15/95.52ms | 零 507014 |
| **fused_graph (q=2)** | 50/50 | 86.85（**-0.3%**） | 16.30s | 96.89ms | 零 507014 |

- **输出吞吐结论: graph 模式下 fused ≈ stock（-0.3%~-0.8%，均在 stock 自身 ±1.5% 噪声带内）**；TTFT 持平（队列主导，prefill 层级收益被稀释）。
- eager 模式 fused 崩溃前 decode ITL 120-125ms 优于 stock 129ms（孤立算子层的小 M 亏损在真实模型内未成立——融合单 op 少 40 层/步的 host 派发），但被 R5 稳定性问题阻断。
- 算子级 ±17~92% 的差异映射到 e2e 仅 ±1%：见 R6 影响范围。

## R5. 稳定性：eager 全 M 连续融合负载下 D3/R6 wedge 恶化为致命崩溃

| 项 | 证据 |
|---|---|
| 现象 | fused_eager 2/2 崩溃: EngineCore 先停摆（shm_broadcast 60s 窗口告警连续出现），随后双 rank `mf310pWaitBatchKernel`/`mf310pQuietKernel` **aicore 超时 507014**（>28s 邮件未达），EngineCore 死亡、剩余 40 请求 Internal Server Error |
| 时间线 | rep1: 健康+8.5min 起停摆、+13.6min 崩溃；rep2: +3.1min 起停摆、+11min 崩溃 |
| 归属 | 首轮 D3/R6 同签名（上游 MemFabric epoch 续命与 aicpu 调度交互）。**恶化因子**: 全 M 融合使 decode 持续走融合路径（首轮 MIN_M=4096 下 decode 走 stock，融合仅 prefill 突发、有喘息窗口，表现为 ~20s 可恢复冻结；本轮连续负载使停摆超过 28s aicore 看门狗，变为致命）。**非本轮集成代码缺陷**（协议/正确性全绿，崩溃 kernel 为等待对端邮件的 wait/quiet） |
| graph 模式 | 3/3 完整跑完（2×q=1 + 1×q=2，各 ~20min）零 507014、零融合归因停摆（各有一次 init 期 stall 告警，stock graph 同样存在，为引擎初始化通用现象） |
| 处置 | 融合长负载（>10min）**必须用 graph 模式**（FULL_DECODE_ONLY）；eager 融合限 <10min 连续负载窗口；已更新使用文档。上游修复前 eager 全 M 融合不可用于生产 |

## R6. 影响范围评估（融合算子占端到端时间比例）

- decode 步（graph, M=10）: stock mm_ar 层耗时 ≈ 40 层 × 0.30ms = 12ms / 96ms 步长 ≈ **12.5%**
- prefill（M=4096）: ≈ 40 层 × 3.28ms = 131ms / ~1.5s ≈ **8.7%**
- e2e 墙钟构成 ≈ decode 85% + prefill 15% → **全程 ~12% 的时间在 40 个融合层内**
- 实测 e2e 净变化 -0.3%~-0.8%（噪声带内）→ 与影响范围自洽: decode 侧融合亏损被模型内 host 派发节省抵消，prefill 侧收益被队列稀释
- 上限推演: 若融合在某工作负载完全失效（最坏 +100% 层耗时），e2e 最多退化 ~12%；若完全隐藏通信，理论上限收益 ~12%（当前实现远未达通信完全隐藏）

## R7. 结论

1. **正确性: PASS**（层级 bit-exact 全矩阵含 M=1 + serve eager/graph greedy 一致 + 路由 40/40 层）
2. **算子级: 大 M 收益保持**（q=2 +17~20%@4096），小 M 亏损如实记录（q=2 -92%@M=1）
3. **端到端: graph 模式输出吞吐与 stock 持平（-0.3~-0.8%，噪声带内）**，TTFT 持平
4. **稳定性: graph 3/3 通过；eager 2/2 致命崩溃（上游 D3/R6 在全 M 连续负载下恶化）——生产阻塞项维持，且 eager 融合新增硬约束**
5. 默认 q=2 维持；影响范围 ~12% e2e 时间

## R8. 工具与证据

- 基准/测试入库: `tests/e2e/_310p/`（functional pytest、算子 A/B、serve 4 组合 launcher、结果汇总 analyze 脚本）+ `benchmarks/scripts/bench_310p_memfabric_mm_ar_layer.py`
- 证据: /tmp/opencode/phase4/{op_q*.csv,op_small_q*.log,serve_q1/,serve_q1_rep2/,serve_stock_g2/,serve_q2_g/}；功能: phase3_functional4.log（2 passed）；正确性: phase2_q{1,2,4}.log
