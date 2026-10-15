# 310P3 MemFabric mm_ar 小 M 计算优化设计

状态: 设计稿（未实施） | 日期: 2026-09-30
关联: [310p_memfabric_mm_ar.md](./310p_memfabric_mm_ar.md) · [VALIDATION_REPORT](../../../csrc/_310P/memfabric_mm_ar/VALIDATION_REPORT.md)

---

## 1. 背景与问题

当前实现（全 M 融合）对任意 M 一律补零到通信批 `batch_m=256q`
后走 8 核协作 MM。算子级实测（2026-09-29）：

| M | fused q=1 (ms) | fused q=2 (ms) | stock (ms) | 浪费 |
|---:|---:|---:|---:|---|
| 1 | 0.472 | 0.550~0.607 | 0.289 | 255/256 行白算、白传 |
| 10（decode 真实形状） | 0.472 | 0.567 | 0.297 | 98% 计算与带宽浪费 |
| 64 | 0.471 | 0.599~0.616 | 0.300~0.326 | 87%+ |
| 256 | 0.473 | 0.628 | 0.382 | 0（q=1 时恰好一批） |

小 M 恰是 decode 主路径（并发 10 → 每步 M≈10，衰减到 1），是 e2e 时间占比
~85% 的 decode 阶段每步 ×40 层都在付这笔浪费。

### 既有分区（三段）与问题

```text
M: 0 ── 补零到 batch_m（单批串行） ──≤ batch_m ── 多批流水 + 尾批补零 ──→
```

单批区（M ≤ batch_m）即使不满批也补零到整批，MM/SDMA 双重浪费。

## 2. 已定设计决策

| 决策 | 内容 |
|---|---|
| D1 | **不搞灰区补零**：M < B 一律 exact-M 串行路径，不出现"补零凑批"的单批区 |
| D2 | **B = batch_m = 256**（q=1）：边界即通信批，两段分区零灰区；M=255 由 exact 路径算 256 行（8核×32行，1 行余量），M=256 恰好整批走批路径 |
| D3 | **备选 Variant B = 512**（batch_m 与 B 同为 512）：保住大 M 收益，代价是 exact 路径需 3 套 per-core 模板（见 §7） |
| D4 | exact 路径逻辑为**串行链**：MM → signal → wait → add → quiet → ack，单批无批间可交叠对象；lookahead 流水仅保留给多批波 |
| D5 | wave 协议骨架（gate/credit/quiet/ack/generation/ready 汇合）**不变**，exact 波同样消耗 1 个 wave credit、复用 batch-0 arena 槽位 |

## 3. 关键性能事实：M-split 下 MM 成本 ∝ B 带宽，不 ∝ M

这是本设计必须先说清的物理约束，也是"直接算 M 行"直觉能否兑现的分水岭：

- 8 核 M-split 中**每核都要流完整份 B 权重（8MB NZ）**。实测 MM 耗时：
  256 行批 ≈ 220µs，512 行批 ≈ 273µs → 纯计算分量仅 ~50µs/256 行，
  **B 流带宽地板 ≈ 170~220µs/核**，与 M 基本无关
- 因此仅把"补零 256 行"改成"exact 16 行"（仍 M-split），MM 只省计算分量
  （~50µs），B 地板原封不动——**Phase 1 的收益上限是 SDMA 载荷与拷贝的消除，
  不是 MM 本身**
- 要让 MM 成本真正随 M 消失，需 **N-split**（每核只流 B 的 1/8 列块，1MB），
  这是 Phase 2，附带有真实的开放问题（§6）

## 4. 分区图（目标状态）

```text
M: 0 ──[ exact-M 串行路径: 只算 ceil(M/16)×16 行, 传 M 行 ]──≤ 256 ──[ 256 行批, lookahead 流水, 尾批补零 ≤255 行 ]──→
```

- decode（M 1~16）、chunked prefill 尾块（如 4100-token prompt 的 4-token 块）→ exact 路径
- prefill 整块（4096）→ 批路径 16 批流水
- 边界自洽：M=255 exact 算 256 行（浪费 1 行）；M=256 整批零浪费

## 5. Phase 1 设计：M-split exact-M + 变长通信（低风险落地）

### 5.1 路由

C++ op 内部分支（Python 层不动，graph 捕获时分支随 M 一起烘焙，replay 的 M
与捕获一致由 vllm capture size 机制保证）：

