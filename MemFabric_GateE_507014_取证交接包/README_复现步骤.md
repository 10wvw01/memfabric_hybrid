# MemFabric Gate E 507014 取证交接包（README · 复现步骤）

> 生成：2026-10-23。
> 来源：依据《310P3 融合算子：V1/V2 性能与稳定性专项排障总账（2026-10-23）》§5.3
> "给 MemFabric 团队的'拿到就能查'资料包"整理。
> 用途：向 MemFabric 团队交接 Gate E 双 Rank 偶发挂死（507014 / P0）的故障事实、
> 精确复现方法与已冻结证据，供联合取证裁决。
> 证据分级：**observed**（日志直录）/ **derived**（带推导链的算术结论）/ **unknown**（无证据，禁止推测补全）。

---

## 1. 一页故障概述

**工况**：Qwen3.6-35B-A3B-w8a8，TP=2（单卡双 die：dev0=rank0 / dev1=rank1），
vLLM Eager 模式长稳验收（Gate E）。负载为随机请求 input=4096 / output=256、
每轮 25 prompts、并发 10，持续 620 秒。

**现象**：连续服务约 6~11 分钟后，两个 Worker 几乎同时（相差秒级）报
`507014 AICore execution timeout`，进程无法恢复。故障为偶发：历史会话级
复现率约 3/14 ≈ 21.4%（G3 1/2 + G1-C 1/2 + G1-D 1/10）。

**故障点（D1R4 会话，2026-10-21 08:23–08:38 UTC，observed）**：V2 融合算子
8 个 AI Core 一致停在代际 1730；两侧 Core0 均停在 **阶段 50**——即
`smem_shm_sdma_wait_at` 等待对端跨卡"到达邮件"，**从未返回**（未达 52/53，
排除"返回后校验失败"分支）。

**D1R4 关键数字（observed，采用 ArrHead 语义更正后的口径，见 §4 更正说明）**：

| 读到的值 | dev0 / rank0 | dev1 / rank1 |
|---|---:|---:|
| V2 代际（8 核一致） | 1730 | 1730 |
| Core0 位置 | 等 chunk0 邮件，阶段 50，`wait_at` 未返回 | 等 chunk2 邮件，阶段 50，`wait_at` 未返回 |
| Core1–7 | 阶段 60（IB 汇合等待，下游） | 阶段 60 |
| ArrHead（接收消费游标，Host D2H 读） | **8632**（冻结 ≥13s） | **8634**（08:34:50 由 8632 跳入后冻结） |
| 真正被等的槽 = ArrHead % 1024 | **440** | **442** |
| 应出现的邮件序号 = ArrHead + 1 | **8633** | **8635** |
| Host 读到该槽旧 stamp | **7609**（= 8633 − 1024，恰差一圈） | **7611**（= 8635 − 1024） |
| AICPU 请求消费 / SQ 状态 | reqSeq 8634，SQ head=tail=302（空，rc=0） | reqSeq 8636，SQ head=tail=308（空） |
| Worker 表面失败 | 507014，08:34:49.395Z | 507014，08:34:58.078Z |

**这组数字为什么关键**：被等槽里的旧单号恰好比期待单号少 1024，说明"旧的一圈
还在，新的一封从未被接收侧看见"。问题被收缩到**单件完成邮件的投递落地**，
而不是所有 chunk 同时损坏，也不是请求环/编排/SQ 排空问题（控制面全绿）。

**当前定性（交接口径，勿加码勿减码）**：这是非常具体的 **MemFabric 投递/内存
可见性接口异常**，理应由 MemFabric 团队主牵头调查；但尚未抓到与坏邮件一一对应的
那 3 条 SQE 的硬件完成记录，因此**还不能断言是 MemFabric Host 代码、AICPU 组装、
CP/SDMA 执行、缓存写回，还是 V2 与 MemFabric 之间并发/内存可见性约束未被满足**。
双方用同一份证据裁决。详见 `已证实_推断_未证实.md`。

---

## 2. 精确复现命令与配置

> 复现前提：310P3（Atlas 300I Duo）双 die 空闲、无外部占用；以下所有命令在
> 310P3 服务器上执行。**D1R4 会话使用的是诊断构建（设备侧 KVER=12），不是
> 生产 v11**——这是有意为之（取证需要），见 §3 与 `环境与SHA256.json`。

### 2.1 代码就位（两仓库）

