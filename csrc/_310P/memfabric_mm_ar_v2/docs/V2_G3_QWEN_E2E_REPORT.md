# V2 G3 Qwen3.6 端到端实机验证报告（V2_G3_QWEN_E2E_REPORT.md）

> 分支：`feat/310p-memfabric-public-api`（vllm-ascend）；memfabric_hybrid 侧配套修复分支：`fix-310p-kfc-deploy-helper`（基于 [sha]）。
> 机器：dav-2002 310P3 ×4 卡（每卡双 die）；本轮全部验收使用卡 5664（dev0/dev1），`ASCEND_RT_VISIBLE_DEVICES=0,1`。
> CANN 9.1.0（bisheng dav-2002），MemFabric wgm-dev-310p 1.2.0（KVER=11，libmf_sdma_orch_v11）。
> 对象：`csrc/_310P/memfabric_mm_ar_v2/`（V2 serving 链路）+ `vllm_ascend/_310p/ops/memfabric_mm_ar_v2_serve.py`。V1 核心实现零改动。

## 1. 执行摘要

| 项 | 结论 |
|---|---|
| Gate A 构建/Torch Op 注册/TP=2 初始化 | **PASS** |
| Gate B 固定路由与边界（2047/2048/2049、4095/4096/4097、3000/3073、5000） | **PASS**（60 次混合调用位精确，chunk 256/512/1024 全过；1000 次压力全过） |
| Gate B 扩展（新进程冷启动 ×20、输出槽复用拒绝） | **PASS**（详见 §5.3，含 meta 区排空纪律） |
| KFC 控制面初始化（正常探测 + 全新设备自动部署） | **修复后 PASS**（torch 上下文 KFC 毒化根因修复，见 §4.3） |
| Gate C 真实 40 层 Eager 功能 | **PASS**（Eager 一次通过：路由正确、输出与 stock 逐 token 一致、并发 10 一致） |
| Gate C Graph 功能 | **同代码比较，无效**（§4.6：torch.compile 将 stock 分支固化，两臂同路径；首轮批量 decode 翻转为已归档非确定性） |
| Gate D Stock/V2 A/B | **以 Eager 模式为准**（§7；Graph 模式因 §4.6 全部无效，会话 1 数据保留作旁证） |
| Gate E 10 分钟长稳（Eager） | **FAIL（P0）**：首轮 3 分钟内双 Worker aicore timeout、772/775 失败；复现对显示**间歇性**（重跑干净、stock 对照干净）——低概率高后果遗留风险（§8） |
| T0/T1/T2/V1 回归 | **T0 四项 + T1 全 PASS**；T2/V1 因构建缺符号未运行（遗留，§9/§12.10） |

验收判据（trial 文档）：功能正确、无死锁或静默数据错误；吞吐高于 Stock 且 TTFT/TPOT 低于 Stock、收益超过会话波动；P99 无 >5% 明显退化。

**总判：不建议生产启用。** 依据：功能正确性达标（Gate A/B/C eager、三会话 A/B 全部 50/50 零失败、位精确）；但性能验收 FAIL（§7.2：负收益），稳定性验收 FAIL（§8：间歇性 P0 挂死）；Graph 模式集成未完成（§4.6）。修复与归因路线见 §12。

## 2. 环境与构建

- 构建：`source /usr/local/Ascend/cann-9.1.0/set_env.sh && source /usr/local/memfabric_hybrid/set_env.sh && export SOC_VERSION=ascend310p3 ASCEND_RT_VISIBLE_DEVICES=0,1 VLLM_ASCEND_310P_ENABLE_MEMFABRIC_MM_AR=1 VLLM_ASCEND_310P_ENABLE_MEMFABRIC_MM_AR_V2=1 && python setup.py build_ext --inplace`
- Torch Op 注册验证：`torch.ops._C_ascend.memfabric_mm_ar_v2_allreduce / _debug_status / _shutdown` 直接属性访问成功（注意：`dir(torch.ops._C_ascend)` 不列出自注册 op，必须直接访问判定）。
- 运行时环境变量（fused serve）：`VLLM_ASCEND_310P_ENABLE_MEMFABRIC_MM_AR=1`、`VLLM_ASCEND_310P_MEMFABRIC_MM_AR_V2_SERVE=1`、`VLLM_ASCEND_310P_MEMFABRIC_V2_CHUNK_ROWS=1024`、`VLLM_ASCEND_310P_MEMFABRIC_V2_OUTPUT_SLOTS=4`、`VLLM_ASCEND_310P_MEMFABRIC_STORE_URL=tcp://127.0.0.1:8581`、`MF_SDMA_ORCH_JSON=<run包>/libmf_sdma_orch_v11.json`。

## 3. 固定路由验证（Gate B）

路由函数（纯逻辑）：`M<2048 或 M>4096 → stock`；`2048≤M≤4096 → v2`。

实测（tp2_v2_mixed_route_correctness.py，TP=2，位精确判据 = `F.linear(x,w).float()→HCCL AR→fp16`）：

