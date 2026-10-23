# D1R4 双 Rank 时间线（Gate E 复现会话，2026-10-21）

> 会话：D1R4（G1-D 复审批次第 1 轮），fused Eager 原工况，全诊断取证。
> 时间均为 UTC。证据分级：observed=日志直录；derived=带推导链的算术结论。
> 数据来源：`sanitized_logs/D1R4_FAULT_WINDOW.log`（脱敏节选）及服务器原始
> `gate_e_D1R4/`（serve.log、设备 plog 29 件、结构化摘要，SHA256 清单随包）。

---

## 1. 会话时间线

| 时刻（UTC） | 事件 | 分级 |
|---|---|---|
| 08:23:12 | fused serve 启动（全诊断环境，G1-D 全取证） | observed |
| 08:26:52 | healthy（80 层全部接入 V2 路由） | observed |
| 08:28:17 | 双 Rank 首调 V2（M=4096 路由） | observed |
| rep1 | 25/25 请求全部完成（健康） | observed |
| ≤08:34:35 | dev0 到达环冻结于 arrHead=8632（此后 14 个 1Hz 采样帧不变，冻结 ≥13s） | observed |
| 08:34:49.395 | rank0（Worker_TP0）`aclrtSynchronizeEvent` **507014 AICore timeout**（设备 plog 0x7150025，硬件超时码 0x25） | observed |
| ~08:34:50.1 | dev1 到达环 8632→8634（rank0 侧 AICPU 编排继续排空请求环，向 dev1 环补发 8633/8634 后冻结） | observed（补发归因 derived） |
| 08:34:50–58 | rank1 先完成 gen1730 的 chunk1 等待返回（mail seq 8633, status=0），再卡 chunk2 等待 | observed |
| 08:34:58.078 | rank1（Worker_TP1）**507014**（同签名；与 rank0 间隔 8.7s） | observed |
| rep2–rep11 | rep2 完成 17 请求后停滞；rep3–11 全 0（连接失败） | observed |

## 2. 停滞瞬间双 Rank 关键状态（最后有效帧）

| 证据 | rank0 / dev0 | rank1 / dev1 | 分级 |
|---|---|---|---|
| V2 8 核代际 | 1730（8/8 一致） | 1730（8/8 一致） | observed |
| V2 core0 | `1730:0:50`＝等 **chunk0** 邮件，**未达 52/53**（wait 从未返回） | `1730:2:50`＝等 **chunk2** 邮件，未达 52/53 | observed |
| V2 core1–7 | `1730:0:60`（IB 汇合等待，下游） | `1730:2:60` | observed |
| `[V2_G1D]` 同代邮件返回 | 末次为 gen1728 c3（seq 8625）；**gen1730 零返回** | gen1730 c1＝seq 8633 **status=0 正常返回**（mailDst=0x12db00400000, len=4194304, imm=0x4d46325600000001）后卡 c2 | observed |
| 本端到达环 ArrHead（Host D2H 读保留区） | **8632**（冻结 ≥13s） | **8634**（08:34:50 冻结） | observed |
| 被等待槽 ＝ ArrHead % 1024 | **440** | **442** | derived（游标数学，见 §3） |
| 期待邮件序号 ＝ ArrHead + 1 | **8633** | **8635** | derived |
| 该槽实读旧 stamp（Host DMA 直读显存） | **7609**（＝8633−1024，上圈值） | **7611**（＝8635−1024） | observed |
| 边界槽 stamp1020..1023 / mail1023 | 8189..8192 / 8192（lap-8 值，环绕自洽） | 同左 | observed |
| `[MF_G1C]` SQ / reqSeq | hwHead=hwTail=**302**（空，rc=0），reqSeq 8634 全消费，phase 8 空闲 | hwHead=hwTail=**308**（空），reqSeq 8636，phase 8 | observed |
| Epoch | 纪 30 运行中，此前全 exitReason=0 | 纪 31 运行中 | observed |
| `[MF_G1D]` snapshotError | 0（全程 449 帧） | 0（全程 459 帧） | observed |
| Worker 表面失败 | 507014 @ 08:34:49.395Z | 507014 @ 08:34:58.078Z | observed |