```bash
# vllm-ascend（V2 融合算子 + 诊断埋点）
git checkout diag/310p3-g1d-minimal-forensics-20261021
#   D1R4 时点该分支 src/cmake/csrc 源码零改动（仅 test/docs/evidence 层）；
#   确切提交号不随包携带，由 V2 侧在联合取证时按二进制哈希对应提供

# memfabric_hybrid（Host 侧 G1-D 诊断构建）
git checkout diag/310p3-g1d-minimal-forensics-20261021
#   部署的 Host SO 为该分支断言式构建产物（二进制 SHA256 见 §2.2）
```

### 2.2 诊断构建（与 D1R4 逐字节一致的配方）

```bash
# MemFabric Host（独立 build-g1d/，不污染原装）
cmake --build build-g1d --config Release          # Ninja RELEASE
#   关键定义：-DMF_SDMA_ORCH_KVER=12 -DMF_SDMA_ORCH_G1C_DEVICE_TRACE=ON
#   产物 libmf_hybm_core.so SHA256 = 6e9dc2d699aa7b718a83f93397fbc2d0f18a799e384ffda9031d62f25195e55a
#   重构建的 KVER=12 设备库 libmf_sdma_orch.so 与保全产物逐字节一致（02290083…）

# V2 设备库（独立名，不覆盖正式库）
#   CMake 预置：VLLM_ASCEND_310P_V2_BUILD_DIAGNOSTICS=ON + G1C_TRACE=ON + G1D_TRACE=ON
#   产物 libmf310p_v2_trial_device_g1d.so SHA256 = d9a6de660677b13dbd83688dd250fbcdd3ea62c3427179b5060a1e64c5fbfa73
```

### 2.3 部署（断言式，先备份原装）

```bash
# 1) 备份原装 Host SO 双位置（/usr/local/memfabric_hybrid/latest/... 与
#    site-packages/memfabric_hybrid/lib/）后，替换为 6e9dc2d6… 诊断版（逐位置哈希断言）
# 2) 设备侧部署 v12（经独立进程 orch_deploy_helper，SHA 849f93c0…）：
#    dev0 rc=0 "deployed and verified"；dev1 rc=3（已存在，双 die 共享设备文件系统）
# 3) 显式指定 v12 launch JSON（不得模糊匹配最新 v*.json）：
export MF_SDMA_ORCH_JSON=/tmp/opencode/g1d/kernel_v12_stage/libmf_sdma_orch_v12.json
#    JSON SHA256 = 90172726965f4944ed955237c296faf1622762897f742ffbecf69a9e2fb554d0
```

### 2.4 启动 Gate E 复现（标准入口）

```bash
# 环境（脚本自带断言门禁：分支/工作树洁净/SO 哈希/JSON 哈希/设备可见性/禁常驻 Epoch）
export ASCEND_RT_VISIBLE_DEVICES=0,1
export G1D_MF_TREE=<memfabric G1-D worktree 绝对路径>
export G1D_HOST_SHA256_EXPECTED=6e9dc2d699aa7b718a83f93397fbc2d0f18a799e384ffda9031d62f25195e55a
export G1D_V12_JSON_SHA256_EXPECTED=90172726965f4944ed955237c296faf1622762897f742ffbecf69a9e2fb554d0
export G1D_PYINJ=<已核验的 sitecustomize 注入目录>   # 真实 Serving 的 import resolver

bash csrc/_310P/memfabric_mm_ar_v2/test/g3_e2e_scripts/g1d_run.sh gate-e <新会话编号>
#   └─ 内部调用 gate_e_forensic_run.sh fused → gate_e_eager.sh
#      （全程 1Hz 采样：npu-smi / 进程快照；provenance 自动记录）
```

真实 Serving 命令（`gate_e_eager.sh` 内核逻辑，模型路径与端口按服务器实际替换）：