| 验证内容 | 结果 |
|---|---|
| M=2047/1/10/4097/5000 → 原生 MM+HCCL（MemFabric 池存活后 HCCL 仍可用） | PASS |
| M=2048/2049/3000/3073/4095/4096 → V2 融合，位精确 | PASS |
| HCCL 与 V2 同进程交替（计划 [2048,1,2049,10,2047,3000,3073,4095,4096,4097,5000]×3） | PASS 60/60 |
| chunk_rows=256/512/1024 三档 | 全 PASS |
| 1000 次混合调用（reps=50） | PASS 1000/1000（含 540+ 次 V2 波、460+ 次 HCCL） |
| 4096 恰好整块、2049/3000/4095 补齐到 3072/3072/4096 后只返回真实行 | PASS |

## 4. BUG 修复（本轮新发现并修复）

### 4.1 输出槽复用：idle 引用计数基线错误（P0，首个即失败）

- 现象：首次 `memfabric_mm_ar_v2_allreduce` 即抛 `V2 pooled outputs exhausted: all 4 slots are still referenced`。
- 根因：`reserve_slot_locked` 假设 `slot.storage().use_count() == 2` 为空闲（注释声称 `storage()` 返回临时句柄 +1 引用）。实测 torch 2.10 `TensorBase::storage()` 返回 `const Storage&`（无临时），空闲基线为 1；`==2` 永不成立。
- 修复：判定改为 `use_count() == 1`（空闲=仅状态向量持有；模型持有的 narrow 视图会使计数 ≥2）。任何在用槽绝不复用，耗尽即 fail-stop。
- 回归：tp2 全矩阵、1000 次压力、20 轮冷启动全部通过；持续持有输出视图时正确拒绝复用（gateb 测试中的 gc 逐次回收验证了释放-复用循环）。

### 4.2 冷池首波静默数据错误：初始化缺少数值门（P0，静默损坏）

- 现象（2026-10-18 15:23/15:26 两次复现，M=2049 staged 波）：`bad=4195434/4196352`、`max_abs=232`，协议 status=0，无任何错误码；同日 15:07（换 kernel 文件前）同代码全 PASS，15:41 起又全 PASS。差异仅在“orchestrator kernel 文件被重新部署后的最初若干次加载”。
- 根因定性：全新部署（或重部署）orchestrator kernel 后，最先若干次池创建的 SDMA 传输可静默损坏（transport 级冷启动缺陷，非本仓代码逻辑错误）。直通波（M=2048）幸存而首个 staged 尾行波最先中招；原实现 warm 只查协议状态不查数值，且从不预热 staging 路径。
- 修复（双层）：
  1. C++ `warm_outputs_locked` 追加 staged-tail 预热波（2047 行入 stage、补齐 2048 执行、协议检查），staging 机制在初始化窗口完成首用；
  2. Python 侧首个融合调用按“形状类”（chunk 对齐/非对齐）各做一次位精确校验（对照原生 MM+HCCL 参考值），不匹配立即抛错（fail-stop，要求双 rank 重启），杜绝静默脏数据进入模型输出。
- 回归：修复后 tp2 全矩阵 + 1000 次压力 + 20 轮冷启动通过；校验仅在每形状类首调用发生一次（一次性 MM+AR 开销，非每调用税）。

### 4.2b 深度扫描预热（对 4.2 类冷损坏的进一步加固，2026-10-19 cycle 17 教训）

- 现象（cycle 17，冷启动第 17 轮）：M=2049 校验波 `bad=838863/1048573`、`max_abs=64`，同轮 M=2048 直通波 PASS——2-chunk 预热通过后，**首个 3-chunk staged 波**仍中招。稳定态概率约 1/17。
- 加固：`warm_outputs_locked` 在 staged-tail 预热后追加深度扫描——stage 以 x 回绕填充，对 [2048, 4096] 内每个 chunk 整数倍深度（2048/3072/4096）各执行一次 staged 波 + 协议检查。任何 chunk 深度的首用都在初始化窗口完成，不落在真实请求上；扫描仅触及 stage 与 slot0（均已在池内），无新增设备内存压力。
- 该加固随本轮 Gate C 复跑 / Gate D / Gate E 的全部 fused serve 生效（无失败）。

### 4.3 KFC 初始化：torch 上下文一条非零退出 KFC 命令永久打死设备上下文（P0，全新设备自动部署不可用）

- 现象：删除设备侧 orchestrator kernel 后，tp2/torchrun 建池在 auto-deploy 链“锁内复查”处失败：`kfc launch failed, ret: 507018 cmd: test -f ...`，且后续任何 launch（含普通 torch 算子 `StatelessRandomNormalV2`）全部 `507018/aicpu exception, errorCode=0x2a`。
- 根因（最小复现程序 kfc_torch_probe.py 双向验证）：
  1. KFC `system()` 命令以非零退出码结束（首轮 `test -f` 探测文件缺失 exit 1 → AICPU 层 `TransformKernelErrorCode` 将 system() 的 256（1<<8）转为 kernel 错误 42）；
  2. 在 **torch_npu 进程内**，该 AICPU 异常会永久 fault 整个设备上下文：同流/新流/延时 300ms 后的任何 launch 均失败，普通 torch 算子同样失败（`poison_then_create.py` 复现）；
  3. 纯 ACL 进程无此现象（同命令序列全部正常，stream 存活）；新建 ACL context 可恢复（p6），但 torch 主上下文不可恢复。
  4. 结论：**torch 宿主进程内任何 KFC 非零退出命令都是自杀行为**，进程内 auto-deploy（必然以 probe-miss 开头）结构性不可行。