## 3. 游标数学（更正后口径，全案钥匙）

`wait_at`（memfabric_hybrid `src/smem/include/device/smem_shm_aicore_base_sdma.h:156-176`）：

```cpp
head   = SdmaReadCell(res, ARR_HEAD);              // 我已消费到第几件
expect = head + 1;                                 // 下一件单号
loop { stamp = SdmaReadCell(res, ARR_STAMP + (head % 1024) * 8);
       if (stamp == expect) break; }               // 盯一个柜子等单据
... 取件后 SdmaWriteCell(res, ARR_HEAD, head + 1); // 自己推进游标
```

**ArrHead 是接收方消费游标，不是发货计数。** 代入 D1R4 冻结值：

| | rank0 / dev0 环 | rank1 / dev1 环 |
|---|---|---|
| 冻结 ArrHead | 8632 | 8634 |
| 盯的柜子（head%1024） | **440 号** | **442 号** |
| 期待单号（head+1） | **8633** | **8635** |
| 柜内实读（Host DMA） | **7609**（旧单，恰差一圈） | **7611**（旧单） |

**双读路径互证（observed）**：Host 用 D2H 直读显存与 AICore 用 volatile load
轮询看到**同一个旧值**；同帧快照里 0/1/2 号柜读到本圈新单（8193–8195），
证明 Host 读取路径正常——唯独被等柜没有新单。

## 4. 判别性发现（相对 G1-C 的增量）

1. **stage 50 ≠ 52/53**：双 Rank 的 `wait_at` 从未返回——直接排除"wait 返回后
   邮件校验失败"分支。
2. **控制面全绿**：请求环全消费（reqSeq 推到头）、SQ 排空（head=tail）、
   rc=0、Epoch 干净轮转、编排空闲——控制面坚信"所有件都派完了"。
   **"SQ 排空不能证明 SQE 正确落地"**。
3. **槽位在环中部**：440/442 位于 lap-9 中部，距 1023 边界约 582 槽——
   故障侧直接反证"仅 1023 边界丢写"假设（健康侧 D2-B 边界取证亦全对）。
4. **等待形态**：三次故障（C2R1 之后的 C2R2 / D1R4）一致——要么即时完成、
   要么永久挂起，无"慢等待/近失"样本。
5. **相邻槽不对称（dev1 环）**：seq 8633（槽 440）正常落环并被消费返回后
   ≤0.2s，下一件（期待 8635）永不出现——支持**单件丢失**而非连续损坏。

## 5. 因果链（derived，强候选——尚需逐件 SQE 证据定案）

V2 核循环为"先 signal 当前块、后 wait 前块"（`mf310p_v2_kernel.asc`）：

- rank0 卡在 k=1 的 `wait(chunk0)`——等的是 rank1 的 chunk0 邮件
  （dev0 环 440 号柜，单号 8633，**原发性丢失**）；
- rank0 因此永远走不到 k=2 的 `signal(chunk2)`；
- rank1 卡在 k=3 的 `wait(chunk2)`——它等的邮件**注定不会来**（对侧未发），
  属级联停滞，无需第二个独立故障解释；
- 两核互相吊住 → AICore 硬件超时（0x25）→ 双 Worker 507014。

即：**表象两方向各丢一封，实为一起单向丢失 + 对侧级联**（C2R2 同构）。
当前证据能夹逼出丢失区间为"请求被 AICPU 消费之后、对端保留区写入落地之前"，
该区间全部位于 MemFabric 执行域（SQE 组装/提交 → CP/SDMA 执行 → 写显存）；
但区间内**具体哪一步**仍缺逐 SQE 硬件完成记录，不能裁决。

## 6. 历史故障同构对照

| 会话 | rank0 等待 | rank1 等待 | 结构 |
|---|---|---|---|
| C2R2（10-20） | chunk1 | chunk3 | 单向原发 + 对侧级联 |
| D1R4（10-21） | chunk0 | chunk2 | 单向原发 + 对侧级联 |

详见 `sanitized_logs/C2R2_171840_172030.log` 与 `已证实_推断_未证实.md` §正负例。