```bash
export VLLM_ASCEND_310P_ENABLE_MEMFABRIC_MM_AR=1
export VLLM_ASCEND_310P_MEMFABRIC_STORE_URL=tcp://127.0.0.1:8581   # 本机回环
export VLLM_ASCEND_310P_MEMFABRIC_LOCAL_BYTES=$((96 * 1024 * 1024))
export VLLM_ASCEND_310P_MEMFABRIC_MM_AR_BATCH_BASEM_COUNT=1
export VLLM_ASCEND_310P_MEMFABRIC_MM_AR_WARMUP_FALLBACK=1
export VLLM_ASCEND_310P_MEMFABRIC_MM_AR_V2_SERVE=1
export VLLM_ASCEND_310P_MEMFABRIC_V2_CHUNK_ROWS=1024
export VLLM_ASCEND_310P_MEMFABRIC_V2_OUTPUT_SLOTS=4

setsid vllm serve "$MODEL" \
    --host 0.0.0.0 --port 8000 --tensor-parallel-size 2 \
    --quantization ascend --served-model-name qwen3.6-35b-a3b-w8a8 \
    --trust-remote-code --dtype float16 --max-num-seqs 16 \
    --max-num-batched-tokens 4096 --max-model-len 8192 \
    --gpu-memory-utilization 0.90 \
    --no-enable-prefix-caching --enable-chunked-prefill \
    --async-scheduling \
    --mamba-ssm-cache-dtype float16 --mamba-cache-mode align \
    --additional-config '{"enable_flashcomm1":false,"enable_flashcomm2_parallel_size":0,"enable_prefill_mc2":false,"enable_fused_mc2":0,"ascend_compilation_config":{"fuse_norm_quant":false,"enable_npugraph_ex":false}}' \
    --enforce-eager
# 模型：Qwen3.6-35B-A3B-w8a8（服务器本地路径，联系 V2 侧获取）
# 健康判定：日志出现 80 层 "Enable 310P3 TP=2 MemFabric unquantized mm_ar"

# 持续负载（滚动直至 620s，GATE_E_DURATION_S 可调）：
HF_HUB_OFFLINE=1 vllm bench serve --backend vllm \
    --endpoint /v1/completions --base-url "http://127.0.0.1:8000" \
    --model qwen3.6-35b-a3b-w8a8 --tokenizer "$MODEL" \
    --dataset-name random --random-input-len 4096 --random-output-len 256 \
    --num-prompts 25 --max-concurrency 10 --ignore-eos \
    --percentile-metrics ttft,tpot,itl --metric-percentiles "50,90,99,99.9" \
    --save-result --result-dir "$OUT_DIR" --result-filename "stability_rep${REP}.json"
```

### 2.5 请求集合 / warmup / 停止与恢复

- 请求集合：`vllm bench serve --dataset-name random`（随机 token，非固定语料）；
  每轮 25 prompts、并发 10，直至时间窗结束。D1R4 时间线：rep1 25/25 完成 →
  rep2 完成 17 个后停滞 → rep3–11 连接失败全 0。
- warmup / 初始化：`VLLM_ASCEND_310P_MEMFABRIC_MM_AR_WARMUP_FALLBACK=1`
  （首写保护 + warmup 回退）；V2 输出槽 4 个、新地址首写设备级同步由算子侧保证。
- **停止纪律（重要）**：**出现 507014 立即停止，不得自动重启**（重启会擦除设备
  现场）；先保全故障窗口的 plog/slog/aicore 错误日志（D1R4 保全设备 plog 29 件），
  再恢复环境（恢复脚本为断言式，恢复后双位置 Host SO 应回 `88d39285…` 原装值）。
- 复现参考：会话级约 21.4%（3/14）；同一二进制下 D1R1–R3 连续 300 请求未复现、
  D1R4 第 1 轮即复现——**样本量不足时"未复现"不构成证据**。

---

## 3. 本包内容清单

| 文件 | 内容 | 对应总账 §5.3 条目 |
|---|---|---|
| `README_复现步骤.md` | 本文件：故障概述 + 精确复现 | 条目 1、2 |
| `环境与SHA256.json` | 两仓库 Git 提交 + 全部二进制 SHA256 + 软件栈版本 | 条目 3 |
| `D1R4_双Rank时间线.md` | D1R4 会话时间线、停滞瞬间双 Rank 状态、判别性发现 | 条目 4 |
| `ARR与SQE单件证据.csv` | 被等槽/邻槽 stamp+mail 实测值；逐 SQE 证据缺口显式标注 | 条目 5、6 |
| `日志清单_SHA256SUMS.txt` | 包内全部文件 SHA256 自校验清单 | — |
| `已证实_推断_未证实.md` | 证据三级分级、正负例与边界反证、接口合同与验收、联合取证建议 | 条目 7、8 |
| `sanitized_logs/D1R4_FAULT_WINDOW.log` | D1R4 故障窗口脱敏节选（08:33:41–08:34:58） | 条目 4、5 |
| `sanitized_logs/C2R2_171840_172030.log` | G1-C C2R2 故障窗口脱敏节选（同签名历史故障） | 条目 7 |
| `sanitized_logs/B02_132050_132210.log` | G1-B B02 故障窗口脱敏节选（最早同类故障） | 条目 7 |
| `sanitized_logs/C2R1_171225_171235.log` | G1-C C2R1 健康对照脱敏节选（15027/15027、SQ 空） | 条目 7 |