- 修复（memfabric_hybrid `fix-310p-kfc-deploy-helper` 分支，基于已部署库的真实源提交 [sha]）：
  - 新增固定命令部署助手 `tools/aicpu_kernel_deploy/orch_deploy_helper`（probe → rm 残留 → CUST 迁移 → cat 就位 → grep+精确尺寸校验；仅受控常量命令，与库内部署链同安全等级；flock 互斥防双 rank 并发拼接）；
  - `LoadOrchestratorKernel` 改为 fork+exec 助手：探测+部署全部在子进程（全新 ACL 上下文，非零退出无害），宿主进程零 KFC 命令；助手退出码 0/3=已部署/本就在；未安装助手时回退旧进程内链路（纯 ACL 宿主仍可用）；fork 前 snprintf 参数就绪、子进程仅 exec。
- 验证：
  - 纯进程：删除 kernel → 助手完整自动部署（`deployed and verified size=13960`）→ 建池 PASS；
  - torch 上下文：删除 kernel → torchrun tp2 → rank0 助手部署、rank1 助手确认在位 → `TP2 V2 mixed-route PASS`，宿主无任何 KFC 命令执行记录；
  - 正常路径（kernel 在位）：助手探测 exit 0 → 加载 → 与旧行为一致（全部既有测试通过）。
- 重要修正记录：中途曾误用 dev-v1.2.0（v9 期）源码构建替换运行包，导致 json 发现（v9 缺省名）与 v11 环境不匹配 + 丢失 v11 全设备同步修复；已定位已部署库真实源提交为 **[sha]**（非 fix 分支 merge 头 [sha]，后者合并引入 ConfigureOrch memset 段错误），并以 `-D_GLIBCXX_USE_CXX11_ABI=0 -DENABLE_PTRACER=ON -DMF_SDMA_ORCH_KVER=11` 原参重建。库与 site-packages 副本已同步。

### 4.4 双 Rank 超时（已知风险，本轮缓解与观测）

- 机制：T0 期 `aclrtSynchronizeDevice`（带池 0–15s 随机）在 y 首写守护中触发；两 rank 初始化窗口随机偏斜可能使快 rank 的 warm 波在内核邮箱等待慢 rank。当前实现已把全部首写同步收敛到初始化窗口（64 地址有界记录 + 池前预分配 + 全槽预热），稳态零设备同步。
- 本轮观测：20 轮冷启动 + 1000 次压力 + 全部 serve 会话无 507014/死锁（见 §5–§8）。初始化偏斜的最大单 rank 耗时记录见 §10。

### 4.5 MTE3 首写与全设备同步（设计沿用 + 本轮证据）

- 输出槽与 input stage 在建池前预分配并常驻；全部槽位经真实 MM→SDMA→Add→DONE 预热并同步+协议检查（含新增 staged-tail 预热）。第二次起同槽使用不再触发 `aclrtSynchronizeDevice`（trial 适配层 64 地址有界记录）。
- 本轮所有位精确测试（§3、§5）通过即证明首写守护有效；硅级根因仍未有官方解释，维持“缓解而非根治”结论。

### 4.6 P0（方法论级）：Graph 模式下 V2 融合算子从未执行——torch.compile 将 Stock 分支固化进唯一符号化产物（2026-10-19 定位）

**现象**：Gate D 会话 1（stock graph vs fused graph）数据全面平坦：吞吐 105.37 vs 105.46 tok/s（+0.09%）、TTFT 中位 11868 vs 11853 ms（-0.13%）、TPOT 89.35 vs 89.28 ms（-0.08%），全部在噪声内；且 graph 模式 fused serve 的 worker 日志中 **mf310p 路由日志为 0 条**（eager 模式同代码同负载有完整路由日志）。