```cpp
if (M < batch_m)  run_exact_serial_path(x, w, M);   // 本设计
else              /* 现有批路径, batch_m=256 */
```

新增 env 开关 `VLLM_ASCEND_310P_MEMFABRIC_MM_AR_SMALL_M_PATH`（默认 1），
便于 A/B 对照。

### 5.2 内核：动态 16 行 fractal 单元分区

- `perCoreRows = 16`（M ≤ 128）/ `32`（128 < M < 256）→ **2 套编译期 tiling
  模板**（basicM=16/32，basicN=256、basicK=64 沿用已验证的 B 槽满配）
- `activeCores = ceil(M / perCoreRows)`；blockdim 仍 8，**空闲核立即发布
  ready 后退出**（core0 的 8 核 ready 汇合逻辑零改动）
- 输入直读 x，不做整体拷贝；**尾 unit 越界处理**：最后一个 16 行 unit 若越过
  M（M%16≠0），该 unit 的 A 指向 64KB 尾部暂存区（16 行，分配时清零一次），
  每次只拷贝 r=M%16 行有效数据进去。陈旧行会被计算但绝不被 reduce
  （add 只读 M 行；MM 输出行只依赖自身输入行），与现有尾批论证同构

### 5.3 变长通信（一处巧合：wait 内核零改动）

| 环节 | 改动 |
|---|---|
| signal | `len = M × 2048 × 2`（参数本就可变），imm=0（batch 0） |
| wait | **内核零改动**：`expectedDst = expectedRecv_base + 0×batchBytes` 恒等于槽基址；`m.len == batchBytes` 校验恰好复用——把期望长度作为既有 `batchBytes` 入参传入即可。仅 adapter 层加一个显式长度变体 |
| add | **零改动**：`add_batch_async(out, batch=0, valid_rows=M)` 现成支持（elems=M×2048，%128 对齐恒成立） |
| gate/quiet/ack | 零改动（exact 波 = 1 credit 的标准波） |

### 5.4 波编排（串行链，无 lookahead）

```text
prepare_wave(gen) → gate → [尾部暂存拷贝, 仅 M%16≠0] → producer_small
→ wait(len=M×row_bytes) → add(valid=M) → quiet → ack
```

### 5.5 预期与定标实验

| M=10 分解 | 现状 q=1 补零 | Phase 1 exact |
|---|---:|---:|
| 协议固定 | ~0.20 | ~0.20 |
| MM | ~0.22（256 行） | **0.10~0.22（B 地板，见下）** |
| SDMA | ~0.05（1MB） | ~0.00（40KB） |
| 尾部拷贝 | ε | ~0.01（≤60KB） |
| **合计** | **0.47** | **0.31~0.43** |

**关键未知量（必须先定标）**：单/少核活跃时 B 地板的实际值。8 样 M-split 下
8 核并发共读 64MB（L2 聚合带宽受限）；1 核活跃只流 8MB——若 L2 聚合带宽是
瓶颈，少核反而更快（MM 可低至 ~100µs）；若单核 MTE1 端口是瓶颈则仍 ~200µs。
定标实验：activeCores ∈ {1,2,4,8} 扫 MM 耗时。

Phase 1 结论区间：M=10 从 0.47 降到 0.31~0.43，**未必追平 stock 0.30**——
它的价值是先落协议/内核骨架并消灭 SDMA 浪费，为 Phase 2 铺路。

## 6. Phase 2 设计：N-split（兑现"计算成本随 M 消失"）

每核处理 B 的一个 256 列块（1MB）而非整份 8MB，MM 期望 ~40~80µs：

- **权重双布局**：`process_weights_after_loading` 一次性额外生成 8 份按核
  NZ 重排的 [2048, 256] 列块（+8MB 显存），exact 路径专用；批路径继续用整份 NZ
- **C 输出三选一**（开放问题，按验证顺序）：
  1. 经典 Matmul ND C 的 stride 写（API 支持性待验证）→ 零额外开销，最优
  2. 每核写独立 [M,256] 暂存 + 自研 gather 内核拼回 batch-0 槽（≤1MB，
     向量核预期 10~30µs，需实测——注意引擎慢路径 4.9GB/s 的教训）
  3. 8 signal 变体（每核直发自己的暂存区）——mail 协议要改 8 mail/波，不推荐