**原始大日志处理原则**：完整原始日志（含 D1R4 设备 plog 29 件）仅存 310P3
服务器只读归档（SHA256 清单入 Git）；本包只含故障窗口相关的脱敏最小集。
脱敏规则：仅替换用户路径/主机名/内网 IP 标识，时间戳、Rank、seq、tag、
设备地址映射关系原样保留（可统一脱敏但不破坏对应关系）。

---

## 4. 更正说明（防误读，交接前必读）

早期 G1-D 报告（`G1D_EXECUTION_REPORT_20261021.md` §6.3）曾把 ArrHead 误认为
"发货计数"，从而把等待槽写成 439/441（head−1）。经核对 MemFabric `wait_at`
源码（`src/smem/include/device/smem_shm_aicore_base_sdma.h:156-176`），
**ArrHead 实际是接收方消费游标**：`wait_at` 盯的是 `ArrHead % 1024` 号槽
（head 本位），期待单号 `ArrHead + 1`，取件后自己推进游标。

因此**有效口径**为：

| | dev0 环 | dev1 环 |
|---|---|---|
| 被等槽（= ArrHead % 1024） | **440** | **442** |
| 期待单号（= ArrHead + 1） | **8633** | **8635** |
| 槽内旧值（Host D2H 实读） | 7609 | 7611 |

旧口径（被等槽 439/441、被等邮件 seq 8632/8634、"被等槽内容未采样"）**作废**；
且 D1R4 动态头快照读的槽位（`headSlot`）恰好就是被等槽——`headStamp=7609/7611`
是被等槽的实测内容，不是"恰好差一槽没读到"。完整更正清单见
`G1D_HANG_ROOT_CAUSE_LOCALIZATION_20261021.md` §七。

---

## 5. 深入材料索引（完整证据，均已公开可复核）

| 材料 | 位置 |
|---|---|
| 排障总账（本包依据） | vllm-ascend fork 分支 `docs/310p-v1-v2-perf-stability-ledger-20261023`，`csrc/_310P/memfabric_mm_ar_v2/docs/V1_V2_性能与稳定性专项排障总账_20261023.md` |
| G1-D 实机执行报告 | `diag/310p3-g1d-minimal-forensics-20261021` 同目录 `G1D_EXECUTION_REPORT_20261021.md` |
| ArrHead 语义更正与根因定位 | `G1D_HANG_ROOT_CAUSE_LOCALIZATION_20261021.md` |
| G1-C 设备第一停点报告 | `G1C_DEVICE_FIRST_STOP_REPORT_20261020.md` |
| G1-C 原始证据复核包 | `G1C_EVIDENCE_REVIEW_PACKAGE_20261021.md` + `evidence/g1c_20261020/`（164 文件 SHA256 清单） |
| G1-D 结构化证据 | `evidence/g1d_20261021/`（索引、会话统计、边界 JSON、脱敏节选） |
| D1R4 故障脱敏节选 | `evidence/g1d_20261021/sanitized/D1R4_FAULT_WINDOW.log`（本包已含副本） |
| MemFabric 到达环布局源码 | memfabric_hybrid `src/smem/include/device/smem_shm_sdma_layout.h`（ARR=1024、请求环/完成戳字段偏移） |
| MemFabric AICPU 设备 SQ 编排源码 | memfabric_hybrid `src/hybm/ops/hybm_kernel/hybm_sdma_orch_kernel.cc`（请求消费、三条 SQE 生成/提交、SQ Head/Tail） |
| 等待/投递原语源码 | memfabric_hybrid `src/smem/include/device/smem_shm_aicore_base_sdma.h`（wait_at:156 / signal_at:130 附近） |
| V2 核循环与埋点 | vllm-ascend `csrc/_310P/memfabric_mm_ar_v2/src/op_kernel/mf310p_v2_kernel.asc` |

> 使用原则：测试报告只证明其特定版本、硬件配置和运行条件下的结果；
> `derived` 不等于真实设备内存直采；**"故障区间已收敛"不等于"根因已解决"**。