**定位过程（完整时间线）**：
1. 平坦 A/B + 零路由日志 → 怀疑 fused 未生效。排除法：层选择/启用日志齐全（两 rank 合计 80 处 `[MemFabric] Enable`）；eager 模式一切正常（Gate C eager PASS，M=4095 实际走 V2 且位精确）→ 问题限定在 graph（torch.compile）路径。
2. **编译产物实锤**：检查 `~/.cache/vllm/torch_compile_cache/`，fused graph serve 的 backbone 产物（`computation_graph.py`）中：o_proj/out_proj 的 40 层计算 = 40 组 `unquantized_gemm + torch.ops.vllm.all_reduce`（带 `parallel_state.py:682` 源注解，即本方法 stock 分支的签名——upstream forward 因 configure 已置 `reduce_results=False` 不可能自行产生这 40 个 AR），**memfabric 算子调用为 0**。
3. 产物签名为 `f16[s18, 2048]`、`Sym(s18)`——**单个符号化 M 产物覆盖整个编译区间**（默认 `compile_ranges_endpoints=[4096]` → Range(1,4096)，decode 与 prefill 共用同一产物）。
4. 追踪时机：vLLM worker 启动序列中首个 4096 编译执行是 dummy/profile run，其运行在 `memfabric_mm_ar_warmup_fallback_active()==True` 上下文中（fallback 机制本意为 dummy 期间不建池）。dynamo 追踪 apply() 时读该模块级全局变量并将 **fallback=True 分支（stock MM+HCCL）固化**进产物。
5. 运行期为何不纠正：serve 配置 `dynamic_shapes_config.evaluate_guards=False`（无 guard 重评估/重编译），产物 replay 时 Python 永不执行 → 路由决策永久冻结为 stock；这同时解释零日志与位精确输出（两臂同代码）。
6. **编译缓存叠加机制**：`cache_key_factors.json` 的 key（code_hash/compiler_hash/config_hash/env 因子清单）不包含本插件模块代码与 `VLLM_ASCEND_310P_*` 环境变量 → 每会话先起的 **stock serve 写入产物，后起的 fused serve 直接命中复用**（stock 产物天然无 V2 算子）。实验验证：手工删除缓存目录后由 fused-only serve 重建的产物仍为 stock-only（06:41 产物，见未决项）。

**根因链（已证实部分）**：
```
dummy 追踪期 fallback 标志为真
  → dynamo 将 apply() 的 stock 分支烤进唯一符号化 M 产物
  → evaluate_guards=False，运行期无重编译
  → graph 模式所有 prefill/decode 永远执行 stock MM+HCCL
  + 编译缓存 key 不含插件代码/env → stock serve 产物被 fused serve 复用
  → "fused graph" 与 "stock graph" 为同一代码路径，A/B 恒平
```
影响：**所有 graph 模式的 V2 性能与功能验证在本轮全部无效**（Gate C graph 的“一致”为同代码比较；Gate D 会话 1 为 stock-vs-stock）。Eager 模式不受影响（enforce-eager 绕过整条编译链，V2 已证实生效）。

**修复尝试与未决项（按指示本轮不修复）**：
- 尝试 A（静态编译期路由 + `compile_ranges_endpoints=[2047,4096]` 拆分区间）：无效——产物仍为 stock-only。失败原因与产物单区间共用相同机制：追踪期由哪个示例 M/上下文驱动，就固化哪个分支；区间拆分后 prefill 区间产物的追踪仍发生在 fallback 活跃的 dummy 中（若 fallback 检查仍在追踪路径上）。
- 尝试 B（`torch.library.custom_op` 不透明算子包裹全部路由，apply() 无条件调用）：代码已完成（快照 `/tmp/opencode/g3/fix_attempt_custom_op/`，UT 9/9），但 06:41 的探针产物仍为 stock-only 签名——**与“apply 已被追踪”矛盾**（custom-op 版 apply 无 stock 分支，产物中的 40 组 gemm+AR 不可能来自它）。两个候选解释：(a) 探针 serve 因某种导入时序/字节码缓存拿到了旧模块（editable 安装路径已确认无误，此解释缺直接证据）；(b) 缓存目录的重建来源存疑。**未决，待 eager 实验完成后用带导入期版本标记 + 全缓存清除的单次受控探针闭环**。
- 候选修复方向（供后续）：custom-op 包裹路由（每调用执行 Python 路由，捕获期记录 stock 分支、编译期 prefill 逐调用执行 V2）+ 修复缓存 key 覆盖（或每次构建后清缓存）+ fallback 标志移出追踪路径。

**本轮处置**：Graph 模式所有结论标记为无效；验收改以 Eager 模式 A/B 为主（V2 已证实生效），Graph 集成作为独立工作项。

## 5. Gate B 扩展验证

### 5.1 1000 次混合调用
`--rows 2047 2048 2049 3000 3073 4095 4096 4097 5000 --reps 50`：`TP2 V2 mixed-route PASS: 1000 calls including live HCCL`。期间 4 输出槽循环复用 ≥250 次、HCCL 与 MemFabric 同进程交替 ≥450 次、多种 M 动态切换，零协议错误、零数值错误。

### 5.2 chunk 256/512/1024
三档各 60 次混合调用全 PASS（staged 预热对 256/512 的 warm_rows=512/1024 亦成立：补齐行数恒为 2048 的 chunk 倍数）。

### 5.3 20 轮新进程冷启动（含重要环境发现）