- add 改为从 gather 后槽位读（方案 1/2）或 8 源散读（自研内核可控）
- **预期**：M=10 ≈ 0.20 协议 + 0.05~0.08 MM + ε ≈ **0.25~0.30，追平或反超
  stock**；graph 模式下协议地板中 host 间隙消失，单层 ~0.1ms，
  decode 每步 40 层可省 ~8ms（对 ~96ms 步长约 +8% 吞吐）

## 7. batch_m=256 vs 512（Variant A/B）取舍

| | A: B=batch_m=256 | B: B=batch_m=512（备选） |
|---|---|---|
| exact 模板 | {16, 32} 2 套 | {16, 32, 64} 3 套 |
| 灰区 | 无 | 无（B=batch_m 恒无灰区） |
| 大 M 批路径 | q=1 实测：2048 → **-23%**、4096 → **-0.9%**（vs stock） | q=2 实测：2048 → +3.3%、4096 → **+20%** |
| prefill 代价 | 每请求 4096 prefill 多付 40 层×0.58ms ≈ **+23ms/请求** | 无 |
| 适用 | decode 占比极端高、prefill 不敏感 | 当前验收负载（4096/2048/并发10） |

**建议执行序**：先按 D2 落 Variant A 看 decode 侧兑现度（含 Phase 1/2 定标），
若 e2e 中 prefill 回退抵消 decode 收益，切 Variant B（仅多 1 套模板，
协议与内核骨架完全复用）。

## 8. 验证计划与验收标准

1. **定标**：activeCores∈{1,2,4,8} 的 MM 耗时；gather/stride-C 成本（Phase 2）
2. **bit-exact（atol=rtol=0）**：M ∈ {1..17, 31..33, 63..65, 127..129, 255, 256}
   × eager/graph × 大小批交错（含先大批后小批的尾部暂存陈旧覆盖）× diverge-va
3. **协议健康**：debug_snapshot status=0，无 trap；graph replay 后 eager 连续性
4. **算子级**：M 梯度 1~4096 vs stock vs 现状补零；batch_m 256/512 大 M 对比
5. **e2e**：四组合复验（重点 fused_graph 的 ITL 与吞吐）
6. **验收**：Phase 1 — M=10 ≤ 0.45ms 且 bit-exact 全绿；Phase 2 — M=10 ≤
   0.30ms；大 M 不劣于所选 Variant 的批路径基线 -5% 以内
7. UT 源码守卫同步：分区分支、尾部暂存一次清零、变长 mail 校验、空闲核 ready

## 9. 不变量与风险

**保留的全部既有边界**：MemFabric 黑盒（仅 public signal/wait/quiet）、
core0 唯一 signal owner、mail 严格校验（变长后校验更严）、generation 协议、
arena 定容、eager 单流、graph 前置 warmup、feature-off 不依赖安装。

**风险**：

- R1: B 地板定标结果差（单核 MTE1 瓶颈）→ Phase 1 收益缩水至仅 SDMA 部分
  （~0.05ms），此时直接跳 Phase 2
- R2: N-split 的 stride-C/gather 成本超预期 → Phase 2 收益被 gather 吃掉，
  回退判定线：gather > 50µs
- R3: 上游 D3/R6 wedge 与本设计正交（eager 长负载崩溃风险不变），e2e 验证
  仍须 graph 模式
- R4: graph capture sizes 若含 (batch_m, B) 之外的自定义值，捕获前需保证
  分支判定与 replay M 一致（vllm capture size 机制已保证，测试覆盖即可）

## 10. 实施状态（2026-09-30）

定标结论（详见第 3 节）后**跳过 Phase 1 直接实施 Phase 2 N-split**，
且不做满 AB 槽 tiling（定标实测 baseN=16/baseK=1024 更差，90/241µs，弃用）：