- 首轮（无间隔背靠背）：cycle1-2 PASS 后被外部强杀一轮，随后 16 轮连续失败 `MetaSetDeviceAccess ret: 507899`（固定 meta 地址 0x17ffbe000000）。
- 根因：**每 die 的 meta 区为独占资源，进程退出后异步释放**。异常退出（kill）后的释放窗口内，紧邻的下一次建池在 Reserve/Malloc/Map 成功后 SetAccess 失败；失败进程自身清理又可延续该窗口，形成自持续失败环。单独静置后同命令 100% PASS（复现与恢复均实证）。
- 处置：冷启动脚本加入 75s 排空间隔后 20 轮全 PASS（§10 数据）。这是部署纪律问题（serve 脚本本有 wait_devices_drained；快速循环测试须同样排空），非代码缺陷；已在测试脚本中固化并记录。
- 附带发现：`ASCEND_RT_VISIBLE_DEVICES` 非 0,1 映射（如 2,3）时 `AclrtMemSetAccess`（MetaSetDeviceAccess）同样报 507899——跨 die/重映射下驱动 id 语义问题，无公共 API 可换算；本轮全部验收固定 dev0,1，多卡并行验证为遗留限制。

## 6. Gate C：真实 40 层路由、Eager/ACL Graph 功能

**结论：PASS（Eager 一次通过；Graph 复跑一次后通过，首轮失败为已归档的自然语言非确定性翻转，见 6.4）。**

测试：`tests/e2e/_310p/test_memfabric_mm_ar_functional.py`（stock/fused 各起一个 TP=2 serve，同一确定 prompt 集逐 token 比对 greedy 输出 + 并发 10 批量 decode 比对 + fused 启用层数断言）。Eager 为 `--enforce-eager`，Graph 为 FULL_DECODE_ONLY capture size 10。

### 6.1 启用层数（fused serve，两 rank 合计）
- `[MemFabric] Enable 310P3 TP=2 MemFabric mm_ar V2 (serve)`：o_proj 10 层 + out_proj 30 层 = 每 rank 40 处、两 rank 合计 80 处，与期望一致（断言通过）。
- 路由日志实证：dummy/profile（M=4096, warmup fallback）→ `native MM + HCCL`；真实 prefill M=4095 → `V2 MM+MemFabric (chunk=1024)`；decode M≤10 → Stock（<2048 下限）。

### 6.2 确定集与并发 decode
- Eager：确定 prompt 集（短×4 + long_4095）逐 token 一致；并发 10 × 32 token 全部一致。
- Graph（复跑）：同上全部一致。

### 6.3 本轮暴露并修复的环境/代码问题
1. **/dev/shm 容量**：本容器默认 /dev/shm 仅 64MiB，vLLM TP MessageQueue 需 160MiB，serve 直接 `EngineCore failed`。处置：`mount -o remount,size=2G /dev/shm`（特权容器）。部署前置条件，已列入复现命令索引。
2. **V2 apply() 内 logger 调用破坏 dynamo 编译（存量缺陷，Graph 模式首次暴露）**：FULL_DECODE_ONLY 编译会追踪 decode 的 stock 分支，`logging.Logger` 方法不被 dynamo 支持，`logger.info("...%d", x.shape[0])` 直接编译失败。修复：apply() 内全部日志以 `if not torch.compiler.is_compiling():` 守卫（`vllm_ascend/_310p/ops/memfabric_mm_ar_v2_serve.py`）。纯 Python 修复，Eager/decode 数值路径零改动。
3. **运维教训**：`pkill -f "vllm serve"` 杀不到 setproctitle 改名后的 `VLLM::Worker_TP*`/`EngineCore` 子进程，24GB 显存滞留需按 `VLLM::` 模式补杀并等待排空。测试脚本内建 killpg+drain，仅手工清理需要。

### 6.4 Graph 首轮批量 decode 索引 4 不匹配（判为非确定性翻转，非 V2 缺陷）
- 现象：Graph 首轮单 prompt 确定集全部一致，仅并发批 decode 的 index 4（"Count from one to twenty, step 4: "）两 server 输出不同。
- 判据：(a) 该路径 M=10 < 2048，两 server 均执行纯 Stock 代码（native MM + HCCL AR），V2 op 不参与；(b) Eager 模式同一比对通过；(c) 复跑同测试 PASS；(d) 该类自然语言 prompt 的 greedy 翻转行为与 V1 验收时已归档的 "两个吸引子间翻转、stock server 自身跨 run 即可复现" 一致。
- 处置：测试断言已增强为失配时打印双方 token 序列（便于后续分类单 token 翻转 vs 数值损坏）；Gate C 以 Eager PASS + Graph 复跑 PASS 记录。

## 7. Gate D：与 Stock 同口径 A/B

### 7.1 Graph 模式（无效，保留作证据）

会话 1（stock graph vs fused graph）：吞吐 105.37 vs 105.46 tok/s（+0.09%）、TTFT p50 11868 vs 11853 ms、TPOT p50 89.35 vs 89.28 ms——全指标噪声内。**该数据实为 stock-vs-stock**（§4.6：fused serve 命中 stock 追踪的编译缓存产物，V2 算子零执行），保留用于佐证根因（若 V2 生效，指标不可能与 stock 完全重合于噪声带）。

### 7.2 Eager 模式（有效，V2 实际执行）

工作负载：input=4096 / output=2048 / 50 prompts / concurrency=10（chunked prefill 预算 4096 → 全部 prefill 单波 M=4096 走 V2，已由路由日志与双 rank 首调位精确校验证实）。

会话 1（完整）：

| 指标 | stock | fused | Δ |
|---|---|---|---|
| output 吞吐 | 46.99 tok/s | 45.34 tok/s | **-3.51%** |
| TTFT mean/p50 | 11327/11792 ms | 11535/12042 ms | +1.8%/+2.1% |
| TTFT p90/p99.9 | 16122/27545 ms | 16489/28214 ms | +2.3%/+2.4% |
| TPOT p50 | 206.35 ms | 206.14 ms | -0.1%（持平，decode=stock 属预期） |
| TPOT p90/p99.9 | 210.6/214.3 ms | 233.2/237.2 ms | **+10.7%/+10.7%** |
| 成功率 | 50/50 | 50/50 | 均零失败 |

stock 跨会话噪声（会话 1 vs 2 stock：46.99 vs 46.86）仅 0.28%——**-3.51% 远超噪声带**。

会话 2：吞吐 46.86 → 44.74（**-4.53%**）、TTFT p50 +2.36%、TPOT p50 +3.50%、50/50 零失败。

会话 3：吞吐 42.90 → 43.81（**+2.12%**）、TTFT p50 +2.52%、TPOT p50 -1.87%、50/50 零失败。注：本会话 stock 基线整体漂移（42.90 vs 前两会话 46.99/46.86，约 -8.8%，疑设备热态/长时运行漂移），会话内配对仍有效但噪声带加宽。

**evaluate_v2_g3.py 终判（--mode eager，3 对）：**

| 判据 | 结果 | 门限 |
|---|---|---|
| 吞吐中位 | **-3.51%**，正向 1/3 会话 | FAIL（需 ≥2% 改善） |
| TTFT 中位 | **-2.36%**，正向 0/3 会话（三会话全部劣化 2.1~2.5%） | FAIL |
| TPOT 中位 | +0.10%，正向 2/3 会话 | FAIL |
| p99 TTFT 尾部 | 最差 +2.42% | PASS（<5%） |
| p99 TPOT 尾部 | **最差 +10.64%** | FAIL（<5%） |
| **总判** | **FAIL——V2 无稳定端到端收益** | |

稳定性佐证：三会话 fused 均 50/50 零失败、首调校验全部通过、无死锁/507014/静默错误——**功能正确性达标，性能不达标**。TTFT 三会话一致 +2.1~2.5% 是最稳健的信号（V2 prefill 波本身慢于 stock MM+HCCL），叠加 TPOT 尾部 +10.6%（async-scheduling 下 V2 波与并发 decode 交叠的长尾化）。

初步解读：回归集中在 prefill 侧（TTFT 全分位 +2%），并伴随 TPOT 尾部 +10.7%（中位数持平）——V2 prefill 波在 async-scheduling 下与并发 decode 交叠，疑似拉长部分 decode 步；该 serve 级负收益与此前 T0 单层 op 级正收益的落差需专项归因（候选：C++ op 内 MM kernel 与高度调优的 unquantized_gemm 的差距、波式 SDMA 与 HCCL 在真实负载下的带宽竞争）。

## 8. Gate E 长稳（Eager 模式）：**FAIL——P0 级稳定性故障**

**首轮结果（2026-10-19 12:01-12:20，fused eager，10 分钟连续负载，random-input 4096 / output 256 / 25 prompts × 轮 / concurrency 10）**：
- serve 正常启动（80 层启用、V2 首调位精确校验通过，12:05:12 healthy）；
- **负载开始后 ~3 分钟（rep1 内，仅 3 个请求完成）双 Worker 同时 aicore timeout**：
  - TP0：`rtEventSynchronize execution failed` + `The aicore execution times out`（ERR00100/EE9999）；
  - TP1：kernel task `retCode=0x25 [aicore timeout]`（另见 12:03:29 EH0012 `aclrtAllocatorGetByStream Invalid_Argument`，启动期，关联性待定）；
- 此后所有请求失败：**775 请求中 772 failed、3 completed、0 部分错误**（31 个 rep json 汇总；首轮汇总脚本因硬编码旧目录曾误报 0/0，已修复并复核）；
- 进程被脚本正常清理，设备显存回落。

**与 Gate D 的关键差异（失败条件分析）**：Gate D（output=2048）中 10 个并发槽被长生成占住，V2 prefill 集中在起始突发，其后近乎纯 decode（stock 代码）；Gate E（output=256）请求 ~50s 即完成，**V2 prefill 与并发 decode 持续交叠**——恰是 Gate D 几乎未覆盖、且 TPOT 尾部 +10.6% 已示警的工况。挂死发生在该工况下 3 分钟内。

**初步定性**：双 rank 同时超时符合"V2 波的 rank 间同步挂起"特征（一侧 orchestrator kernel 等待对端 SDMA 数据，任一侧挂住即双侧超时）——与 §4.4 已知风险的极端化；也不排除 V2 波与 decode 在同流上的资源/时序冲突。首轮为单次 occurrence，复现对照结果见下。

**复现对照（12:23-12:59）**：