- **已实现**（源码 + feature-on 编译通过 + 27 项 UT 源码守卫全绿）：
    - `memfabric310p_device.asc`：`MF310P_NSPLIT_TILING(16/32/64/128/256)`
    模板、`Mf310pNsplitProducerKernel`（8 核全活、共享 staging A、每核
    1 MiB 列切片、blocked slot `[8][T][256]`、core0 唯一 signal owner）、
    `mf310pAddBlockedKernel`（256 元素段非阻塞寻址，每段恰为一行×一个
    N 块）、全套 launcher + 首启 no-op warmup 入口
    - adapter：`mf310p_build_weight_slices`（8×`aclrtMemcpy2dAsync` strided
    重排，NZ 模型 [k_f=128][n_f=128][512B]，slice c=n_f∈[16c,16c+16)）及
    small producer/wait（显式长度复用既有 wait 内核）/add-blocked/warmup ABI
    - runtime：路由 `M < min(batch_m, 256)` 且
    `VLLM_ASCEND_310P_MEMFABRIC_MM_AR_SMALL_M_PATH`（默认开）；staging 复用
    producer scratch；串行链 copy→prepare→gate→producer→wait→add→quiet→ack；
    权重切片按指针缓存（上限 64 条）；任何 eager 融合调用预热全部 5 个模板
    符号 + blocked add；`BATCH_BASEM_COUNT` 默认 2→1（决策 D2）
- **待实机验证**：bit-exact 全 M 矩阵（同时验证 NZ 切片布局模型——若错则
  翻转 k_f/n_f 解释重写 memcpy2d 参数）、算子级性能（M 梯度 vs stock）、
  e2e 四组合、UT/文档随测更新
- **设备阻塞（2026-09-30 深入定位）**：机器于 09-29 13:32 UTC 重启后，KFC
  `system()` 通道全机（4 卡同症）失效，`smem_shm_create` 阶段即失败，与本次
  代码无关（上次成功建池 09-28 23:34 UTC）。两级根因：
  1. 设备侧 KFC handler 重启后不再走"soname 缺失→dlsym 回退"路径（对照
     9-22 工作日志 `ae_kernel_lib_aicpu_kfc.cpp:168` 成功 vs 现 `:95` 失败）。
     **已找到绕过**：json 的 `kernelSo` 改指设备上必然存在的 built-in
     kernel（`libcpu_kernels.so`）后 api 解析成功（实测日志
     `Get api system from so libcpu_kernels.so success`）。
  2. 但 `system()` 执行本身返回 -1（fork/exec 失败，设备 OS 层），且本次
     重启后 OPP 包签名校验被启用（E30009，未签名 cann-kfc-compat 包导致
     TsdOpen 507033，已回滚恢复设备打开）。签名强制 + fork 受阻共同指向
     重启后设备侧安全/沙箱模式变化，容器内无法处置。
    - **恢复所需（宿主机侧）**：a) `npu-smi set -t reset -i 5664 -c 0`（须在
    宿主机执行，容器内被拒）；b) 若复位无效，排查该次重启启用的
    driver/firmware 安全模式（签名校验/沙箱），恢复 9-29 重启前的配置；
    c) 恢复后验证顺序：`mini_kfc 0 "echo hello"` sync=0 → 若仍 -1 而解析
    成功，可用 `kfc_try 0 libcpu_kernels.so` 载体 + CUST 迁移把
    `kfc_min.so` 落到设备搜索路径（bootstrap 全链），再跑 bit-exact。
    - **KFC 必要性定论（2026-09-30 实验链）**：KFC 仅是初始化期把
    orchestrator kernel 从 CUST 落盘目录（/home/CustAiCpuUser）搬到
    regular 引擎搜索目录（aicpu_kernels_device）的唯一"桥"，运行时数据面
    不依赖它。绕行实验全部完成：CUST 迁移通道重启后完好，经其直连
    （memfabric_hybrid `MF_SDMA_ORCH_CUST_DIRECT=1`，commit 254e6d85）可
    **KFC-free 完成池创建**，epoch kernel 亦能在 aicpu_custom_scheduler
    内执行；但 custom 引擎 LD_LIBRARY_PATH 受限（无 /home/HwHiAiUser/hs
    运行库）且 SQ 属 regular 引擎进程上下文，epoch 的 halSqCqQuery 返回
    24，数据面不可用。AICPUKernel 类（路由 regular 引擎）则因 so 不在
    搜索目录而无法执行。`ASCEND_CUST_AICPU_KERNEL_CACHE_PATH` 为宿主侧
    变量、不改设备侧落盘路径；包配置 `verifyFlag:Close` 不豁免设备级签名
    校验。结论：替代 KFC 需要一个可写 regular 引擎搜索目录的通道，当前
    仅 KFC / 华为签名包 / 设备复位三者可得——**复位仍是最短恢复路径**。