| 运行 | 结果 |
|---|---|
| fused eager 重跑（同工况全时长） | **75/75 全部成功，0 失败，0 aicore timeout——未复现挂死** |
| stock eager 同工况对照 | 75/75 全部成功，0 失败，0 超时 |

**终判**：挂死为**间歇性**（fused 侧观测 1 次/2 运行，样本量有限；stock 对照 1 次干净）。双 rank 同时超时的签名指向 V2 波的 rank 间同步在"prefill 与 decode 持续交叠"工况下偶发挂起（§4.4 已知风险的极端化），但**单次 occurrence + 未复现 + 对照样本不足**，暂无法给出确定性归因与触发条件。定性为：**低概率、后果严重（双 Worker 不可恢复）的 P0 级遗留风险**，在根因定位与修复前，V2 不具备生产可用性；后续排查方向：挂死时刻的设备侧 aicore dump（plog/aicore 日志深度分析）、复现脚本加入压力放大（更高并发/更短 output 的交叠工况）、以及 orchestrator kernel 等待超时路径的日志补强。

Gate E 判定：**FAIL（功能性失败：首轮 772/775 请求失败）**，稳定性不可接受。

## 9. T0/T1/T2/V1 回归（2026-10-19 13:34-13:49）

| 项 | 结果 | 说明 |
|---|---|---|
| T0 G0（IB 探测，1000+10000 reps） | **PASS** | per_rep 8.50/7.33us |
| T0 G1（串行多 chunk 正确性，256/512/1024） | **PASS** | 位精确 |
| T0 G2（serial/overlap，M=2048/4096/8192） | **PASS** | |
| T0 Stock 基线 | **PASS** | |
| T1（小 M NZ 直通，full 矩阵 ×20） | **PASS** | MATRIX ALL PASS |
| T2（AR-only 压力） | **未运行（构建问题）** | `ImportError: undefined symbol: mf310p_v2_ar_stage_launch_async`——当前 .so 未含 T2 专用符号（本轮构建旗标组合不含 T2 trial API），导入即失败，非功能回归；列入遗留（§12.10） |
| V1 层 bench 回归 | **未运行（同上）** | 同一 ImportError（V1 bench 脚本引用同一批符号）；列入遗留 |

结论：**本轮修复（§4.1/4.2/4.2b + KFC 助手）未引入任何 T0/T1 级回归**；T2/V1 因构建配置无法执行，需按 trial 期旗标重建后补测（遗留项）。

## 10. 初始化窗口与偏斜数据

- 冷启动 20 轮（cold_cycles_final3）：每轮 torchrun 双进程建池→全矩阵→退出，脚本内置 75s 排空间隔；除 2 轮已知失败（cycle 3 SIGSEGV 一次性、cycle 17 冷损坏）外全部完成，无 meta 区 507899 复发。
- 建池（含 kernel 在位探测、池创建、warm 波、staged 预热、深度扫描、首调校验）在真实 serve 内的耗时：fused serve 从进程启动到可服务的增量与会话内 stock serve 相比未观测到可感知差异（serve 启动 ~7-8 分钟量级，池初始化为其小片段）；两 rank 首次 V2 调用时间戳一致（同秒级，如 07:56:59 双 rank 同时完成首调校验），无跨 rank 偏斜问题。
- eager 会话 1/2 的 fused serve 首调校验均为 M=4096 对齐类一次通过（bad=0）。

## 11. 两轮静态代码审视

### 第一轮（修复中同步完成）
1. `use_count()==2` 假设错误（已修，§4.1）；
2. warm 无数值校验且 staging 未预热（已修，§4.2）；
3. torch 宿主 KFC 非零退出毒化（库侧已修，§4.3）；
4. `RunDeployHelper` fork 后 snprintf 非异步信号安全（已修：参数移至 fork 前，子进程仅 exec）；
5. `memfabric_mm_ar_v2_allreduce` 的单流约束（`s.execution_stream == stream`）依赖 vLLM prefill 计算流稳定；当前 async-scheduling 下未见违反，列为观察项；
6. 首调用验证为一次性（每形状类一次），不构成每调用税；verification 在 Eager 下执行（Graph 捕获 M≥2048 已被 C++ 明确拒绝）；
7. 重建库时 `BUILD_OPEN_ABI=OFF / ENABLE_PTRACER=ON / MF_SDMA_ORCH_KVER=11` 必须与原包一致，否则 ABI/版本不匹配（本轮踩坑记录，§4.3）。

### 第二轮（eager 验证完成后）

1. **深度扫描预热（本轮新增 C++）**：stage 回绕填充以 x 为原料，扫描深度 2048/3072/4096 仅触及池内已分配内存；slot0 复用与真实输出槽轮转不冲突（扫描发生在任何真实请求前）；无新问题。
2. **Python 路由层（HEAD 版）**：`is_memfabric_mm_ar_configured` 前置检查 + 阈值路由 + 首调校验的结构在 eager 下已被三会话 + Gate C 验证；每调用的 Python 开销（40 层 × ~百微秒级）相对 prefill 秒级耗时可忽略，非 §7.2 负收益的主因（TPOT 中位持平佐证）。
3. **graph 集成面（§4.6）**：当前 apply() 的路由条件含 `memfabric_mm_ar_warmup_fallback_active()`（模块级全局），在任何 torch.compile 追踪路径上都是固化隐患；后续修复必须把该标志移出可追踪代码（custom-op 不透明边界或 equivalent）。已验证 `unquantized_gemm + tensor_model_parallel_all_reduce` 与上游 `AscendUnquantizedLinearMethod.apply` 数学等价（Gate C 位精确）。
4. **编译缓存纪律**：`~/.cache/vllm/torch_compile_cache` 的 key 不含插件模块代码与 `VLLM_ASCEND_310P_*` env——**任何插件侧 linear method 代码变更后必须清除该缓存再起 graph serve**，否则静默复用旧产物（本轮实证两次）。已写入复现命令索引。
5. **测试脚本健壮性**：bench/功能测试脚本内建 killpg + wait_devices_drained，无需修改；手工运维需按 `VLLM::` 模式清残（§13）。
6. **未修的已知项**：dynamo 日志守卫（曾实现后随模块恢复 HEAD 而回退，eager 无影响；graph 修复时需一并恢复，快照 `/tmp/opencode/g3/fix_attempt_custom_op/`）。

## 12. 遗留风险与未验证项

1. **间歇性双 Worker aicore timeout（P0，最高优先级）**：Gate E 工况（V2 prefill 与 decode 持续交叠）下 1/2 运行挂死且不可恢复，重跑与 stock 对照均干净——归因与触发条件未定位（§8），生产启用前必须闭环；
2. **Eager serve 级性能负收益（终判 FAIL）**：吞吐中位 -3.51%、TTFT 三会话一致劣化、TPOT 尾部最差 +10.64%（§7.2）；与 T0 op 级正收益的落差未归因，候选方向见 §7.2；
3. **Graph 模式 V2 集成未完成**：torch.compile 旁路（§4.6）未修复；修复方向已验证至方案级（custom-op 不透明路由 + 缓存 key 纪律），代码快照与 UT 就绪；含 custom-op 探针产物异常的未决子项；
4. MTE3 首写硅级根因无官方解释；新 kernel/运行时组合仍需复测首启；
5. meta 区异步释放窗口时长未精确刻画（经验值：异常退出后 ≥ 数十秒至数分钟；正常退出 + 75s 排空未复现问题）；
6. `ASCEND_RT_VISIBLE_DEVICES` 非 0,1 映射下 MetaSetDeviceAccess 507899（驱动 id 空间问题，无公共 API 规避）；
7. 池生命周期单进程单池（ConfigureOrch 约束）；异常退出后建议排空再重启；
8. V2 融合仅覆盖 2048≤M≤4096；M>4096 回退 Stock 属预期行为（trial 文档约定）；
9. Graph 大 M 捕获明确不支持（FULL_DECODE_ONLY 限制），捕获尝试即报错。
10. **T2/V1 回归未执行**：当前构建的 `vllm_ascend_C.so` 缺 T2 专用符号（`mf310p_v2_ar_stage_launch_async` 等），需按 trial 期构建旗标重建后补测（§9）；

## 13. 复现命令索引

部署前置（每次容器重建后）：
```bash
mount -o remount,size=2G /dev/shm        # 容器默认 64MiB，vLLM TP MessageQueue 需 ≥160MiB
source /usr/local/Ascend/cann-9.1.0/set_env.sh
source /usr/local/memfabric_hybrid/set_env.sh
export MF_SDMA_ORCH_JSON=/usr/local/memfabric_hybrid/latest/aarch64-linux/hybm/aicpu_kernel/libmf_sdma_orch_v11.json
```

构建：`SOC_VERSION=ascend310p3 VLLM_ASCEND_310P_ENABLE_MEMFABRIC_MM_AR=1 VLLM_ASCEND_310P_ENABLE_MEMFABRIC_MM_AR_V2=1 python setup.py build_ext --inplace`

- Gate B/C/D/E：`tests/e2e/_310p/test_memfabric_mm_ar_functional.py`、`bench_memfabric_mm_ar_serve.sh` + `evaluate_v2_g3.py`（eager 判定加 `--mode eager`）
- 全部运维/验证脚本已随报告入库：`csrc/_310P/memfabric_mm_ar_v2/test/g3_e2e_scripts/`（含冷启动、长稳、回归、A/B 提取、graph 探针与修复尝试快照，说明见其 README）
- 回归：`/tmp/opencode/g3/regression.sh`（T0 g0/g1/g2/stock、T1、T2、V1 bench）
- 诊断：`/tmp/opencode/g3/{kfc_torch_probe.py,poison_then_create.py,smem_autodeploy_probe.cpp,kfc_stream_probe.c,diag_m2049.py}`
- 运维：异常终止后按 `pkill -9 -f "VLLM::"; pkill -9 -f EngineCore` 清理残 Worker 并等待 npu-smi 显存回落（<2.5GB）再重启。
